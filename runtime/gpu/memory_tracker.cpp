// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Leonida contributors
// Copyright (C) 2026 the G6Recomp contributors
//
// memory_tracker (memory_tracker.h). Placeholders are the host's way to reserve address space
// that later takes pieces of other kinds: a placeholder can be split, replaced in place by an
// allocation or a mapped view, and put back. The console's physical-page ranges need exactly
// that, so every range carved out of a placeholder stays a host placeholder until something is
// allocated or mapped into it.
//
// Physical pages follow Leonida's runtime/shims/xmem.cpp (commit dde82f27): a pagefile-backed
// section per allocation call, page numbers handed out as slot * 16 with freed slots reused, one
// MapViewOfFile3 view per mapped page, and read-write views where the range has no access. Here
// the ranges and the mapped pages live in the tracker's one table instead of a table of windows.

#include "g6/gpu/memory_tracker.h"

#include <limits>

#include "g6/log.h"

namespace g6::gpu {
namespace {

constexpr size_t kGranularity = 64 * 1024;
constexpr uint64_t kSlotLimit = kPhysicalPageNumberLimit / kFramesPerPhysicalPage;

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

void* at(uintptr_t address) {
    return reinterpret_cast<void*>(address);
}

} // namespace

struct MemoryTracker::Section {
    explicit Section(HANDLE section) : handle(section) {}
    ~Section() { CloseHandle(handle); }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    Section(Section&&) = delete;
    Section& operator=(Section&&) = delete;

    HANDLE handle;
};

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
                                         bool physical, Protection protection) {
    size = round_up(size, kGranularity);
    void* base = virtual_alloc(address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                               alignment);
    if (base == nullptr) {
        return nullptr;
    }
    const auto begin = reinterpret_cast<uintptr_t>(base);
    const std::scoped_lock lock(mutex_);
    ranges_[begin] = Range{.origin = begin,
                           .size = size,
                           .graphics = protection.graphics,
                           .protection = protection.host,
                           .physical = physical,
                           .placeholder = true};
    return base;
}

std::map<uintptr_t, MemoryTracker::Range>::iterator MemoryTracker::piece_at(uintptr_t address) {
    auto it = ranges_.upper_bound(address);
    if (it == ranges_.begin()) {
        return ranges_.end();
    }
    --it;
    return address < it->first + it->second.size ? it : ranges_.end();
}

std::map<uintptr_t, MemoryTracker::Range>::iterator
MemoryTracker::placeholder_holding(uintptr_t begin, uintptr_t end) {
    auto it = piece_at(begin);
    if (it == ranges_.end()) {
        return it;
    }
    const bool inside = end <= it->first + it->second.size;
    return inside && it->second.placeholder ? it : ranges_.end();
}

std::map<uintptr_t, MemoryTracker::Range>::iterator
MemoryTracker::split(std::map<uintptr_t, Range>::iterator it, uintptr_t begin, uintptr_t end) {
    const uintptr_t piece_begin = it->first;
    const uintptr_t piece_end = piece_begin + it->second.size;
    if (begin == piece_begin && end == piece_end) {
        return it;
    }
    if (VirtualFree(at(begin), end - begin, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
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

void* MemoryTracker::carve_physical(void* address, size_t size, Protection protection) {
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
    it->second.graphics = protection.graphics;
    it->second.protection = protection.host;
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
                it->second.protection = 0;
            }
            return result;
        }
    }
    void* result = virtual_alloc(address, size, type, protection, alignment);
    if (result != nullptr && (type & MEM_RESERVE) != 0) {
        const auto begin = reinterpret_cast<uintptr_t>(result);
        ranges_[begin] = Range{.origin = begin, .size = round_up(size, 4096), .graphics = graphics};
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
        // A piece of a larger reservation goes back to being a placeholder in it. A mapped
        // page is unmapped, and its piece stays part of the range for physical pages.
        Range& piece = found->second;
        if (piece.section != nullptr) {
            return unmap_page(found) ? TRUE : FALSE;
        }
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
    // The whole reservation: every piece it was split into, and every page mapped in it.
    BOOL ok = TRUE;
    for (auto it = ranges_.lower_bound(origin);
         it != ranges_.end() && it->second.origin == origin;) {
        const BOOL released = it->second.section != nullptr
                                  ? UnmapViewOfFile2(GetCurrentProcess(), at(it->first), 0)
                                  : VirtualFree(at(it->first), 0, MEM_RELEASE);
        if (released == 0) {
            LOG_WARN(memory, "releasing {:#x} of the range at {:#x} failed (error {})", it->first,
                     origin, GetLastError());
            ok = FALSE;
        }
        it = ranges_.erase(it);
    }
    return ok;
}

BOOL MemoryTracker::allocate_pages(uint64_t* count, uint64_t* pages) {
    if (count == nullptr || (*count != 0 && pages == nullptr)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const uint64_t wanted = *count;
    if (wanted == 0) {
        return TRUE;
    }
    const std::scoped_lock lock(mutex_);
    if (wanted > free_slots_.size() + (kSlotLimit - next_slot_)) {
        *count = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    // Below 2^37 bytes, since fewer than 2^21 pages fit.
    const uint64_t bytes = wanted * kPhysicalPageSize;
    HANDLE handle =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                           static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (handle == nullptr) {
        const DWORD error = GetLastError();
        *count = 0;
        SetLastError(error);
        return FALSE;
    }
    const auto section = std::make_shared<const Section>(handle);
    for (uint64_t i = 0; i < wanted; ++i) {
        uint64_t slot = 0;
        if (!free_slots_.empty()) {
            slot = free_slots_.back();
            free_slots_.pop_back();
        } else {
            slot = next_slot_++;
        }
        const uint64_t page = slot * kFramesPerPhysicalPage;
        pages_[page] = Page{section, i * kPhysicalPageSize};
        pages[i] = page;
    }
    return TRUE;
}

BOOL MemoryTracker::free_pages(uint64_t count, const uint64_t* pages) {
    if (count != 0 && pages == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::scoped_lock lock(mutex_);
    for (uint64_t i = 0; i < count; ++i) {
        const auto found = pages_.find(pages[i]);
        if (found == pages_.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        pages_.erase(found);
        free_slots_.push_back(pages[i] / kFramesPerPhysicalPage);
    }
    return TRUE;
}

bool MemoryTracker::unmap_page(const std::map<uintptr_t, Range>::iterator& it) {
    if (UnmapViewOfFile2(GetCurrentProcess(), at(it->first), MEM_PRESERVE_PLACEHOLDER) == 0) {
        return false;
    }
    it->second.section.reset();
    it->second.page = 0;
    it->second.placeholder = true;
    return true;
}

bool MemoryTracker::map_page(uintptr_t address, uint64_t page) {
    auto it = piece_at(address);
    if (it == ranges_.end() || !it->second.physical) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    if (it->second.section != nullptr && !unmap_page(it)) {
        return false;
    }
    if (page == 0) {
        return true;
    }
    const auto found = pages_.find(page);
    if (found == pages_.end()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (!it->second.placeholder) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    it = split(it, address, address + kPhysicalPageSize);
    if (it == ranges_.end()) {
        return false;
    }
    const DWORD asked = it->second.protection;
    const DWORD protection = asked == 0 || asked == PAGE_NOACCESS ? PAGE_READWRITE : asked;
    const void* view = MapViewOfFile3(found->second.section->handle, GetCurrentProcess(),
                                      at(address), found->second.offset, kPhysicalPageSize,
                                      MEM_REPLACE_PLACEHOLDER, protection, nullptr, 0);
    if (view == nullptr) {
        return false;
    }
    it->second.placeholder = false;
    it->second.section = found->second.section;
    it->second.page = page;
    return true;
}

BOOL MemoryTracker::map_pages(void* address, uint64_t count, const uint64_t* pages) {
    const auto start = reinterpret_cast<uintptr_t>(address);
    if (start % kPhysicalPageSize != 0 ||
        count > (std::numeric_limits<uintptr_t>::max() - start) / kPhysicalPageSize) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::scoped_lock lock(mutex_);
    for (uint64_t i = 0; i < count; ++i) {
        if (!map_page(start + i * kPhysicalPageSize, pages != nullptr ? pages[i] : 0)) {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL MemoryTracker::map_pages_scatter(const void* const* addresses, uint64_t count,
                                      const uint64_t* pages) {
    if (count != 0 && addresses == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::scoped_lock lock(mutex_);
    for (uint64_t i = 0; i < count; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(addresses[i]);
        if (address % kPhysicalPageSize != 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (!map_page(address, pages != nullptr ? pages[i] : 0)) {
            return FALSE;
        }
    }
    return TRUE;
}

std::optional<RangeInfo> MemoryTracker::find(const void* address) const {
    const auto at_address = reinterpret_cast<uintptr_t>(address);
    const std::scoped_lock lock(mutex_);
    auto it = ranges_.upper_bound(at_address);
    if (it == ranges_.begin()) {
        return std::nullopt;
    }
    --it;
    if (at_address >= it->first + it->second.size) {
        return std::nullopt;
    }
    const Range& range = it->second;
    return RangeInfo{.base = it->first,
                     .size = range.size,
                     .origin = range.origin,
                     .graphics = range.graphics,
                     .protection = range.protection,
                     .physical = range.physical,
                     .placeholder = range.placeholder,
                     .page = range.page};
}

} // namespace g6::gpu
