#pragma once

// Shannon entropy of a byte string, in bits per byte: 0 for a run of one value, 8 for bytes spread
// evenly over all 256 values. x64 code sits near 6; compressed or encrypted data sits just under
// 8, which is how `x64recomp info` flags a section that is packed or encrypted on disk.

#include <cstdint>
#include <span>

namespace g6 {

double shannon_entropy(std::span<const uint8_t> data);

} // namespace g6
