// Jump-table resolution on short hand-assembled switch dispatches (written for these tests):
// each table shape the analysis knows, the ways it finds an entry count, and the cases it must
// refuse. Every test lays out the same shape: a dispatch at the start of .text, case blocks at
// kCases (`mov eax, n; ret`, 8 bytes apart), a default block, and tables in .rdata.

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "code_walker.h"
#include "function_discovery.h"
#include "g6test.h"
#include "pe_builder.h"

using g6::recomp::CodeWalker;
using g6::recomp::JumpTable;
using g6::recomp::Range;
using g6::recomp::WalkResult;
using g6::test::PeBuilder;

namespace {

constexpr uint32_t kText = 0x1000;
constexpr uint32_t kCases = 0x1080;
constexpr uint32_t kDefault = 0x10F0;
constexpr uint32_t kFunctionEnd = 0x1100;
constexpr uint32_t kOutside = 0x1200; // in .text, but past the function
constexpr uint32_t kTable = 0x2000;
constexpr uint32_t kByteTable = 0x2100;

constexpr uint32_t case_block(uint32_t n) {
    return kCases + (n * 8);
}

// Appends instruction bytes and patches PC-relative fields.
class Assembler {
public:
    explicit Assembler(PeBuilder& builder, uint32_t pc = kText) : builder_(builder), pc_(pc) {}

    Assembler& bytes(std::initializer_list<uint8_t> values) {
        builder_.put_bytes(pc_, values);
        pc_ += static_cast<uint32_t>(values.size());
        return *this;
    }
    // A rel32 branch displacement or RIP-relative disp32 that ends its instruction.
    Assembler& relative(uint32_t target) {
        builder_.put_rel32(pc_, target);
        pc_ += 4;
        return *this;
    }
    uint32_t pc() const { return pc_; }

private:
    PeBuilder& builder_;
    uint32_t pc_;
};

struct Switch {
    PeBuilder builder;

    Switch() {
        builder.add_section(".text", PeBuilder::kCode, 0x400, 0xCC);
        builder.add_section(".rdata", PeBuilder::kRdata, 0x200);
        for (uint32_t n = 0; n < 8; ++n) {
            builder.put_bytes(case_block(n), {0xB8}); // mov eax, n; ret
            builder.put<uint32_t>(case_block(n) + 1, n);
            builder.put_bytes(case_block(n) + 5, {0xC3});
        }
        builder.put_bytes(kDefault, {0x31, 0xC0, 0xC3}); // xor eax,eax; ret
        builder.put_bytes(kOutside, {0xC3});
    }

    // 32-bit entries holding each target minus `base`.
    void offsets(uint32_t at, std::initializer_list<uint32_t> targets, uint32_t base = 0) {
        for (const uint32_t target : targets) {
            builder.put<int32_t>(at, static_cast<int32_t>(int64_t{target} - base));
            at += 4;
        }
    }

    // `table_starts`: tables known from elsewhere, as discovery passes them on.
    WalkResult walk(std::vector<uint32_t> table_starts = {}) const {
        const g6::pe::Image image = builder.image();
        CodeWalker walker(image);
        walker.set_table_starts(std::move(table_starts));
        const uint32_t entry = kText;
        const Range range{kText, kFunctionEnd};
        return walker.walk(std::span<const uint32_t>(&entry, 1), std::span<const Range>(&range, 1),
                           [](uint32_t) { return false; });
    }
};

// `lea reg, [rip + disp]` pointing at the image base (RVA 0); `prefix` selects the register.
void lea_image_base(Assembler& code, std::initializer_list<uint8_t> prefix) {
    code.bytes(prefix).relative(0);
}

std::vector<uint32_t> cases(std::initializer_list<uint32_t> numbers) {
    std::vector<uint32_t> targets;
    for (const uint32_t n : numbers) {
        targets.push_back(case_block(n));
    }
    return targets;
}

// The image-relative dispatch tail: mov ecx,[rdx+rax*4+kTable]; add rcx,rdx; jmp rcx.
void image_relative_tail(Assembler& code) {
    code.bytes({0x8B, 0x8C, 0x82, 0x00, 0x20, 0x00, 0x00})
        .bytes({0x48, 0x01, 0xD1})
        .bytes({0xFF, 0xE1});
}

} // namespace

TEST_CASE("jump table: image-relative entries bounded by cmp/ja") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x03});              // cmp ecx, 3
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    code.bytes({0x48, 0x63, 0xC1});              // movsxd rax, ecx
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    const uint32_t jump = code.pc() + 10;
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    CHECK_EQ(walk.indirect_jumps, 0u);
    REQUIRE(walk.jump_tables.size() == 1);
    const JumpTable& table = walk.jump_tables[0];
    CHECK_EQ(table.jump, jump);
    CHECK_EQ(table.table, kTable);
    CHECK_EQ(table.entry_size, uint8_t{4});
    CHECK(table.bounded);
    CHECK(table.targets == cases({0, 1, 2, 3})); // the fifth entry is past the bound
    // 7 dispatch instructions, 2 in each of the 4 cases, 2 in the default block.
    CHECK_EQ(walk.instructions, 17u);
}

TEST_CASE("jump table: table-relative signed entries, index copied after the check") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xFF, 0x02});                  // cmp edi, 2
    code.bytes({0x0F, 0x87}).relative(kDefault);     // ja default
    code.bytes({0x89, 0xF8});                        // mov eax, edi
    code.bytes({0x48, 0x8D, 0x0D}).relative(kTable); // lea rcx, [table]
    code.bytes({0x48, 0x63, 0x04, 0x81});            // movsxd rax, dword [rcx+rax*4]
    code.bytes({0x48, 0x01, 0xC8});                  // add rax, rcx
    code.bytes({0xFF, 0xE0});                        // jmp rax
    // The table sits after the code, so every entry is negative.
    s.offsets(kTable, {case_block(2), case_block(0), case_block(1)}, kTable);

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({2, 0, 1}));
}

TEST_CASE("jump table: index copied before the check on its source") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x89, 0xC8});                    // mov eax, ecx
    code.bytes({0x83, 0xF9, 0x03});              // cmp ecx, 3
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2, 3})); // the fifth entry is past the bound
}

TEST_CASE("jump table: the check on a copy of the index") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x89, 0xC1});                    // mov ecx, eax
    code.bytes({0x83, 0xF9, 0x02});              // cmp ecx, 2
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2}));
}

TEST_CASE("jump table: a check on a register changed since the copy bounds nothing") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x89, 0xC8});                    // mov eax, ecx
    code.bytes({0x83, 0xC1, 0x01});              // add ecx, 1
    code.bytes({0x83, 0xF9, 0x03});              // cmp ecx, 3
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(!walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2, 3, 4}));
}

TEST_CASE("jump table: sparse cases through a byte table, image base register untraced") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x09});              // cmp ecx, 9
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    code.bytes({0x48, 0x63, 0xC1});              // movsxd rax, ecx
    code.bytes({0x41, 0x0F, 0xB6, 0x84, 0x04, 0x00, 0x21, 0x00,
                0x00}); // movzx eax, byte [r12+rax+kByteTable]
    code.bytes({0x41, 0x8B, 0x84, 0x84, 0x00, 0x20, 0x00, 0x00}); // mov eax, [r12+rax*4+kTable]
    code.bytes({0x4C, 0x01, 0xE0});                               // add rax, r12
    code.bytes({0xFF, 0xE0});                                     // jmp rax
    const uint8_t numbers[10] = {0, 1, 2, 2, 1, 0, 2, 2, 2, 1};
    for (uint32_t i = 0; i < 10; ++i) {
        s.builder.put<uint8_t>(kByteTable + i, numbers[i]);
    }
    s.offsets(kTable, {case_block(5), case_block(6), case_block(7), case_block(0)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    const JumpTable& table = walk.jump_tables[0];
    CHECK(table.bounded);
    CHECK_EQ(table.index_table, kByteTable);
    CHECK_EQ(table.index_count, 10u);
    CHECK(table.targets == cases({5, 6, 7})); // largest byte is 2
}

TEST_CASE("jump table: bound carried through lea, and, mov and shr") {
    Switch s;
    Assembler code(s.builder);
    lea_image_base(code, {0x4C, 0x8D, 0x15});                     // lea r10, [image base]
    code.bytes({0x49, 0x83, 0xF8, 0x40});                         // cmp r8, 0x40
    code.bytes({0x0F, 0x83}).relative(kDefault);                  // jae default
    code.bytes({0x4D, 0x8D, 0x48, 0x0F});                         // lea r9, [r8+15]
    code.bytes({0x49, 0x83, 0xE1, 0xF0});                         // and r9, -16
    code.bytes({0x4D, 0x89, 0xCB});                               // mov r11, r9
    code.bytes({0x49, 0xC1, 0xEB, 0x04});                         // shr r11, 4
    code.bytes({0x47, 0x8B, 0x9C, 0x9A, 0x00, 0x20, 0x00, 0x00}); // mov r11d, [r10+r11*4+kTable]
    code.bytes({0x4D, 0x01, 0xD3});                               // add r11, r10
    code.bytes({0x41, 0xFF, 0xE3});                               // jmp r11
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4),
                       case_block(5), case_block(6)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    // r8 <= 63, so (r8 + 15) / 16 rounded down is at most 4: five entries.
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2, 3, 4}));
}

TEST_CASE("jump table: a mask bounds the index") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x89, 0xC8});                 // mov eax, ecx
    code.bytes({0x83, 0xE0, 0x03});           // and eax, 3
    lea_image_base(code, {0x48, 0x8D, 0x15}); // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(3), case_block(2), case_block(1), case_block(0), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({3, 2, 1, 0}));
}

TEST_CASE("jump table: the check may branch to the dispatch") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x02});           // cmp ecx, 2
    code.bytes({0x76, 0x05});                 // jbe dispatch
    code.bytes({0xE9}).relative(kDefault);    // jmp default
    code.bytes({0x48, 0x63, 0xC1});           // dispatch: movsxd rax, ecx
    lea_image_base(code, {0x48, 0x8D, 0x15}); // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2}));
}

TEST_CASE("jump table: 64-bit absolute entries read by the jmp itself") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF8, 0x01});                  // cmp eax, 1
    code.bytes({0x0F, 0x87}).relative(kDefault);     // ja default
    code.bytes({0x48, 0x8D, 0x15}).relative(kTable); // lea rdx, [table]
    code.bytes({0xFF, 0x24, 0xC2});                  // jmp [rdx+rax*8]
    s.builder.put<uint64_t>(kTable, PeBuilder::kImageBase + case_block(1));
    s.builder.put<uint64_t>(kTable + 8, PeBuilder::kImageBase + case_block(3));

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK_EQ(walk.jump_tables[0].entry_size, uint8_t{8});
    CHECK(walk.jump_tables[0].targets == cases({1, 3}));
}

TEST_CASE("jump table: without a usable bound, entries are read while they stay valid") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x01});              // cmp ecx, 1
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    code.bytes({0x83, 0xE9, 0x01});              // sub ecx, 1: may wrap, so the check is lost
    code.bytes({0x48, 0x63, 0xC1});              // movsxd rax, ecx
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), kOutside});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(!walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2}));
}

TEST_CASE("jump table: a signed branch between the check and its unsigned branch") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x04});              // cmp ecx, 4
    code.bytes({0x0F, 0x8F}).relative(kDefault); // jg default: says nothing unsigned
    code.bytes({0x0F, 0x83}).relative(kDefault); // jae default
    code.bytes({0x48, 0x63, 0xC1});              // movsxd rax, ecx
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2, 3}));
}

TEST_CASE("jump table: an equality test that went the unequal way, then the check") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x04});              // cmp ecx, 4
    code.bytes({0x75, 0x05});                    // jne check (taken on the way to the dispatch)
    code.bytes({0xE9}).relative(case_block(4));  // jmp case 4
    code.bytes({0x0F, 0x83}).relative(kDefault); // check: jae default
    code.bytes({0x48, 0x63, 0xC1});              // movsxd rax, ecx
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2, 3}));
}

TEST_CASE("jump table: a byte-wide index caps an unbounded read") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x0F, 0xB6, 0xC1});           // movzx eax, cl
    code.bytes({0xC1, 0xE8, 0x06});           // shr eax, 6: at most 3
    lea_image_base(code, {0x48, 0x8D, 0x15}); // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(!walk.jump_tables[0].bounded); // a cap, not a check
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2, 3}));
}

TEST_CASE("jump table: an unbounded read stops at the next known table") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x48, 0x63, 0xC1});           // movsxd rax, ecx: no check
    lea_image_base(code, {0x48, 0x8D, 0x15}); // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});

    const WalkResult alone = s.walk();
    REQUIRE(alone.jump_tables.size() == 1);
    CHECK(alone.jump_tables[0].targets == cases({0, 1, 2, 3, 4}));

    const WalkResult walk = s.walk({kTable, kTable + 12});
    CHECK(walk.ok());
    REQUIRE(walk.jump_tables.size() == 1);
    CHECK(!walk.jump_tables[0].bounded);
    CHECK(walk.jump_tables[0].targets == cases({0, 1, 2}));
}

TEST_CASE("jump table: a bounded table with an entry outside the function is rejected") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x02});              // cmp ecx, 2
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    code.bytes({0x48, 0x63, 0xC1});              // movsxd rax, ecx
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), kOutside, case_block(2)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    CHECK(walk.jump_tables.empty());
    CHECK_EQ(walk.rejected_tables, 1u);
    CHECK_EQ(walk.indirect_jumps, 1u);
}

TEST_CASE("jump table: an untraced base is only trusted with a bound") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x48, 0x63, 0xC1}); // movsxd rax, ecx
    image_relative_tail(code);      // rdx never set in this function
    s.offsets(kTable, {case_block(0), case_block(1)});

    const WalkResult walk = s.walk();
    CHECK(walk.ok());
    CHECK(walk.jump_tables.empty());
    CHECK_EQ(walk.indirect_jumps, 1u);
}

TEST_CASE("jump table: jumps through loaded pointers are tail calls") {
    Switch s;
    Assembler(s.builder).bytes({0x48, 0x8B, 0x41, 0x08, 0xFF, 0xE0}); // mov rax,[rcx+8]; jmp rax
    const WalkResult through_register = s.walk();
    CHECK(through_register.ok());
    CHECK_EQ(through_register.indirect_tail_calls, 1u);
    CHECK_EQ(through_register.indirect_jumps, 0u);

    Assembler(s.builder).bytes({0xFF, 0x61, 0x10}); // jmp [rcx+0x10]
    const WalkResult through_memory = s.walk();
    CHECK_EQ(through_memory.indirect_tail_calls, 1u);
    CHECK_EQ(through_memory.indirect_jumps, 0u);
}

TEST_CASE("jump table: discovery keeps tables with their function") {
    Switch s;
    Assembler code(s.builder);
    code.bytes({0x83, 0xF9, 0x03});              // cmp ecx, 3
    code.bytes({0x0F, 0x87}).relative(kDefault); // ja default
    code.bytes({0x48, 0x63, 0xC1});              // movsxd rax, ecx
    lea_image_base(code, {0x48, 0x8D, 0x15});    // lea rdx, [image base]
    image_relative_tail(code);
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3)});
    s.builder.set_entry_point(kText);

    const g6::pe::Image image = s.builder.image();
    const auto result = g6::recomp::discover_functions(image);
    const auto* function = result.find(kText);
    REQUIRE(function != nullptr);
    REQUIRE(function->jump_tables.size() == 1);
    CHECK(function->jump_tables[0].targets == cases({0, 1, 2, 3}));
    CHECK_EQ(function->end, kDefault + 3);
    CHECK_EQ(result.stats.jump_tables, size_t{1});
    CHECK_EQ(result.stats.jump_table_targets, size_t{4});
    CHECK_EQ(result.stats.unresolved_indirect_jumps, size_t{0});
}

TEST_CASE("jump table: discovery stops an unbounded table at another one it found") {
    Switch s;
    Assembler code(s.builder);
    constexpr uint32_t kSecond = kText + 31;
    code.bytes({0x83, 0xFA, 0x01});                               // cmp edx, 1
    code.bytes({0x0F, 0x86}).relative(kSecond);                   // jbe second
    code.bytes({0x48, 0x63, 0xC1});                               // movsxd rax, ecx: no check
    lea_image_base(code, {0x48, 0x8D, 0x15});                     // lea rdx, [image base]
    image_relative_tail(code);                                    // through kTable
    REQUIRE(code.pc() == kSecond);                                // second:
    code.bytes({0x48, 0x63, 0xC2});                               // movsxd rax, edx
    lea_image_base(code, {0x4C, 0x8D, 0x05});                     // lea r8, [image base]
    code.bytes({0x41, 0x8B, 0x8C, 0x80, 0x0C, 0x20, 0x00, 0x00}); // mov ecx, [r8+rax*4+kTable+12]
    code.bytes({0x4C, 0x01, 0xC1});                               // add rcx, r8
    code.bytes({0xFF, 0xE1});                                     // jmp rcx
    // The second table follows the first, and its entries are valid targets for the first too.
    s.offsets(kTable, {case_block(0), case_block(1), case_block(2), case_block(3), case_block(4)});
    s.builder.set_entry_point(kText);

    const g6::pe::Image image = s.builder.image();
    const auto result = g6::recomp::discover_functions(image);
    const auto* function = result.find(kText);
    REQUIRE(function != nullptr);
    REQUIRE(function->jump_tables.size() == 2);
    for (const JumpTable& table : function->jump_tables) {
        if (table.table == kTable) {
            CHECK(!table.bounded);
            CHECK(table.targets == cases({0, 1, 2}));
        } else {
            CHECK_EQ(table.table, kTable + 12);
            CHECK(table.bounded);
            CHECK(table.targets == cases({3, 4}));
        }
    }
    const std::vector<uint32_t> starts{kTable, kTable + 12};
    CHECK(result.table_starts == starts);
    CHECK_EQ(result.stats.jump_tables_unbounded, size_t{1});
}
