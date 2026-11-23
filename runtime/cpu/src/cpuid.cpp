#include "g6/cpu/cpuid.h"

#include <algorithm>
#include <immintrin.h>
#include <intrin.h>

namespace g6::cpu {
namespace {

constexpr uint32_t bit(unsigned n) {
    return uint32_t{1} << n;
}

// The masks list the features of AMD's Zen 2 as AMD documents them. A feature is reported only
// if both the host and Zen 2 have it, so guest code never takes a path the host can't run or
// the console never takes.

// Zen 2's highest basic leaf. Higher leaves describe features it doesn't have.
constexpr uint32_t kMaxBasicLeaf = 0x10;

// Leaf 1, ECX. Bit 31 (running under a hypervisor) is left out, so the hypervisor leaves
// below report nothing, consistently.
constexpr uint32_t kLeaf1Ecx = bit(0)     // SSE3
                               | bit(1)   // PCLMULQDQ
                               | bit(3)   // MONITOR/MWAIT
                               | bit(9)   // SSSE3
                               | bit(12)  // FMA
                               | bit(13)  // CMPXCHG16B
                               | bit(19)  // SSE4.1
                               | bit(20)  // SSE4.2
                               | bit(22)  // MOVBE
                               | bit(23)  // POPCNT
                               | bit(25)  // AES
                               | bit(26)  // XSAVE
                               | bit(27)  // OSXSAVE
                               | bit(28)  // AVX
                               | bit(29)  // F16C
                               | bit(30); // RDRAND

// Leaf 7, subleaf 0, EBX: no AVX-512, ERMS, or anything newer than Zen 2.
constexpr uint32_t kLeaf7Ebx = bit(0)     // FSGSBASE
                               | bit(3)   // BMI1
                               | bit(5)   // AVX2
                               | bit(7)   // SMEP
                               | bit(8)   // BMI2
                               | bit(12)  // platform QoS monitoring
                               | bit(15)  // platform QoS enforcement
                               | bit(18)  // RDSEED
                               | bit(19)  // ADX
                               | bit(20)  // SMAP
                               | bit(23)  // CLFLUSHOPT
                               | bit(24)  // CLWB
                               | bit(29); // SHA

// Leaf 7, subleaf 0, ECX. EDX has nothing Zen 2 reports there.
constexpr uint32_t kLeaf7Ecx = bit(2)     // UMIP
                               | bit(22); // RDPID

// Leaf 0xD: the register state components (x87, SSE, AVX) and the XSAVE variants.
constexpr uint32_t kXcr0 = bit(0) | bit(1) | bit(2);
constexpr uint32_t kXsaveVariants = bit(0)    // XSAVEOPT
                                    | bit(1)  // XSAVEC
                                    | bit(2)  // XGETBV with ecx = 1
                                    | bit(3); // XSAVES

// Leaf 0x80000001, ECX: the instruction-set and topology bits Zen 2 has. Bulldozer's XOP, FMA4,
// TBM, and LWP are left out, as are the system features user-mode code can't use.
constexpr uint32_t kLeaf80000001Ecx = bit(0)     // LAHF/SAHF in 64-bit mode
                                      | bit(1)   // core multi-processing legacy mode (topology)
                                      | bit(5)   // LZCNT
                                      | bit(6)   // SSE4A
                                      | bit(7)   // misaligned SSE mode
                                      | bit(8)   // PREFETCHW
                                      | bit(22)  // topology extensions
                                      | bit(29); // MONITORX/MWAITX

} // namespace

CpuidResult guest_cpuid(uint32_t leaf, uint32_t subleaf) {
    if (leaf > kMaxBasicLeaf && leaf < 0x80000000) {
        return {};
    }
    int r[4] = {};
    __cpuidex(r, static_cast<int>(leaf), static_cast<int>(subleaf));
    CpuidResult result{static_cast<uint32_t>(r[0]), static_cast<uint32_t>(r[1]),
                       static_cast<uint32_t>(r[2]), static_cast<uint32_t>(r[3])};
    switch (leaf) {
    case 0: result.eax = std::min(result.eax, kMaxBasicLeaf); break;
    case 1:
        // EDX holds the baseline features (x87 through SSE2) and passes through.
        result.ecx &= kLeaf1Ecx;
        break;
    case 7:
        if (subleaf != 0) {
            return {};
        }
        result.eax = 0; // no further subleaves
        result.ebx &= kLeaf7Ebx;
        result.ecx &= kLeaf7Ecx;
        result.edx = 0;
        break;
    case 0xD:
        if (subleaf == 0) {
            result.eax &= kXcr0;
            result.edx = 0;
        } else if (subleaf == 1) {
            result.eax &= kXsaveVariants;
            result.ecx = 0;
            result.edx = 0;
        } else if (subleaf > 2) {
            return {}; // components past AVX
        }
        break;
    case 0x80000001: result.ecx &= kLeaf80000001Ecx; break;
    default: break;
    }
    return result;
}

uint64_t guest_xgetbv(uint32_t index) {
    const uint64_t value = _xgetbv(index);
    return index <= 1 ? value & kXcr0 : value;
}

} // namespace g6::cpu
