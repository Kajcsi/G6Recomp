#pragma once

// Inline helpers that recompiled code calls. A generated function keeps registers and status
// flags in locals and uses these for anything longer than an expression: memory access, flag
// computation, multiplication and division, bit scans, string instructions, and moving values
// in and out of vector registers. Each helper implements the documented x86-64 behavior of its
// instruction. Where the architecture leaves a flag undefined, the helper's choice is noted;
// the differential tests don't compare those flags.

#include <bit>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>
#include <limits>
#include <type_traits>

#include "g6/cpu/context.h"
#include "g6/cpu/cpuid.h"
#include "g6/cpu/dispatch.h"

namespace g6::cpu {

// ---- Memory -------------------------------------------------------------------------------

// Guest addresses are host addresses (docs/codegen.md). memcpy keeps unaligned access defined;
// it compiles to a single move.
template <typename T>
inline T load(uint64_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
inline void store(uint64_t address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

// A `lock`-prefixed read-modify-write: `op` maps the old value to the new one and may set
// flags; it runs again if another thread changed the value in between, so the flags match the
// update that took effect. Returns the old value.
template <typename T, typename Op>
inline T atomic_update(uint64_t address, Op op) {
    T* const pointer = reinterpret_cast<T*>(address);
    T expected = __atomic_load_n(pointer, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(pointer, &expected, op(expected), false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED)) {
    }
    return expected;
}

template <typename T>
inline T atomic_exchange(uint64_t address, T value) {
    return __atomic_exchange_n(reinterpret_cast<T*>(address), value, __ATOMIC_SEQ_CST);
}

// On failure `expected` receives the current value.
template <typename T>
inline bool atomic_compare_exchange(uint64_t address, T& expected, T desired) {
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(address), &expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// CMPXCHG16B: if RDX:RAX equals the 16 bytes at `address`, stores RCX:RBX there; otherwise
// loads them into RDX:RAX. Returns ZF. It is always atomic: the instruction is almost always
// locked, and an atomic update is also a valid outcome of the unlocked form. Like the
// instruction, it faults if `address` isn't 16-byte aligned.
inline bool cmpxchg16b(uint64_t address, uint64_t& rax, uint64_t& rdx, uint64_t rbx, uint64_t rcx) {
    long long seen[2] = {static_cast<long long>(rax), static_cast<long long>(rdx)};
    const bool equal = _InterlockedCompareExchange128(
                           reinterpret_cast<volatile long long*>(address),
                           static_cast<long long>(rcx), static_cast<long long>(rbx), seen) != 0;
    // On success `seen` still holds RDX:RAX, so writing it back changes nothing.
    rax = static_cast<uint64_t>(seen[0]);
    rdx = static_cast<uint64_t>(seen[1]);
    return equal;
}

// ---- Status flags -------------------------------------------------------------------------

struct Flags {
    bool cf = false;
    bool pf = false;
    bool af = false;
    bool zf = false;
    bool sf = false;
    bool of = false;
    bool df = false;
};

template <typename T>
inline constexpr unsigned kBits = sizeof(T) * 8;

template <typename T>
inline bool msb(T value) {
    return ((value >> (kBits<T> - 1)) & 1) != 0;
}

// PF: set when the low byte of the result has an even number of one bits.
inline bool even_parity(uint8_t value) {
    const unsigned nibble = (value ^ (value >> 4)) & 0xF;
    return ((0x9669u >> nibble) & 1) != 0;
}

template <typename T>
inline void set_result_flags(Flags& f, T result) {
    f.zf = result == 0;
    f.sf = msb(result);
    f.pf = even_parity(static_cast<uint8_t>(result));
}

template <typename T>
inline bool adjust_flag(T a, T b, T result) {
    return (((a ^ b ^ result) >> 4) & 1) != 0;
}

template <typename T>
inline T add(Flags& f, T a, T b) {
    const auto r = static_cast<T>(a + b);
    f.cf = r < a;
    f.of = msb(static_cast<T>((a ^ r) & (b ^ r)));
    f.af = adjust_flag(a, b, r);
    set_result_flags(f, r);
    return r;
}

template <typename T>
inline T adc(Flags& f, T a, T b) {
    const bool carry = f.cf;
    const auto r = static_cast<T>(a + b + (carry ? 1 : 0));
    f.cf = carry ? r <= a : r < a;
    f.of = msb(static_cast<T>((a ^ r) & (b ^ r)));
    f.af = adjust_flag(a, b, r);
    set_result_flags(f, r);
    return r;
}

template <typename T>
inline T sub(Flags& f, T a, T b) {
    const auto r = static_cast<T>(a - b);
    f.cf = a < b;
    f.of = msb(static_cast<T>((a ^ b) & (a ^ r)));
    f.af = adjust_flag(a, b, r);
    set_result_flags(f, r);
    return r;
}

template <typename T>
inline T sbb(Flags& f, T a, T b) {
    const bool borrow = f.cf;
    const auto r = static_cast<T>(a - b - (borrow ? 1 : 0));
    f.cf = borrow ? a <= b : a < b;
    f.of = msb(static_cast<T>((a ^ b) & (a ^ r)));
    f.af = adjust_flag(a, b, r);
    set_result_flags(f, r);
    return r;
}

// AND, OR, XOR, TEST: CF and OF clear. AF is undefined; it is cleared.
template <typename T>
inline T logic(Flags& f, T result) {
    f.cf = false;
    f.of = false;
    f.af = false;
    set_result_flags(f, result);
    return result;
}

// INC and DEC leave CF alone.
template <typename T>
inline T inc(Flags& f, T a) {
    const bool carry = f.cf;
    const T r = add<T>(f, a, 1);
    f.cf = carry;
    return r;
}

template <typename T>
inline T dec(Flags& f, T a) {
    const bool carry = f.cf;
    const T r = sub<T>(f, a, 1);
    f.cf = carry;
    return r;
}

template <typename T>
inline T neg(Flags& f, T a) {
    return sub<T>(f, 0, a);
}

// Shifts and rotates mask the count to 5 bits (6 for 64-bit operands); a masked count of zero
// changes neither the operand nor any flag. OF is only defined for a count of one; the helpers
// compute the count-of-one formula for every count. AF is undefined; shifts clear it.
inline constexpr uint8_t shift_mask(unsigned bits) {
    return bits == 64 ? 63 : 31;
}

template <typename T>
inline T shl(Flags& f, T a, uint8_t count) {
    count &= shift_mask(kBits<T>);
    if (count == 0) {
        return a;
    }
    T r;
    bool carry;
    if constexpr (kBits<T> == 64) {
        r = a << count;
        carry = ((a >> (64 - count)) & 1) != 0;
    } else {
        const uint64_t shifted = static_cast<uint64_t>(a) << count; // count <= 31: no overflow
        r = static_cast<T>(shifted);
        carry = ((shifted >> kBits<T>)&1) != 0;
    }
    f.cf = carry;
    f.of = msb(r) != carry;
    f.af = false;
    set_result_flags(f, r);
    return r;
}

template <typename T>
inline T shr(Flags& f, T a, uint8_t count) {
    count &= shift_mask(kBits<T>);
    if (count == 0) {
        return a;
    }
    const auto r = count >= kBits<T> ? T{0} : static_cast<T>(a >> count);
    f.cf = ((static_cast<uint64_t>(a) >> (count - 1)) & 1) != 0;
    f.of = msb(a);
    f.af = false;
    set_result_flags(f, r);
    return r;
}

template <typename T>
inline T sar(Flags& f, T a, uint8_t count) {
    count &= shift_mask(kBits<T>);
    if (count == 0) {
        return a;
    }
    const auto wide = static_cast<int64_t>(static_cast<std::make_signed_t<T>>(a));
    const auto r = static_cast<T>(wide >> count);
    f.cf = ((wide >> (count - 1)) & 1) != 0;
    f.of = false;
    f.af = false;
    set_result_flags(f, r);
    return r;
}

// Rotates change only CF and OF.
template <typename T>
inline T rol(Flags& f, T a, uint8_t count) {
    count &= shift_mask(kBits<T>);
    if (count == 0) {
        return a;
    }
    const unsigned n = count % kBits<T>;
    const auto r = n == 0 ? a : static_cast<T>((a << n) | (a >> (kBits<T> - n)));
    f.cf = (r & 1) != 0;
    f.of = msb(r) != f.cf;
    return r;
}

template <typename T>
inline T ror(Flags& f, T a, uint8_t count) {
    count &= shift_mask(kBits<T>);
    if (count == 0) {
        return a;
    }
    const unsigned n = count % kBits<T>;
    const auto r = n == 0 ? a : static_cast<T>((a >> n) | (a << (kBits<T> - n)));
    f.cf = msb(r);
    f.of = msb(r) != (((r >> (kBits<T> - 2)) & 1) != 0);
    return r;
}

// SHLD/SHRD: `a` shifted with bits filled in from `b`. For 16-bit operands a count above 16
// gives an undefined result (and flags); this computes something deterministic for it.
template <typename T>
inline T shld(Flags& f, T a, T b, uint8_t count) {
    count &= shift_mask(kBits<T>);
    if (count == 0) {
        return a;
    }
    T r;
    bool carry;
    if constexpr (kBits<T> == 64) {
        r = (a << count) | (b >> (64 - count));
        carry = ((a >> (64 - count)) & 1) != 0;
    } else {
        const uint64_t joined = static_cast<uint64_t>(a) << kBits<T> | b; // a:b
        r = static_cast<T>((joined << count) >> kBits<T>);
        carry = ((joined >> (2 * kBits<T> - count)) & 1) != 0;
    }
    f.cf = carry;
    f.of = msb(r) != msb(a);
    f.af = false;
    set_result_flags(f, r);
    return r;
}

template <typename T>
inline T shrd(Flags& f, T a, T b, uint8_t count) {
    count &= shift_mask(kBits<T>);
    if (count == 0) {
        return a;
    }
    T r;
    bool carry;
    if constexpr (kBits<T> == 64) {
        r = (a >> count) | (b << (64 - count));
        carry = ((a >> (count - 1)) & 1) != 0;
    } else {
        const uint64_t joined = static_cast<uint64_t>(b) << kBits<T> | a; // b:a
        r = static_cast<T>(joined >> count);
        carry = ((joined >> (count - 1)) & 1) != 0;
    }
    f.cf = carry;
    f.of = msb(r) != msb(a);
    f.af = false;
    set_result_flags(f, r);
    return r;
}

// BMI1 and BMI2. AF and PF are undefined for all of them and left unchanged.
template <typename T>
inline T blsr(Flags& f, T a) {
    const auto r = static_cast<T>(a & (a - 1));
    f.cf = a == 0;
    f.of = false;
    f.zf = r == 0;
    f.sf = msb(r);
    return r;
}

template <typename T>
inline T blsi(Flags& f, T a) {
    const auto r = static_cast<T>(a & (T{0} - a));
    f.cf = a != 0;
    f.of = false;
    f.zf = r == 0;
    f.sf = msb(r);
    return r;
}

template <typename T>
inline T blsmsk(Flags& f, T a) {
    const auto r = static_cast<T>(a ^ (a - 1));
    f.cf = a == 0;
    f.of = false;
    f.zf = false;
    f.sf = msb(r);
    return r;
}

template <typename T>
inline T bzhi(Flags& f, T a, T index_operand) {
    const unsigned index = static_cast<uint8_t>(index_operand);
    T r = a;
    if (index < kBits<T>) {
        r = static_cast<T>(a & ((T{1} << index) - 1));
    }
    f.cf = index > kBits<T> - 1;
    f.of = false;
    f.zf = r == 0;
    f.sf = msb(r);
    return r;
}

template <typename T>
inline T bextr(Flags& f, T a, T control) {
    const unsigned start = static_cast<uint8_t>(control);
    const unsigned length = static_cast<uint8_t>(control >> 8);
    T r = 0;
    if (start < kBits<T>) {
        r = static_cast<T>(a >> start);
        if (length < kBits<T>) {
            r = static_cast<T>(r & ((T{1} << length) - 1));
        }
    }
    f.cf = false;
    f.of = false;
    f.zf = r == 0;
    return r;
}

// MUL: the low half is returned and the high half stored in `high`. CF and OF are set when the
// high half is nonzero. SF, ZF, AF, and PF are undefined; they are left unchanged.
template <typename T>
inline T mul(Flags& f, T a, T b, T& high) {
    T low;
    if constexpr (kBits<T> == 64) {
        unsigned long long h = 0;
        low = _umul128(a, b, &h);
        high = h;
    } else {
        const uint64_t product = static_cast<uint64_t>(a) * b;
        low = static_cast<T>(product);
        high = static_cast<T>(product >> kBits<T>);
    }
    f.cf = high != 0;
    f.of = f.cf;
    return low;
}

// IMUL in every form: CF and OF are set when the full signed product doesn't fit in the low
// half. The two- and three-operand forms just ignore `high`. Other flags as for MUL.
template <typename T>
inline T imul(Flags& f, T a, T b, T& high) {
    using S = std::make_signed_t<T>;
    T low;
    if constexpr (kBits<T> == 64) {
        long long h = 0;
        low = static_cast<T>(_mul128(static_cast<long long>(a), static_cast<long long>(b), &h));
        high = static_cast<T>(h);
        f.cf = h != (static_cast<int64_t>(low) >> 63);
    } else {
        const int64_t product = int64_t{static_cast<S>(a)} * int64_t{static_cast<S>(b)};
        low = static_cast<T>(product);
        high = static_cast<T>(static_cast<uint64_t>(product) >> kBits<T>);
        f.cf = product != int64_t{static_cast<S>(low)};
    }
    f.of = f.cf;
    return low;
}

// 128-by-64 unsigned division on the host's own DIV. The callers rule out the cases that would
// raise #DE first.
inline uint64_t divide_128(uint64_t high, uint64_t low, uint64_t divisor, uint64_t& remainder) {
    uint64_t quotient = 0;
    __asm__("divq %[divisor]"
            : "=a"(quotient), "=d"(remainder)
            : "a"(low), "d"(high), [divisor] "r"(divisor));
    return quotient;
}

// DIV: divides high:low by `divisor`. Division by zero and a quotient that doesn't fit raise
// #DE, which is a guest exception; until the exception model exists, it is a fault. Flags are
// undefined and left unchanged.
template <typename T>
inline T divide(GuestContext& ctx, uint64_t at, T high, T low, T divisor, T& remainder) {
    if (divisor == 0) {
        fault(ctx, at, "integer division by zero");
    }
    if constexpr (kBits<T> == 64) {
        if (high >= divisor) {
            fault(ctx, at, "integer division overflow");
        }
        uint64_t rem = 0;
        const uint64_t quotient = divide_128(high, low, divisor, rem);
        remainder = rem;
        return quotient;
    } else {
        const uint64_t dividend = (static_cast<uint64_t>(high) << kBits<T>) | low;
        const uint64_t quotient = dividend / divisor;
        if ((quotient >> kBits<T>) != 0) {
            fault(ctx, at, "integer division overflow");
        }
        remainder = static_cast<T>(dividend % divisor);
        return static_cast<T>(quotient);
    }
}

// IDIV: as DIV, signed. The remainder takes the dividend's sign.
template <typename T>
inline T divide_signed(GuestContext& ctx, uint64_t at, T high, T low, T divisor, T& remainder) {
    using S = std::make_signed_t<T>;
    if (divisor == 0) {
        fault(ctx, at, "integer division by zero");
    }
    if constexpr (kBits<T> == 64) {
        const bool negative_dividend = static_cast<int64_t>(high) < 0;
        const bool negative_divisor = static_cast<int64_t>(divisor) < 0;
        uint64_t magnitude_high = high;
        uint64_t magnitude_low = low;
        if (negative_dividend) {
            magnitude_low = ~magnitude_low + 1;
            magnitude_high = ~magnitude_high + (magnitude_low == 0 ? 1 : 0);
        }
        const uint64_t magnitude_divisor = negative_divisor ? ~divisor + 1 : divisor;
        if (magnitude_high >= magnitude_divisor) {
            fault(ctx, at, "integer division overflow");
        }
        uint64_t rem = 0;
        const uint64_t quotient = divide_128(magnitude_high, magnitude_low, magnitude_divisor, rem);
        const bool negative_quotient = negative_dividend != negative_divisor;
        if (quotient > (negative_quotient ? uint64_t{1} << 63 : (uint64_t{1} << 63) - 1)) {
            fault(ctx, at, "integer division overflow");
        }
        remainder = negative_dividend ? ~rem + 1 : rem;
        return negative_quotient ? ~quotient + 1 : quotient;
    } else {
        constexpr unsigned kWide = 2 * kBits<T>;
        const uint64_t raw = (static_cast<uint64_t>(high) << kBits<T>) | low;
        // Sign-extend the double-width dividend to 64 bits.
        const auto dividend = static_cast<int64_t>(raw << (64 - kWide)) >> (64 - kWide);
        const int64_t signed_divisor = static_cast<S>(divisor);
        if (signed_divisor == -1 && dividend == std::numeric_limits<int64_t>::min()) {
            fault(ctx, at, "integer division overflow");
        }
        const int64_t quotient = dividend / signed_divisor;
        if (quotient < std::numeric_limits<S>::min() || quotient > std::numeric_limits<S>::max()) {
            fault(ctx, at, "integer division overflow");
        }
        remainder = static_cast<T>(dividend % signed_divisor);
        return static_cast<T>(quotient);
    }
}

// TZCNT, LZCNT, POPCNT. For the counts, OF, SF, AF, and PF are undefined and left unchanged.
template <typename T>
inline T tzcnt(Flags& f, T a) {
    const auto r = a == 0 ? static_cast<T>(kBits<T>) : static_cast<T>(__builtin_ctzll(a));
    f.cf = a == 0;
    f.zf = r == 0;
    return r;
}

template <typename T>
inline T lzcnt(Flags& f, T a) {
    const auto r =
        a == 0 ? static_cast<T>(kBits<T>) : static_cast<T>(__builtin_clzll(a) - (64 - kBits<T>));
    f.cf = a == 0;
    f.zf = r == 0;
    return r;
}

template <typename T>
inline T popcnt(Flags& f, T a) {
    const auto r = static_cast<T>(__builtin_popcountll(a));
    f.cf = false;
    f.of = false;
    f.sf = false;
    f.af = false;
    f.pf = false;
    f.zf = a == 0;
    return r;
}

// The RFLAGS image PUSHF produces: the six status flags and DF, plus bit 1 (always set) and IF
// (set in user mode).
inline uint64_t pack_flags(const Flags& f) {
    return uint64_t{f.cf} | 0x2 | uint64_t{f.pf} << 2 | uint64_t{f.af} << 4 | uint64_t{f.zf} << 6 |
           uint64_t{f.sf} << 7 | 0x200 | uint64_t{f.df} << 10 | uint64_t{f.of} << 11;
}

inline void unpack_flags(Flags& f, uint64_t value) {
    f.cf = (value & 0x1) != 0;
    f.pf = (value & 0x4) != 0;
    f.af = (value & 0x10) != 0;
    f.zf = (value & 0x40) != 0;
    f.sf = (value & 0x80) != 0;
    f.df = (value & 0x400) != 0;
    f.of = (value & 0x800) != 0;
}

// ---- String instructions ------------------------------------------------------------------

template <typename T>
inline uint64_t string_step(bool df) {
    return df ? uint64_t{0} - sizeof(T) : sizeof(T);
}

template <typename T>
inline void movs(uint64_t& rsi, uint64_t& rdi, bool df) {
    store<T>(rdi, load<T>(rsi));
    rsi += string_step<T>(df);
    rdi += string_step<T>(df);
}

// REP MOVS copies element by element. A forward copy only differs from memmove when the
// destination starts inside the source, so every other case takes the fast path.
template <typename T>
inline void rep_movs(uint64_t& rsi, uint64_t& rdi, uint64_t& rcx, bool df) {
    if (!df && rcx != 0) {
        const uint64_t bytes = rcx * sizeof(T);
        if (rdi - rsi >= bytes) {
            std::memmove(reinterpret_cast<void*>(rdi), reinterpret_cast<const void*>(rsi), bytes);
            rsi += bytes;
            rdi += bytes;
            rcx = 0;
            return;
        }
    }
    for (; rcx != 0; --rcx) {
        movs<T>(rsi, rdi, df);
    }
}

template <typename T>
inline void stos(uint64_t& rdi, T value, bool df) {
    store<T>(rdi, value);
    rdi += string_step<T>(df);
}

template <typename T>
inline void rep_stos(uint64_t& rdi, uint64_t& rcx, T value, bool df) {
    if constexpr (sizeof(T) == 1) {
        if (!df) {
            std::memset(reinterpret_cast<void*>(rdi), value, rcx);
            rdi += rcx;
            rcx = 0;
            return;
        }
    }
    for (; rcx != 0; --rcx) {
        stos<T>(rdi, value, df);
    }
}

// ---- x87 control --------------------------------------------------------------------------

// The x87 control word and exception flags are the host thread's own, like MXCSR: guest
// threads run on host threads, and the runtime itself never uses x87. Only these state
// operations are translated (docs/codegen.md).
inline void fnstcw(uint64_t address) {
    uint16_t control = 0;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    store<uint16_t>(address, control);
}

inline void fldcw(uint64_t address) {
    const auto control = load<uint16_t>(address);
    __asm__ volatile("fldcw %0" : : "m"(control));
}

inline void fnclex() {
    __asm__ volatile("fnclex");
}

// ---- Vector registers ---------------------------------------------------------------------

inline __m128i lo(__m256i v) {
    return _mm256_castsi256_si128(v);
}

// Legacy SSE writes change the low 128 bits and keep bits 255:128.
inline __m256i with_lo(__m256i v, __m128i x) {
    return _mm256_blend_epi32(v, _mm256_castsi128_si256(x), 0x0F);
}

// VEX.128 writes clear bits 255:128.
inline __m256i zext(__m128i x) {
    return _mm256_zextsi128_si256(x);
}

// Loads of exactly the operand's size, zero-extended to 128 bits.
inline __m128i load32(uint64_t address) {
    return _mm_cvtsi32_si128(load<int32_t>(address));
}

inline __m128i load64(uint64_t address) {
    return _mm_cvtsi64_si128(load<int64_t>(address));
}

inline __m128i load128(uint64_t address) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(address));
}

inline __m256i load256(uint64_t address) {
    return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(address));
}

inline void store32(uint64_t address, __m128i x) {
    store<int32_t>(address, _mm_cvtsi128_si32(x));
}

inline void store64(uint64_t address, __m128i x) {
    store<int64_t>(address, _mm_cvtsi128_si64(x));
}

inline void store128(uint64_t address, __m128i x) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(address), x);
}

inline void store256(uint64_t address, __m256i x) {
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(address), x);
}

// An explicit length of an SSE4.2 string comparison with REX.W (RAX or RDX). The instruction
// uses the length's absolute value, saturated to the element count (at most 16), so clamping
// to [-16, 16] keeps its meaning in the intrinsic's 32-bit argument. The 32-bit form passes
// EAX or EDX through unchanged.
inline int string_length64(uint64_t value) {
    const auto length = static_cast<int64_t>(value);
    return static_cast<int>(length < -16 ? -16 : (length > 16 ? 16 : length));
}

// COMISS/COMISD/UCOMISS/UCOMISD: unordered sets ZF, PF, and CF; OF, SF, and AF are cleared.
// The two forms differ only in which NaNs raise the invalid-operation exception, which the
// host compare raises the same way.
inline void compare_flags(Flags& f, bool unordered, bool equal, bool less) {
    f.zf = unordered || equal;
    f.pf = unordered;
    f.cf = unordered || less;
    f.of = false;
    f.sf = false;
    f.af = false;
}

inline void comiss(Flags& f, __m128i a, __m128i b) {
    const float x = _mm_cvtss_f32(_mm_castsi128_ps(a));
    const float y = _mm_cvtss_f32(_mm_castsi128_ps(b));
    compare_flags(f, x != x || y != y, x == y, x < y);
}

inline void comisd(Flags& f, __m128i a, __m128i b) {
    const double x = _mm_cvtsd_f64(_mm_castsi128_pd(a));
    const double y = _mm_cvtsd_f64(_mm_castsi128_pd(b));
    compare_flags(f, x != x || y != y, x == y, x < y);
}

} // namespace g6::cpu
