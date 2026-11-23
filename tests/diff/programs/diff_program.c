// An original test program for the recompiler's differential tests. It is built as a plain x64
// DLL with no CRT and no imports, once for the baseline target (SSE2) and once with /arch:AVX2,
// and test_diff.cpp checks that every exported function gives the same results natively and
// recompiled.
//
// The C functions cover what an optimizing compiler emits for ordinary code: loops, jump
// tables, recursion, calls through function pointers, tail calls, stack arguments, structure
// returns, 128-bit multiplies, floating-point conversions, and autovectorized loops. The AVX2
// build turns the same source into VEX, FMA, and BMI code.
//
// The assembly probes (probe_list.h) are written out instruction by instruction, so they don't
// depend on the target. Only the baseline build includes them.

#include <intrin.h>
#include <stddef.h>
#include <stdint.h>

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

// ---- What the compiler expects from a CRT -------------------------------------------------------

int _fltused;

#pragma function(memset, memcpy)

// The string instructions are written as inline assembly: clang lowers __stosb and __movsb to
// memset and memcpy calls, which would make these functions call themselves forever.
void* memset(void* dst, int value, size_t size) {
    void* cursor = dst;
    __asm__ volatile("rep stosb" : "+D"(cursor), "+c"(size) : "a"(value) : "memory");
    return dst;
}

void* memcpy(void* dst, const void* src, size_t size) {
    void* cursor = dst;
    __asm__ volatile("rep movsb" : "+D"(cursor), "+S"(src), "+c"(size) : : "memory");
    return dst;
}

// ---- Assembly probes
// ------------------------------------------------------------------------------

#ifdef G6_DIFF_PROBES
// An exported function written in assembly. The .drectve section passes /EXPORT to the linker.
#define G6_ASM_FUNCTION(name, body)                                                                \
    __asm__(".intel_syntax noprefix\n.text\n.p2align 4\n.globl " name "\n" name ":\n" body         \
            ".att_syntax prefix\n.section .drectve,\"yni\"\n.ascii \" /EXPORT:" name "\"\n"        \
            ".text\n");
#define PROBE(name, width, input, flags, param, body) G6_ASM_FUNCTION(name, body)
#include "probe_list.h"
#undef PROBE
#endif

// ---- Integer code -------------------------------------------------------------------------------

EXPORT uint64_t c_hash(const uint8_t* data, size_t size, uint64_t seed) {
    uint64_t h = seed ^ 0x6a09e667f3bcc909ull;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 31;
    }
    return h ^ (h >> 17);
}

// The caller keeps b away from 0 and -1 in both 64 and 32 bits.
EXPORT void c_divide(int64_t a, int64_t b, int64_t* out) {
    out[0] = a / b;
    out[1] = a % b;
    out[2] = (int64_t)((uint64_t)a / (uint64_t)b);
    out[3] = (int64_t)((uint64_t)a % (uint64_t)b);
    out[4] = a / 7;
    out[5] = a % 10;
    out[6] = (int64_t)((uint64_t)a / 1000003u);
    out[7] = (int32_t)a / -5;
    out[8] = (uint32_t)a / 3u;
    out[9] = (int32_t)a / (int32_t)b;
    out[10] = (int32_t)a % (int32_t)b;
    out[11] = (int16_t)a / 9;
}

// Different work in each case, so the compiler builds a jump table instead of a lookup table.
EXPORT int64_t c_switch(int64_t selector, int64_t x) {
    switch (selector) {
    case 0: return x + 1;
    case 1: return x * 3;
    case 2: return x ^ 0x5a5a;
    case 3: return x >> 3;
    case 4: return ~x;
    case 5: return x * x;
    case 6: return x - 1000;
    case 7: return (int64_t)(((uint64_t)x << 7) | 1);
    case 8: return x & 0xffff;
    case 9: return (int64_t)(0 - (uint64_t)x);
    case 10: return x / 3;
    case 11: return x | 0x100;
    default: return 42;
    }
}

NOINLINE static uint64_t fib(unsigned n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

EXPORT uint64_t c_fib(unsigned n) {
    return fib(n % 22);
}

typedef int64_t (*BinaryOp)(int64_t, int64_t);

NOINLINE static int64_t op_add(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a + (uint64_t)b);
}
NOINLINE static int64_t op_sub(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a - (uint64_t)b);
}
NOINLINE static int64_t op_mul(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a * (uint64_t)b);
}
NOINLINE static int64_t op_min(int64_t a, int64_t b) {
    return a < b ? a : b;
}
NOINLINE static int64_t op_rotate(int64_t a, int64_t b) {
    const unsigned n = (unsigned)b & 63;
    return (int64_t)(((uint64_t)a << n) | ((uint64_t)a >> ((64 - n) & 63)));
}

static BinaryOp const kOps[] = {op_add, op_sub, op_mul, op_min, op_rotate};

EXPORT int64_t c_dispatch(uint64_t which, int64_t a, int64_t b) {
    return kOps[which % (sizeof(kOps) / sizeof(kOps[0]))](a, b);
}

// Calls through the table in a loop, so the indirect call is not a tail call. Starting at a
// run-time index keeps the compiler from resolving the targets itself.
EXPORT int64_t c_dispatch_all(uint64_t start, int64_t a, int64_t b) {
    const size_t count = sizeof(kOps) / sizeof(kOps[0]);
    int64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        sum = (int64_t)((uint64_t)sum * 31 + (uint64_t)kOps[(start + i) % count](a, b));
    }
    return sum;
}

// Mutual recursion in tail position: the compiler turns both calls into jumps.
NOINLINE static uint64_t chain_b(uint64_t x, unsigned n);

NOINLINE static uint64_t chain_a(uint64_t x, unsigned n) {
    if (n == 0) {
        return x;
    }
    return chain_b(x * 3 + 1, n - 1);
}

NOINLINE static uint64_t chain_b(uint64_t x, unsigned n) {
    if (n == 0) {
        return x ^ 0xabc;
    }
    return chain_a((x >> 1) ^ n, n - 1);
}

EXPORT uint64_t c_tail_chain(uint64_t x, unsigned n) {
    return chain_a(x, n % 64);
}

static void insertion_sort(int32_t* values, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        const int32_t key = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > key) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = key;
    }
}

NOINLINE static void quick_sort(int32_t* values, size_t count) {
    while (count > 12) {
        const int32_t pivot = values[count / 2];
        size_t i = 0;
        size_t j = count - 1;
        for (;;) {
            while (values[i] < pivot) {
                ++i;
            }
            while (values[j] > pivot) {
                --j;
            }
            if (i >= j) {
                break;
            }
            const int32_t t = values[i];
            values[i] = values[j];
            values[j] = t;
            ++i;
            --j;
        }
        quick_sort(values, j + 1);
        values += j + 1;
        count -= j + 1;
    }
    insertion_sort(values, count);
}

EXPORT void c_sort(int32_t* values, size_t count) {
    quick_sort(values, count);
}

EXPORT void c_fill_copy(uint8_t* __restrict dst, const uint8_t* __restrict src, size_t size,
                        int value) {
    memset(dst, value, size);
    for (size_t i = 0; i < size / 2; ++i) {
        dst[i] = src[i];
    }
    for (size_t i = size / 2; i + 3 < size; i += 3) {
        dst[i] ^= src[i];
    }
}

EXPORT uint32_t c_histogram(const uint8_t* data, size_t size) {
    uint32_t counts[256];
    memset(counts, 0, sizeof(counts));
    for (size_t i = 0; i < size; ++i) {
        ++counts[data[i]];
    }
    uint32_t best = 0;
    for (uint32_t i = 1; i < 256; ++i) {
        if (counts[i] > counts[best]) {
            best = i;
        }
    }
    return best << 16 | counts[best];
}

static int64_t g_counter;

EXPORT int64_t c_counter(int64_t add) {
    g_counter = (int64_t)((uint64_t)g_counter * 5 + (uint64_t)add);
    return g_counter;
}

EXPORT uint64_t c_bits(uint64_t x, uint64_t y) {
    const unsigned n = (unsigned)(y & 63);
    uint64_t r = (uint64_t)__builtin_popcountll(x);
    r = r * 131 + (uint64_t)__builtin_ctzll(x | (1ull << 63));
    r = r * 131 + (uint64_t)__builtin_clzll(x | 1);
    r = r * 131 + __builtin_bswap64(y);
    r ^= (x << n) | (x >> ((64 - n) & 63));
    r += (x >> n) ^ (y << (x & 63));
    r += (uint64_t)((int64_t)y >> (x & 63));
    r += x & (x - 1);
    r += x & (0 - x);
    r += x ^ (x - 1);
    r += x & ((1ull << n) - 1);
    r += (uint64_t)__builtin_popcount((uint32_t)y);
    r += (uint64_t)__builtin_bswap32((uint32_t)x);
    r += ~x & y;
    return r;
}

// CPU identification. Recompiled, these report the console's features rather than the host's,
// so the test checks them against that rule instead of comparing with the native result.
EXPORT void c_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* out) {
    int r[4];
    __cpuidex(r, (int)leaf, (int)subleaf);
    for (int i = 0; i < 4; ++i) {
        out[i] = (uint32_t)r[i];
    }
}

EXPORT uint64_t c_xgetbv(uint32_t index) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(index));
    return (uint64_t)high << 32 | low;
}

// RDSSPQ RAX (raw bytes: the instruction needs a CET-aware assembler). Without an active shadow
// stack it leaves RAX alone, so this returns 0; Zen 2 has no shadow stack at all.
EXPORT uint64_t c_rdssp(void) {
    uint64_t value = 0;
    __asm__ volatile(".byte 0xf3, 0x48, 0x0f, 0x1e, 0xc8" : "+a"(value));
    return value;
}

EXPORT uint64_t c_mul128(uint64_t a, uint64_t b, uint64_t* out) {
    uint64_t high = 0;
    out[0] = _umul128(a, b, &high);
    out[1] = high;
    int64_t signed_high = 0;
    out[2] = (uint64_t)_mul128((int64_t)a, (int64_t)b, &signed_high);
    out[3] = (uint64_t)signed_high;
    out[4] = __umulh(a, b ^ 0x5555);
    out[5] = (uint64_t)__mulh((int64_t)a, (int64_t)(b + 7));
    return high ^ (uint64_t)signed_high;
}

// Win64 passes the first four arguments in registers by position (integer or vector) and the
// rest on the stack.
EXPORT double c_many_args(int64_t a, double b, int32_t c, float d, int64_t e, double f, int8_t g,
                          uint16_t h, float i) {
    return (double)a * b + (double)((float)c * d) + (double)e - f + (double)(g * h) + (double)i;
}

typedef struct {
    int64_t a;
    int64_t b;
    int64_t c;
} Triple;

// Returned through a hidden pointer argument.
EXPORT Triple c_struct_return(int64_t x, int64_t y) {
    const Triple t = {x + y, x * 2 - y, (x ^ y) >> 2};
    return t;
}

// ---- Floating point -----------------------------------------------------------------------------

EXPORT double c_float_mix(double x, double y, int64_t n) {
    double r = x * y + (double)n;
    if (r > 1e6 || r < -1e6) {
        r = r / 1024.0;
    }
    const float f = (float)r;
    r += _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(__builtin_fabs(r))));
    if (x > -9.0e18 && x < 9.0e18) {
        r -= (double)(int64_t)(x * 0.5);
    }
    const double magnitude = __builtin_fabs(y);
    if (magnitude < 1.8e19) {
        r += (double)(uint64_t)magnitude;
    }
    r += (double)(uint64_t)n;
    r += (double)(uint32_t)n;
    return r * (double)f;
}

EXPORT float c_float_math(float a, float b, int32_t n) {
    float r = a * b - a / (b == 0.0f ? 1.0f : b);
    r = r < a ? a : r;
    r = r > 1e30f ? 1e30f : r;
    if (a > -2.0e9f && a < 2.0e9f) {
        r += (float)(int32_t)a;
    }
    r += (float)n * 0.25f;
    r += (float)(uint32_t)n;
    return r == r ? r : -1.0f;
}

EXPORT int32_t c_float_compare(double a, double b) {
    return (a < b) | (a <= b) << 1 | (a == b) << 2 | (a != b) << 3 | (a > b) << 4 | (a >= b) << 5 |
           (a != a) << 6;
}

// ---- Loops the compiler vectorizes --------------------------------------------------------------

EXPORT int64_t c_dot_i32(const int32_t* a, const int32_t* b, size_t n) {
    int64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += (int64_t)a[i] * b[i];
    }
    return sum;
}

EXPORT void c_saxpy(float a, const float* __restrict x, float* __restrict y, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        y[i] = a * x[i] + y[i];
    }
}

EXPORT void c_scale_f64(double* __restrict out, const double* __restrict in, size_t n,
                        double scale) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = in[i] * scale + (double)i;
    }
}

EXPORT uint32_t c_byte_sum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += p[i];
    }
    return sum;
}

EXPORT size_t c_count_if(const int32_t* v, size_t n, int32_t threshold) {
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        count += v[i] > threshold;
    }
    return count;
}

EXPORT void c_minmax(const int16_t* v, size_t n, int16_t* out) {
    int16_t lo = INT16_MAX;
    int16_t hi = INT16_MIN;
    for (size_t i = 0; i < n; ++i) {
        lo = v[i] < lo ? v[i] : lo;
        hi = v[i] > hi ? v[i] : hi;
    }
    out[0] = lo;
    out[1] = hi;
}

EXPORT void c_clamp_bytes(const int32_t* __restrict in, uint8_t* __restrict out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const int32_t v = in[i] >> 3;
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

EXPORT void c_widen(const uint16_t* __restrict in, int64_t* __restrict out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = (int64_t)in[i] * 3 - (int16_t)in[i];
    }
}

EXPORT void c_float_to_int(const float* __restrict in, int32_t* __restrict out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const float v = in[i];
        out[i] = (v > -1e9f && v < 1e9f) ? (int32_t)v : 0;
    }
}
