// memory_tracker (memory_tracker.h). Placeholders are the host's way to reserve address space
// that later takes pieces of other kinds: a placeholder can be split, replaced in place by an
// allocation or a mapped view, and put back. The console's physical-page ranges need exactly
// that, so every range carved out of a placeholder stays a host placeholder until something is
// allocated or mapped into it.

#include "g6/gpu/memory_tracker.h"

#include "g6/log.h"

namespace g6::gpu {
namespace {

constexpr size_t kGranularity = 64 * 1024;

size_t round_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// VirtualAlloc2 with an alignment requirement when the address is free to choose and the
// alignment is more than the host's own 64 KiB.
void* virtual_alloc(void* address, size_t size, DWORD type, DWORD protection, size_t alignment) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    MEM_EXTENDED_PARAMETER parameter{};
    ULONG count = 0;
    if (address == nullptr && alignment > kGranularity) {
        requirements.Alignment = alignment;
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        count = 1;
    }
    return VirtualAlloc2(GetCurrentProcess(), address, size, type, protection,
                         count != 0 ? &parameter : nullptr, count);
}

} // namespace

Protection split_protection(uint32_t console_protection, bool committed) {
    Protection result;
    result.graphics = console_protection & kGraphicsProtectionMask;
    DWORD base = console_protection & 0xFF;
    if (base == 0) {
        const bool gpu_only = result.graphics != 0 && result.graphics != PAGE_GRAPHICS_NOACCESS;
        base = committed && gpu_only ? PAGE_READWRITE : PAGE_NOACCESS;
    }
    result.host = base | (console_protection & PAGE_GUARD);
    return result;
}

MemoryTracker& MemoryTracker::instance() {
    static MemoryTracker tracker;
    return tracker;
}

void* MemoryTracker::reserve_placeholder(void* address, size_t size, size_t alignment,
                                         bool physical, uint32_t graphics) {
    size = round_up(size, kGranularity);
    void* base = virtual_alloc(address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                               alignment);
    if (base == nullptr) {
        return nullptr;
    }
    const auto begin = reinterpret_cast<uintptr_t>(base);
    const std::scoped_lock lock(mutex_);
    ranges_[begin] = Range{begin, size, graphics, physical, true};
    return base;
}

std::map<uintptr_t, MemoryTracker::Range>::iterator
MemoryTracker::placeholder_holding(uintptr_t begin, uintptr_t end) {
    auto it = ranges_.upper_bound(begin);
    if (it == ranges_.begin()) {
        return ranges_.end();
    }
    --it;
    const bool inside = begin >= it->first && end <= it->first + it->second.size;
    return inside && it->second.placeholder ? it : ranges_.end();
}

std::map<uintptr_t, MemoryTracker::Range>::iterator
MemoryTracker::split(std::map<uintptr_t, Range>::iterator it, uintptr_t begin, uintptr_t end) {
    const uintptr_t piece_begin = it->first;
    const uintptr_t piece_end = piece_begin + it->second.size;
    if (begin == piece_begin && end == piece_end) {
        return it;
    }
    if (VirtualFree(reinterpret_cast<void*>(begin), end - begin,
                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
        return ranges_.end();
    }
    const Range whole = it->second;
    ranges_.erase(it);
    if (piece_begin < begin) {
        Range before = whole;
        before.size = begin - piece_begin;
        ranges_[piece_begin] = before;
    }
    if (end < piece_end) {
        Range after = whole;
        after.size = piece_end - end;
        ranges_[end] = after;
    }
    Range middle = whole;
    middle.size = end - begin;
    return ranges_.insert_or_assign(begin, middle).first;
}

void* MemoryTracker::carve_physical(void* address, size_t size, uint32_t graphics) {
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = begin + round_up(size, kGranularity);
    const std::scoped_lock lock(mutex_);
    auto it = placeholder_holding(begin, end);
    if (it == ranges_.end() || begin % kGranularity != 0) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    it = split(it, begin, end);
    if (it == ranges_.end()) {
        return nullptr;
    }
    it->second.physical = true;
    it->second.graphics = graphics;
    return address;
}

void* MemoryTracker::allocate(void* address, size_t size, DWORD type, DWORD protection,
                              size_t alignment, uint32_t graphics) {
    const std::scoped_lock lock(mutex_);
    if (address != nullptr) {
        const auto begin = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = begin + round_up(size, kGranularity);
        auto it = placeholder_holding(begin, end);
        if (it != ranges_.end()) {
            if (begin % kGranularity != 0) {
                SetLastError(ERROR_INVALID_ADDRESS);
                return nullptr;
            }
            it = split(it, begin, end);
            if (it == ranges_.end()) {
                return nullptr;
            }
            void* result = virtual_alloc(
                address, end - begin, (type & MEM_COMMIT) | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                protection, kGranularity);
            if (result != nullptr) {
                it->second.placeholder = false;
                it->second.physical = false;
                it->second.graphics = graphics;
            }
            return result;
        }
    }
    void* result = virtual_alloc(address, size, type, protection, alignment);
    if (result != nullptr && (type & MEM_RESERVE) != 0) {
        const auto begin = reinterpret_cast<uintptr_t>(result);
        ranges_[begin] = Range{begin, round_up(size, 4096), graphics, false, false};
    }
    return result;
}

BOOL MemoryTracker::free(void* address, size_t size, DWORD type) {
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const std::scoped_lock lock(mutex_);
    const auto found = ranges_.find(begin);
    if ((type & MEM_RELEASE) == 0 || found == ranges_.end()) {
        return VirtualFree(address, size, type);
    }
    if (size != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const uintptr_t origin = found->second.origin;
    if (begin != origin) {
        // A piece of a larger reservation goes back to being a placeholder in it.
        Range& piece = found->second;
        // Windows wants the allocation's size here, not the 0 of an ordinary release.
        if (!piece.placeholder &&
            VirtualFree(address, piece.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
            return FALSE;
        }
        piece.placeholder = true;
        piece.physical = false;
        piece.graphics = 0;
        return TRUE;
    }
    // The whole reservation: every piece it was split into.
    BOOL ok = TRUE;
    for (auto it = ranges_.lower_bound(origin);
         it != ranges_.end() && it->second.origin == origin;) {
        if (VirtualFree(reinterpret_cast<void*>(it->first), 0, MEM_RELEASE) == 0) {
            LOG_WARN(memory, "releasing {:#x} of the range at {:#x} failed (error {})", it->first,
                     origin, GetLastError());
            ok = FALSE;
        }
        it = ranges_.erase(it);
    }
    return ok;
}

std::optional<RangeInfo> MemoryTracker::find(const void* address) const {
    const auto at = reinterpret_cast<uintptr_t>(address);
    const std::scoped_lock lock(mutex_);
    auto it = ranges_.upper_bound(at);
    if (it == ranges_.begin()) {
        return std::nullopt;
    }
    --it;
    if (at >= it->first + it->second.size) {
        return std::nullopt;
    }
    const Range& range = it->second;
    return RangeInfo{it->first,      range.size,     range.origin,
                     range.graphics, range.physical, range.placeholder};
}

} // namespace g6::gpu
