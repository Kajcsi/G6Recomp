#pragma once

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Leonida contributors
// Copyright (C) 2026 the G6Recomp contributors
//
// memory_tracker: the title's memory as the console's memory functions describe it (CLAUDE.md,
// "GPU"). The console's CPU and GPU share one memory, so the game asks for GPU access when it
// allocates and then writes GPU resources through CPU pointers. Everything the guest allocates
// with GPU access, or as a range for physical pages, goes through here, so that runtime/gpu can
// find the host memory behind a guest address at M3.
//
// So far it keeps the CPU side: placeholder reservations, the physical pages mapped into them,
// allocations at the alignment of the console page size they ask for, and the GPU access each
// range asked for. The guest's own allocation functions (XMemVirtualAlloc, VirtualAlloc, and
// xmem's physical-page functions) decode the console's flags and call it with host flags.
//
// The physical pages follow Leonida's (https://github.com/ubiquitarianism/leonida, commit dde82f27,
// runtime/shims/xmem.cpp, GPL-3.0-or-later): one section per allocation call, one view per
// mapped page, page numbers that count 4 KiB frames, and read-write views in no-access ranges.
// They are rebuilt here on the tracker's own table of ranges (docs/boot.md, "Physical pages").

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
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

// A physical page is 64 KiB. Its number counts 4 KiB frames, as the title expects: a nonzero
// multiple of 16 below 2^25. In the map calls, 0 unmaps.
inline constexpr size_t kPhysicalPageSize = 64 * 1024;
inline constexpr uint64_t kFramesPerPhysicalPage = kPhysicalPageSize / 4096;
inline constexpr uint64_t kPhysicalPageNumberLimit = uint64_t{1} << 25;

// What the tracker knows about the range around an address.
struct RangeInfo {
    uintptr_t base = 0;       // start of the piece the address is in
    size_t size = 0;          // size of that piece
    uintptr_t origin = 0;     // start of the reservation the guest made, which the piece is part of
    uint32_t graphics = 0;    // PAGE_GRAPHICS_* bits it asked for
    DWORD protection = 0;     // the host protection a range for physical pages asked for
    bool physical = false;    // a range for physical pages
    bool placeholder = false; // still a host placeholder: nothing allocated or mapped in it
    uint64_t page = 0;        // the physical page mapped here, or 0
};

class MemoryTracker {
public:
    static MemoryTracker& instance();

    MemoryTracker() = default;
    MemoryTracker(const MemoryTracker&) = delete;
    MemoryTracker& operator=(const MemoryTracker&) = delete;

    // Reserves `size` bytes as a placeholder, at `address` or, when it is null, anywhere aligned
    // to `alignment` (a power of two of at least 64 KiB). `physical` marks a range for physical
    // pages, which take `protection`'s host part when they are mapped. Returns null with the
    // host's last error on failure.
    void* reserve_placeholder(void* address, size_t size, size_t alignment, bool physical,
                              Protection protection);

    // Splits [address, address + size) out of a placeholder this tracker made and keeps it a
    // placeholder, marked as a range for physical pages. Fails with ERROR_INVALID_ADDRESS when
    // the range isn't inside one placeholder.
    void* carve_physical(void* address, size_t size, Protection protection);

    // Reserves and/or commits ordinary memory (`type` is MEM_RESERVE and/or MEM_COMMIT, plus
    // MEM_TOP_DOWN) with a host protection. A null `address` is placed anywhere aligned to
    // `alignment`. An address inside a placeholder this tracker made is split out of it and
    // allocated in place; any other address goes to VirtualAlloc as it is.
    void* allocate(void* address, size_t size, DWORD type, DWORD protection, size_t alignment,
                   uint32_t graphics);

    // Frees what `allocate`, `reserve_placeholder`, or `carve_physical` made, like VirtualFree.
    // MEM_RELEASE of a range's base releases every piece the tracker split it into, unmapping
    // the pages mapped there. The placeholder forms keep the table in step with the host:
    // MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER splits [address, address + size) out of a
    // placeholder, or puts the allocation or mapped page at `address` back into one, and
    // MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS joins the placeholders in that range into one.
    // Anything else, or memory the tracker didn't make, goes to VirtualFree as it is.
    BOOL free(void* address, size_t size, DWORD type);

    // Allocates `*count` physical pages and writes their numbers to `pages`. Each call is backed
    // by one pagefile-backed section, committed at once and zero-filled. A page keeps its
    // contents while it is unmapped, and can be mapped at several addresses at once. On failure
    // `*count` is 0: ERROR_NOT_ENOUGH_MEMORY when the page numbers run out, or the host's error.
    BOOL allocate_pages(uint64_t* count, uint64_t* pages);

    // Frees pages; their numbers are handed out again. A page that is still mapped keeps its
    // memory until it is unmapped. Fails with ERROR_INVALID_PARAMETER at the first number that
    // isn't a live page, with the ones before it freed.
    BOOL free_pages(uint64_t count, const uint64_t* pages);

    // Maps pages[i] at address + i * 64 KiB, in place of any page mapped there; an entry of 0,
    // or a null `pages`, unmaps instead. Every address must lie in a range for physical pages
    // (ERROR_INVALID_ADDRESS otherwise). A page takes the host protection of its range, or
    // read-write in a range that has none, since the runtime reads GPU memory on the CPU. A
    // failure part-way leaves the pages before it mapped.
    BOOL map_pages(void* address, uint64_t count, const uint64_t* pages);

    // Maps pages[i] at addresses[i], by the same rules.
    BOOL map_pages_scatter(const void* const* addresses, uint64_t count, const uint64_t* pages);

    std::optional<RangeInfo> find(const void* address) const;

private:
    struct Section; // a pagefile-backed section, closed once no page of it is live or mapped

    struct Range {
        uintptr_t origin = 0; // base of the reservation the guest made, which this piece is from
        size_t size = 0;
        uint32_t graphics = 0;
        DWORD protection = 0; // host protection asked for, which mapped pages take
        bool physical = false;
        bool placeholder = false; // still a host placeholder
        // A mapped physical page: a 64 KiB view of its section.
        std::shared_ptr<const Section> section;
        uint64_t page = 0;
    };

    struct Page {
        std::shared_ptr<const Section> section;
        uint64_t offset = 0;
    };

    // The placeholder piece containing [begin, end), if there is one.
    std::map<uintptr_t, Range>::iterator placeholder_holding(uintptr_t begin, uintptr_t end);
    // Splits the placeholder at `it` so that [begin, end) is a placeholder of its own, and
    // returns that piece.
    std::map<uintptr_t, Range>::iterator split(std::map<uintptr_t, Range>::iterator it,
                                               uintptr_t begin, uintptr_t end);
    // The piece holding `address`, if there is one.
    std::map<uintptr_t, Range>::iterator piece_at(uintptr_t address);
    // With the mutex held: maps `page` (0 unmaps) at the 64 KiB-aligned `address`.
    bool map_page(uintptr_t address, uint64_t page);
    // With the mutex held: unmaps the page mapped in piece `it`, which becomes a placeholder.
    bool unmap_page(const std::map<uintptr_t, Range>::iterator& it);
    // With the mutex held: VirtualFree's MEM_PRESERVE_PLACEHOLDER and MEM_COALESCE_PLACEHOLDERS
    // forms (`free`).
    BOOL preserve_placeholder(uintptr_t begin, size_t size, DWORD type);
    BOOL coalesce_placeholders(uintptr_t begin, size_t size, DWORD type);

    mutable std::mutex mutex_;
    // Every range the tracker made, by base. Placeholder pieces are listed as Windows sees
    // them: each one is a placeholder of its own. So is each mapped page.
    std::map<uintptr_t, Range> ranges_;
    // Live physical pages by number, and the slots (number / 16) freed for reuse.
    std::unordered_map<uint64_t, Page> pages_;
    std::vector<uint64_t> free_slots_;
    uint64_t next_slot_ = 1;
};

} // namespace g6::gpu
