// memory_tracker (g6/gpu/memory_tracker.h): console protections split into host and GPU parts,
// placeholder reservations, physical ranges carved out of them, allocations made in place inside
// them and put back, whole releases, allocations aligned to the console's large page sizes, and
// physical pages: their numbers, maps, aliases, remaps, unmaps, and frees while mapped.

#include <cstdint>
#include <cstring>
#include <windows.h>

#include "g6/gpu/memory_tracker.h"
#include "g6test.h"

using g6::gpu::kFramesPerPhysicalPage;
using g6::gpu::kPhysicalPageNumberLimit;
using g6::gpu::MemoryTracker;
using g6::gpu::split_protection;

namespace {

constexpr size_t k64K = 64 * 1024;
constexpr size_t k2M = 2 * 1024 * 1024;
constexpr size_t k4M = 4 * 1024 * 1024;

MEMORY_BASIC_INFORMATION query(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info));
    return info;
}

void* at(uintptr_t address) {
    return reinterpret_cast<void*>(address);
}

} // namespace

TEST_CASE("a console protection splits into the host page and the GPU access") {
    // What the executable's second start-up allocation asks for: no CPU access, GPU no access.
    auto split = split_protection(PAGE_NOACCESS | PAGE_GRAPHICS_NOACCESS, true);
    CHECK_EQ(split.host, DWORD{PAGE_NOACCESS});
    CHECK_EQ(split.graphics, uint32_t{PAGE_GRAPHICS_NOACCESS});

    split =
        split_protection(PAGE_READWRITE | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_COHERENT, true);
    CHECK_EQ(split.host, DWORD{PAGE_READWRITE});
    CHECK_EQ(split.graphics, uint32_t{PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_COHERENT});

    // GPU access alone: the runtime copies the memory to the GPU, so the host can read and write
    // it once committed. A reserve has no pages to protect.
    split = split_protection(PAGE_GRAPHICS_READONLY, true);
    CHECK_EQ(split.host, DWORD{PAGE_READWRITE});
    split = split_protection(PAGE_GRAPHICS_READONLY, false);
    CHECK_EQ(split.host, DWORD{PAGE_NOACCESS});

    split = split_protection(PAGE_READONLY | PAGE_GUARD, true);
    CHECK_EQ(split.host, DWORD{PAGE_READONLY | PAGE_GUARD});
    CHECK_EQ(split.graphics, 0U);
}

TEST_CASE("a placeholder is reserved at the alignment asked for") {
    MemoryTracker tracker;
    void* base = tracker.reserve_placeholder(nullptr, 3 * k64K + 1, k2M, true, {});
    REQUIRE(base != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(base);
    CHECK_EQ(begin % k2M, uintptr_t{0});

    const auto info = tracker.find(at(begin + k64K));
    REQUIRE(info.has_value());
    CHECK_EQ(info->base, begin);
    CHECK_EQ(info->size, 4 * k64K); // rounded up to whole 64 KiB pages
    CHECK_EQ(info->origin, begin);
    CHECK(info->physical);
    CHECK(info->placeholder);
    CHECK_EQ(query(begin).State, DWORD{MEM_RESERVE});
    CHECK(!tracker.find(at(begin + 4 * k64K)).has_value());

    CHECK(tracker.free(base, 0, MEM_RELEASE) != FALSE);
    CHECK_EQ(query(begin).State, DWORD{MEM_FREE});
    CHECK(!tracker.find(base).has_value());
}

TEST_CASE("memory is allocated in place inside a placeholder and put back") {
    MemoryTracker tracker;
    void* base = tracker.reserve_placeholder(nullptr, 4 * k64K, k64K, false, {});
    REQUIRE(base != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(base);

    void* middle = tracker.allocate(at(begin + k64K), k64K, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE, k64K, PAGE_GRAPHICS_READWRITE);
    REQUIRE(middle == at(begin + k64K));
    REQUIRE(query(begin + k64K).State == MEM_COMMIT);
    CHECK_EQ(query(begin + k64K).Protect, DWORD{PAGE_READWRITE});
    std::memset(middle, 0x5A, k64K);
    CHECK_EQ(static_cast<const uint8_t*>(middle)[k64K - 1], uint8_t{0x5A});

    // The placeholder is now three pieces: before, the allocation, after.
    auto info = tracker.find(middle);
    REQUIRE(info.has_value());
    CHECK_EQ(info->base, begin + k64K);
    CHECK_EQ(info->size, k64K);
    CHECK_EQ(info->origin, begin);
    CHECK_EQ(info->graphics, uint32_t{PAGE_GRAPHICS_READWRITE});
    CHECK(!info->placeholder);
    info = tracker.find(base);
    REQUIRE(info.has_value());
    CHECK_EQ(info->size, k64K);
    CHECK(info->placeholder);
    info = tracker.find(at(begin + 3 * k64K));
    REQUIRE(info.has_value());
    CHECK_EQ(info->base, begin + 2 * k64K);
    CHECK_EQ(info->size, 2 * k64K);

    // Releasing the piece makes it a placeholder again, which takes a new allocation.
    CHECK(tracker.free(middle, 0, MEM_RELEASE) != FALSE);
    CHECK_EQ(query(begin + k64K).State, DWORD{MEM_RESERVE});
    info = tracker.find(middle);
    REQUIRE(info.has_value());
    CHECK(info->placeholder);
    CHECK_EQ(info->graphics, 0U);
    const void* again =
        tracker.allocate(middle, k64K, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY, k64K, 0);
    REQUIRE(again == middle);
    CHECK_EQ(static_cast<const uint8_t*>(again)[0], uint8_t{0}); // fresh pages

    // Releasing the base releases every piece.
    CHECK(tracker.free(base, 0, MEM_RELEASE) != FALSE);
    for (uintptr_t page = begin; page < begin + 4 * k64K; page += k64K) {
        CHECK_EQ(query(page).State, DWORD{MEM_FREE});
    }
    CHECK(!tracker.find(middle).has_value());
}

TEST_CASE("physical ranges are carved out of a placeholder and stay placeholders") {
    MemoryTracker tracker;
    void* base = tracker.reserve_placeholder(nullptr, 8 * k64K, k64K, false, {});
    REQUIRE(base != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(base);

    const void* carved = tracker.carve_physical(at(begin + 2 * k64K), 3 * k64K,
                                                {PAGE_READWRITE, PAGE_GRAPHICS_READWRITE});
    REQUIRE(carved == at(begin + 2 * k64K));
    const auto info = tracker.find(at(begin + 4 * k64K));
    REQUIRE(info.has_value());
    CHECK_EQ(info->base, begin + 2 * k64K);
    CHECK_EQ(info->size, 3 * k64K);
    CHECK(info->physical);
    CHECK(info->placeholder);
    CHECK_EQ(info->protection, DWORD{PAGE_READWRITE});
    CHECK_EQ(query(begin + 2 * k64K).State, DWORD{MEM_RESERVE});
    const auto before = tracker.find(base);
    REQUIRE(before.has_value());
    CHECK(!before->physical);

    // Not on a 64 KiB boundary, past the placeholder's end, or outside any placeholder.
    SetLastError(0);
    CHECK(tracker.carve_physical(at(begin + 4096), k64K, {}) == nullptr);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_ADDRESS});
    CHECK(tracker.carve_physical(at(begin + 7 * k64K), 2 * k64K, {}) == nullptr);
    int outside = 0;
    CHECK(tracker.carve_physical(&outside, k64K, {}) == nullptr);

    CHECK(tracker.free(base, 0, MEM_RELEASE) != FALSE);
    CHECK_EQ(query(begin + 2 * k64K).State, DWORD{MEM_FREE});
}

TEST_CASE("allocations anywhere take the console page size's alignment") {
    MemoryTracker tracker;
    for (const size_t alignment : {k64K, k2M, k4M}) {
        INFO(fmt::format("alignment {:#x}", alignment));
        void* block = tracker.allocate(nullptr, alignment, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
                                       alignment, 0);
        REQUIRE(block != nullptr);
        CHECK_EQ(reinterpret_cast<uintptr_t>(block) % alignment, uintptr_t{0});
        static_cast<uint8_t*>(block)[alignment - 1] = 1;
        const auto info = tracker.find(block);
        REQUIRE(info.has_value());
        CHECK(!info->placeholder);
        CHECK_EQ(info->origin, reinterpret_cast<uintptr_t>(block));
        CHECK(tracker.free(block, 0, MEM_RELEASE) != FALSE);
        CHECK(!tracker.find(block).has_value());
    }
}

TEST_CASE("frees follow VirtualFree's rules") {
    MemoryTracker tracker;
    void* block =
        tracker.allocate(nullptr, 2 * k64K, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, k64K, 0);
    REQUIRE(block != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(block);

    // MEM_RELEASE takes size 0; a decommit goes straight to the host and keeps the reservation.
    SetLastError(0);
    CHECK(tracker.free(block, k64K, MEM_RELEASE) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_PARAMETER});
    CHECK(tracker.free(at(begin + k64K), k64K, MEM_DECOMMIT) != FALSE);
    CHECK_EQ(query(begin + k64K).State, DWORD{MEM_RESERVE});
    CHECK(tracker.find(block).has_value());
    CHECK(tracker.free(block, 0, MEM_RELEASE) != FALSE);

    // Memory the tracker didn't make is freed as VirtualFree would.
    void* foreign = VirtualAlloc(nullptr, k64K, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(foreign != nullptr);
    CHECK(tracker.free(foreign, 0, MEM_RELEASE) != FALSE);
    CHECK_EQ(query(reinterpret_cast<uintptr_t>(foreign)).State, DWORD{MEM_FREE});
}

TEST_CASE("physical pages are numbered in 4 KiB frames, and freed numbers come back") {
    MemoryTracker tracker;
    uint64_t pages[3] = {};
    uint64_t count = 3;
    REQUIRE(tracker.allocate_pages(&count, pages) != FALSE);
    CHECK_EQ(count, uint64_t{3});
    for (const uint64_t page : pages) {
        CHECK(page != 0);
        CHECK_EQ(page % kFramesPerPhysicalPage, uint64_t{0});
        CHECK(page < kPhysicalPageNumberLimit);
    }
    CHECK(pages[0] != pages[1]);
    CHECK(pages[1] != pages[2]);
    CHECK(pages[0] != pages[2]);

    // Nothing to allocate, and no count to read.
    count = 0;
    CHECK(tracker.allocate_pages(&count, nullptr) != FALSE);
    SetLastError(0);
    CHECK(tracker.allocate_pages(nullptr, pages) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_PARAMETER});

    // A freed number is handed out again; one that isn't live can't be freed.
    REQUIRE(tracker.free_pages(1, &pages[1]) != FALSE);
    uint64_t again = 0;
    count = 1;
    REQUIRE(tracker.allocate_pages(&count, &again) != FALSE);
    CHECK_EQ(again, pages[1]);
    const uint64_t unknown = pages[2] + 64 * kFramesPerPhysicalPage;
    SetLastError(0);
    CHECK(tracker.free_pages(1, &unknown) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_PARAMETER});
    CHECK(tracker.free_pages(3, pages) != FALSE);
}

TEST_CASE("physical pages are mapped, aliased, remapped, and unmapped in their range") {
    MemoryTracker tracker;
    void* window = tracker.reserve_placeholder(nullptr, 4 * k64K, k64K, true,
                                               {PAGE_READWRITE, PAGE_GRAPHICS_READWRITE});
    REQUIRE(window != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(window);
    auto* bytes = static_cast<volatile uint8_t*>(window);
    uint64_t pages[2] = {};
    uint64_t count = 2;
    REQUIRE(tracker.allocate_pages(&count, pages) != FALSE);

    // Two pages, one after the other, zero-filled and with the range's protection.
    REQUIRE(tracker.map_pages(window, 2, pages) != FALSE);
    CHECK_EQ(query(begin).State, DWORD{MEM_COMMIT});
    CHECK_EQ(query(begin).Type, DWORD{MEM_MAPPED});
    CHECK_EQ(query(begin).Protect, DWORD{PAGE_READWRITE});
    CHECK_EQ(bytes[k64K - 1], uint8_t{0});
    bytes[0] = 0x11;
    bytes[k64K] = 0x22;
    auto info = tracker.find(window);
    REQUIRE(info.has_value());
    CHECK_EQ(info->page, pages[0]);
    CHECK_EQ(info->size, k64K);
    CHECK(info->physical);
    CHECK(!info->placeholder);
    CHECK_EQ(info->graphics, uint32_t{PAGE_GRAPHICS_READWRITE});

    // The first page again, at a second address: both addresses show the same memory.
    const void* third = at(begin + 2 * k64K);
    REQUIRE(tracker.map_pages_scatter(&third, 1, &pages[0]) != FALSE);
    CHECK_EQ(bytes[2 * k64K], uint8_t{0x11});
    bytes[2 * k64K] = 0x33;
    CHECK_EQ(bytes[0], uint8_t{0x33});

    // Mapping over a mapped page replaces it.
    REQUIRE(tracker.map_pages(window, 1, &pages[1]) != FALSE);
    CHECK_EQ(bytes[0], uint8_t{0x22});
    info = tracker.find(window);
    REQUIRE(info.has_value());
    CHECK_EQ(info->page, pages[1]);

    // A 0 entry unmaps: the address is a placeholder again, and the page keeps its contents.
    const void* second = at(begin + k64K);
    const uint64_t none = 0;
    REQUIRE(tracker.map_pages_scatter(&second, 1, &none) != FALSE);
    CHECK_EQ(query(begin + k64K).State, DWORD{MEM_RESERVE});
    info = tracker.find(second);
    REQUIRE(info.has_value());
    CHECK(info->placeholder);
    CHECK_EQ(info->page, uint64_t{0});
    REQUIRE(tracker.map_pages(at(begin + 3 * k64K), 1, &pages[0]) != FALSE);
    CHECK_EQ(bytes[3 * k64K], uint8_t{0x33});

    // A null page array unmaps the whole run, mapped or not.
    REQUIRE(tracker.map_pages(window, 4, nullptr) != FALSE);
    for (uintptr_t page = begin; page < begin + 4 * k64K; page += k64K) {
        CHECK_EQ(query(page).State, DWORD{MEM_RESERVE});
    }
    CHECK(tracker.free_pages(2, pages) != FALSE);
    CHECK(tracker.free(window, 0, MEM_RELEASE) != FALSE);
    CHECK_EQ(query(begin).State, DWORD{MEM_FREE});
}

TEST_CASE("a page freed while mapped keeps its memory until the range goes") {
    MemoryTracker tracker;
    void* window = tracker.reserve_placeholder(nullptr, 2 * k64K, k64K, true, {PAGE_READWRITE});
    REQUIRE(window != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(window);
    auto* bytes = static_cast<volatile uint8_t*>(window);
    uint64_t page = 0;
    uint64_t count = 1;
    REQUIRE(tracker.allocate_pages(&count, &page) != FALSE);
    REQUIRE(tracker.map_pages(window, 1, &page) != FALSE);
    bytes[0] = 0x44;

    REQUIRE(tracker.free_pages(1, &page) != FALSE);
    CHECK_EQ(bytes[0], uint8_t{0x44});

    // Its number goes to a new page with memory of its own.
    uint64_t fresh = 0;
    REQUIRE(tracker.allocate_pages(&count, &fresh) != FALSE);
    CHECK_EQ(fresh, page);
    REQUIRE(tracker.map_pages(at(begin + k64K), 1, &fresh) != FALSE);
    CHECK_EQ(bytes[k64K], uint8_t{0});
    CHECK_EQ(bytes[0], uint8_t{0x44});

    // Releasing the range unmaps both pages with it.
    CHECK(tracker.free(window, 0, MEM_RELEASE) != FALSE);
    CHECK_EQ(query(begin).State, DWORD{MEM_FREE});
    CHECK_EQ(query(begin + k64K).State, DWORD{MEM_FREE});
    CHECK(!tracker.find(window).has_value());
    CHECK(tracker.free_pages(1, &fresh) != FALSE);
}

TEST_CASE("pages map only at 64 KiB addresses in ranges for physical pages") {
    MemoryTracker tracker;
    uint64_t pages[2] = {};
    uint64_t count = 2;
    REQUIRE(tracker.allocate_pages(&count, pages) != FALSE);

    // A range with no access for the CPU still gives read-write pages: the runtime reads GPU
    // memory on the CPU.
    void* window = tracker.reserve_placeholder(nullptr, 4 * k64K, k64K, true,
                                               {PAGE_NOACCESS, PAGE_GRAPHICS_NOACCESS});
    REQUIRE(window != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(window);
    REQUIRE(tracker.map_pages(window, 1, pages) != FALSE);
    CHECK_EQ(query(begin).Protect, DWORD{PAGE_READWRITE});
    static_cast<volatile uint8_t*>(window)[1] = 1;

    SetLastError(0);
    CHECK(tracker.map_pages(at(begin + 4096), 1, pages) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_PARAMETER});
    const void* misaligned = at(begin + 4096);
    CHECK(tracker.map_pages_scatter(&misaligned, 1, pages) == FALSE);

    // An unknown number fails, and the pages before it in the call stay mapped.
    const uint64_t run[2] = {pages[1], pages[1] + 64 * kFramesPerPhysicalPage};
    SetLastError(0);
    CHECK(tracker.map_pages(at(begin + k64K), 2, run) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_PARAMETER});
    CHECK_EQ(query(begin + k64K).State, DWORD{MEM_COMMIT});
    CHECK_EQ(query(begin + 2 * k64K).State, DWORD{MEM_RESERVE});

    // Not a range for physical pages: a plain placeholder, memory allocated in place in one,
    // and memory the tracker didn't make.
    void* plain = tracker.reserve_placeholder(nullptr, 4 * k64K, k64K, false, {});
    REQUIRE(plain != nullptr);
    const auto plain_begin = reinterpret_cast<uintptr_t>(plain);
    SetLastError(0);
    CHECK(tracker.map_pages(plain, 1, pages) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_ADDRESS});
    void* carved = tracker.carve_physical(at(plain_begin + 2 * k64K), 2 * k64K, {PAGE_READWRITE});
    REQUIRE(carved != nullptr);
    REQUIRE(tracker.map_pages(carved, 1, &pages[1]) != FALSE);
    CHECK_EQ(query(plain_begin + 2 * k64K).State, DWORD{MEM_COMMIT});
    void* allocated = tracker.allocate(at(plain_begin + k64K), k64K, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE, k64K, 0);
    REQUIRE(allocated != nullptr);
    SetLastError(0);
    CHECK(tracker.map_pages(allocated, 1, pages) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_ADDRESS});
    void* foreign = VirtualAlloc(nullptr, k64K, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(foreign != nullptr);
    CHECK(tracker.map_pages(foreign, 1, pages) == FALSE);
    CHECK(VirtualFree(foreign, 0, MEM_RELEASE) != FALSE);

    CHECK(tracker.free(window, 0, MEM_RELEASE) != FALSE);
    CHECK(tracker.free(plain, 0, MEM_RELEASE) != FALSE);
    CHECK_EQ(query(plain_begin + 2 * k64K).State, DWORD{MEM_FREE});
    CHECK(tracker.free_pages(2, pages) != FALSE);
}
