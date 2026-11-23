// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the TomBox developers (MIT; see NOTICES.md)
// Copyright (C) 2026 the xbox-emulator contributors
// Copyright (C) 2026 the G6Recomp contributors
//
// Ported to C++ from tombox-xshader's RDNA 2 decoder in xbox-emulator
// (https://github.com/omonkeyz/xbox-emulator, commit 51f5319, crates/tombox-xshader/src/rdna2.rs
// and src/rdna2/mnem.rs), licensed GPL-3.0-or-later. Most of those files come from TomBox
// (https://github.com/drycheetah84/TomBox, commit 7e8064f), whose tombox-xshader crate is MIT OR
// Apache-2.0; that part is used here under MIT. Changes from the original: VOP3P is its own
// encoding instead of VOP3 opcodes offset by 0x400; VOP3B carry-outs, VOP3 op_sel, the DPP control
// word, the K constant of v_madmk/v_madak/v_fmamk/v_fmaak, and the SMEM SOFFSET register are
// decoded; v_readlane and v_readfirstlane write a scalar in VOP3 too; an instruction cut off by
// the end of the code is marked instead of decoded from words that aren't there; FLAT names
// follow the segment; opcodes are added to the name tables; decode_program(), flow_of() and
// DecodeStats are new.

#pragma once

// Decoder for the console's native GPU code: RDNA 2 (GFX10.3) machine code, the body of every
// FSSM image (docs/shaders.md). It classifies each instruction by encoding, gets its length right
// (trailing literals, SDWA and DPP words, VOP3 literals, MIMG address words), and decodes its
// operands. Encodings follow AMD's public "RDNA 2 Instruction Set Architecture" reference.
// Opcodes it can't name stay unnamed; nothing is guessed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace g6::shader::rdna2 {

enum class Encoding : uint8_t {
    kSop2,
    kSopk,
    kSop1,
    kSopc,
    kSopp,
    kSmem,
    kVop2,
    kVop1,
    kVopc,
    kVop3,  // VOP3A and VOP3B
    kVop3p, // packed math
    kVinterp,
    kDs,
    kMubuf,
    kMtbuf,
    kMimg,
    kFlat, // FLAT, GLOBAL and SCRATCH
    kExport,
    kUnknown,
};

std::string_view encoding_name(Encoding encoding);

enum class OperandKind : uint8_t {
    kSgpr, // value: register number
    kVgpr, // value: register number
    kVccLo,
    kVccHi,
    kExecLo,
    kExecHi,
    kM0,
    kScc,
    kNull,
    kInlineInt,   // value: the integer, two's complement (-16 to 64)
    kInlineFloat, // value: the float's bits (0.5, 1, 2, 4, their negatives, 1/(2*pi))
    kLiteral,     // value: the trailing 32-bit literal
    kOther,       // value: the operand code, not modelled yet
};

struct Operand {
    OperandKind kind = OperandKind::kOther;
    uint32_t value = 0;
    bool operator==(const Operand&) const = default;
};

// The longest instruction is MIMG with three extra address words: five dwords.
inline constexpr size_t kMaxDwords = 5;
inline constexpr size_t kMaxSources = 16;

// One decoded instruction. Destinations and sources are in encoding order. The fields after them
// mean different things per encoding:
//   SOPK, SOPP       imm: SIMM16, sign-extended
//   SMEM             imm: the 21-bit OFFSET as stored; imm2: SDATA for s_atc_probe*; sources:
//                    the base SGPR pair, then SOFFSET unless it is null
//   VOP3, VOP3P      mask: abs (bits 0-2; VOP3P: neg_hi) | neg << 3; imm: clamp; imm2: omod
//                    (VOP3P: op_sel_hi, bits 0-1 from dword 1 and bit 2 from dword 0);
//                    op_sel: OPSEL
//   MIMG             mask: dmask; imm: dim; imm2: number of extra address words
//   FLAT             mask: segment (0 flat, 1 scratch, 2 global); imm: offset, signed unless flat
//   DS               imm: offset0 | offset1 << 8; mask: gds
//   MUBUF, MTBUF     imm: offset; mask: offen | idxen << 1 | glc << 2; imm2 (MTBUF): format
//   VINTRP           imm: attribute; imm2: channel
//   EXP              imm: target; mask: enabled channels; imm2: compr | done << 1 | vm << 2
// sdwa and dpp hold the extra dword of VOP1, VOP2 and VOPC when src0 selects one; src0 is then
// the register that dword names.
struct Instruction {
    size_t offset = 0; // bytes from the start of the code
    uint8_t dwords = 0;
    bool truncated = false; // the code ended before the instruction did
    Encoding encoding = Encoding::kUnknown;
    uint16_t opcode = 0;
    std::array<uint32_t, kMaxDwords> words{};
    std::array<Operand, 2> dst{};
    uint8_t dst_count = 0;
    std::array<Operand, kMaxSources> src{};
    uint8_t src_count = 0;
    int64_t imm = 0;
    int64_t imm2 = 0;
    uint8_t mask = 0;
    uint8_t op_sel = 0;
    std::optional<uint32_t> sdwa;
    std::optional<uint32_t> dpp;
    // The target of an s_setpc_b64 or s_swappc_b64 that decode_program() found to be a far jump
    // or call (see there), in bytes from the start of the code; it may lie outside the code.
    std::optional<int64_t> far_target;

    std::span<const Operand> destinations() const { return {dst.data(), dst_count}; }
    std::span<const Operand> sources() const { return {src.data(), src_count}; }
};

Encoding classify(uint32_t word);

// Decodes the instruction at words[0]; words must not be empty.
Instruction decode_one(std::span<const uint32_t> words);

// Decodes code from start to end, data included: a sweep that doesn't know where code stops.
// A trailing partial dword is ignored.
std::vector<Instruction> decode(std::span<const uint8_t> code);

// The code a program can reach from its first instruction. Programs keep constant tables after
// their last s_endpgm, so decode_program() follows control flow instead of sweeping: it walks
// on from each instruction, follows every branch, and stops at s_endpgm and s_setpc_b64.
//
// A far jump is followed too. A branch further than s_branch's 16-bit offset reaches is built
// as s_getpc_b64 s[a:a+1], then s_add_u32 s[a] and s_addc_u32 s[a+1] of a constant (s_sub_u32
// and s_subb_u32 backwards), then s_setpc_b64 s[a:a+1]. The two adds add one 64-bit constant to
// the address after s_getpc_b64, so the target is known wherever the code is loaded. Only that
// unbroken run of four, met along one path, counts; the same run ending in s_swappc_b64 is a far
// call. Either gets its far_target, and flow_of() treats it as direct.
struct Program {
    std::vector<Instruction> instructions; // the reached instructions, in offset order
    size_t unreached_bytes = 0; // bytes no path reaches, apart from the s_code_end padding
    bool falls_off = false;     // a path runs past the end of the code, or into s_code_end
    bool bad_branch = false;    // a branch leaves the code or lands inside an instruction
    bool indirect = false;      // s_setpc_b64 or s_swappc_b64 to a target that isn't known
};
Program decode_program(std::span<const uint8_t> code);

// Where control can go after an instruction, from its offset: on to the next instruction, to a
// branch target, or both. A call (s_call_b64, s_swappc_b64) also goes on to the next instruction,
// where the callee returns.
struct Flow {
    bool falls_through = true;
    std::optional<int64_t> target; // bytes from the start of the code; may lie outside it
    bool indirect = false;         // s_setpc_b64 or s_swappc_b64 without a far_target
    bool code_end = false;         // s_code_end: a path ran into the padding
};
Flow flow_of(const Instruction& insn);

// The opcode's name, or nullopt when the tables don't know it.
std::optional<std::string_view> opcode_name(Encoding encoding, uint16_t opcode);
// The name, or the encoding and opcode, as in "vop3<0x1c7>", when it isn't known.
std::string mnemonic(const Instruction& instruction);
// Whether mnemonic() has a name for it: from the tables, or the sample and gather4 pattern.
bool named(const Instruction& instruction);

std::string operand_text(const Operand& operand);
// One line of assembly-like text, for looking at programs on the user's own machine.
std::string text(const Instruction& instruction);

// Opcodes the analysis relies on.
inline constexpr uint16_t kSoppEndpgm = 0x01;
inline constexpr uint16_t kSoppBranch = 0x02;
inline constexpr uint16_t kSoppEndpgmSaved = 0x1B;
inline constexpr uint16_t kSoppEndpgmOrderedPsDone = 0x1E;
inline constexpr uint16_t kSoppCodeEnd = 0x1F;
inline constexpr uint16_t kSopkCall = 0x16;
inline constexpr uint16_t kSopkSubvectorLoopBegin = 0x1B;
inline constexpr uint16_t kSopkSubvectorLoopEnd = 0x1C;
inline constexpr uint16_t kSop1Getpc = 0x1F;
inline constexpr uint16_t kSop1Setpc = 0x20;
inline constexpr uint16_t kSop1Swappc = 0x21;
inline constexpr uint16_t kSop2AddU32 = 0x00;
inline constexpr uint16_t kSop2SubU32 = 0x01;
inline constexpr uint16_t kSop2AddcU32 = 0x04;
inline constexpr uint16_t kSop2SubbU32 = 0x05;
inline constexpr uint16_t kDsGwsFirst = 0x18; // ds_gws_sema_release_all
inline constexpr uint16_t kDsGwsLast = 0x1D;  // ds_gws_barrier
inline constexpr uint16_t kDsConsume = 0x3D;
inline constexpr uint16_t kDsAppend = 0x3E;
inline constexpr uint16_t kDsOrderedCount = 0x3F;
inline constexpr uint32_t kCodeEndWord = 0xBF9F0000;

// Whether a DS instruction works on the global data share or the global wave sync unit, which
// D3D12 on a PC has no counterpart for: the gds bit is set, or it is a ds_gws op or
// ds_ordered_count.
bool uses_gds(const Instruction& insn);
// Whether it is ds_append or ds_consume, on LDS or GDS.
bool appends_or_consumes(const Instruction& insn);

// What decoding whole programs found. `shaderrecomp scan` prints it, and the census test on the
// user's files checks it.
struct DecodeStats {
    size_t programs = 0;
    size_t instructions = 0;
    size_t unknown = 0;           // reached instructions of no known encoding
    size_t truncated = 0;         // programs with an instruction that runs past the end of the code
    size_t falls_off = 0;         // programs with a path that runs off the end
    size_t bad_branch = 0;        // programs with a branch out of the code or into an instruction
    size_t indirect = 0;          // programs with s_setpc_b64 or s_swappc_b64 to an unknown target
    size_t far_jumps = 0;         // s_setpc_b64 and s_swappc_b64 with a far_target
    size_t far_jump_programs = 0; // programs with one
    size_t with_data = 0;         // programs with bytes no path reaches
    size_t data_bytes = 0;
    size_t gds = 0;                       // instructions for which uses_gds() holds
    size_t gds_programs = 0;              // programs with one
    size_t append_consume = 0;            // ds_append and ds_consume, on LDS or GDS
    size_t append_consume_programs = 0;   // programs with one
    std::map<Encoding, size_t> encodings; // instructions per encoding
    std::map<std::pair<Encoding, uint16_t>, size_t> unnamed; // instructions per unnamed opcode

    void add(const Program& program);
};

} // namespace g6::shader::rdna2
