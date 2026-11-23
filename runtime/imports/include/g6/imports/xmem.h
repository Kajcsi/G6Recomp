#pragma once

// xmem.dll, the console's general allocator (docs/side-dlls.md, "xmem"). It has no host
// counterpart, so its functions are named after the game's import table.
//
// Every call takes an attribute word (XALLOC_ATTRIBUTES). Its layout comes from SlimEra's public
// reference (docs/related-work.md) and agrees with both words the side DLLs pass.

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace g6::loader {
class ImportRegistry;
}

namespace g6::imports::xmem {

// Memory types in the attribute word. 0 is ordinary CPU memory; the others are GPU-visible or
// physically contiguous memory, which needs memory_tracker.
inline constexpr uint32_t kMemoryTypeHeap = 0;

struct Attributes {
    uint32_t object_type = 0;    // bits 0-13
    uint32_t page_size = 0;      // bits 14-15: 0 is 4 KiB, 1 is 64 KiB, 2 is 4 MiB
    uint32_t allocator_id = 0;   // bits 16-23
    uint32_t alignment_log2 = 0; // bits 24-28
    uint32_t memory_type = 0;    // bits 29-32

    static constexpr Attributes decode(uint64_t word) {
        return Attributes{
            .object_type = static_cast<uint32_t>(word & 0x3FFF),
            .page_size = static_cast<uint32_t>((word >> 14) & 0x3),
            .allocator_id = static_cast<uint32_t>((word >> 16) & 0xFF),
            .alignment_log2 = static_cast<uint32_t>((word >> 24) & 0x1F),
            .memory_type = static_cast<uint32_t>((word >> 29) & 0xF),
        };
    }

    // The alignment an allocation gets: what the word asks for, and never less than the 16
    // bytes the host heap guarantees.
    constexpr size_t alignment() const {
        const size_t requested = size_t{1} << alignment_log2;
        return requested < 16 ? 16 : requested;
    }
};

// The HLE functions, with the signatures from SlimEra's reference. Heap memory comes from a
// dedicated host heap, aligned and zeroed. Other memory types log once and return null until
// memory_tracker exists.
void* XMemAlloc(size_t size, uint64_t attributes);
void XMemFree(void* address, uint64_t attributes);

// What XMemVirtualQuery fills: the host's own record for the region, then a word of GPU access
// bits. The one side DLL that imports it (the BVH builder, which we replace) zeroes 64 bytes
// before each call and accepts either of bits 0-1 as "GPU memory"; what each bit means is
// unknown. The rest of the 64 bytes is never written.
struct XMemBasicInformation {
    MEMORY_BASIC_INFORMATION basic;
    uint32_t gpu_access;
};
static_assert(offsetof(XMemBasicInformation, gpu_access) == 48);

// The bit we report for memory the GPU can use. memory_tracker will be able to sync any
// committed, readable memory to the GPU, so all of it counts.
inline constexpr uint32_t kGpuAccessVisible = 0x1;

// Queries the region at `address` of `process` (GetCurrentProcess() in every known caller).
// Fails like VirtualQueryEx, with its last error.
BOOL XMemVirtualQuery(HANDLE process, const void* address, XMemBasicInformation* info);

// The two fields of the console's working set statistics known from callers: the memory limit
// of the title, and how much of it is in use. The structure is bigger and its size differs
// between GDK versions, so only these 16 bytes are ever written.
struct XMemWorkingSetStatistics {
    uint64_t limit;
    uint64_t used;
};

// The game's memory limit on a Series X, the console the registry HLE reports
// (g6/imports/registry.h): 13.5 GiB of its 16.
inline constexpr uint64_t kSeriesXTitleMemory = uint64_t{27} << 29;

// Fills `statistics` with kSeriesXTitleMemory and the process's private bytes, capped at that
// limit. `attributes` names an allocator (the FSR upscaler passes 0x20000001); it is ignored,
// so every allocator reports the whole title.
BOOL XMemGetWorkingSetStatistics(uint64_t attributes, XMemWorkingSetStatistics* statistics);

// Moves `size` bytes of the console's memory budget in the direction `direction` names (the main
// executable passes 1 and 16 MiB once, at start; Leonida found the call). The host has no fixed
// budget to move between, so it succeeds and changes nothing.
BOOL XMemTransferMemory(uint32_t direction, size_t size);

// The console's VirtualAlloc with an attribute word, as the executable calls it (docs/boot.md):
// `address`, `size`, `type`, `attributes`, then `protection`. What the attribute word means is
// unknown (the executable passes 3, 7, and 0x21); it is logged and ignored. The page-size bits
// of `type` give the alignment: MEM_4MB_PAGES 4 MiB, MEM_64K_PAGES 64 KiB, MEM_LARGE_PAGES alone
// 2 MiB. A reserve with MEM_PHYSICAL and no MEM_COMMIT is a range for physical pages, which
// memory_tracker keeps as a placeholder; with MEM_REPLACE_PLACEHOLDER it is carved out of an
// earlier placeholder. MEM_RESERVE_PLACEHOLDER reserves a placeholder. Everything else is
// ordinary memory, allocated in place when it replaces part of a placeholder. The PAGE_GRAPHICS_*
// bits of `protection` go to memory_tracker, not to the host page.
void* XMemVirtualAlloc(void* address, size_t size, uint32_t type, uint64_t attributes,
                       uint32_t protection);

// The default allocator behind the hooks a title can install (the public GDK's
// XMemSetAllocationHooks): the same as XMemAlloc and XMemFree, since we install none.
void* XMemAllocDefault(size_t size, uint64_t attributes);
void XMemFreeDefault(void* address, uint64_t attributes);

// Whether the development kit's extra tools memory is there. A retail console has none.
BOOL XMemIsToolsMemAvailable();

// Binds xmem.dll's imports to the functions above.
void register_xmem(loader::ImportRegistry& imports);

} // namespace g6::imports::xmem
