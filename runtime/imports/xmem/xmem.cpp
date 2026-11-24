// xmem.dll (docs/side-dlls.md, "xmem"). XMemAlloc gives heap memory only, for now: its graphics
// and physical memory types fail honestly rather than hand out CPU memory the GPU can't see.
// XMemVirtualAlloc and the physical-page functions go through memory_tracker. The two queries
// answer from the host's own view of memory.

#include "g6/imports/xmem.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>
#include <unordered_map>
#include <windows.h>
// After windows.h.
#include <psapi.h>

#include "g6/gpu/memory_tracker.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"

namespace g6::imports::xmem {
namespace {

// A heap of its own, so guest allocations never mix with the runtime's, and a table from each
// live allocation to the host block it sits in. XMemFree looks the address up rather than
// reading anything in front of it, so a pointer that isn't ours, or one freed already, is
// recognized without touching its memory.
struct State {
    HANDLE heap = nullptr;
    std::mutex mutex;
    std::unordered_map<uintptr_t, void*> blocks;

    State() {
        heap = HeapCreate(0, 0, 0);
        if (heap == nullptr) {
            LOG_ERROR(xmem, "cannot create the xmem heap (error {}); using the process heap",
                      GetLastError());
            heap = GetProcessHeap();
        }
    }
};

State& state() {
    static State instance;
    return instance;
}

// Memory types already reported as unsupported, one bit each.
std::atomic<uint32_t> g_reported_types{0};

bool first_report(uint32_t memory_type) {
    const uint32_t bit = 1U << (memory_type & 0x1F);
    return (g_reported_types.fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
}

std::atomic<bool> g_reported_foreign_free{false};
std::atomic<bool> g_reported_unknown_type{false};

// Attribute words XMemAllocatePhysicalPages has seen, logged once each: what they mean is
// unknown, and every page is 64 KiB whatever they say.
std::mutex g_physical_attributes_mutex;

std::unordered_map<uint64_t, bool>& physical_attributes() {
    static std::unordered_map<uint64_t, bool> seen;
    return seen;
}

void note_physical_attributes(uint64_t attributes, uint64_t count) {
    const std::scoped_lock lock(g_physical_attributes_mutex);
    if (!physical_attributes().emplace(attributes, true).second) {
        return;
    }
    LOG_INFO(xmem, "XMemAllocatePhysicalPages: first call with attributes {:#x} ({} pages)",
             attributes, count);
}

} // namespace

void* XMemAlloc(size_t size, uint64_t attributes) {
    const Attributes decoded = Attributes::decode(attributes);
    if (decoded.memory_type != kMemoryTypeHeap) {
        if (first_report(decoded.memory_type)) {
            LOG_WARN(xmem,
                     "XMemAlloc: memory type {} (attributes {:#x}) needs memory_tracker; "
                     "returning null",
                     decoded.memory_type, attributes);
        }
        return nullptr;
    }
    // The host heap returns 16-byte aligned blocks, so padding to any larger power-of-two
    // alignment fits in `alignment - 16` extra bytes.
    const size_t alignment = decoded.alignment();
    const size_t padding = alignment - 16;
    if (size > SIZE_MAX - padding) {
        return nullptr;
    }
    State& xmem = state();
    // A zero-byte request still gets a unique address.
    void* block = HeapAlloc(xmem.heap, HEAP_ZERO_MEMORY, size + padding == 0 ? 1 : size + padding);
    if (block == nullptr) {
        return nullptr;
    }
    const uintptr_t aligned =
        (reinterpret_cast<uintptr_t>(block) + alignment - 1) & ~(uintptr_t{alignment} - 1);
    try {
        const std::scoped_lock lock(xmem.mutex);
        xmem.blocks.emplace(aligned, block);
    } catch (const std::bad_alloc&) {
        HeapFree(xmem.heap, 0, block);
        return nullptr;
    }
    LOG_TRACE(xmem, "XMemAlloc({}, {:#x}) -> {:#x}", size, attributes, aligned);
    return reinterpret_cast<void*>(aligned);
}

void XMemFree(void* address, uint64_t attributes) {
    if (address == nullptr) {
        return;
    }
    State& xmem = state();
    void* block = nullptr;
    {
        const std::scoped_lock lock(xmem.mutex);
        const auto found = xmem.blocks.find(reinterpret_cast<uintptr_t>(address));
        if (found != xmem.blocks.end()) {
            block = found->second;
            xmem.blocks.erase(found);
        }
    }
    if (block == nullptr) {
        // Not from XMemAlloc, or freed already. Ignoring it is safer than corrupting the heap.
        if (!g_reported_foreign_free.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(xmem,
                      "XMemFree({}, {:#x}): not a live xmem allocation; ignored (reported once)",
                      address, attributes);
        }
        return;
    }
    HeapFree(xmem.heap, 0, block);
}

BOOL XMemVirtualQuery(HANDLE process, const void* address, XMemBasicInformation* info) {
    if (info == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MEMORY_BASIC_INFORMATION basic{};
    if (VirtualQueryEx(process, address, &basic, sizeof(basic)) == 0) {
        return FALSE;
    }
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const bool usable = basic.State == MEM_COMMIT && (basic.Protect & kReadable) != 0 &&
                        (basic.Protect & PAGE_GUARD) == 0;
    info->basic = basic;
    info->gpu_access = usable ? kGpuAccessVisible : 0;
    return TRUE;
}

BOOL XMemGetWorkingSetStatistics(uint64_t /*attributes*/, XMemWorkingSetStatistics* statistics) {
    if (statistics == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) == 0) {
        return FALSE;
    }
    statistics->limit = kSeriesXTitleMemory;
    statistics->used = std::min<uint64_t>(counters.PrivateUsage, kSeriesXTitleMemory);
    return TRUE;
}

BOOL XMemTransferMemory(uint32_t direction, size_t size) {
    LOG_DEBUG(xmem, "XMemTransferMemory({}, {:#x}): nothing to move on the host", direction, size);
    return TRUE;
}

void* XMemVirtualAlloc(void* address, size_t size, uint32_t type, uint64_t attributes,
                       uint32_t protection) {
    constexpr uint32_t kPageSizes = MEM_LARGE_PAGES | MEM_PHYSICAL | MEM_4MB_PAGES;
    constexpr uint32_t kKnown = MEM_COMMIT | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER |
                                MEM_RESERVE_PLACEHOLDER | MEM_TOP_DOWN | kPageSizes;
    if ((type & ~kKnown) != 0 && !g_reported_unknown_type.exchange(true)) {
        LOG_WARN(xmem,
                 "XMemVirtualAlloc: unknown allocation type bits {:#x} ignored (reported once)",
                 type & ~kKnown);
    }
    const bool commit = (type & MEM_COMMIT) != 0;
    const gpu::Protection split = gpu::split_protection(protection, commit);
    size_t alignment = 64 * 1024;
    if ((type & MEM_4MB_PAGES) != 0) {
        alignment = 4 * 1024 * 1024;
    } else if ((type & MEM_64K_PAGES) == MEM_LARGE_PAGES) {
        alignment = 2 * 1024 * 1024;
    }

    gpu::MemoryTracker& tracker = gpu::MemoryTracker::instance();
    void* result = nullptr;
    if ((type & MEM_RESERVE_PLACEHOLDER) != 0) {
        result = tracker.reserve_placeholder(address, size, alignment, false, split);
    } else if ((type & MEM_PHYSICAL) != 0 && !commit) {
        result = (type & MEM_REPLACE_PLACEHOLDER) != 0
                     ? tracker.carve_physical(address, size, split)
                     : tracker.reserve_placeholder(address, size, alignment, true, split);
    } else {
        result = tracker.allocate(address, size, type & (MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN),
                                  split.host, alignment, split.graphics);
    }
    if (result == nullptr) {
        LOG_WARN(xmem, "XMemVirtualAlloc({}, {:#x}, {:#x}, {:#x}, {:#x}) failed (error {})",
                 address, size, type, attributes, protection, GetLastError());
    } else {
        LOG_DEBUG(xmem, "XMemVirtualAlloc({}, {:#x}, {:#x}, {:#x}, {:#x}) -> {}", address, size,
                  type, attributes, protection, result);
    }
    return result;
}

void* XMemAllocDefault(size_t size, uint64_t attributes) {
    return XMemAlloc(size, attributes);
}

void XMemFreeDefault(void* address, uint64_t attributes) {
    XMemFree(address, attributes);
}

BOOL XMemIsToolsMemAvailable() {
    return FALSE;
}

BOOL XMemAllocatePhysicalPages(uint64_t attributes, uint64_t* count, uint64_t* pages) {
    const uint64_t wanted = count != nullptr ? *count : 0;
    note_physical_attributes(attributes, wanted);
    if (gpu::MemoryTracker::instance().allocate_pages(count, pages) == FALSE) {
        LOG_WARN(xmem, "XMemAllocatePhysicalPages({:#x}, {}) failed (error {})", attributes, wanted,
                 GetLastError());
        return FALSE;
    }
    LOG_DEBUG(xmem, "XMemAllocatePhysicalPages({:#x}, {}) -> first page {:#x}", attributes, wanted,
              wanted != 0 ? pages[0] : 0);
    return TRUE;
}

BOOL XMemFreePhysicalPages(uint64_t count, const uint64_t* pages) {
    if (gpu::MemoryTracker::instance().free_pages(count, pages) == FALSE) {
        LOG_WARN(xmem, "XMemFreePhysicalPages({}) failed (error {})", count, GetLastError());
        return FALSE;
    }
    LOG_DEBUG(xmem, "XMemFreePhysicalPages({})", count);
    return TRUE;
}

void* XMemMapPhysicalPages(void* address, uint64_t count, const uint64_t* pages) {
    if (gpu::MemoryTracker::instance().map_pages(address, count, pages) == FALSE) {
        LOG_WARN(xmem, "XMemMapPhysicalPages({}, {}, {}) failed (error {})", address, count,
                 static_cast<const void*>(pages), GetLastError());
        return nullptr;
    }
    LOG_DEBUG(xmem, "XMemMapPhysicalPages({}, {}, {})", address, count,
              pages != nullptr ? "pages" : "unmap");
    return address;
}

BOOL XMemMapPhysicalPagesScatter(void* const* addresses, uint32_t count, const uint64_t* pages) {
    if (gpu::MemoryTracker::instance().map_pages_scatter(addresses, count, pages) == FALSE) {
        LOG_WARN(xmem, "XMemMapPhysicalPagesScatter({}) failed (error {})", count, GetLastError());
        return FALSE;
    }
    LOG_DEBUG(xmem, "XMemMapPhysicalPagesScatter({})", count);
    return TRUE;
}

void register_xmem(loader::ImportRegistry& imports) {
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemAlloc"),
                reinterpret_cast<const void*>(&XMemAlloc));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemFree"),
                reinterpret_cast<const void*>(&XMemFree));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemVirtualQuery"),
                reinterpret_cast<const void*>(&XMemVirtualQuery));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemGetWorkingSetStatistics"),
                reinterpret_cast<const void*>(&XMemGetWorkingSetStatistics));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemTransferMemory"),
                reinterpret_cast<const void*>(&XMemTransferMemory));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemVirtualAlloc"),
                reinterpret_cast<const void*>(&XMemVirtualAlloc));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemAllocDefault"),
                reinterpret_cast<const void*>(&XMemAllocDefault));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemFreeDefault"),
                reinterpret_cast<const void*>(&XMemFreeDefault));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemIsToolsMemAvailable"),
                reinterpret_cast<const void*>(&XMemIsToolsMemAvailable));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemAllocatePhysicalPages"),
                reinterpret_cast<const void*>(&XMemAllocatePhysicalPages));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemFreePhysicalPages"),
                reinterpret_cast<const void*>(&XMemFreePhysicalPages));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemMapPhysicalPages"),
                reinterpret_cast<const void*>(&XMemMapPhysicalPages));
    imports.hle("xmem.dll", loader::Symbol::by_name("XMemMapPhysicalPagesScatter"),
                reinterpret_cast<const void*>(&XMemMapPhysicalPagesScatter));
}

} // namespace g6::imports::xmem
