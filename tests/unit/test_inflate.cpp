// DEFLATE and zlib decoding (common/include/g6/inflate.h). The compressed vectors were made with
// Python's zlib from the original text each test names; the long-code block is built bit by bit
// here, since zlib rarely emits codes longer than the decoder's lookup table.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "g6/inflate.h"
#include "g6test.h"

namespace {

std::string as_text(const std::vector<uint8_t>& data) {
    return {data.begin(), data.end()};
}

std::span<const uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// Fields go least significant bit first; Huffman codes most significant bit first (RFC 1951
// 3.1.1).
class BitWriter {
public:
    void field(uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            bit((value >> i) & 1);
        }
    }
    void code(uint32_t value, unsigned count) {
        for (unsigned i = count; i > 0; --i) {
            bit((value >> (i - 1)) & 1);
        }
    }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    void bit(uint32_t value) {
        if (used_ % 8 == 0) {
            data_.push_back(0);
        }
        data_.back() |= static_cast<uint8_t>(value << (used_ % 8));
        ++used_;
    }

    std::vector<uint8_t> data_;
    size_t used_ = 0;
};

// The 600-word text the dynamic Huffman vector compresses.
std::string word_text() {
    constexpr std::array<std::string_view, 12> kWords = {"native", "loader",  "guest",   "host",
                                                         "import", "policy",  "thread",  "fiber",
                                                         "shader", "archive", "deflate", "huffman"};
    std::string text;
    for (size_t i = 0; i < 600; ++i) {
        if (i != 0) {
            text += ' ';
        }
        text += kWords[(i * i + 3 * i) % kWords.size()];
    }
    return text;
}

constexpr std::array<uint8_t, 15> kFixed = {0x73, 0x37, 0x0b, 0x4a, 0x4d, 0xce, 0xcf, 0x2d,
                                            0x50, 0x70, 0x47, 0x67, 0x28, 0x02, 0x00};

constexpr std::array<uint8_t, 81> kDynamicZlib = {
    0x78, 0xda, 0xed, 0xcc, 0xc1, 0x09, 0xc0, 0x30, 0x0c, 0x04, 0xc1, 0x56, 0xd4, 0x9a,
    0xc0, 0x32, 0x36, 0x24, 0x8a, 0x31, 0xc2, 0xf5, 0xe7, 0x11, 0xa9, 0x88, 0xc0, 0xbe,
    0x0e, 0x96, 0x63, 0x5c, 0x63, 0x1e, 0x93, 0x79, 0xaf, 0x67, 0x87, 0x34, 0xeb, 0x97,
    0x86, 0x49, 0x8c, 0x6d, 0xda, 0xaa, 0xe6, 0x64, 0xac, 0x4f, 0x56, 0xff, 0x80, 0xaa,
    0xb5, 0x8e, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b, 0x8b,
    0x8b, 0x8b, 0x8b, 0xfb, 0x4f, 0xf7, 0x05, 0x05, 0xf2, 0x7e, 0x57};

// Two compressed blocks and the empty stored block of a sync flush between them; the second block
// copies from the first.
constexpr std::array<uint8_t, 68> kMultiBlock = {
    0x2a, 0xc9, 0x48, 0x55, 0x48, 0xcb, 0x2c, 0x2a, 0x2e, 0x51, 0x48, 0xca, 0xc9, 0x4f,
    0xce, 0x56, 0xc8, 0x4b, 0xcc, 0x4d, 0x2d, 0x56, 0x48, 0x04, 0x8a, 0x25, 0xa5, 0x16,
    0x29, 0x24, 0xe6, 0xa5, 0x00, 0xd9, 0x25, 0x19, 0x45, 0xa9, 0x89, 0x29, 0xd6, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x2b, 0x01, 0xaa, 0x2d, 0x4e, 0x4d, 0xce, 0x07,
    0x0a, 0x12, 0x52, 0xac, 0x90, 0x98, 0x9e, 0x98, 0x99, 0xa7, 0x07, 0x00};

constexpr std::array<uint8_t, 27> kStored = {0x01, 0x16, 0x00, 0xe9, 0xff, 0x73, 0x74, 0x6f, 0x72,
                                             0x65, 0x64, 0x2c, 0x20, 0x6e, 0x6f, 0x74, 0x20, 0x63,
                                             0x6f, 0x6d, 0x70, 0x72, 0x65, 0x73, 0x73, 0x65, 0x64};

// 300 times 'a': one literal, then copies at distance 1 that overlap what they produce.
constexpr std::array<uint8_t, 7> kRun = {0x4b, 0x4c, 0x1c, 0x05, 0xc4, 0x02, 0x00};

template <size_t N>
std::span<const uint8_t> span_of(const std::array<uint8_t, N>& data) {
    return data;
}

} // namespace

TEST_CASE("inflate decodes a fixed Huffman block") {
    const g6::Inflated out = g6::inflate_raw(span_of(kFixed), 1024);
    CHECK_EQ(as_text(out.data), std::string("G6Recomp G6Recomp G6Recomp!"));
    CHECK_EQ(out.consumed, kFixed.size());
}

TEST_CASE("inflate decodes a dynamic Huffman block in a zlib stream") {
    const g6::Inflated out = g6::inflate_zlib(span_of(kDynamicZlib), 1 << 20);
    CHECK_EQ(as_text(out.data), word_text());
    CHECK_EQ(out.consumed, kDynamicZlib.size());
}

TEST_CASE("inflate copies across blocks and reads stored blocks") {
    const g6::Inflated multi = g6::inflate_raw(span_of(kMultiBlock), 1024);
    CHECK_EQ(as_text(multi.data),
             std::string("the first block names a fiber and a thread; the second block names a "
                         "fiber and a thread again."));
    const g6::Inflated stored = g6::inflate_raw(span_of(kStored), 1024);
    CHECK_EQ(as_text(stored.data), std::string("stored, not compressed"));
}

TEST_CASE("inflate expands overlapping copies") {
    const g6::Inflated out = g6::inflate_raw(span_of(kRun), 1024);
    CHECK_EQ(as_text(out.data), std::string(300, 'a'));
}

TEST_CASE("inflate decodes codes longer than its lookup table") {
    // A dynamic block whose literal code gives 'A' to 'N' lengths 1 to 14 and 'O' and end-of-block
    // length 15, a complete code. The code length code gives each length it uses 4 bits.
    BitWriter w;
    w.field(1, 1);  // final block
    w.field(2, 2);  // dynamic Huffman
    w.field(0, 5);  // 257 literal/length codes
    w.field(0, 5);  // 1 distance code
    w.field(15, 4); // all 19 code length code lengths follow
    constexpr std::array<int, 19> kOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                            11, 4,  12, 3, 13, 2, 14, 1, 15};
    for (const int symbol : kOrder) {
        w.field(symbol == 0 || symbol == 16 || symbol == 17 ? 0 : 4, 3);
    }
    // Code length code: symbols 1-15 have codes 0-14 and symbol 18 (a run of zeros) code 15.
    const auto length = [&](unsigned value) { w.code(value - 1, 4); };
    const auto zeros = [&](unsigned count) {
        w.code(15, 4);
        w.field(count - 11, 7);
    };
    zeros(65); // literals 0-64
    for (unsigned value = 1; value <= 15; ++value) {
        length(value); // 'A' (65) to 'O' (79)
    }
    zeros(138); // literals 80-217
    zeros(38);  // literals 218-255
    length(15); // end of block
    length(1);  // the one distance code
    // Canonical codes: length k < 15 is k-1 ones and a zero; the two of length 15 are 15 bits of
    // ones ending in 0 ('O') and in 1 (end of block).
    const std::string_view text = "ABCDEFGHIJKLMNOONMLKJIHGFEDCBA";
    for (const char c : text) {
        const unsigned k = static_cast<unsigned>(c - 'A') + 1;
        w.code((1u << k) - 2, k);
    }
    w.code((1u << 15) - 1, 15);

    const g6::Inflated out = g6::inflate_raw(w.data(), 1024);
    CHECK_EQ(as_text(out.data), std::string(text));
    CHECK_EQ(out.consumed, w.data().size());
}

TEST_CASE("inflate reports how much input the stream used") {
    std::vector<uint8_t> input(kFixed.begin(), kFixed.end());
    input.insert(input.end(), {0xAA, 0xBB, 0xCC});
    CHECK_EQ(g6::inflate_raw(input, 1024).consumed, kFixed.size());
    std::vector<uint8_t> zlib(kDynamicZlib.begin(), kDynamicZlib.end());
    zlib.push_back(0x5A);
    CHECK_EQ(g6::inflate_zlib(zlib, 1 << 20).consumed, kDynamicZlib.size());
}

TEST_CASE("inflate rejects malformed streams") {
    // Block type 3 is reserved.
    CHECK_THROWS_AS(g6::inflate_raw(std::array<uint8_t, 1>{0x07}, 1024), g6::InflateError);
    // A fixed block whose first symbol copies from distance 1 before anything was written.
    CHECK_THROWS_AS(g6::inflate_raw(std::array<uint8_t, 3>{0x03, 0x02, 0x00}, 1024),
                    g6::InflateError);
    // Cut short.
    CHECK_THROWS_AS(g6::inflate_raw(span_of(kDynamicZlib).subspan(2, 30), 1 << 20),
                    g6::InflateError);
    CHECK_THROWS_AS(g6::inflate_raw(span_of(kStored).first(10), 1024), g6::InflateError);
    // A stored block whose length and complement disagree.
    std::array<uint8_t, kStored.size()> stored = kStored;
    stored[3] ^= 1;
    CHECK_THROWS_AS(g6::inflate_raw(stored, 1024), g6::InflateError);
    // More output than allowed.
    CHECK_THROWS_AS(g6::inflate_raw(span_of(kRun), 299), g6::InflateError);
    CHECK_EQ(g6::inflate_raw(span_of(kRun), 300).data.size(), size_t{300});
}

TEST_CASE("inflate_zlib checks the header and the Adler-32") {
    std::array<uint8_t, kDynamicZlib.size()> bad_sum = kDynamicZlib;
    bad_sum.back() ^= 1;
    CHECK_THROWS_AS(g6::inflate_zlib(bad_sum, 1 << 20), g6::InflateError);
    std::array<uint8_t, kDynamicZlib.size()> bad_check = kDynamicZlib;
    bad_check[1] ^= 1;
    CHECK_THROWS_AS(g6::inflate_zlib(bad_check, 1 << 20), g6::InflateError);
    CHECK_THROWS_AS(g6::inflate_zlib(span_of(kDynamicZlib).first(kDynamicZlib.size() - 2), 1 << 20),
                    g6::InflateError);
}

TEST_CASE("adler32 matches the published example") {
    CHECK_EQ(g6::adler32(bytes("Wikipedia")), uint32_t{0x11E60398});
    // Continuing a checksum gives the same result as one pass.
    const uint32_t first = g6::adler32(bytes("Wiki"));
    CHECK_EQ(g6::adler32(bytes("pedia"), first), uint32_t{0x11E60398});
    CHECK_EQ(g6::adler32(bytes(word_text())), uint32_t{0x05f27e57});
}
