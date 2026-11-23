// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the G6Recomp contributors

#pragma once

// Which registers an RDNA 2 instruction reads and writes, and which registers a whole program
// reads before anything writes them: the inputs the hardware and the driver set up when a wave
// starts (docs/shaders.md). Written from AMD's public "RDNA 2 Instruction Set Architecture"
// reference. How many registers an operand spans comes from the opcode's name (rdna2.h): a
// 64-bit type takes two, a load of four dwords four.

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "rdna2.h"

namespace g6::shader::rdna2 {

enum class RegisterFile : uint8_t { kScalar, kVector };

// Scalar registers are numbered as their operand codes: s0 to s105, then vcc_lo and vcc_hi,
// m0, and exec_lo and exec_hi.
inline constexpr uint16_t kScalarVcc = 106;
inline constexpr uint16_t kScalarM0 = 124;
inline constexpr uint16_t kScalarExec = 126;
inline constexpr size_t kScalarRegisters = 128;
inline constexpr size_t kVectorRegisters = 256;

struct RegisterRange {
    RegisterFile file = RegisterFile::kScalar;
    uint16_t first = 0;
    uint16_t count = 0;
    bool operator==(const RegisterRange&) const = default;
};

struct Footprint {
    bool modelled = false; // false: the opcode's operands aren't modelled, and nothing is listed
    bool dynamic = false;  // it also reads or writes registers picked at run time through m0
    bool call = false;     // s_swappc_b64 or s_call_b64: it calls a subroutine
    std::array<RegisterRange, 20> read{};
    uint8_t read_count = 0;
    std::array<RegisterRange, 4> written{};
    uint8_t written_count = 0;

    std::span<const RegisterRange> reads() const { return {read.data(), read_count}; }
    std::span<const RegisterRange> writes() const { return {written.data(), written_count}; }
};

// `wave_size`, 32 or 64, sets how many registers a lane mask takes: vcc, a compare result, a
// carry. Reads come before writes, so an instruction that accumulates lists its destination in
// both. A write that changes only part of a register (a d16 half, one lane, one bit) counts as
// a write of all of it, as a vector write under exec does; the part it keeps isn't a read. SCC
// isn't tracked.
Footprint footprint(const Instruction& insn, unsigned wave_size);

// How many address dwords an image instruction takes, from its opcode, dimension and A16 bit;
// nothing when the instruction isn't an image instruction this models. An NSA instruction lists
// that many address registers, the rest of its last address dword being padding.
std::optional<unsigned> image_address_count(const Instruction& insn);

// 64 when the program treats exec (or vcc) as a 64-bit mask, 32 when it treats exec only as a
// 32-bit one, and nothing when it does neither or both.
std::optional<unsigned> wave_size(const Program& program);

// One past the highest SGPR below vcc, and one past the highest VGPR, that any decoded instruction
// reads or writes, reached or not; zero when none does. Registers an instruction picks at run
// time through m0 count only as far as its footprint lists them.
struct RegisterExtent {
    unsigned sgprs = 0;
    unsigned vgprs = 0;
};
RegisterExtent register_extent(const Program& program, unsigned wave_size);

// The registers some reached instruction reads although no path from the start of the program
// writes them first: inputs. exec is set up by the hardware and never counted.
struct LiveIns {
    std::bitset<kScalarRegisters> scalar;
    std::bitset<kVectorRegisters> vector;
    // Read after a call without any path writing them first, unless the callee did: what the
    // callee hands back. A vertex shader's fetch subroutine returns its inputs this way.
    std::bitset<kScalarRegisters> scalar_after_call;
    std::bitset<kVectorRegisters> vector_after_call;
    size_t unmodelled = 0; // reached instructions whose operands aren't modelled
    bool dynamic = false;  // some instruction picks registers at run time
    bool call = false;
};
LiveIns live_ins(const Program& program, unsigned wave_size);

} // namespace g6::shader::rdna2
