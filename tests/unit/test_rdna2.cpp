// shaderrecomp's RDNA 2 decoder (tools/shaderrecomp/src/rdna2.h) on instructions encoded here by
// hand from the field layouts in AMD's public "RDNA 2 Instruction Set Architecture" reference.
// None of them comes from a game program.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "g6test.h"
#include "rdna2.h"

namespace r = g6::shader::rdna2;

namespace {

using Words = std::vector<uint32_t>;

r::Instruction one(const Words& words) {
    return r::decode_one(std::span(words));
}

r::Operand sgpr(uint32_t n) {
    return {r::OperandKind::kSgpr, n};
}
r::Operand vgpr(uint32_t n) {
    return {r::OperandKind::kVgpr, n};
}
r::Operand literal(uint32_t value) {
    return {r::OperandKind::kLiteral, value};
}
r::Operand kind(r::OperandKind k) {
    return {k, 0};
}

std::vector<uint8_t> bytes(const Words& words) {
    std::vector<uint8_t> out;
    for (const uint32_t word : words) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>(word >> (8 * i)));
        }
    }
    return out;
}

} // namespace

TEST_CASE("scalar instructions decode with their literals and inline constants") {
    // s_mov_b32 s0, 0x12345678: SOP1 with a trailing literal.
    const r::Instruction mov = one({0xBE8003FF, 0x12345678, 0xBF810000});
    CHECK(mov.encoding == r::Encoding::kSop1);
    CHECK_EQ(mov.dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(mov), std::string("s_mov_b32"));
    REQUIRE(mov.dst_count == 1);
    REQUIRE(mov.src_count == 1);
    CHECK(mov.dst[0] == sgpr(0));
    CHECK(mov.src[0] == literal(0x12345678));

    // s_add_u32 s0, -1, 0.5: SOP2 with two inline constants and no literal.
    const r::Instruction add = one({0x8000F0C1});
    CHECK(add.encoding == r::Encoding::kSop2);
    CHECK_EQ(add.dwords, uint8_t{1});
    CHECK_EQ(r::mnemonic(add), std::string("s_add_u32"));
    REQUIRE(add.src_count == 2);
    CHECK(add.src[0] == (r::Operand{r::OperandKind::kInlineInt, 0xFFFFFFFF}));
    CHECK(add.src[1] == (r::Operand{r::OperandKind::kInlineFloat, 0x3F000000}));
    CHECK_EQ(r::operand_text(add.src[0]), std::string("-1"));
    CHECK_EQ(r::operand_text(add.src[1]), std::string("0.5"));

    // s_setreg_imm32_b32 hwreg, 3: SOPK that always carries a literal.
    const r::Instruction setreg = one({0xBA800801, 0x00000003});
    CHECK(setreg.encoding == r::Encoding::kSopk);
    CHECK_EQ(setreg.dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(setreg), std::string("s_setreg_imm32_b32"));
    CHECK_EQ(setreg.imm, int64_t{0x0801});
    REQUIRE(setreg.src_count == 1);
    CHECK(setreg.src[0] == literal(3));

    // s_cbranch_scc0 -2: SOPP with a negative offset.
    const r::Instruction branch = one({0xBF84FFFE});
    CHECK(branch.encoding == r::Encoding::kSopp);
    CHECK_EQ(r::mnemonic(branch), std::string("s_cbranch_scc0"));
    CHECK_EQ(branch.imm, int64_t{-2});

    CHECK(r::classify(0xBF810000) == r::Encoding::kSopp);
    CHECK_EQ(one({0xBF810000}).opcode, r::kSoppEndpgm);
    CHECK_EQ(one({0xBF9F0000}).opcode, r::kSoppCodeEnd);
}

TEST_CASE("SMEM loads name their base pair and an SGPR offset unless it is null") {
    // s_load_dwordx4 s[4:7], s[0:1], 0x10 with SOFFSET null.
    const r::Instruction load = one({0xF4080100, 0xFA000010});
    CHECK(load.encoding == r::Encoding::kSmem);
    CHECK_EQ(load.dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(load), std::string("s_load_dwordx4"));
    CHECK_EQ(load.imm, int64_t{0x10});
    REQUIRE(load.dst_count == 1);
    REQUIRE(load.src_count == 1);
    CHECK(load.dst[0] == sgpr(4));
    CHECK(load.src[0] == sgpr(0));

    // The same load with s5 as SOFFSET.
    const r::Instruction offset = one({0xF4080100, 0x0A000010});
    REQUIRE(offset.src_count == 2);
    CHECK(offset.src[1] == sgpr(5));

    // The offset is signed: 0x1FFFF0 is -16, and 0x0FFFFF the largest positive offset.
    CHECK_EQ(one({0xF4080100, 0xFA1FFFF0}).imm, int64_t{-16});
    CHECK_EQ(one({0xF4080100, 0xFA0FFFFF}).imm, int64_t{0xFFFFF});
}

TEST_CASE("VOP1, VOP2 and VOPC decode their extra dwords") {
    // v_add_f32 v1, s2, v3
    const r::Instruction add = one({0x06020602});
    CHECK(add.encoding == r::Encoding::kVop2);
    CHECK_EQ(r::mnemonic(add), std::string("v_add_f32"));
    REQUIRE(add.dst_count == 1);
    REQUIRE(add.src_count == 2);
    CHECK(add.dst[0] == vgpr(1));
    CHECK(add.src[0] == sgpr(2));
    CHECK(add.src[1] == vgpr(3));

    // v_mul_f32 v0, 0x40400000, v1: a literal src0.
    const r::Instruction mul = one({0x100002FF, 0x40400000});
    CHECK_EQ(mul.dwords, uint8_t{2});
    CHECK(mul.src[0] == literal(0x40400000));

    // v_fmamk_f32 v0, v1, 0x40000000, v2: K always follows; it is the third source.
    const r::Instruction fmamk = one({0x58000501, 0x40000000});
    CHECK_EQ(fmamk.dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(fmamk), std::string("v_fmamk_f32"));
    REQUIRE(fmamk.src_count == 3);
    CHECK(fmamk.src[0] == vgpr(1));
    CHECK(fmamk.src[1] == vgpr(2));
    CHECK(fmamk.src[2] == literal(0x40000000));

    // v_mov_b32 v1, v2 with a DPP16 word (row_shr:1, all rows and banks).
    const r::Instruction dpp = one({0x7E0202FA, 0xFF111102});
    CHECK(dpp.encoding == r::Encoding::kVop1);
    CHECK_EQ(dpp.dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(dpp), std::string("v_mov_b32"));
    REQUIRE(dpp.dpp.has_value());
    CHECK_EQ(*dpp.dpp, uint32_t{0xFF111102});
    CHECK(dpp.src[0] == vgpr(2));

    // v_add_f32 v1, v2, s3 with an SDWA word that makes src1 a scalar.
    const r::Instruction sdwa = one({0x060206F9, 0x86060602});
    CHECK_EQ(sdwa.dwords, uint8_t{2});
    REQUIRE(sdwa.sdwa.has_value());
    CHECK(sdwa.src[0] == vgpr(2));
    CHECK(sdwa.src[1] == sgpr(3));

    // v_readfirstlane_b32 s4, v0 writes a scalar.
    const r::Instruction readfirstlane = one({0x7E080500});
    CHECK_EQ(r::mnemonic(readfirstlane), std::string("v_readfirstlane_b32"));
    CHECK(readfirstlane.dst[0] == sgpr(4));

    // v_cmp_lt_f32 vcc, v0, v1 and v_cmpx_eq_u32 v0, v1.
    const r::Instruction cmp = one({0x7C020300});
    CHECK(cmp.encoding == r::Encoding::kVopc);
    CHECK_EQ(r::mnemonic(cmp), std::string("v_cmp_lt_f32"));
    CHECK(cmp.dst[0] == kind(r::OperandKind::kVccLo));
    CHECK(cmp.src[0] == vgpr(0));
    CHECK(cmp.src[1] == vgpr(1));
    const r::Instruction cmpx = one({0x7DA40300});
    CHECK_EQ(r::mnemonic(cmpx), std::string("v_cmpx_eq_u32"));
    CHECK(cmpx.dst[0] == kind(r::OperandKind::kExecLo));
}

TEST_CASE("VOP3 and VOP3P decode modifiers, carry-outs and literals") {
    // v_fma_f32 v0, v1, v2, 1.0 as a literal.
    const r::Instruction fma = one({0xD54B0000, 0x03FE0501, 0x3F800000});
    CHECK(fma.encoding == r::Encoding::kVop3);
    CHECK_EQ(fma.dwords, uint8_t{3});
    CHECK_EQ(r::mnemonic(fma), std::string("v_fma_f32"));
    REQUIRE(fma.src_count == 3);
    CHECK(fma.src[0] == vgpr(1));
    CHECK(fma.src[1] == vgpr(2));
    CHECK(fma.src[2] == literal(0x3F800000));

    // v_add_f32 promoted to VOP3: v1, |v2|, -v3.
    const r::Instruction add = one({0xD5030101, 0x40020702});
    CHECK_EQ(add.dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(add), std::string("v_add_f32"));
    CHECK_EQ(add.mask, uint8_t{0x11});
    CHECK(add.dst[0] == vgpr(1));

    // v_add_co_u32 v0, vcc_lo, v1, v2: VOP3B with its carry-out.
    const r::Instruction carry = one({0xD70F6A00, 0x00020501});
    CHECK_EQ(r::mnemonic(carry), std::string("v_add_co_u32"));
    REQUIRE(carry.dst_count == 2);
    CHECK(carry.dst[0] == vgpr(0));
    CHECK(carry.dst[1] == kind(r::OperandKind::kVccLo));

    // v_cmp_lt_f32 s[4:5], v0, v1 in VOP3 writes the scalar mask.
    const r::Instruction cmp = one({0xD4010004, 0x00020300});
    CHECK_EQ(r::mnemonic(cmp), std::string("v_cmp_lt_f32"));
    CHECK(cmp.dst[0] == sgpr(4));

    // A literal follows whenever a source field selects it, even a field the opcode doesn't read.
    // v_nop has no sources, but with src0 set to the literal code it is two dwords long: console
    // shaders carry 32-bit words in front of their code that way.
    const r::Instruction nop = one({0x7E0000FF, 0x12345678, 0xBF810000});
    CHECK(nop.encoding == r::Encoding::kVop1);
    CHECK_EQ(r::mnemonic(nop), std::string("v_nop"));
    CHECK_EQ(nop.dwords, uint8_t{2});
    // v_add_f32 v0, v1, v2 in VOP3 reads two sources; src2 still selects a literal.
    const r::Instruction add3 = one({0xD5030000, 0x03FE0501, 0xCAFEF00D, 0xBF810000});
    CHECK_EQ(r::mnemonic(add3), std::string("v_add_f32"));
    CHECK_EQ(add3.dwords, uint8_t{3});
    const std::vector<r::Instruction> swept =
        r::decode(bytes({0xD5030000, 0x03FE0501, 0xCAFEF00D, 0xBF810000}));
    REQUIRE(swept.size() == 2);
    CHECK_EQ(r::mnemonic(swept[1]), std::string("s_endpgm"));

    // v_pk_fma_f16 v0, v1, v2, v3 with op_sel_hi set for all three sources.
    const r::Instruction pk = one({0xCC0E4000, 0x1C0E0501});
    CHECK(pk.encoding == r::Encoding::kVop3p);
    CHECK_EQ(pk.dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(pk), std::string("v_pk_fma_f16"));
    CHECK_EQ(pk.imm2, int64_t{7});
    CHECK(pk.src[2] == vgpr(3));
}

TEST_CASE("memory, interpolation and export instructions decode") {
    // image_sample v[0:3], v[4:5], s[8:15], s[16:19] dmask:0xf dim:2D with one NSA word (v5).
    const r::Instruction sample = one({0xF0800F0A, 0x00820004, 0x00000005});
    CHECK(sample.encoding == r::Encoding::kMimg);
    CHECK_EQ(sample.dwords, uint8_t{3});
    CHECK_EQ(r::mnemonic(sample), std::string("image_sample"));
    CHECK_EQ(sample.mask, uint8_t{0xF});
    CHECK_EQ(sample.imm, int64_t{1});
    REQUIRE(sample.src_count == 7);
    CHECK(sample.src[0] == vgpr(4));
    CHECK(sample.src[1] == vgpr(5));
    CHECK(sample.src[5] == sgpr(8));
    CHECK(sample.src[6] == sgpr(16));
    // image_sample_c_lz_o, by the naming pattern.
    CHECK_EQ(r::mnemonic(one({0xF0FC0F00, 0x00820004})), std::string("image_sample_c_lz_o"));
    // Gather4 has no derivative forms, so 0x42 and 0x5B have no name; 0x40 and 0x47 do.
    CHECK_EQ(r::mnemonic(one({0xF1000F00, 0x00820004})), std::string("image_gather4"));
    CHECK_EQ(r::mnemonic(one({0xF11C0F00, 0x00820004})), std::string("image_gather4_lz"));
    CHECK(!r::named(one({0xF1080F00, 0x00820004})));
    CHECK(!r::named(one({0xF16C0F00, 0x00820004})));
    CHECK(r::named(one({0xF0880F00, 0x00820004}))); // image_sample_d

    // global_load_dword v1, v[2:3], off
    const r::Instruction global = one({0xDC308000, 0x017D0002});
    CHECK(global.encoding == r::Encoding::kFlat);
    CHECK_EQ(r::mnemonic(global), std::string("global_load_dword"));
    CHECK(global.dst[0] == vgpr(1));
    CHECK(global.src[2] == kind(r::OperandKind::kNull));

    // ds_read_b32 v1, v2 offset:16
    const r::Instruction ds = one({0xD8D80010, 0x01000002});
    CHECK_EQ(r::mnemonic(ds), std::string("ds_read_b32"));
    CHECK_EQ(ds.imm, int64_t{16});
    CHECK(ds.dst[0] == vgpr(1));
    CHECK(ds.src[0] == vgpr(2));

    // buffer_load_dword v1, v2, s[4:7], 0 offen offset:4
    const r::Instruction buffer = one({0xE0301004, 0x80010102});
    CHECK_EQ(r::mnemonic(buffer), std::string("buffer_load_dword"));
    CHECK_EQ(buffer.imm, int64_t{4});
    CHECK_EQ(buffer.mask, uint8_t{1});
    CHECK(buffer.src[1] == sgpr(4));
    CHECK(buffer.src[2] == (r::Operand{r::OperandKind::kInlineInt, 0}));

    // tbuffer_load_format_d16_x: opcode bit 3 sits in the second dword.
    const r::Instruction tbuffer = one({0xE8B00000, 0x80210102});
    CHECK(tbuffer.encoding == r::Encoding::kMtbuf);
    CHECK_EQ(r::mnemonic(tbuffer), std::string("tbuffer_load_format_d16_x"));
    CHECK_EQ(tbuffer.imm2, int64_t{0x16});

    // v_interp_p1_f32 v2, v0, attr1.y
    const r::Instruction interp = one({0xC8080500});
    CHECK(interp.encoding == r::Encoding::kVinterp);
    CHECK_EQ(r::mnemonic(interp), std::string("v_interp_p1_f32"));
    CHECK_EQ(interp.imm, int64_t{1});
    CHECK_EQ(interp.imm2, int64_t{1});
    CHECK(interp.dst[0] == vgpr(2));
    CHECK_EQ(r::text(interp), std::string("v_interp_p1_f32 v2, v0, attr1.y"));
    const std::optional<r::InterpAttribute> interp_attr = r::interp_attribute(interp);
    REQUIRE(interp_attr.has_value());
    CHECK_EQ(interp_attr->attribute, uint8_t{1});
    CHECK_EQ(interp_attr->channel, uint8_t{1});
    CHECK(!interp_attr->high);

    // exp mrt0 v0, v1, v2, v3 done vm
    const r::Instruction exp = one({0xF800180F, 0x03020100});
    CHECK(exp.encoding == r::Encoding::kExport);
    CHECK_EQ(exp.mask, uint8_t{0xF});
    CHECK_EQ(exp.imm, int64_t{0});
    CHECK_EQ(exp.imm2, int64_t{6});
    REQUIRE(exp.src_count == 4);
    CHECK(exp.src[3] == vgpr(3));
}

TEST_CASE("VOP3 interpolations keep their attribute, channel and high bit in src0") {
    // v_interp_p2_f16 v3, attr2.z high, v1, v4: src0 holds 2 | 2 << 6 | 1 << 8.
    const r::Instruction p2 = one({0xD75A0003, 0x04120382});
    CHECK(p2.encoding == r::Encoding::kVop3);
    CHECK_EQ(r::mnemonic(p2), std::string("v_interp_p2_f16"));
    REQUIRE(p2.src_count == 3);
    CHECK(p2.src[0].kind == r::OperandKind::kAttribute);
    CHECK(p2.src[1] == vgpr(1));
    CHECK(p2.src[2] == vgpr(4));
    CHECK_EQ(r::text(p2), std::string("v_interp_p2_f16 v3, attr2.z high, v1, v4"));
    const std::optional<r::InterpAttribute> high = r::interp_attribute(p2);
    REQUIRE(high.has_value());
    CHECK_EQ(high->attribute, uint8_t{2});
    CHECK_EQ(high->channel, uint8_t{2});
    CHECK(high->high);

    // v_interp_p1ll_f16 v5, attr0.w, v0: the low half. It has no third source, so the zero
    // in the src2 field isn't a read of s0.
    const r::Instruction p1ll = one({0xD7420005, 0x000200C0});
    CHECK_EQ(r::mnemonic(p1ll), std::string("v_interp_p1ll_f16"));
    CHECK_EQ(p1ll.src_count, uint8_t{2});
    CHECK_EQ(r::text(p1ll), std::string("v_interp_p1ll_f16 v5, attr0.w, v0"));
    const std::optional<r::InterpAttribute> low = r::interp_attribute(p1ll);
    REQUIRE(low.has_value());
    CHECK_EQ(low->attribute, uint8_t{0});
    CHECK_EQ(low->channel, uint8_t{3});
    CHECK(!low->high);

    // v_interp_p1lv_f16 v2, attr1.x, v0, v3 takes P0's half from its third source.
    const r::Instruction p1lv = one({0xD7430002, 0x040E0001});
    CHECK_EQ(p1lv.src_count, uint8_t{3});
    CHECK(p1lv.src[2] == vgpr(3));

    // An attribute field of 255 (attr63.w) isn't the literal code: no literal follows. The src1
    // field of v_interp_mov_f32 picks P10, P20 or P0.
    const std::vector<r::Instruction> swept =
        r::decode(bytes({0xD6020000, 0x000000FF, 0xBF810000}));
    REQUIRE(swept.size() == 2);
    CHECK_EQ(swept[0].dwords, uint8_t{2});
    CHECK_EQ(r::mnemonic(swept[0]), std::string("v_interp_mov_f32"));
    CHECK_EQ(r::text(swept[0]), std::string("v_interp_mov_f32 v0, attr63.w, p10"));
    CHECK_EQ(r::text(one({0xD6020000, 0x00000400})),
             std::string("v_interp_mov_f32 v0, attr0.x, p0"));
    CHECK_EQ(r::mnemonic(swept[1]), std::string("s_endpgm"));
    // The VINTRP form names the parameter in its only source.
    CHECK_EQ(r::text(one({0xC8020002})), std::string("v_interp_mov_f32 v0, p0, attr0.x"));

    // Other VOP3 instructions aren't interpolations.
    CHECK(!r::interp_attribute(one({0xD5030000, 0x00020501})).has_value());
}

TEST_CASE("cut-off and unknown instructions are marked, not guessed") {
    const r::Instruction short_vop3 = one({0xD54B0000});
    CHECK(short_vop3.truncated);
    CHECK_EQ(short_vop3.dwords, uint8_t{1});
    CHECK_EQ(short_vop3.src_count, uint8_t{0});

    const r::Instruction no_literal = one({0xD54B0000, 0x03FE0501});
    CHECK(no_literal.truncated);
    CHECK_EQ(no_literal.dwords, uint8_t{2});

    const r::Instruction unknown = one({0xFC000000});
    CHECK(unknown.encoding == r::Encoding::kUnknown);
    CHECK_EQ(unknown.dwords, uint8_t{1});
    CHECK(!r::named(unknown));
    CHECK_EQ(r::mnemonic(unknown), std::string("unknown<0x0>"));
    CHECK_EQ(r::text(unknown), std::string("unknown<0x0> fc000000"));
}

TEST_CASE("the opcode tables name promoted and split compare opcodes") {
    CHECK(r::opcode_name(r::Encoding::kVop3, 0x182) == std::string_view("v_readfirstlane_b32"));
    CHECK(r::opcode_name(r::Encoding::kVop3, 0x0C8) == std::string_view("v_cmp_f_f16"));
    CHECK(r::opcode_name(r::Encoding::kVopc, 0xE8) == std::string_view("v_cmp_u_f16"));
    CHECK(r::opcode_name(r::Encoding::kVopc, 0xFF) == std::string_view("v_cmpx_tru_f16"));
    CHECK(r::opcode_name(r::Encoding::kVopc, 0x8F) == std::string_view("v_cmp_class_f16"));
    CHECK(r::opcode_name(r::Encoding::kVopc, 0x99) == std::string_view("v_cmpx_lt_i16"));
    CHECK(!r::opcode_name(r::Encoding::kVopc, 0xAF).has_value());
    CHECK(r::opcode_name(r::Encoding::kVop3, 0x16D) == std::string_view("v_div_scale_f32"));
    // VOP2-only and VOP1-only opcodes have no VOP3 form.
    for (const uint16_t opcode : {0x120, 0x121, 0x12C, 0x12D, 0x137, 0x138, 0x13C, 0x1E5}) {
        CHECK(!r::opcode_name(r::Encoding::kVop3, opcode).has_value());
    }
    CHECK(r::opcode_name(r::Encoding::kVop2, 0x3C) == std::string_view("v_pk_fmac_f16"));
    CHECK(r::opcode_name(r::Encoding::kVop1, 0x65) == std::string_view("v_swap_b32"));
    CHECK(r::opcode_name(r::Encoding::kVop3, 0x12B) == std::string_view("v_fmac_f32"));
    CHECK(r::opcode_name(r::Encoding::kDs, 0xA1) == std::string_view("ds_write_b16_d16_hi"));
    CHECK(r::opcode_name(r::Encoding::kDs, 0xA7) == std::string_view("ds_read_u16_d16_hi"));
    CHECK(r::opcode_name(r::Encoding::kExport, 0) == std::string_view("exp"));
}

TEST_CASE("a sweep decodes every word in order") {
    // s_mov_b32 s0, lit; v_add_f32 v1, s2, v3; s_endpgm; s_code_end; s_code_end
    const Words program = {0xBE8003FF, 0x12345678, 0x06020602, 0xBF810000, 0xBF9F0000, 0xBF9F0000};
    const std::vector<r::Instruction> insns = r::decode(bytes(program));
    REQUIRE(insns.size() == 5);
    CHECK_EQ(insns[0].offset, size_t{0});
    CHECK_EQ(insns[1].offset, size_t{8});
    CHECK_EQ(insns[2].offset, size_t{12});
    CHECK_EQ(insns[4].offset, size_t{20});
    CHECK_EQ(r::text(insns[1]), std::string("v_add_f32 v1, s2, v3"));
}

TEST_CASE("decode_program follows branches and leaves the data after s_endpgm alone") {
    const Words words = {
        0xBF840002, // 0: s_cbranch_scc0 to 3
        0xBE800380, // 1: s_mov_b32 s0, 0
        0xBF820001, // 2: s_branch to 4
        0xBE800381, // 3: s_mov_b32 s0, 1
        0xBF810000, // 4: s_endpgm
        0x3F800000, // 5: a constant table
        0x00000000, // 6
        0xBF9F0000, // 7: s_code_end padding
        0xBF9F0000, // 8
    };
    const r::Program program = r::decode_program(bytes(words));
    REQUIRE(program.instructions.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        CHECK_EQ(program.instructions[i].offset, 4 * i);
    }
    CHECK_EQ(r::mnemonic(program.instructions[0]), std::string("s_cbranch_scc0"));
    CHECK_EQ(program.instructions[0].imm, int64_t{2});
    CHECK_EQ(program.unreached_bytes, size_t{8});
    CHECK(!program.falls_off);
    CHECK(!program.bad_branch);
    CHECK(!program.indirect);

    r::DecodeStats stats;
    stats.add(program);
    CHECK_EQ(stats.programs, size_t{1});
    CHECK_EQ(stats.instructions, size_t{5});
    CHECK_EQ(stats.unknown, size_t{0});
    CHECK_EQ(stats.truncated, size_t{0});
    CHECK_EQ(stats.falls_off, size_t{0});
    CHECK_EQ(stats.bad_branch, size_t{0});
    CHECK_EQ(stats.with_data, size_t{1});
    CHECK_EQ(stats.data_bytes, size_t{8});
    CHECK(stats.unnamed.empty());
    CHECK_EQ(stats.encodings.at(r::Encoding::kSopp), size_t{3});
}

TEST_CASE("decode_program flags paths off the end, bad branches and indirect jumps") {
    // A path with no s_endpgm runs off the end, or into the padding.
    CHECK(r::decode_program(bytes({0xBE800380})).falls_off);
    CHECK(r::decode_program(bytes({0xBE800380, 0xBF9F0000})).falls_off);
    CHECK(!r::decode_program(bytes({0xBF810000})).falls_off);

    // s_branch -16 leaves the code.
    CHECK(r::decode_program(bytes({0xBF82FFF0})).bad_branch);

    // s_cbranch_scc0 lands on the literal of the s_mov_b32 after it.
    const r::Program into =
        r::decode_program(bytes({0xBF840001, 0xBE8003FF, 0x12345678, 0xBF810000}));
    CHECK(into.bad_branch);
    CHECK_EQ(into.instructions.size(), size_t{3});

    // A VOP3 cut off by the end of the code.
    const r::Program cut = r::decode_program(bytes({0xD54B0000}));
    CHECK(cut.falls_off);
    r::DecodeStats stats;
    stats.add(cut);
    CHECK_EQ(stats.truncated, size_t{1});

    // s_setpc_b64 s[0:1] ends the path at a target the decoder doesn't follow.
    const r::Program setpc = r::decode_program(bytes({0xBE802000, 0x3F800000}));
    CHECK(setpc.indirect);
    CHECK(!setpc.falls_off);
    CHECK_EQ(setpc.unreached_bytes, size_t{4});
}

TEST_CASE("decode_program follows far jumps built from s_getpc_b64 and a constant") {
    // Forward: the address after s_getpc_b64 is 4, and 4 + 20 lands on the s_endpgm at 24.
    const r::Program forward = r::decode_program(bytes({
        0xBE841F00, // 0: s_getpc_b64 s[4:5]
        0x8004FF04, // 4: s_add_u32 s4, s4, a literal
        0x00000014, // 8: the literal, 20
        0x82058005, // 12: s_addc_u32 s5, s5, 0
        0xBE802004, // 16: s_setpc_b64 s[4:5]
        0x3F800000, // 20: data
        0xBF810000, // 24: s_endpgm
    }));
    REQUIRE(forward.instructions.size() == 5);
    const r::Instruction& jump = forward.instructions[3];
    CHECK_EQ(jump.offset, size_t{16});
    CHECK_EQ(jump.far_target, std::optional<int64_t>{24});
    const r::Flow flow = r::flow_of(jump);
    CHECK(!flow.falls_through);
    CHECK_EQ(flow.target, std::optional<int64_t>{24});
    CHECK(!flow.indirect);
    CHECK(r::text(jump).ends_with(" ; far target 0x18"));
    CHECK(!forward.indirect);
    CHECK(!forward.bad_branch);
    CHECK_EQ(forward.unreached_bytes, size_t{4});
    r::DecodeStats stats;
    stats.add(forward);
    CHECK_EQ(stats.indirect, size_t{0});
    CHECK_EQ(stats.far_jumps, size_t{1});
    CHECK_EQ(stats.far_jump_programs, size_t{1});

    // Backward, with s_sub_u32 and s_subb_u32: 20 - 16 lands on the s_endpgm at 4.
    const r::Program backward = r::decode_program(bytes({
        0xBF820003, // 0: s_branch to 16
        0xBF810000, // 4: s_endpgm
        0x3F800000, // 8: data
        0x00000000, // 12: data
        0xBE861F00, // 16: s_getpc_b64 s[6:7]
        0x80869006, // 20: s_sub_u32 s6, s6, 16
        0x82878007, // 24: s_subb_u32 s7, s7, 0
        0xBE802006, // 28: s_setpc_b64 s[6:7]
    }));
    CHECK_EQ(backward.instructions.size(), size_t{6});
    CHECK_EQ(backward.instructions.back().far_target, std::optional<int64_t>{4});
    CHECK(!backward.indirect);
    CHECK(!backward.falls_off);
    CHECK_EQ(backward.unreached_bytes, size_t{8});

    // A far call: the callee at 24 is followed, and its return through s[30:31] stays indirect.
    const r::Program call = r::decode_program(bytes({
        0xBE841F00, // 0: s_getpc_b64 s[4:5]
        0x8004FF04, // 4: s_add_u32 s4, s4, a literal
        0x00000014, // 8: the literal, 20
        0x82058005, // 12: s_addc_u32 s5, s5, 0
        0xBE9E2104, // 16: s_swappc_b64 s[30:31], s[4:5]
        0xBF810000, // 20: s_endpgm
        0xBE80201E, // 24: s_setpc_b64 s[30:31]
    }));
    REQUIRE(call.instructions.size() == 6);
    CHECK_EQ(call.instructions[3].far_target, std::optional<int64_t>{24});
    CHECK(r::flow_of(call.instructions[3]).falls_through);
    CHECK(!call.instructions[5].far_target);
    CHECK(call.indirect);
    CHECK_EQ(call.unreached_bytes, size_t{0});

    // Anything inside the run, here an s_nop between the two adds, leaves the jump indirect: the
    // carry the second add takes is no longer known to come from the first.
    const r::Program broken = r::decode_program(bytes({
        0xBE841F00, // 0: s_getpc_b64 s[4:5]
        0x8004FF04, // 4: s_add_u32 s4, s4, a literal
        0x00000018, // 8: the literal, 24
        0xBF800000, // 12: s_nop 0
        0x82058005, // 16: s_addc_u32 s5, s5, 0
        0xBE802004, // 20: s_setpc_b64 s[4:5]
        0xBF810000, // 24: s_endpgm
    }));
    CHECK(broken.indirect);
    CHECK(!broken.instructions.back().far_target);
    CHECK_EQ(broken.unreached_bytes, size_t{4});

    // So does adding a register instead of a constant.
    const r::Program from_register = r::decode_program(bytes({
        0xBE841F00, // 0: s_getpc_b64 s[4:5]
        0x80040804, // 4: s_add_u32 s4, s4, s8
        0x82058005, // 8: s_addc_u32 s5, s5, 0
        0xBE802004, // 12: s_setpc_b64 s[4:5]
    }));
    CHECK(from_register.indirect);
}

TEST_CASE("DecodeStats counts GDS, GWS, and append and consume counters") {
    const r::Program program = r::decode_program(bytes({
        0xD8F80000, // 0: ds_append v1, on LDS
        0x01000000,
        0xD8660000, // 8: ds_gws_init v0 gds
        0x00000000,
        0xD8020000, // 16: ds_add_u32 v0, v1 gds
        0x00000100,
        0xD8000000, // 24: ds_add_u32 v0, v1, on LDS
        0x00000100,
        0xBF810000, // 32: s_endpgm
    }));
    REQUIRE(program.instructions.size() == 5);
    CHECK_EQ(r::mnemonic(program.instructions[0]), "ds_append");
    CHECK(!r::uses_gds(program.instructions[0]));
    CHECK_EQ(r::mnemonic(program.instructions[1]), "ds_gws_init");
    CHECK(r::uses_gds(program.instructions[1]));
    CHECK(r::uses_gds(program.instructions[2]));
    CHECK(!r::uses_gds(program.instructions[3]));
    r::DecodeStats stats;
    stats.add(program);
    stats.add(r::decode_program(bytes({0xBF810000})));
    CHECK_EQ(stats.gds, size_t{2});
    CHECK_EQ(stats.gds_programs, size_t{1});
    CHECK_EQ(stats.append_consume, size_t{1});
    CHECK_EQ(stats.append_consume_programs, size_t{1});
}
