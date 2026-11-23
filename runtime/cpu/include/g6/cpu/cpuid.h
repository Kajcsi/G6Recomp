#pragma once

// What `cpuid` and `xgetbv` report to guest code: the host's answer, limited to what the
// console's CPU (Zen 2) also supports. Guest code picks its code paths from these, and only
// the paths a Zen 2 can take are known to translate; an AVX-512 path, for example, would stop
// at its first untranslated instruction. See docs/codegen.md.

#include <cstdint>

namespace g6::cpu {

struct CpuidResult {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
};

// `cpuid` with eax = leaf and ecx = subleaf.
CpuidResult guest_cpuid(uint32_t leaf, uint32_t subleaf);

// `xgetbv` with ecx = index: the enabled register state, limited to x87, SSE, and AVX.
uint64_t guest_xgetbv(uint32_t index);

} // namespace g6::cpu
