// Shannon entropy (see entropy.h).

#include "g6/entropy.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace g6 {

double shannon_entropy(std::span<const uint8_t> data) {
    if (data.empty()) {
        return 0.0;
    }
    std::array<size_t, 256> counts{};
    for (const uint8_t byte : data) {
        ++counts[byte];
    }
    const auto total = static_cast<double>(data.size());
    double bits = 0.0;
    for (const size_t count : counts) {
        if (count != 0) {
            const double p = static_cast<double>(count) / total;
            bits -= p * std::log2(p);
        }
    }
    return bits;
}

} // namespace g6
