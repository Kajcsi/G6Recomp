// xmem.dll (docs/side-dlls.md, "xmem"). Heap memory only, for now: the graphics and physical
// memory types wait for memory_tracker, and fail honestly until then rather than handing out
// CPU memory the GPU can't see. The two queries answer from the host's own view of memory.

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
}

} // namespace g6::imports::xmem
