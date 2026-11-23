// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the TomBox developers (MIT; see NOTICES.md)
// Copyright (C) 2026 the xbox-emulator contributors
// Copyright (C) 2026 the G6Recomp contributors
//
// Ported to C++ from tombox-xshader's RDNA 2 decoder in xbox-emulator
// (https://github.com/omonkeyz/xbox-emulator, commit 51f5319, crates/tombox-xshader/src/rdna2.rs),
// licensed GPL-3.0-or-later. Most of that file comes from TomBox
// (https://github.com/drycheetah84/TomBox, commit 7e8064f), whose tombox-xshader crate is MIT OR
// Apache-2.0; that part is used here under MIT. rdna2.h lists the changes. Field layouts are
// those of AMD's public "RDNA 2 Instruction Set Architecture" reference.

#include "rdna2.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <optional>
#include <utility>

#include <fmt/format.h>

namespace g6::shader::rdna2 {

namespace {

constexpr uint32_t kSrcSdwa = 249;
constexpr uint32_t kSrcDpp8 = 233;
constexpr uint32_t kSrcDpp8Fi = 234;
constexpr uint32_t kSrcDpp16 = 250;
constexpr uint32_t kSrcLiteral = 255;
constexpr uint32_t kSgprNull = 125;
constexpr uint32_t kInvTwoPiBits = 0x3E22F983;              // 1/(2*pi) as a float
constexpr uint64_t kInvTwoPiBits64 = 0x3FC45F306DC9C882ULL; // 1/(2*pi) as a double

// Source codes that read hardware state; 253 (SCC) has an operand kind of its own.
std::optional<std::string_view> hardware_source_name(uint32_t code) {
    switch (code) {
    case 235: return "src_shared_base";
    case 236: return "src_shared_limit";
    case 237: return "src_private_base";
    case 238: return "src_private_limit";
    case 239: return "src_pops_exiting_wave_id";
    case 251: return "src_vccz";
    case 252: return "src_execz";
    case 254: return "src_lds_direct";
    default: return std::nullopt;
    }
}

constexpr uint32_t field(uint32_t word, unsigned low, unsigned count) {
    return (word >> low) & ((1U << count) - 1);
}

constexpr bool extra_dword_src0(uint32_t src0) {
    return src0 == kSrcLiteral || src0 == kSrcSdwa || src0 == kSrcDpp8 || src0 == kSrcDpp8Fi ||
           src0 == kSrcDpp16;
}

// VOP2 opcodes that always carry a 32-bit constant K after the instruction.
constexpr bool vop2_has_k(uint32_t opcode) {
    switch (opcode) {
    case 0x20: // v_madmk_f32
    case 0x21: // v_madak_f32
    case 0x2C: // v_fmamk_f32
    case 0x2D: // v_fmaak_f32
    case 0x37: // v_fmamk_f16
    case 0x38: // v_fmaak_f16
        return true;
    default: return false;
    }
}

// VOP3 opcodes in the VOP3B layout: bits 8-14 hold a scalar destination, the carry-out or
// the division scale flag, instead of abs and op_sel.
constexpr bool vop3b(uint32_t opcode) {
    switch (opcode) {
    case 0x128: // v_add_co_ci_u32
    case 0x129: // v_sub_co_ci_u32
    case 0x12A: // v_subrev_co_ci_u32
    case 0x16D: // v_div_scale_f32
    case 0x16E: // v_div_scale_f64
    case 0x176: // v_mad_u64_u32
    case 0x177: // v_mad_i64_i32
    case 0x30F: // v_add_co_u32
    case 0x310: // v_sub_co_u32
    case 0x319: // v_subrev_co_u32
        return true;
    default: return false;
    }
}

Operand op(OperandKind kind, uint32_t value = 0) {
    return {.kind = kind, .value = value};
}

// An 8-bit scalar operand code (SSRC, SDST, SOFFSET).
Operand scalar(uint32_t code, uint32_t literal) {
    if (code <= 105) {
        return op(OperandKind::kSgpr, code);
    }
    if (code == 128) {
        return op(OperandKind::kInlineInt, 0);
    }
    if (code >= 129 && code <= 192) {
        return op(OperandKind::kInlineInt, code - 128);
    }
    if (code >= 193 && code <= 208) {
        return op(OperandKind::kInlineInt, 0U - (code - 192));
    }
    switch (code) {
    case 106: return op(OperandKind::kVccLo);
    case 107: return op(OperandKind::kVccHi);
    case 124: return op(OperandKind::kM0);
    case kSgprNull: return op(OperandKind::kNull);
    case 126: return op(OperandKind::kExecLo);
    case 127: return op(OperandKind::kExecHi);
    case 240: return op(OperandKind::kInlineFloat, 0x3F000000); // 0.5
    case 241: return op(OperandKind::kInlineFloat, 0xBF000000); // -0.5
    case 242: return op(OperandKind::kInlineFloat, 0x3F800000); // 1.0
    case 243: return op(OperandKind::kInlineFloat, 0xBF800000); // -1.0
    case 244: return op(OperandKind::kInlineFloat, 0x40000000); // 2.0
    case 245: return op(OperandKind::kInlineFloat, 0xC0000000); // -2.0
    case 246: return op(OperandKind::kInlineFloat, 0x40800000); // 4.0
    case 247: return op(OperandKind::kInlineFloat, 0xC0800000); // -4.0
    case 248: return op(OperandKind::kInlineFloat, kInvTwoPiBits);
    case 253: return op(OperandKind::kScc);
    case kSrcLiteral: return op(OperandKind::kLiteral, literal);
    default: return op(OperandKind::kOther, code);
    }
}

// A 9-bit source code: 256 and up select a VGPR.
Operand source9(uint32_t code, uint32_t literal) {
    return code >= 256 ? op(OperandKind::kVgpr, code - 256) : scalar(code, literal);
}

Operand vgpr(uint32_t number) {
    return op(OperandKind::kVgpr, number);
}
Operand sgpr(uint32_t number) {
    return op(OperandKind::kSgpr, number);
}

uint16_t opcode_of(Encoding encoding, uint32_t w, uint32_t next) {
    switch (encoding) {
    case Encoding::kSop2: return field(w, 23, 7);
    case Encoding::kSopk: return field(w, 23, 5);
    case Encoding::kSop1: return field(w, 8, 8);
    case Encoding::kSopc:
    case Encoding::kSopp: return field(w, 16, 7);
    case Encoding::kSmem:
    case Encoding::kDs:
    case Encoding::kMubuf: return field(w, 18, 8);
    case Encoding::kVop2: return field(w, 25, 6);
    case Encoding::kVop1: return field(w, 9, 8);
    case Encoding::kVopc: return field(w, 17, 8);
    case Encoding::kVop3: return field(w, 16, 10);
    case Encoding::kVop3p: return field(w, 16, 7);
    case Encoding::kVinterp: return field(w, 16, 2);
    case Encoding::kMtbuf: return field(w, 16, 3) | (field(next, 21, 1) << 3);
    case Encoding::kMimg: return field(w, 18, 7) | ((w & 1) << 7);
    case Encoding::kFlat: return field(w, 18, 7);
    case Encoding::kExport:
    case Encoding::kUnknown: return 0;
    }
    return 0;
}

// Dwords before any trailing literal, and whether a literal or other extra dword follows.
// `next` is the second dword, or zero when there is none.
std::pair<size_t, bool> length_of(Encoding encoding, uint32_t w, uint32_t next) {
    switch (encoding) {
    case Encoding::kSop2:
    case Encoding::kSopc:
        return {1, field(w, 0, 8) == kSrcLiteral || field(w, 8, 8) == kSrcLiteral};
    case Encoding::kSop1: return {1, field(w, 0, 8) == kSrcLiteral};
    case Encoding::kSopk: return {1, field(w, 23, 5) == 0x15}; // s_setreg_imm32_b32
    case Encoding::kSopp:
    case Encoding::kVinterp:
    case Encoding::kUnknown: return {1, false};
    case Encoding::kVop2:
        return {1, extra_dword_src0(field(w, 0, 9)) || vop2_has_k(field(w, 25, 6))};
    case Encoding::kVop1:
    case Encoding::kVopc: return {1, extra_dword_src0(field(w, 0, 9))};
    case Encoding::kVop3:
        // An interpolation never takes a literal: its src0 field holds the attribute, and its
        // other sources are VGPRs or unused.
        return {2, !vop3_interp(field(w, 16, 10)) &&
                       (field(next, 0, 9) == kSrcLiteral || field(next, 9, 9) == kSrcLiteral ||
                        field(next, 18, 9) == kSrcLiteral)};
    case Encoding::kVop3p:
        return {2, field(next, 0, 9) == kSrcLiteral || field(next, 9, 9) == kSrcLiteral ||
                       field(next, 18, 9) == kSrcLiteral};
    case Encoding::kSmem:
    case Encoding::kDs:
    case Encoding::kMubuf:
    case Encoding::kMtbuf:
    case Encoding::kFlat:
    case Encoding::kExport: return {2, false};
    case Encoding::kMimg: return {2 + field(w, 1, 2), false}; // NSA: extra address dwords
    }
    return {1, false};
}

class Builder {
public:
    explicit Builder(Instruction& insn) : insn_(insn) {}
    void dst(Operand operand) { insn_.dst[insn_.dst_count++] = operand; }
    void src(Operand operand) { insn_.src[insn_.src_count++] = operand; }

private:
    Instruction& insn_;
};

// src0 of VOP1, VOP2 and VOPC, which may select an SDWA or DPP dword.
Operand vop_src0(Instruction& insn, uint32_t src0) {
    const uint32_t extra = insn.words[1];
    if (src0 == kSrcSdwa) {
        insn.sdwa = extra;
        return field(extra, 23, 1) != 0 ? scalar(field(extra, 0, 8), 0) : vgpr(field(extra, 0, 8));
    }
    if (src0 == kSrcDpp8 || src0 == kSrcDpp8Fi || src0 == kSrcDpp16) {
        insn.dpp = extra;
        return vgpr(field(extra, 0, 8));
    }
    return source9(src0, extra);
}

// vsrc1 of VOP2 and VOPC: an SDWA dword can turn it into a scalar.
Operand vop_src1(const Instruction& insn, uint32_t vsrc1) {
    if (insn.sdwa && field(*insn.sdwa, 31, 1) != 0) {
        return scalar(vsrc1, 0);
    }
    return vgpr(vsrc1);
}

void decode_operands(Instruction& insn) {
    const uint32_t w = insn.words[0];
    const uint32_t d1 = insn.words[1];
    Builder b(insn);
    switch (insn.encoding) {
    case Encoding::kSop2:
        b.dst(scalar(field(w, 16, 7), 0));
        b.src(scalar(field(w, 0, 8), d1));
        b.src(scalar(field(w, 8, 8), d1));
        break;
    case Encoding::kSop1:
        b.dst(scalar(field(w, 16, 7), 0));
        b.src(scalar(field(w, 0, 8), d1));
        break;
    case Encoding::kSopk:
        b.dst(scalar(field(w, 16, 7), 0));
        insn.imm = static_cast<int16_t>(field(w, 0, 16));
        if (insn.opcode == 0x15) { // s_setreg_imm32_b32
            b.src(op(OperandKind::kLiteral, d1));
        }
        break;
    case Encoding::kSopc:
        b.dst(op(OperandKind::kScc));
        b.src(scalar(field(w, 0, 8), d1));
        b.src(scalar(field(w, 8, 8), d1));
        break;
    case Encoding::kSopp: insn.imm = static_cast<int16_t>(field(w, 0, 16)); break;
    case Encoding::kSmem: {
        const uint32_t sdata = field(w, 6, 7);
        const uint32_t sbase = field(w, 0, 6) * 2; // an even SGPR pair, stored halved
        const uint32_t soffset = field(d1, 25, 7);
        // A signed 21-bit byte offset (LLVM decodes it so, and RDNA 2 hardware reads it so).
        insn.imm = static_cast<int64_t>(field(d1, 0, 21) ^ 0x100000U) - 0x100000;
        switch (insn.opcode) {
        case 0x1F: // s_gl1_inv
        case 0x20: // s_dcache_inv
            return;
        case 0x24: // s_memtime
        case 0x25: // s_memrealtime
            b.dst(scalar(sdata, 0));
            return;
        case 0x26: // s_atc_probe
        case 0x27: // s_atc_probe_buffer
            insn.imm2 = sdata;
            b.src(sgpr(sbase));
            return;
        default:
            b.dst(scalar(sdata, 0));
            b.src(sgpr(sbase));
            if (soffset != kSgprNull) {
                b.src(scalar(soffset, 0));
            }
            return;
        }
    }
    case Encoding::kVop2: {
        b.dst(vgpr(field(w, 17, 8)));
        b.src(vop_src0(insn, field(w, 0, 9)));
        b.src(vop_src1(insn, field(w, 9, 8)));
        if (vop2_has_k(insn.opcode)) {
            b.src(op(OperandKind::kLiteral, d1));
        }
        break;
    }
    case Encoding::kVop1: {
        const uint32_t vdst = field(w, 17, 8);
        // v_readfirstlane_b32 writes a scalar register.
        b.dst(insn.opcode == 0x02 ? scalar(vdst, 0) : vgpr(vdst));
        b.src(vop_src0(insn, field(w, 0, 9)));
        break;
    }
    case Encoding::kVopc: {
        const Operand src0 = vop_src0(insn, field(w, 0, 9));
        const bool cmpx = (insn.opcode & 0x10) != 0;
        if (cmpx) {
            b.dst(op(OperandKind::kExecLo));
        } else if (insn.sdwa && field(*insn.sdwa, 15, 1) != 0) {
            b.dst(scalar(field(*insn.sdwa, 8, 7), 0)); // SDWA's explicit scalar destination
        } else {
            b.dst(op(OperandKind::kVccLo));
        }
        b.src(src0);
        b.src(vop_src1(insn, field(w, 9, 8)));
        break;
    }
    case Encoding::kVop3: {
        const uint32_t vdst = field(w, 0, 8);
        const uint32_t literal = insn.dwords > 2 ? insn.words[2] : 0;
        const uint32_t neg = field(d1, 29, 3);
        insn.imm = field(w, 15, 1);
        insn.imm2 = field(d1, 27, 2);
        if (vop3b(insn.opcode)) {
            insn.mask = static_cast<uint8_t>(neg << 3);
            b.dst(vgpr(vdst));
            b.dst(scalar(field(w, 8, 7), 0));
        } else {
            insn.mask = static_cast<uint8_t>(field(w, 8, 3) | (neg << 3));
            insn.op_sel = static_cast<uint8_t>(field(w, 11, 4));
            if (insn.opcode < 0x100) { // a compare: v_cmpx writes EXEC, v_cmp a scalar mask
                b.dst((insn.opcode & 0x10) != 0 ? op(OperandKind::kExecLo) : scalar(vdst, 0));
            } else if (insn.opcode == 0x182 || insn.opcode == 0x360) {
                b.dst(scalar(vdst, 0)); // v_readfirstlane_b32, v_readlane_b32
            } else {
                b.dst(vgpr(vdst));
            }
        }
        if (vop3_interp(insn.opcode)) {
            // The attribute sits where src0 would, then the I or J register, or the parameter
            // v_interp_mov_f32 reads. Only p1lv and the f16 p2 take a third source.
            b.src(op(OperandKind::kAttribute, field(d1, 0, 9)));
            b.src(insn.opcode == kVop3InterpMovF32 ? op(OperandKind::kInterpParam, field(d1, 9, 9))
                                                   : source9(field(d1, 9, 9), literal));
            if (insn.opcode == kVop3InterpP1lvF16 || insn.opcode == kVop3InterpP2F16) {
                b.src(source9(field(d1, 18, 9), literal));
            }
            break;
        }
        b.src(source9(field(d1, 0, 9), literal));
        b.src(source9(field(d1, 9, 9), literal));
        b.src(source9(field(d1, 18, 9), literal));
        break;
    }
    case Encoding::kVop3p: {
        const uint32_t literal = insn.dwords > 2 ? insn.words[2] : 0;
        insn.mask = static_cast<uint8_t>(field(w, 8, 3) | (field(d1, 29, 3) << 3));
        insn.op_sel = static_cast<uint8_t>(field(w, 11, 3));
        insn.imm = field(w, 15, 1);
        insn.imm2 = field(d1, 27, 2) | (field(w, 14, 1) << 2);
        b.dst(vgpr(field(w, 0, 8)));
        b.src(source9(field(d1, 0, 9), literal));
        b.src(source9(field(d1, 9, 9), literal));
        b.src(source9(field(d1, 18, 9), literal));
        break;
    }
    case Encoding::kVinterp: {
        insn.imm = field(w, 10, 6);
        insn.imm2 = field(w, 8, 2);
        b.dst(vgpr(field(w, 18, 8)));
        // v_interp_mov_f32 has no I/J register: the field selects P10, P20 or P0.
        b.src(insn.opcode == 2 ? op(OperandKind::kInterpParam, field(w, 0, 8))
                               : vgpr(field(w, 0, 8)));
        break;
    }
    case Encoding::kDs:
        insn.imm = field(w, 0, 16);
        insn.mask = static_cast<uint8_t>(field(w, 17, 1)); // gds
        b.dst(vgpr(field(d1, 24, 8)));
        b.src(vgpr(field(d1, 0, 8)));
        b.src(vgpr(field(d1, 8, 8)));
        b.src(vgpr(field(d1, 16, 8)));
        break;
    case Encoding::kMubuf:
    case Encoding::kMtbuf:
        insn.imm = field(w, 0, 12);
        insn.mask =
            static_cast<uint8_t>(field(w, 12, 1) | (field(w, 13, 1) << 1) | (field(w, 14, 1) << 2));
        if (insn.encoding == Encoding::kMtbuf) {
            insn.imm2 = field(w, 19, 7);
        }
        b.dst(vgpr(field(d1, 8, 8))); // vdata, for stores too
        b.src(vgpr(field(d1, 0, 8)));
        b.src(sgpr(field(d1, 16, 5) * 4));
        b.src(scalar(field(d1, 24, 8), 0));
        break;
    case Encoding::kMimg: {
        const uint32_t nsa = field(w, 1, 2);
        insn.mask = static_cast<uint8_t>(field(w, 8, 4));
        insn.imm = field(w, 3, 3);
        insn.imm2 = nsa;
        b.dst(vgpr(field(d1, 8, 8)));
        b.src(vgpr(field(d1, 0, 8)));
        for (uint32_t k = 0; k < nsa; ++k) {
            const uint32_t address = insn.words[2 + k];
            for (unsigned byte = 0; byte < 4; ++byte) {
                b.src(vgpr(field(address, 8 * byte, 8)));
            }
        }
        b.src(sgpr(field(d1, 16, 5) * 4));
        b.src(sgpr(field(d1, 21, 5) * 4));
        break;
    }
    case Encoding::kFlat: {
        const uint32_t segment = field(w, 14, 2);
        const uint32_t saddr = field(d1, 16, 7);
        insn.mask = static_cast<uint8_t>(segment);
        insn.imm = segment == 0 ? static_cast<int64_t>(field(w, 0, 11))
                                : static_cast<int64_t>(static_cast<int32_t>(w << 20) >> 20);
        b.dst(vgpr(field(d1, 24, 8)));
        b.src(vgpr(field(d1, 0, 8)));
        b.src(vgpr(field(d1, 8, 8)));
        b.src(saddr == 0x7F || segment == 0 ? op(OperandKind::kNull) : scalar(saddr, 0));
        break;
    }
    case Encoding::kExport:
        insn.mask = static_cast<uint8_t>(field(w, 0, 4));
        insn.imm = field(w, 4, 6);
        insn.imm2 = field(w, 10, 3);
        for (unsigned byte = 0; byte < 4; ++byte) {
            b.src(vgpr(field(d1, 8 * byte, 8)));
        }
        break;
    case Encoding::kUnknown: break;
    }
}

} // namespace

std::string_view encoding_name(Encoding encoding) {
    switch (encoding) {
    case Encoding::kSop2: return "sop2";
    case Encoding::kSopk: return "sopk";
    case Encoding::kSop1: return "sop1";
    case Encoding::kSopc: return "sopc";
    case Encoding::kSopp: return "sopp";
    case Encoding::kSmem: return "smem";
    case Encoding::kVop2: return "vop2";
    case Encoding::kVop1: return "vop1";
    case Encoding::kVopc: return "vopc";
    case Encoding::kVop3: return "vop3";
    case Encoding::kVop3p: return "vop3p";
    case Encoding::kVinterp: return "vintrp";
    case Encoding::kDs: return "ds";
    case Encoding::kMubuf: return "mubuf";
    case Encoding::kMtbuf: return "mtbuf";
    case Encoding::kMimg: return "mimg";
    case Encoding::kFlat: return "flat";
    case Encoding::kExport: return "exp";
    case Encoding::kUnknown: return "unknown";
    }
    return "unknown";
}

Encoding classify(uint32_t w) {
    // Scalar ALU and program flow first: SOPP, SOP1 and SOPC sit inside SOPK's prefix, and
    // SOPK inside SOP2's.
    if (w >> 23 == 0x17F) {
        return Encoding::kSopp;
    }
    if (w >> 23 == 0x17D) {
        return Encoding::kSop1;
    }
    if (w >> 23 == 0x17E) {
        return Encoding::kSopc;
    }
    if (w >> 28 == 0xB) {
        return Encoding::kSopk;
    }
    if (w >> 30 == 0x2) {
        return Encoding::kSop2;
    }
    if (w >> 25 == 0x3F) {
        return Encoding::kVop1;
    }
    if (w >> 25 == 0x3E) {
        return Encoding::kVopc;
    }
    if (w >> 31 == 0) {
        return Encoding::kVop2;
    }
    switch (w >> 26) {
    case 0x32: return Encoding::kVinterp;
    case 0x33: return Encoding::kVop3p;
    case 0x35: return Encoding::kVop3;
    case 0x36: return Encoding::kDs;
    case 0x37: return Encoding::kFlat;
    case 0x38: return Encoding::kMubuf;
    case 0x3A: return Encoding::kMtbuf;
    case 0x3C: return Encoding::kMimg;
    case 0x3D: return Encoding::kSmem;
    case 0x3E: return Encoding::kExport;
    default: return Encoding::kUnknown;
    }
}

Instruction decode_one(std::span<const uint32_t> words) {
    Instruction insn;
    const uint32_t w = words[0];
    const uint32_t next = words.size() > 1 ? words[1] : 0;
    insn.encoding = classify(w);
    const auto [base, extra] = length_of(insn.encoding, w, next);
    const size_t length = base + (extra ? 1 : 0);
    insn.dwords = static_cast<uint8_t>(std::min(length, words.size()));
    insn.truncated = length > words.size();
    std::copy_n(words.begin(), insn.dwords, insn.words.begin());
    insn.opcode = opcode_of(insn.encoding, w, insn.words[1]);
    if (!insn.truncated) {
        decode_operands(insn);
    }
    return insn;
}

namespace {

std::vector<uint32_t> to_words(std::span<const uint8_t> code) {
    std::vector<uint32_t> words(code.size() / 4);
    for (size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<uint32_t>(code[4 * i]) |
                   (static_cast<uint32_t>(code[(4 * i) + 1]) << 8) |
                   (static_cast<uint32_t>(code[(4 * i) + 2]) << 16) |
                   (static_cast<uint32_t>(code[(4 * i) + 3]) << 24);
    }
    return words;
}

// Every combination exists for sample (0x20-0x3F). Gather4 (0x40-0x5F) has no derivative forms:
// the _d and _d_cl slots (modifier 2 and 3) are not gather4 opcodes.
bool sample_or_gather(const Instruction& insn) {
    if (insn.encoding != Encoding::kMimg || insn.opcode < 0x20 || insn.opcode >= 0x60) {
        return false;
    }
    return insn.opcode < 0x40 || (insn.opcode & 6) != 2;
}

} // namespace

// SOPP and SOPK branches are one dword, and their SIMM16 counts dwords from the next one.
Flow flow_of(const Instruction& insn) {
    Flow flow;
    const int64_t relative = (static_cast<int64_t>(insn.offset / 4) + 1 + insn.imm) * 4;
    switch (insn.encoding) {
    case Encoding::kSopp:
        switch (insn.opcode) {
        case kSoppEndpgm:
        case kSoppEndpgmSaved:
        case kSoppEndpgmOrderedPsDone: flow.falls_through = false; break;
        case kSoppCodeEnd:
            flow.falls_through = false;
            flow.code_end = true;
            break;
        case kSoppBranch:
            flow.falls_through = false;
            flow.target = relative;
            break;
        case 0x04: // s_cbranch_scc0
        case 0x05: // s_cbranch_scc1
        case 0x06: // s_cbranch_vccz
        case 0x07: // s_cbranch_vccnz
        case 0x08: // s_cbranch_execz
        case 0x09: // s_cbranch_execnz
        case 0x17: // s_cbranch_cdbgsys
        case 0x18: // s_cbranch_cdbguser
        case 0x19: // s_cbranch_cdbgsys_or_user
        case 0x1A: // s_cbranch_cdbgsys_and_user
            flow.target = relative;
            break;
        default: break;
        }
        break;
    case Encoding::kSopk:
        if (insn.opcode == kSopkCall || insn.opcode == kSopkSubvectorLoopBegin ||
            insn.opcode == kSopkSubvectorLoopEnd) {
            flow.target = relative;
        }
        break;
    case Encoding::kSop1:
        if (insn.opcode == kSop1Setpc) {
            flow.falls_through = false;
            flow.target = insn.far_target;
            flow.indirect = !insn.far_target;
        } else if (insn.opcode == kSop1Swappc) {
            flow.target = insn.far_target; // a call: the callee returns to the next instruction
            flow.indirect = !insn.far_target;
        }
        break;
    case Encoding::kUnknown:
        flow.falls_through = false; // its length isn't known
        break;
    default: break;
    }
    return flow;
}

std::vector<Instruction> decode(std::span<const uint8_t> code) {
    const std::vector<uint32_t> words = to_words(code);
    std::vector<Instruction> out;
    size_t i = 0;
    while (i < words.size()) {
        Instruction insn = decode_one(std::span(words).subspan(i));
        insn.offset = i * 4;
        i += insn.dwords;
        out.push_back(insn);
    }
    return out;
}

namespace {

bool is_sgpr(const Operand& operand, uint32_t reg) {
    return operand.kind == OperandKind::kSgpr && operand.value == reg;
}

// The constant an s_add_u32-like instruction adds to reg in place, when it is one. Adds take
// the register as either source; subtracts only as the first.
std::optional<uint32_t> constant_into(const Instruction& insn, uint32_t reg, bool commutes) {
    if (insn.dst_count != 1 || insn.src_count != 2 || !is_sgpr(insn.dst[0], reg)) {
        return std::nullopt;
    }
    const auto constant = [](const Operand& operand) -> std::optional<uint32_t> {
        if (operand.kind == OperandKind::kLiteral || operand.kind == OperandKind::kInlineInt) {
            return operand.value;
        }
        return std::nullopt;
    };
    if (is_sgpr(insn.src[0], reg)) {
        return constant(insn.src[1]);
    }
    if (commutes && is_sgpr(insn.src[1], reg)) {
        return constant(insn.src[0]);
    }
    return std::nullopt;
}

// Follows one straight path for the far jump run described at Program.
class FarJumps {
public:
    // Call for each instruction of the path, in order. Returns the target when insn ends a run.
    std::optional<int64_t> step(const Instruction& insn) {
        const Stage stage = stage_;
        stage_ = Stage::kNone;
        if (insn.encoding == Encoding::kSop1 && insn.opcode == kSop1Getpc &&
            insn.dst[0].kind == OperandKind::kSgpr) {
            stage_ = Stage::kPc;
            pair_ = insn.dst[0].value;
            pc_ = insn.offset + 4;
            return std::nullopt;
        }
        switch (stage) {
        case Stage::kPc:
            if (insn.encoding == Encoding::kSop2 &&
                (insn.opcode == kSop2AddU32 || insn.opcode == kSop2SubU32)) {
                subtract_ = insn.opcode == kSop2SubU32;
                if (const std::optional<uint32_t> low = constant_into(insn, pair_, !subtract_)) {
                    low_ = *low;
                    stage_ = Stage::kLow;
                }
            }
            break;
        case Stage::kLow:
            if (insn.encoding == Encoding::kSop2 &&
                insn.opcode == (subtract_ ? kSop2SubbU32 : kSop2AddcU32)) {
                if (const std::optional<uint32_t> high =
                        constant_into(insn, pair_ + 1, !subtract_)) {
                    const uint64_t k = (uint64_t{*high} << 32) | low_;
                    pc_ = subtract_ ? pc_ - k : pc_ + k;
                    stage_ = Stage::kHigh;
                }
            }
            break;
        case Stage::kHigh:
            if (insn.encoding == Encoding::kSop1 &&
                (insn.opcode == kSop1Setpc || insn.opcode == kSop1Swappc) &&
                is_sgpr(insn.src[0], pair_)) {
                return static_cast<int64_t>(pc_);
            }
            break;
        case Stage::kNone: break;
        }
        return std::nullopt;
    }

private:
    enum class Stage : uint8_t { kNone, kPc, kLow, kHigh };
    Stage stage_ = Stage::kNone;
    uint32_t pair_ = 0;
    bool subtract_ = false;
    uint32_t low_ = 0;
    uint64_t pc_ = 0; // bytes from the start of the code
};

} // namespace

Program decode_program(std::span<const uint8_t> code) {
    enum State : uint8_t { kUnseen, kStart, kInside };
    const std::vector<uint32_t> words = to_words(code);
    std::vector<State> state(words.size(), kUnseen);
    Program program;
    std::vector<size_t> pending = {0};
    while (!pending.empty()) {
        size_t at = pending.back();
        pending.pop_back();
        FarJumps far;
        while (true) {
            if (at >= words.size()) {
                program.falls_off = true;
                break;
            }
            if (state[at] != kUnseen) {
                program.bad_branch = program.bad_branch || state[at] == kInside;
                break;
            }
            Instruction insn = decode_one(std::span(words).subspan(at));
            insn.offset = at * 4;
            const bool overlaps =
                std::any_of(state.begin() + static_cast<ptrdiff_t>(at) + 1,
                            state.begin() + static_cast<ptrdiff_t>(at) + insn.dwords,
                            [](State s) { return s != kUnseen; });
            if (overlaps) {
                program.bad_branch = true;
                break;
            }
            state[at] = kStart;
            std::fill_n(state.begin() + static_cast<ptrdiff_t>(at) + 1, insn.dwords - 1, kInside);
            insn.far_target = far.step(insn);
            program.instructions.push_back(insn);
            if (insn.truncated) {
                program.falls_off = true;
                break;
            }
            const Flow flow = flow_of(insn);
            program.indirect = program.indirect || flow.indirect;
            program.falls_off = program.falls_off || flow.code_end;
            if (flow.target) {
                if (*flow.target < 0 || std::cmp_greater_equal(*flow.target / 4, words.size())) {
                    program.bad_branch = true;
                } else {
                    pending.push_back(static_cast<size_t>(*flow.target / 4));
                }
            }
            if (!flow.falls_through) {
                break;
            }
            at += insn.dwords;
        }
    }
    std::ranges::sort(program.instructions, {}, &Instruction::offset);
    for (size_t i = 0; i < words.size(); ++i) {
        if (state[i] == kUnseen && words[i] != kCodeEndWord) {
            program.unreached_bytes += 4;
        }
    }
    return program;
}

bool named(const Instruction& insn) {
    return sample_or_gather(insn) || opcode_name(insn.encoding, insn.opcode).has_value();
}

std::string mnemonic(const Instruction& insn) {
    if (sample_or_gather(insn)) {
        // Sample and gather4 names are regular: base, _c, modifier, _o.
        static constexpr std::array<std::string_view, 8> kModifiers = {
            "", "_cl", "_d", "_d_cl", "_l", "_b", "_b_cl", "_lz"};
        return fmt::format("{}{}{}{}", insn.opcode >= 0x40 ? "image_gather4" : "image_sample",
                           (insn.opcode & 0x08) != 0 ? "_c" : "", kModifiers[insn.opcode & 7],
                           (insn.opcode & 0x10) != 0 ? "_o" : "");
    }
    const std::optional<std::string_view> name = opcode_name(insn.encoding, insn.opcode);
    if (!name) {
        return fmt::format("{}<{:#x}>", encoding_name(insn.encoding), insn.opcode);
    }
    if (insn.encoding == Encoding::kFlat && insn.mask != 0 && name->starts_with("flat_")) {
        return fmt::format("{}{}", insn.mask == 1 ? "scratch_" : "global_", name->substr(5));
    }
    return std::string(*name);
}

std::string operand_text(const Operand& operand) {
    switch (operand.kind) {
    case OperandKind::kSgpr: return fmt::format("s{}", operand.value);
    case OperandKind::kVgpr: return fmt::format("v{}", operand.value);
    case OperandKind::kVccLo: return "vcc_lo";
    case OperandKind::kVccHi: return "vcc_hi";
    case OperandKind::kExecLo: return "exec_lo";
    case OperandKind::kExecHi: return "exec_hi";
    case OperandKind::kM0: return "m0";
    case OperandKind::kScc: return "scc";
    case OperandKind::kNull: return "null";
    case OperandKind::kInlineInt: return fmt::format("{}", static_cast<int32_t>(operand.value));
    case OperandKind::kInlineFloat:
        return operand.value == kInvTwoPiBits
                   ? std::string("0.15915494")
                   : fmt::format("{}", std::bit_cast<float>(operand.value));
    case OperandKind::kLiteral: return fmt::format("{:#x}", operand.value);
    case OperandKind::kOther: {
        const std::optional<std::string_view> name = hardware_source_name(operand.value);
        return name ? std::string(*name) : fmt::format("src{}", operand.value);
    }
    case OperandKind::kAttribute:
        return fmt::format("attr{}.{}{}", operand.value & 0x3F, "xyzw"[(operand.value >> 6) & 3],
                           (operand.value & 0x100) != 0 ? " high" : "");
    case OperandKind::kInterpParam:
        switch (operand.value) {
        case 0: return "p10";
        case 1: return "p20";
        case 2: return "p0";
        default: return fmt::format("param{}", operand.value);
        }
    }
    return "?";
}

uint64_t inline_float64(uint32_t float_bits) {
    if (float_bits == kInvTwoPiBits) {
        return kInvTwoPiBits64;
    }
    return std::bit_cast<uint64_t>(static_cast<double>(std::bit_cast<float>(float_bits)));
}

std::string text(const Instruction& insn) {
    std::string line = mnemonic(insn);
    if (insn.truncated || insn.encoding == Encoding::kUnknown) {
        for (size_t i = 0; i < insn.dwords; ++i) {
            line += fmt::format(" {:08x}", insn.words[i]);
        }
        return insn.truncated ? line + " (truncated)" : line;
    }
    const char* separator = " ";
    for (const Operand& operand : insn.destinations()) {
        line += separator + operand_text(operand);
        separator = ", ";
    }
    for (const Operand& operand : insn.sources()) {
        line += separator + operand_text(operand);
        separator = ", ";
    }
    if (insn.encoding == Encoding::kVinterp) {
        // ATTR and ATTRCHAN, the only fields left.
        return line + fmt::format(", attr{}.{}", insn.imm, "xyzw"[insn.imm2 & 3]);
    }
    if (insn.imm != 0 || insn.imm2 != 0 || insn.mask != 0 || insn.op_sel != 0) {
        line += fmt::format(" ; imm {} imm2 {} mask {:#x}", insn.imm, insn.imm2, insn.mask);
        if (insn.op_sel != 0) {
            line += fmt::format(" op_sel {:#x}", insn.op_sel);
        }
    }
    if (insn.sdwa) {
        line += fmt::format(" ; sdwa {:#010x}", *insn.sdwa);
    }
    if (insn.dpp) {
        line += fmt::format(" ; dpp {:#010x}", *insn.dpp);
    }
    if (insn.far_target) {
        line += fmt::format(" ; far target {:#x}", *insn.far_target);
    }
    return line;
}

bool vop3_interp(uint16_t opcode) {
    switch (opcode) {
    case kVop3InterpP1F32:
    case kVop3InterpP2F32:
    case kVop3InterpMovF32:
    case kVop3InterpP1llF16:
    case kVop3InterpP1lvF16:
    case kVop3InterpP2F16: return true;
    default: return false;
    }
}

std::optional<InterpAttribute> interp_attribute(const Instruction& insn) {
    if (insn.encoding == Encoding::kVinterp) {
        return InterpAttribute{.attribute = static_cast<uint8_t>(insn.imm),
                               .channel = static_cast<uint8_t>(insn.imm2)};
    }
    if (insn.encoding == Encoding::kVop3 && insn.src_count > 0 &&
        insn.src[0].kind == OperandKind::kAttribute) {
        const uint32_t value = insn.src[0].value;
        return InterpAttribute{.attribute = static_cast<uint8_t>(value & 0x3F),
                               .channel = static_cast<uint8_t>((value >> 6) & 3),
                               .high = (value & 0x100) != 0};
    }
    return std::nullopt;
}

bool uses_gds(const Instruction& insn) {
    return insn.encoding == Encoding::kDs &&
           (insn.mask != 0 || (insn.opcode >= kDsGwsFirst && insn.opcode <= kDsGwsLast) ||
            insn.opcode == kDsOrderedCount);
}

bool appends_or_consumes(const Instruction& insn) {
    return insn.encoding == Encoding::kDs &&
           (insn.opcode == kDsAppend || insn.opcode == kDsConsume);
}

void DecodeStats::add(const Program& program) {
    ++programs;
    instructions += program.instructions.size();
    bool cut_off = false;
    bool far = false;
    bool global = false;
    bool counter = false;
    for (const Instruction& insn : program.instructions) {
        ++encodings[insn.encoding];
        cut_off = cut_off || insn.truncated;
        if (insn.far_target) {
            ++far_jumps;
            far = true;
        }
        if (uses_gds(insn)) {
            ++gds;
            global = true;
        }
        if (appends_or_consumes(insn)) {
            ++append_consume;
            counter = true;
        }
        if (insn.encoding == Encoding::kUnknown) {
            ++unknown;
        } else if (!named(insn)) {
            ++unnamed[{insn.encoding, insn.opcode}];
        }
    }
    truncated += cut_off ? 1 : 0;
    falls_off += program.falls_off ? 1 : 0;
    bad_branch += program.bad_branch ? 1 : 0;
    indirect += program.indirect ? 1 : 0;
    far_jump_programs += far ? 1 : 0;
    gds_programs += global ? 1 : 0;
    append_consume_programs += counter ? 1 : 0;
    with_data += program.unreached_bytes != 0 ? 1 : 0;
    data_bytes += program.unreached_bytes;
}

} // namespace g6::shader::rdna2
