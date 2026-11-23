#pragma once

// memory_tracker: the title's memory as the console's memory functions describe it (CLAUDE.md,
// "GPU"). The console's CPU and GPU share one memory, so the game asks for GPU access when it
// allocates and then writes GPU resources through CPU pointers. Everything the guest allocates
// with GPU access, or as a range for physical pages, goes through here, so that runtime/gpu can
// find the host memory behind a guest address at M3.
//
// So far it keeps the CPU side: placeholder reservations that physical pages are mapped into,
// allocations at the alignment of the console page size they ask for, and the GPU access each
// range asked for. The guest's own allocation functions (XMemVirtualAlloc, VirtualAlloc) decode
// the console's flags and call it with host flags.

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <windows.h>

namespace g6::gpu {

// The PAGE_GRAPHICS_* bits of a console protection, which desktop Windows refuses.
inline constexpr uint32_t kGraphicsProtectionMask = 0x7F800;

// A console protection split into what the host page gets and the GPU access the title asked
// for. The host keeps only the base protection and PAGE_GUARD: write-combined or uncached host
// pages would make the runtime's own reads of GPU memory slow. Memory that asks for GPU access
// alone is readable and writable on the host, since the runtime copies it to the GPU.
struct Protection {
    DWORD host = PAGE_NOACCESS;
    uint32_t graphics = 0;
};
Protection split_protection(uint32_t console_protection, bool committed);

// What the tracker knows about the range around an address.
struct RangeInfo {
    uintptr_t base = 0;       // start of the piece the address is in
    size_t size = 0;          // size of that piece
    uintptr_t origin = 0;     // start of the reservation the guest made, which the piece is part of
    uint32_t graphics = 0;    // PAGE_GRAPHICS_* bits it asked for
    bool physical = false;    // a range for physical pages
    bool placeholder = false; // still a host placeholder: nothing allocated or mapped in it
};

class MemoryTracker {
public:
    static MemoryTracker& instance();

    MemoryTracker() = default;
    MemoryTracker(const MemoryTracker&) = delete;
    MemoryTracker& operator=(const MemoryTracker&) = delete;

    // Reserves `size` bytes as a placeholder, at `address` or, when it is null, anywhere aligned
    // to `alignment` (a power of two of at least 64 KiB). `physical` marks a range for physical
    // pages. Returns null with the host's last error on failure.
    void* reserve_placeholder(void* address, size_t size, size_t alignment, bool physical,
                              uint32_t graphics);

    // Splits [address, address + size) out of a placeholder this tracker made and keeps it a
    // placeholder, marked as a range for physical pages. Fails with ERROR_INVALID_ADDRESS when
    // the range isn't inside one placeholder.
    void* carve_physical(void* address, size_t size, uint32_t graphics);

    // Reserves and/or commits ordinary memory (`type` is MEM_RESERVE and/or MEM_COMMIT, plus
    // MEM_TOP_DOWN) with a host protection. A null `address` is placed anywhere aligned to
    // `alignment`. An address inside a placeholder this tracker made is split out of it and
    // allocated in place; any other address goes to VirtualAlloc as it is.
    void* allocate(void* address, size_t size, DWORD type, DWORD protection, size_t alignment,
                   uint32_t graphics);

    // Frees what `allocate`, `reserve_placeholder`, or `carve_physical` made, like VirtualFree.
    // MEM_RELEASE of a range's base releases every piece the tracker split it into.
    BOOL free(void* address, size_t size, DWORD type);

    std::optional<RangeInfo> find(const void* address) const;

private:
    struct Range {
        uintptr_t origin = 0; // base of the reservation the guest made, which this piece is from
        size_t size = 0;
        uint32_t graphics = 0;
        bool physical = false;
        bool placeholder = false; // still a host placeholder
    };

    // The placeholder piece containing [begin, end), if there is one.
    std::map<uintptr_t, Range>::iterator placeholder_holding(uintptr_t begin, uintptr_t end);
    // Splits the placeholder at `it` so that [begin, end) is a placeholder of its own, and
    // returns that piece.
    std::map<uintptr_t, Range>::iterator split(std::map<uintptr_t, Range>::iterator it,
                                               uintptr_t begin, uintptr_t end);

    mutable std::mutex mutex_;
    // Every range the tracker made, by base. Placeholder pieces are listed as Windows sees
    // them: each one is a placeholder of its own.
    std::map<uintptr_t, Range> ranges_;
};

} // namespace g6::gpu
