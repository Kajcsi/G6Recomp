// memory_tracker (g6/gpu/memory_tracker.h): console protections split into host and GPU parts,
// placeholder reservations, physical ranges carved out of them, allocations made in place inside
// them and put back, whole releases, and allocations aligned to the console's large page sizes.

#include <cstdint>
#include <cstring>
#include <windows.h>

#include "g6/gpu/memory_tracker.h"
#include "g6test.h"

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
    void* base = tracker.reserve_placeholder(nullptr, 3 * k64K + 1, k2M, true, 0);
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
    void* base = tracker.reserve_placeholder(nullptr, 4 * k64K, k64K, false, 0);
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
    void* base = tracker.reserve_placeholder(nullptr, 8 * k64K, k64K, false, 0);
    REQUIRE(base != nullptr);
    const auto begin = reinterpret_cast<uintptr_t>(base);

    const void* carved =
        tracker.carve_physical(at(begin + 2 * k64K), 3 * k64K, PAGE_GRAPHICS_READWRITE);
    REQUIRE(carved == at(begin + 2 * k64K));
    const auto info = tracker.find(at(begin + 4 * k64K));
    REQUIRE(info.has_value());
    CHECK_EQ(info->base, begin + 2 * k64K);
    CHECK_EQ(info->size, 3 * k64K);
    CHECK(info->physical);
    CHECK(info->placeholder);
    CHECK_EQ(query(begin + 2 * k64K).State, DWORD{MEM_RESERVE});
    const auto before = tracker.find(base);
    REQUIRE(before.has_value());
    CHECK(!before->physical);

    // Not on a 64 KiB boundary, past the placeholder's end, or outside any placeholder.
    SetLastError(0);
    CHECK(tracker.carve_physical(at(begin + 4096), k64K, 0) == nullptr);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_ADDRESS});
    CHECK(tracker.carve_physical(at(begin + 7 * k64K), 2 * k64K, 0) == nullptr);
    int outside = 0;
    CHECK(tracker.carve_physical(&outside, k64K, 0) == nullptr);

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
