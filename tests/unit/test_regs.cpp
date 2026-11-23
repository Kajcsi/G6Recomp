// shaderrecomp's register model (tools/shaderrecomp/src/regs.h) on instructions encoded here by
// hand from the field layouts in AMD's public "RDNA 2 Instruction Set Architecture" reference.
// None of them comes from a game program.

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "g6test.h"
#include "rdna2.h"
#include "regs.h"

namespace r = g6::shader::rdna2;

namespace {

using Words = std::vector<uint32_t>;

r::Instruction one(const Words& words) {
    return r::decode_one(std::span(words));
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

// "s4:2 v0:1": each range as its file, first register and count, in the order listed.
std::string text(std::span<const r::RegisterRange> ranges) {
    std::string out;
    for (const r::RegisterRange& range : ranges) {
        if (!out.empty()) {
            out += ' ';
        }
        out += range.file == r::RegisterFile::kScalar ? 's' : 'v';
        out += std::to_string(range.first) + ':' + std::to_string(range.count);
    }
    return out;
}

std::string reads(const Words& words, unsigned wave = 64) {
    return text(r::footprint(one(words), wave).reads());
}
std::string writes(const Words& words, unsigned wave = 64) {
    return text(r::footprint(one(words), wave).writes());
}

// "0 1 5": the registers a set holds.
template <size_t N>
std::string text(const std::bitset<N>& set) {
    std::string out;
    for (size_t i = 0; i < N; ++i) {
        if (set[i]) {
            out += (out.empty() ? "" : " ") + std::to_string(i);
        }
    }
    return out;
}

r::LiveIns inputs(const Words& words, unsigned wave = 64) {
    return r::live_ins(r::decode_program(bytes(words)), wave);
}

} // namespace

TEST_CASE("scalar footprints take their widths from the opcode name") {
    // s_mov_b64 s[2:3], s[4:5]
    CHECK_EQ(r::mnemonic(one({0xBE820404})), std::string("s_mov_b64"));
    CHECK_EQ(reads({0xBE820404}), std::string("s4:2"));
    CHECK_EQ(writes({0xBE820404}), std::string("s2:2"));

    // s_lshl_b64 s[0:1], s[2:3], 4: the shift count is 32-bit, and an inline constant anyway.
    CHECK_EQ(r::mnemonic(one({0x8F808402})), std::string("s_lshl_b64"));
    CHECK_EQ(reads({0x8F808402}), std::string("s2:2"));
    CHECK_EQ(writes({0x8F808402}), std::string("s0:2"));

    // s_and_saveexec_b64 s[0:1], vcc also reads and writes exec.
    CHECK_EQ(reads({0xBE80246A}), std::string("s106:2 s126:2"));
    CHECK_EQ(writes({0xBE80246A}), std::string("s0:2 s126:2"));

    // s_load_dwordx4 s[4:7], s[0:1], 0x10
    CHECK_EQ(reads({0xF4080100, 0xFA000010}), std::string("s0:2"));
    CHECK_EQ(writes({0xF4080100, 0xFA000010}), std::string("s4:4"));

    // s_buffer_load_dwordx2 s[4:5], s[8:11], s5: a four-register descriptor and an SGPR offset.
    CHECK_EQ(r::mnemonic(one({0xF4240104, 0x0A000000})), std::string("s_buffer_load_dwordx2"));
    CHECK_EQ(reads({0xF4240104, 0x0A000000}), std::string("s8:4 s5:1"));
    CHECK_EQ(writes({0xF4240104, 0x0A000000}), std::string("s4:2"));

    // s_swappc_b64 s[30:31], s[2:3] is a call.
    const r::Footprint call = r::footprint(one({0xBE9E2102}), 64);
    CHECK(call.modelled);
    CHECK(call.call);
    CHECK_EQ(text(call.reads()), std::string("s2:2"));
    CHECK_EQ(text(call.writes()), std::string("s30:2"));

    // s_cbranch_vccnz reads vcc, one register or two by the wave size.
    CHECK_EQ(r::mnemonic(one({0xBF870001})), std::string("s_cbranch_vccnz"));
    CHECK_EQ(reads({0xBF870001}, 32), std::string("s106:1"));
    CHECK_EQ(reads({0xBF870001}, 64), std::string("s106:2"));
}

TEST_CASE("vector footprints: 64-bit types, lane masks and accumulators") {
    // v_add_f64 v[0:1], v[2:3], v[4:5]
    CHECK_EQ(r::mnemonic(one({0xD5640000, 0x00020902})), std::string("v_add_f64"));
    CHECK_EQ(reads({0xD5640000, 0x00020902}), std::string("v2:2 v4:2"));
    CHECK_EQ(writes({0xD5640000, 0x00020902}), std::string("v0:2"));

    // v_cvt_f64_i32 v[0:1], v2: the destination's type, then the source's.
    CHECK_EQ(reads({0x7E000902}), std::string("v2:1"));
    CHECK_EQ(writes({0x7E000902}), std::string("v0:2"));

    // v_cndmask_b32 v0, v1, v2, vcc
    CHECK_EQ(r::mnemonic(one({0x02000501})), std::string("v_cndmask_b32"));
    CHECK_EQ(reads({0x02000501}, 32), std::string("v1:1 v2:1 s106:1"));
    CHECK_EQ(reads({0x02000501}, 64), std::string("v1:1 v2:1 s106:2"));

    // v_fmac_f32 v0, v1, v2 accumulates into v0.
    CHECK_EQ(r::mnemonic(one({0x56000501})), std::string("v_fmac_f32"));
    CHECK_EQ(reads({0x56000501}), std::string("v1:1 v2:1 v0:1"));
    CHECK_EQ(writes({0x56000501}), std::string("v0:1"));

    // v_add_co_u32 v0, vcc, v1, v2: the carry-out is a lane mask.
    CHECK_EQ(writes({0xD70F6A00, 0x00020501}, 64), std::string("v0:1 s106:2"));
    CHECK_EQ(writes({0xD70F6A00, 0x00020501}, 32), std::string("v0:1 s106:1"));

    // v_cmp_lt_f32 vcc, v0, v1
    CHECK_EQ(reads({0x7C020300}), std::string("v0:1 v1:1"));
    CHECK_EQ(writes({0x7C020300}, 64), std::string("s106:2"));

    // v_lshlrev_b64 v[0:1], 2, v[2:3]: a 32-bit shift count and a 64-bit value.
    CHECK_EQ(r::mnemonic(one({0xD6FF0000, 0x00020482})), std::string("v_lshlrev_b64"));
    CHECK_EQ(reads({0xD6FF0000, 0x00020482}), std::string("v2:2"));
    CHECK_EQ(writes({0xD6FF0000, 0x00020482}), std::string("v0:2"));

    // v_mad_u64_u32 v[0:1], s[4:5], v2, v3, v[4:5]
    CHECK_EQ(r::mnemonic(one({0xD5760400, 0x04120702})), std::string("v_mad_u64_u32"));
    CHECK_EQ(reads({0xD5760400, 0x04120702}), std::string("v2:1 v3:1 v4:2"));
    CHECK_EQ(writes({0xD5760400, 0x04120702}), std::string("v0:2 s4:2"));

    // v_readlane_b32 s0, v1, s2
    CHECK_EQ(r::mnemonic(one({0xD7600000, 0x00000501})), std::string("v_readlane_b32"));
    CHECK_EQ(reads({0xD7600000, 0x00000501}), std::string("v1:1 s2:1"));
    CHECK_EQ(writes({0xD7600000, 0x00000501}), std::string("s0:1"));
}

TEST_CASE("memory footprints count address, descriptor and data registers") {
    // buffer_load_dword v1, v2, s[4:7], 0 offen
    CHECK_EQ(reads({0xE0301004, 0x80010102}), std::string("v2:1 s4:4"));
    CHECK_EQ(writes({0xE0301004, 0x80010102}), std::string("v1:1"));

    // buffer_load_dwordx4 v[4:7], v[2:3], s[8:11], s1 idxen offen, then with tfe.
    CHECK_EQ(r::mnemonic(one({0xE0383000, 0x01020402})), std::string("buffer_load_dwordx4"));
    CHECK_EQ(reads({0xE0383000, 0x01020402}), std::string("v2:2 s8:4 s1:1"));
    CHECK_EQ(writes({0xE0383000, 0x01020402}), std::string("v4:4"));
    CHECK_EQ(writes({0xE0383000, 0x01820402}), std::string("v4:5"));

    // global_load_dword v1, v[2:3], off; global_load_dwordx2 v[0:1], v2, s[4:5]
    CHECK_EQ(reads({0xDC308000, 0x017D0002}), std::string("v2:2"));
    CHECK_EQ(writes({0xDC308000, 0x017D0002}), std::string("v1:1"));
    CHECK_EQ(r::mnemonic(one({0xDC348000, 0x00040002})), std::string("global_load_dwordx2"));
    CHECK_EQ(reads({0xDC348000, 0x00040002}), std::string("s4:2 v2:1"));
    CHECK_EQ(writes({0xDC348000, 0x00040002}), std::string("v0:2"));

    // A d16 load writes half of its VGPR and keeps the other half, so it reads the register too:
    // buffer_load_short_d16_hi v1, v2, s[4:7], 0 offen; global_load_ubyte_d16 v1, v[2:3], off;
    // ds_read_u16_d16_hi v4, v1. buffer_store_byte_d16_hi v1, v2, s[4:7], 0 offen only reads.
    CHECK_EQ(r::mnemonic(one({0xE0941004, 0x80010102})), std::string("buffer_load_short_d16_hi"));
    CHECK_EQ(reads({0xE0941004, 0x80010102}), std::string("v2:1 s4:4 v1:1"));
    CHECK_EQ(writes({0xE0941004, 0x80010102}), std::string("v1:1"));
    CHECK_EQ(r::mnemonic(one({0xDC808000, 0x017D0002})), std::string("global_load_ubyte_d16"));
    CHECK_EQ(reads({0xDC808000, 0x017D0002}), std::string("v2:2 v1:1"));
    CHECK_EQ(writes({0xDC808000, 0x017D0002}), std::string("v1:1"));
    CHECK_EQ(r::mnemonic(one({0xDA9C0000, 0x04000001})), std::string("ds_read_u16_d16_hi"));
    CHECK_EQ(reads({0xDA9C0000, 0x04000001}), std::string("v1:1 v4:1"));
    CHECK_EQ(writes({0xDA9C0000, 0x04000001}), std::string("v4:1"));
    CHECK_EQ(r::mnemonic(one({0xE0641004, 0x80010102})), std::string("buffer_store_byte_d16_hi"));
    CHECK_EQ(reads({0xE0641004, 0x80010102}), std::string("v2:1 s4:4 v1:1"));
    CHECK_EQ(writes({0xE0641004, 0x80010102}), std::string(""));
    // The packing d16 format loads write whole registers: tbuffer_load_format_d16_x reads only
    // its address and descriptor.
    CHECK_EQ(r::mnemonic(one({0xE8001004, 0x80210102})), std::string("tbuffer_load_format_d16_x"));
    CHECK_EQ(reads({0xE8001004, 0x80210102}), std::string("v2:1 s4:4"));
    CHECK_EQ(writes({0xE8001004, 0x80210102}), std::string("v1:1"));

    // ds_write2_b64 v1, v[2:3], v[4:5]; ds_read2_b32 v[4:5], v1
    CHECK_EQ(r::mnemonic(one({0xD9380000, 0x00040201})), std::string("ds_write2_b64"));
    CHECK_EQ(reads({0xD9380000, 0x00040201}), std::string("v1:1 v2:2 v4:2"));
    CHECK_EQ(writes({0xD9380000, 0x00040201}), std::string(""));
    CHECK_EQ(r::mnemonic(one({0xD8DC0000, 0x04000001})), std::string("ds_read2_b32"));
    CHECK_EQ(reads({0xD8DC0000, 0x04000001}), std::string("v1:1"));
    CHECK_EQ(writes({0xD8DC0000, 0x04000001}), std::string("v4:2"));

    // exp mrt0 v0, v1 compr: one register per pair of channels. Uncompressed with channels 0
    // and 2 enabled, it reads v0 and v2.
    CHECK_EQ(reads({0xF800040F, 0x03020100}), std::string("v0:1 v1:1"));
    CHECK_EQ(reads({0xF8000005, 0x03020100}), std::string("v0:1 v2:1"));

    // v_interp_p2_f32 v2, v0, attr1.y adds to v2 and takes the attribute through m0.
    CHECK_EQ(r::mnemonic(one({0xC8090500})), std::string("v_interp_p2_f32"));
    CHECK_EQ(reads({0xC8090500}), std::string("s124:1 v0:1 v2:1"));
    CHECK_EQ(writes({0xC8090500}), std::string("v2:1"));
}

TEST_CASE("image footprints follow the dimension, the sample modifiers and A16") {
    // image_sample v[0:3], [v4, v5], s[8:15], s[16:19] dmask:0xf dim:2D, the second address in
    // an NSA word whose other three bytes are padding.
    const Words nsa = {0xF0800F0A, 0x00820004, 0x00000005};
    CHECK_EQ(r::image_address_count(one(nsa)), std::optional<unsigned>(2));
    CHECK_EQ(reads(nsa), std::string("v4:1 v5:1 s8:8 s16:4"));
    CHECK_EQ(writes(nsa), std::string("v0:4"));

    // image_sample_c_l_o v[0:3], v[4:8], 2D: offset, compare, s, t and the LOD. Under A16 the
    // offset and compare stay 32-bit and s, t and the LOD pack into two registers.
    const Words sample = {0xF0F00F08, 0x00820004};
    CHECK_EQ(r::mnemonic(one(sample)), std::string("image_sample_c_l_o"));
    CHECK_EQ(r::image_address_count(one(sample)), std::optional<unsigned>(5));
    CHECK_EQ(reads(sample), std::string("v4:5 s8:8 s16:4"));
    CHECK_EQ(r::image_address_count(one({0xF0F00F08, 0x40820004})), std::optional<unsigned>(4));

    // image_sample_d, 3D: three gradients each way and three coordinates.
    CHECK_EQ(r::mnemonic(one({0xF0880F10, 0x00820004})), std::string("image_sample_d"));
    CHECK_EQ(r::image_address_count(one({0xF0880F10, 0x00820004})), std::optional<unsigned>(9));
    CHECK_EQ(r::image_address_count(one({0xF0880F10, 0x40820004})), std::optional<unsigned>(6));

    // image_load_mip v[0:3], v[4:7], s[8:15] dim:2D array: s, t, the slice and the mip, and no
    // sampler.
    const Words load = {0xF0040F28, 0x00020004};
    CHECK_EQ(r::mnemonic(one(load)), std::string("image_load_mip"));
    CHECK_EQ(r::image_address_count(one(load)), std::optional<unsigned>(4));
    CHECK_EQ(reads(load), std::string("v4:4 s8:8"));
    CHECK_EQ(writes(load), std::string("v0:4"));

    // NSA with the wrong number of address words for the opcode isn't modelled: image_sample in
    // 2D takes two addresses, which don't need a second NSA word.
    CHECK(!r::footprint(one({0xF0800F0C, 0x00820004, 0x00000005, 0x00000000}), 64).modelled);
}

TEST_CASE("an opcode the model doesn't know lists nothing") {
    const r::Instruction unknown = one({0xD5C70000, 0x00000000});
    CHECK(!r::named(unknown));
    const r::Footprint f = r::footprint(unknown, 64);
    CHECK(!f.modelled);
    CHECK(f.reads().empty());
    CHECK(f.writes().empty());
}

TEST_CASE("wave_size reads the width the program gives exec and vcc") {
    // s_and_saveexec_b64 s[0:1], vcc; s_endpgm
    CHECK_EQ(r::wave_size(r::decode_program(bytes({0xBE80246A, 0xBF810000}))),
             std::optional<unsigned>(64));
    // s_mov_b32 exec_lo, s0; s_endpgm
    CHECK_EQ(r::wave_size(r::decode_program(bytes({0xBEFE0300, 0xBF810000}))),
             std::optional<unsigned>(32));
    // s_and_saveexec_b32 s0, vcc_lo; s_endpgm: exec is implicit, the suffix says 32.
    CHECK_EQ(r::wave_size(r::decode_program(bytes({0xBE803C6A, 0xBF810000}))),
             std::optional<unsigned>(32));
    // Both, or neither.
    CHECK(!r::wave_size(r::decode_program(bytes({0xBE80246A, 0xBEFE0300, 0xBF810000}))));
    CHECK(!r::wave_size(r::decode_program(bytes({0xBF810000}))));
}

TEST_CASE("live_ins finds the registers a program reads before writing them") {
    // s_load_dwordx4 s[4:7], s[0:1], 0x10; buffer_load_dword v1, v2, s[4:7], 0 offen; s_endpgm
    const r::LiveIns loads = inputs({0xF4080100, 0xFA000010, 0xE0301004, 0x80010102, 0xBF810000});
    CHECK_EQ(text(loads.scalar), std::string("0 1"));
    CHECK_EQ(text(loads.vector), std::string("2"));
    CHECK_EQ(loads.unmodelled, size_t{0});
    CHECK(!loads.call);

    // s_cbranch_scc0 over s_mov_b32 s0, 1 to s_add_u32 s1, s0, s2; s_endpgm. One path writes s0,
    // which is enough: only s2 counts.
    const r::LiveIns branch = inputs({0xBF840001, 0xBE800381, 0x80010200, 0xBF810000});
    CHECK_EQ(text(branch.scalar), std::string("2"));
    CHECK(branch.vector.none());

    // s_swappc_b64 s[30:31], s[2:3]; v_mov_b32 v0, v4; s_endpgm. s[2:3] is an input; v4 is
    // read after the call, so it is either an input or what the callee returns.
    const r::LiveIns call = inputs({0xBE9E2102, 0x7E000304, 0xBF810000});
    CHECK(call.call);
    CHECK_EQ(text(call.scalar), std::string("2 3"));
    CHECK(call.vector.none());
    CHECK_EQ(text(call.vector_after_call), std::string("4"));
    CHECK(call.scalar_after_call.none());

    // An unknown VOP3 opcode is counted, not guessed.
    CHECK_EQ(inputs({0xD5C70000, 0x00000000, 0xBF810000}).unmodelled, size_t{1});
}
