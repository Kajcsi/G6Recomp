// Shannon entropy (common/include/g6/entropy.h) on byte strings whose entropy is known exactly.

#include <cmath>
#include <cstdint>
#include <vector>

#include "g6/entropy.h"
#include "g6test.h"

namespace {

bool near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

} // namespace

TEST_CASE("entropy of uniform and single-valued data") {
    CHECK(near(g6::shannon_entropy({}), 0.0));
    const std::vector<uint8_t> run(1000, 0xCC);
    CHECK(near(g6::shannon_entropy(run), 0.0));

    std::vector<uint8_t> every_byte;
    every_byte.reserve(256 * 4);
    for (int i = 0; i < 256 * 4; ++i) {
        every_byte.push_back(static_cast<uint8_t>(i));
    }
    CHECK(near(g6::shannon_entropy(every_byte), 8.0));
}

TEST_CASE("entropy of a few equally likely values is log2 of their count") {
    const std::vector<uint8_t> two = {1, 2, 1, 2, 2, 1};
    CHECK(near(g6::shannon_entropy(two), 1.0));
    const std::vector<uint8_t> four = {9, 8, 7, 6, 6, 7, 8, 9};
    CHECK(near(g6::shannon_entropy(four), 2.0));
}

TEST_CASE("entropy of a skewed distribution") {
    // p = 3/4 and 1/4: 0.75 log2(4/3) + 0.25 log2(4) = 0.811278...
    const std::vector<uint8_t> skewed = {0, 0, 0, 1};
    CHECK(near(g6::shannon_entropy(skewed), (0.75 * std::log2(4.0 / 3.0)) + 0.5));
}
