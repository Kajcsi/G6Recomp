// Function discovery on a synthetic image with hand-assembled code (written for this test):
// a .pdata function calling two leaves, an exported leaf that falls through into a leaf only
// reachable through a pointer table, an import thunk, a bogus pointer into the middle of a
// loop, and a pointer at int3 padding.

#include "function_discovery.h"
#include "g6/pe/unwind.h"
#include "g6test.h"
#include "pe_builder.h"

using g6::pe::Directory;
using g6::recomp::DiscoveredFunction;
using g6::recomp::DiscoveryOptions;
using g6::recomp::DiscoveryResult;
using g6::test::PeBuilder;
namespace recomp = g6::recomp;

namespace {

constexpr uint32_t kF = 0x1000;     // .pdata function: calls L1 and L4
constexpr uint32_t kL1 = 0x1020;    // leaf: lea eax,[rcx+rdx]; ret
constexpr uint32_t kL2 = 0x1030;    // exported leaf: inc ecx, then falls into L3
constexpr uint32_t kL3 = 0x1032;    // leaf reached only through a pointer: mov eax,ecx; ret
constexpr uint32_t kL4 = 0x1040;    // leaf with a loop
constexpr uint32_t kL4Mid = 0x1044; // inside L4's loop body
constexpr uint32_t kL5 = 0x1050;    // exported import thunk
constexpr uint32_t kPadding = 0x1060;

PeBuilder synthetic_image() {
    PeBuilder b;
    const uint32_t text = b.add_section(".text", PeBuilder::kCode, 0x100, 0xCC);
    const uint32_t rdata = b.add_section(".rdata", PeBuilder::kRdata, 0x400);
    (void)text;

    // F: sub rsp,28h; call L1; call L4; add rsp,28h; ret
    b.put_bytes(kF, {0x48, 0x83, 0xEC, 0x28, 0xE8});
    b.put_rel32(kF + 5, kL1);
    b.put_bytes(kF + 9, {0xE8});
    b.put_rel32(kF + 10, kL4);
    b.put_bytes(kF + 14, {0x48, 0x83, 0xC4, 0x28, 0xC3});
    b.put_bytes(kL1, {0x8D, 0x04, 0x11, 0xC3});
    b.put_bytes(kL2, {0xFF, 0xC1});
    b.put_bytes(kL3, {0x89, 0xC8, 0xC3});
    b.put_bytes(kL4, {0x31, 0xC0, 0xFF, 0xC0, 0xFF, 0xC9, 0x75, 0xFA, 0xC3});
    b.put_bytes(kL5, {0xFF, 0x25});
    b.put<int32_t>(kL5 + 2, static_cast<int32_t>(rdata + 0x300 - (kL5 + 6)));
    b.set_entry_point(kF);

    // Unwind info and .pdata for F: one AllocSmall(0x28) at prolog offset 4.
    b.put_bytes(rdata, {0x01, 0x04, 0x01, 0x00, 0x04, 0x42, 0x00, 0x00});
    b.put<uint32_t>(rdata + 0x10, kF);
    b.put<uint32_t>(rdata + 0x14, kF + 19);
    b.put<uint32_t>(rdata + 0x18, rdata);
    b.set_directory(Directory::Exception, rdata + 0x10, 12);

    // Exports: L2 and L5.
    const uint32_t ed = rdata + 0x40;
    b.put<uint32_t>(ed + 12, rdata + 0xB0);
    b.put<uint32_t>(ed + 16, 1);
    b.put<uint32_t>(ed + 20, 2);
    b.put<uint32_t>(ed + 24, 2);
    b.put<uint32_t>(ed + 28, rdata + 0x80);
    b.put<uint32_t>(ed + 32, rdata + 0x90);
    b.put<uint32_t>(ed + 36, rdata + 0xA0);
    b.put<uint32_t>(rdata + 0x80, kL2);
    b.put<uint32_t>(rdata + 0x84, kL5);
    b.put<uint32_t>(rdata + 0x90, rdata + 0xC0);
    b.put<uint32_t>(rdata + 0x94, rdata + 0xD0);
    b.put<uint16_t>(rdata + 0xA0, 0);
    b.put<uint16_t>(rdata + 0xA2, 1);
    b.put_string(rdata + 0xB0, "synthetic.dll");
    b.put_string(rdata + 0xC0, "two");
    b.put_string(rdata + 0xD0, "five");
    b.set_directory(Directory::Export, ed, 0xA0);

    // A pointer table with relocations: L3, the middle of L4's loop, and int3 padding.
    b.put<uint64_t>(rdata + 0x100, PeBuilder::kImageBase + kL3);
    b.put<uint64_t>(rdata + 0x108, PeBuilder::kImageBase + kL4Mid);
    b.put<uint64_t>(rdata + 0x110, PeBuilder::kImageBase + kPadding);
    b.put<uint32_t>(rdata + 0x200, rdata);
    b.put<uint32_t>(rdata + 0x204, 16);
    b.put<uint16_t>(rdata + 0x208, (10 << 12) | 0x100);
    b.put<uint16_t>(rdata + 0x20A, (10 << 12) | 0x108);
    b.put<uint16_t>(rdata + 0x20C, (10 << 12) | 0x110);
    b.set_directory(Directory::BaseReloc, rdata + 0x200, 16);
    return b;
}

const DiscoveredFunction& require_function(const DiscoveryResult& result, uint32_t start) {
    const DiscoveredFunction* function = result.find(start);
    if (function == nullptr) {
        g6::test::report_failure(__FILE__, __LINE__,
                                 fmt::format("no function discovered at {:#x}", start));
        throw g6::test::RequireFailed{};
    }
    return *function;
}

} // namespace

TEST_CASE("discovery: seeds, calls, fall-through split and rejections") {
    const g6::pe::Image image = synthetic_image().image();
    const DiscoveryResult result = recomp::discover_functions(image);

    CHECK_EQ(result.functions.size(), size_t{6});
    CHECK_EQ(result.stats.pdata_functions, size_t{1});
    CHECK_EQ(result.stats.leaf_functions, size_t{5});
    CHECK_EQ(result.stats.leaf_splits, size_t{1});
    CHECK_EQ(result.stats.import_thunks, size_t{1});
    CHECK_EQ(result.stats.seeds_mid_function, size_t{1});  // the pointer into L4's loop
    CHECK_EQ(result.stats.candidates_rejected, size_t{1}); // the pointer at padding
    CHECK_EQ(result.stats.decode_errors, size_t{0});

    const DiscoveredFunction& f = require_function(result, kF);
    CHECK(f.has_unwind_info);
    CHECK_EQ(f.end, kF + 19);
    CHECK_EQ(f.sources, uint32_t{recomp::kSourcePdata | recomp::kSourceEntryPoint});

    const DiscoveredFunction& l1 = require_function(result, kL1);
    CHECK(!l1.has_unwind_info);
    CHECK_EQ(l1.end, kL1 + 4);
    CHECK_EQ(l1.sources, uint32_t{recomp::kSourceDirectCall});

    const DiscoveredFunction& l2 = require_function(result, kL2);
    CHECK_EQ(l2.end, kL3); // cut where it falls into L3
    CHECK_EQ(l2.sources, uint32_t{recomp::kSourceExport});

    const DiscoveredFunction& l3 = require_function(result, kL3);
    CHECK_EQ(l3.end, kL3 + 3);
    CHECK_EQ(l3.sources, uint32_t{recomp::kSourceRelocation});

    const DiscoveredFunction& l4 = require_function(result, kL4);
    CHECK_EQ(l4.end, kL4 + 9);
    CHECK(result.find(kL4Mid) == nullptr);

    const DiscoveredFunction& l5 = require_function(result, kL5);
    CHECK_EQ(l5.end, kL5 + 6);
    CHECK(result.find(kPadding) == nullptr);

    CHECK(result.containing(kL4Mid) == &l4);
    CHECK(result.containing(kF + 18) == &f);
    CHECK(result.containing(kF + 19) == nullptr); // int3 padding between F and L1
    CHECK(result.containing(kL3) == &l3);
}

TEST_CASE("discovery: options turn seed sources off") {
    const g6::pe::Image image = synthetic_image().image();

    DiscoveryOptions no_relocs;
    no_relocs.use_relocations = false;
    const DiscoveryResult without_relocs = recomp::discover_functions(image, no_relocs);
    // Without the pointer table nothing marks L3 as an entry, so L2 keeps L3's body.
    CHECK(without_relocs.find(kL3) == nullptr);
    REQUIRE(without_relocs.find(kL2) != nullptr);
    CHECK_EQ(without_relocs.find(kL2)->end, kL3 + 3);
    CHECK_EQ(without_relocs.stats.candidates_rejected, size_t{0});
    CHECK_EQ(without_relocs.stats.seeds_mid_function, size_t{0});

    DiscoveryOptions no_follow;
    no_follow.follow_calls = false;
    const DiscoveryResult without_calls = recomp::discover_functions(image, no_follow);
    CHECK(without_calls.find(kL1) == nullptr);
    CHECK(without_calls.find(kL4) == nullptr);
    CHECK(without_calls.find(kL2) != nullptr);
}

TEST_CASE("discovery: a lea of a padded address splits the leaf that jumps to it") {
    // A leaf that ends in a jump to a second function placed after int3 padding, the way a
    // compiler lays out a tail call to a function it emits right after. Without .pdata or a
    // pointer to the second function, only the `lea` that takes its address marks it.
    constexpr uint32_t kA = 0x1000;
    constexpr uint32_t kB = 0x1020;
    constexpr uint32_t kInsideB = 0x1030; // 16-byte aligned, but no padding before it
    PeBuilder b;
    b.add_section(".text", PeBuilder::kCode, 0x100, 0xCC);
    b.put_bytes(kA, {0x48, 0x8D, 0x05}); // lea rax, [B]
    b.put<int32_t>(kA + 3, static_cast<int32_t>(kB - (kA + 7)));
    b.put_bytes(kA + 7, {0x48, 0x8D, 0x0D}); // lea rcx, [inside B]
    b.put<int32_t>(kA + 10, static_cast<int32_t>(kInsideB - (kA + 14)));
    b.put_bytes(kA + 14, {0xEB, static_cast<uint8_t>(kB - (kA + 16))}); // jmp B
    b.put_bytes(kB, {0xB8, 0x01, 0x00, 0x00, 0x00});                    // mov eax, 1
    for (uint32_t rva = kB + 5; rva < kInsideB; ++rva) {
        b.put_bytes(rva, {0x90});
    }
    b.put_bytes(kInsideB, {0xC3});
    b.set_entry_point(kA);
    const g6::pe::Image image = b.image();

    const DiscoveryResult result = recomp::discover_functions(image);
    CHECK_EQ(result.functions.size(), size_t{2});
    CHECK_EQ(result.stats.leaf_splits, size_t{1});
    CHECK_EQ(result.stats.seeds_mid_function, size_t{0});
    CHECK_EQ(result.stats.candidates_rejected, size_t{0});
    const DiscoveredFunction& a = require_function(result, kA);
    CHECK_EQ(a.end, kA + 16);
    const DiscoveredFunction& second = require_function(result, kB);
    CHECK_EQ(second.end, kInsideB + 1);
    CHECK_EQ(second.sources, uint32_t{recomp::kSourceLea});
    CHECK(result.find(kInsideB) == nullptr);

    DiscoveryOptions no_follow;
    no_follow.follow_calls = false;
    const DiscoveryResult without = recomp::discover_functions(image, no_follow);
    CHECK_EQ(without.functions.size(), size_t{1});
    REQUIRE(without.find(kA) != nullptr);
    CHECK_EQ(without.find(kA)->end, kInsideB + 1);
}

TEST_CASE("describe_sources joins names in a fixed order") {
    CHECK_EQ(recomp::describe_sources(0), std::string("none"));
    CHECK_EQ(recomp::describe_sources(recomp::kSourceTailCall | recomp::kSourcePdata),
             std::string("pdata+tail"));
}
