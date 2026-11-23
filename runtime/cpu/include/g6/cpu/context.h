#pragma once

// The guest CPU state recompiled code works on. It holds only what must survive a call: the
// general-purpose and vector registers and the TEB base. Status flags and the direction flag are
// locals inside each generated function (the Win64 ABI leaves them undefined or clear across
// calls), MXCSR is the host thread's own register, and x87 is not modeled. See docs/codegen.md.

#include <cstdint>
#include <immintrin.h>

namespace g6::cpu {

// General-purpose registers in x86 encoding order, so the recompiler can index them by number.
enum Gpr : uint8_t {
    kRax,
    kRcx,
    kRdx,
    kRbx,
    kRsp,
    kRbp,
    kRsi,
    kRdi,
    kR8,
    kR9,
    kR10,
    kR11,
    kR12,
    kR13,
    kR14,
    kR15,
};

inline constexpr int kGprCount = 16;
inline constexpr int kVectorCount = 16;

struct alignas(32) GuestContext {
    uint64_t gpr[kGprCount] = {};
    __m256i ymm[kVectorCount] = {}; // full AVX width; the low 128 bits are xmm0-15
    uint64_t gs_base = 0;           // the guest thread's TEB, base of gs:-relative accesses
};

} // namespace g6::cpu
