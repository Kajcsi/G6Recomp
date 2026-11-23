// The xmem HLE (docs/side-dlls.md, "xmem"): the attribute word, aligned and zeroed heap memory,
// honest failure for memory types that need memory_tracker, frees that aren't ours, and the
// memory and working-set queries.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "g6/imports/xmem.h"
#include "g6test.h"

using g6::imports::xmem::Attributes;
using g6::imports::xmem::XMemAlloc;
using g6::imports::xmem::XMemFree;

namespace {

// The two words the side DLLs pass (docs/side-dlls.md).
constexpr uint64_t kBinkWord = 0x05e50000;
constexpr uint64_t kUpscalerWord = 0xa8004000;

constexpr uint64_t heap_word(uint32_t alignment_log2) {
    return uint64_t{alignment_log2} << 24;
}

bool all_zero(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    return std::all_of(bytes, bytes + size, [](uint8_t byte) { return byte == 0; });
}

} // namespace

TEST_CASE("the attribute word decodes as SlimEra describes it") {
    constexpr Attributes bink = Attributes::decode(kBinkWord);
    static_assert(bink.memory_type == g6::imports::xmem::kMemoryTypeHeap);
    CHECK_EQ(bink.object_type, 0U);
    CHECK_EQ(bink.page_size, 0U);
    CHECK_EQ(bink.allocator_id, 0xe5U);
    CHECK_EQ(bink.alignment_log2, 5U);
    CHECK_EQ(bink.memory_type, 0U);
    CHECK_EQ(bink.alignment(), size_t{32});

    constexpr Attributes upscaler = Attributes::decode(kUpscalerWord);
    CHECK_EQ(upscaler.object_type, 0U);
    CHECK_EQ(upscaler.page_size, 1U); // 64 KiB
    CHECK_EQ(upscaler.allocator_id, 0U);
    CHECK_EQ(upscaler.alignment_log2, 8U);
    CHECK_EQ(upscaler.memory_type, 5U);
    CHECK_EQ(upscaler.alignment(), size_t{256});

    // Never less than the host heap's own 16 bytes.
    CHECK_EQ(Attributes::decode(heap_word(0)).alignment(), size_t{16});
    CHECK_EQ(Attributes::decode(heap_word(3)).alignment(), size_t{16});
    CHECK_EQ(Attributes::decode(0x3FFF).object_type, 0x3FFFU);
}

TEST_CASE("heap allocations are aligned and zeroed") {
    for (const uint32_t log2 : {0U, 4U, 5U, 6U, 8U, 12U, 16U}) {
        const size_t alignment = size_t{1} << std::max(log2, 4U);
        std::vector<void*> blocks;
        for (const size_t size : {size_t{0}, size_t{1}, size_t{24}, size_t{4096}, size_t{100000}}) {
            void* block = XMemAlloc(size, heap_word(log2));
            INFO("alignment " + std::to_string(alignment) + ", size " + std::to_string(size));
            REQUIRE(block != nullptr);
            CHECK_EQ(reinterpret_cast<uintptr_t>(block) % alignment, uintptr_t{0});
            CHECK(all_zero(block, size));
            std::memset(block, 0xCD, size);
            blocks.push_back(block);
        }
        // Zero-byte allocations still get addresses of their own.
        CHECK(std::ranges::count(blocks, blocks.front()) == 1);
        for (void* block : blocks) {
            XMemFree(block, heap_word(log2));
        }
    }

    void* bink = XMemAlloc(256, kBinkWord);
    REQUIRE(bink != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(bink) % 32, uintptr_t{0});
    XMemFree(bink, kBinkWord);
}

TEST_CASE("memory types that need memory_tracker fail instead of returning CPU memory") {
    CHECK(XMemAlloc(4096, kUpscalerWord) == nullptr);
    CHECK(XMemAlloc(4096, kUpscalerWord) == nullptr); // logged once, still null
    CHECK(XMemAlloc(64, uint64_t{1} << 29) == nullptr);
}

TEST_CASE("frees of anything but a live allocation are ignored") {
    XMemFree(nullptr, kBinkWord);

    // A pointer from elsewhere: not freed, and its memory untouched.
    std::vector<uint8_t> foreign(64, 0xAB);
    XMemFree(foreign.data() + 32, kBinkWord);
    CHECK(std::ranges::all_of(foreign, [](uint8_t byte) { return byte == 0xAB; }));

    // A pointer into an allocation, and a second free.
    void* block = XMemAlloc(128, kBinkWord);
    REQUIRE(block != nullptr);
    XMemFree(static_cast<uint8_t*>(block) + 16, kBinkWord);
    XMemFree(block, kBinkWord);
    XMemFree(block, kBinkWord);

    // The heap is still sound.
    void* again = XMemAlloc(128, kBinkWord);
    REQUIRE(again != nullptr);
    CHECK(all_zero(again, 128));
    XMemFree(again, kBinkWord);
}

TEST_CASE("XMemVirtualQuery reports the host's view and marks usable memory GPU-visible") {
    using g6::imports::xmem::kGpuAccessVisible;
    using g6::imports::xmem::XMemBasicInformation;
    using g6::imports::xmem::XMemVirtualQuery;

    // Two pages: the first committed read-write, the second only reserved.
    auto* region = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(region != nullptr);
    REQUIRE(VirtualAlloc(region, 0x1000, MEM_COMMIT, PAGE_READWRITE) != nullptr);

    // Callers pass 64 bytes; only the first 52 are ours to write.
    struct Buffer {
        XMemBasicInformation info;
        uint8_t rest[64 - sizeof(XMemBasicInformation)];
    };
    static_assert(sizeof(XMemBasicInformation) <= 56);
    Buffer buffer{};
    std::memset(&buffer, 0xEE, sizeof(buffer));
    REQUIRE(XMemVirtualQuery(GetCurrentProcess(), region + 0x10, &buffer.info) != FALSE);
    CHECK(buffer.info.basic.BaseAddress == region);
    CHECK(buffer.info.basic.AllocationBase == region);
    CHECK_EQ(buffer.info.basic.State, DWORD{MEM_COMMIT});
    CHECK_EQ(buffer.info.basic.Protect, DWORD{PAGE_READWRITE});
    CHECK_EQ(buffer.info.basic.RegionSize, SIZE_T{0x1000});
    CHECK_EQ(buffer.info.gpu_access, kGpuAccessVisible);
    CHECK(std::ranges::all_of(buffer.rest, [](uint8_t byte) { return byte == 0xEE; }));

    REQUIRE(XMemVirtualQuery(GetCurrentProcess(), region + 0x1000, &buffer.info) != FALSE);
    CHECK_EQ(buffer.info.basic.State, DWORD{MEM_RESERVE});
    CHECK_EQ(buffer.info.gpu_access, 0U);

    // Committed but unreadable, and guarded.
    DWORD old = 0;
    REQUIRE(VirtualProtect(region, 0x1000, PAGE_NOACCESS, &old) != FALSE);
    REQUIRE(XMemVirtualQuery(GetCurrentProcess(), region, &buffer.info) != FALSE);
    CHECK_EQ(buffer.info.gpu_access, 0U);
    REQUIRE(VirtualProtect(region, 0x1000, PAGE_READONLY | PAGE_GUARD, &old) != FALSE);
    REQUIRE(XMemVirtualQuery(GetCurrentProcess(), region, &buffer.info) != FALSE);
    CHECK_EQ(buffer.info.gpu_access, 0U);
    VirtualFree(region, 0, MEM_RELEASE);

    // Beyond the user address space, as VirtualQueryEx fails.
    CHECK(XMemVirtualQuery(GetCurrentProcess(), reinterpret_cast<const void*>(~uintptr_t{0}),
                           &buffer.info) == FALSE);
    CHECK_EQ(GetLastError(), DWORD{ERROR_INVALID_PARAMETER});
    CHECK(XMemVirtualQuery(GetCurrentProcess(), region, nullptr) == FALSE);
}

TEST_CASE("XMemGetWorkingSetStatistics writes the limit and usage, and nothing else") {
    using g6::imports::xmem::kSeriesXTitleMemory;
    using g6::imports::xmem::XMemGetWorkingSetStatistics;
    using g6::imports::xmem::XMemWorkingSetStatistics;

    static_assert(kSeriesXTitleMemory == uint64_t{13} * (uint64_t{1} << 30) + (uint64_t{1} << 29));
    // The FSR upscaler passes a 128-byte buffer; older GDK callers pass less.
    uint64_t buffer[16];
    std::ranges::fill(buffer, 0xEEEEEEEEEEEEEEEE);
    REQUIRE(XMemGetWorkingSetStatistics(
                0x20000001, reinterpret_cast<XMemWorkingSetStatistics*>(buffer)) != FALSE);
    CHECK_EQ(buffer[0], kSeriesXTitleMemory);
    CHECK(buffer[1] > 0);
    CHECK(buffer[1] <= kSeriesXTitleMemory);
    CHECK(std::all_of(buffer + 2, std::end(buffer),
                      [](uint64_t word) { return word == 0xEEEEEEEEEEEEEEEE; }));
    CHECK(XMemGetWorkingSetStatistics(0, nullptr) == FALSE);
}
