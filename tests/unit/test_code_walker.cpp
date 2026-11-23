// CodeWalker on short hand-assembled sequences (written for these tests): control-flow
// following, call and tail-jump collection, import thunks, traps, fall-through, and failures.

#include <algorithm>
#include <vector>

#include "code_walker.h"
#include "g6test.h"
#include "pe_builder.h"

using g6::recomp::CodeWalker;
using g6::recomp::Range;
using g6::recomp::WalkResult;
using g6::test::PeBuilder;

namespace {

constexpr uint32_t kText = 0x1000;

struct Code {
    PeBuilder builder;

    Code() {
        builder.add_section(".text", PeBuilder::kCode, 0x400, 0xCC);
        builder.add_section(".rdata", PeBuilder::kRdata, 0x100);
    }

    WalkResult walk(uint32_t entry, uint32_t end, std::vector<uint32_t> others = {}) {
        const g6::pe::Image image = builder.image();
        const CodeWalker walker(image);
        const Range range{entry, end};
        return walker.walk(
            std::span<const uint32_t>(&entry, 1), std::span<const Range>(&range, 1),
            [&](uint32_t rva) { return std::ranges::find(others, rva) != others.end(); });
    }
};

} // namespace

TEST_CASE("walker: straight-line code ending in ret") {
    Code code;
    code.builder.put_bytes(kText, {0x31, 0xC0, 0xC3}); // xor eax,eax; ret
    const WalkResult walk = code.walk(kText, kText + 0x100);
    CHECK(walk.ok());
    CHECK_EQ(walk.instructions, 2u);
    CHECK_EQ(walk.extent_end, kText + 3);
    CHECK(!walk.first_is_trap);
}

TEST_CASE("walker: branches, calls and tail jumps") {
    Code code;
    code.builder.put_bytes(kText + 0x00, {0x85, 0xC9}); // test ecx,ecx
    code.builder.put_bytes(kText + 0x02, {0x74, 0x06}); // je +6 -> 0x100A
    code.builder.put_bytes(kText + 0x04, {0xE8});       // call 0x1100
    code.builder.put_rel32(kText + 0x05, kText + 0x100);
    code.builder.put_bytes(kText + 0x09, {0xC3}); // ret
    code.builder.put_bytes(kText + 0x0A, {0xE9}); // jmp 0x1200
    code.builder.put_rel32(kText + 0x0B, kText + 0x200);
    const WalkResult walk = code.walk(kText, kText + 0x0F);
    CHECK(walk.ok());
    CHECK_EQ(walk.instructions, 5u);
    REQUIRE(walk.call_targets.size() == 1);
    CHECK_EQ(walk.call_targets[0], kText + 0x100);
    REQUIRE(walk.tail_targets.size() == 1);
    CHECK_EQ(walk.tail_targets[0], kText + 0x200);
}

TEST_CASE("walker: a conditional jump to another function is a tail call") {
    Code code;
    code.builder.put_bytes(kText + 0x00, {0x85, 0xC9, 0x74, 0x0E, 0x31, 0xC0, 0xC3}); // je 0x1012
    const WalkResult walk = code.walk(kText, kText + 0x100, {kText + 0x12});
    CHECK(walk.ok());
    REQUIRE(walk.tail_targets.size() == 1);
    CHECK_EQ(walk.tail_targets[0], kText + 0x12);
    CHECK_EQ(walk.extent_end, kText + 7);
}

TEST_CASE("walker: loops terminate") {
    Code code;
    // xor eax,eax; L: inc eax; dec ecx; jnz L; ret
    code.builder.put_bytes(kText, {0x31, 0xC0, 0xFF, 0xC0, 0xFF, 0xC9, 0x75, 0xFA, 0xC3});
    const WalkResult walk = code.walk(kText, kText + 0x100);
    CHECK(walk.ok());
    CHECK_EQ(walk.instructions, 5u);
    CHECK(walk.tail_targets.empty());
}

TEST_CASE("walker: jmp through a RIP-relative slot is an import thunk") {
    Code code;
    code.builder.put_bytes(kText, {0xFF, 0x25}); // jmp [rip+disp32]
    code.builder.put<int32_t>(kText + 2, 0x2000 - (kText + 6));
    const WalkResult walk = code.walk(kText, kText + 0x100);
    CHECK(walk.ok());
    CHECK_EQ(walk.instructions, 1u);
    REQUIRE(walk.import_jump_slots.size() == 1);
    CHECK_EQ(walk.import_jump_slots[0], 0x2000u);
    CHECK_EQ(walk.indirect_jumps, 0u);
}

TEST_CASE("walker: register-indirect jumps are counted, not followed") {
    Code code;
    code.builder.put_bytes(kText, {0xFF, 0xE0}); // jmp rax
    const WalkResult walk = code.walk(kText, kText + 0x100);
    CHECK(walk.ok());
    CHECK_EQ(walk.indirect_jumps, 1u);
}

TEST_CASE("walker: traps end a path and are flagged at the entry") {
    Code code;
    const WalkResult walk = code.walk(kText + 0x80, kText + 0x100); // int3 padding
    CHECK(walk.ok());
    CHECK(walk.first_is_trap);
    CHECK_EQ(walk.instructions, 1u);

    code.builder.put_bytes(kText, {0x31, 0xC0, 0x0F, 0x0B}); // xor eax,eax; ud2
    const WalkResult ud2 = code.walk(kText, kText + 0x100);
    CHECK(ud2.ok());
    CHECK(!ud2.first_is_trap);
    CHECK_EQ(ud2.instructions, 2u);

    code.builder.put_bytes(kText, {0xB9, 0x07, 0x00, 0x00, 0x00, 0xCD, 0x29}); // __fastfail(7)
    CHECK(code.walk(kText, kText + 0x100).ok());
}

TEST_CASE("walker: sequential flow into another function is a fall-through") {
    Code code;
    code.builder.put_bytes(kText, {0xFF, 0xC1, 0x89, 0xC8, 0xC3}); // inc ecx | mov eax,ecx; ret
    const WalkResult walk = code.walk(kText, kText + 0x100, {kText + 2});
    CHECK(walk.ok());
    CHECK_EQ(walk.instructions, 1u);
    REQUIRE(walk.fallthrough_targets.size() == 1);
    CHECK_EQ(walk.fallthrough_targets[0], kText + 2);
}

TEST_CASE("walker: running off the range, bad targets and undecodable bytes fail") {
    Code code;
    code.builder.put_bytes(kText, {0x90, 0x90}); // nop; nop; then out of range
    const WalkResult off_end = code.walk(kText, kText + 2);
    CHECK(!off_end.ok());
    CHECK_EQ(off_end.ran_off_end, 1u);

    code.builder.put_bytes(kText, {0xE9}); // jmp far outside the image
    code.builder.put<int32_t>(kText + 1, 0x7FFFFFF0);
    const WalkResult bad = code.walk(kText, kText + 0x100);
    CHECK(!bad.ok());
    CHECK_EQ(bad.bad_targets, 1u);

    code.builder.put_bytes(kText, {0x06}); // not a valid opcode in 64-bit mode
    const WalkResult invalid = code.walk(kText, kText + 0x100);
    CHECK(!invalid.ok());
    CHECK_EQ(invalid.decode_errors, 1u);
}

TEST_CASE("walker: branch targets inside the ranges and the instruction visitor") {
    Code code;
    // xor eax,eax; L: inc eax; dec ecx; jnz L; jmp M; int3; M: ret
    code.builder.put_bytes(
        kText, {0x31, 0xC0, 0xFF, 0xC0, 0xFF, 0xC9, 0x75, 0xFA, 0xEB, 0x01, 0xCC, 0xC3});
    const g6::pe::Image image = code.builder.image();
    const CodeWalker walker(image);
    const Range range{kText, kText + 0x100};
    const uint32_t entry = kText;
    std::vector<uint32_t> visited;
    const WalkResult walk = walker.walk(
        std::span<const uint32_t>(&entry, 1), std::span<const Range>(&range, 1),
        [](uint32_t) { return false; },
        [&](uint32_t rva, const ZydisDecodedInstruction&) { visited.push_back(rva); });
    CHECK(walk.ok());
    CHECK((walk.branch_targets == std::vector<uint32_t>{kText + 2, kText + 11}));
    std::ranges::sort(visited);
    CHECK((visited ==
           std::vector<uint32_t>{kText, kText + 2, kText + 4, kText + 6, kText + 8, kText + 11}));
}
