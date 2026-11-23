// DEFLATE and zlib decoding from RFC 1951 and RFC 1950 (see inflate.h).
//
// Huffman codes decode through a table indexed by the next kFastBits input bits, which holds every
// code that short. Longer codes are rare, and fall back to walking the canonical code one bit at
// a time.

#include "g6/inflate.h"

#include <algorithm>
#include <array>

namespace g6 {
namespace {

constexpr unsigned kMaxBits = 15;
constexpr unsigned kFastBits = 9;
constexpr size_t kLitLenSymbols = 288; // 286 used, two more in the fixed code
constexpr size_t kDistSymbols = 30;
constexpr size_t kCodeLengthSymbols = 19;
constexpr size_t kInitialReserve = 1u << 20;

constexpr std::array<uint16_t, 29> kLengthBase = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                  15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                  67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<uint16_t, kDistSymbols> kDistBase = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, kDistSymbols> kDistExtra = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                          4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                          9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order code length code lengths arrive in (RFC 1951 3.2.7).
constexpr std::array<uint8_t, kCodeLengthSymbols> kCodeLengthOrder = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// Input bits, least significant first (RFC 1951 3.1.1). Reading past the end supplies zero bytes;
// a stream that actually uses them is rejected at the next refill or at the end.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> input) : input_(input) {}

    uint32_t peek(unsigned count) {
        if (available_ < count) {
            refill();
        }
        return static_cast<uint32_t>(buffer_ & ((uint64_t{1} << count) - 1));
    }
    void skip(unsigned count) {
        buffer_ >>= count;
        available_ -= count;
    }
    uint32_t read(unsigned count) {
        const uint32_t value = peek(count);
        skip(count);
        return value;
    }

    // Drops the bits up to the next byte boundary.
    void align() { skip(available_ % 8); }

    // Copies `count` bytes to `out`. Only on a byte boundary, after align().
    void copy_bytes(size_t count, std::vector<uint8_t>& out) {
        if (used_bits() / 8 + count > input_.size()) {
            throw InflateError("stored block runs past the end of the input");
        }
        for (; count > 0 && available_ >= 8; --count) {
            out.push_back(static_cast<uint8_t>(read(8)));
        }
        out.insert(out.end(), input_.begin() + static_cast<ptrdiff_t>(position_),
                   input_.begin() + static_cast<ptrdiff_t>(position_ + count));
        position_ += count;
    }

    // Bytes the stream occupied, counting a partly used last byte.
    size_t finish() const {
        const uint64_t bits = used_bits();
        if (bits > uint64_t{input_.size()} * 8) {
            throw InflateError("stream ends early");
        }
        return static_cast<size_t>((bits + 7) / 8);
    }

private:
    uint64_t used_bits() const { return (uint64_t{position_} * 8) - available_; }

    void refill() {
        if (used_bits() > uint64_t{input_.size()} * 8) {
            throw InflateError("stream ends early");
        }
        while (available_ <= 56) {
            const uint64_t byte = position_ < input_.size() ? input_[position_] : 0;
            ++position_;
            buffer_ |= byte << available_;
            available_ += 8;
        }
    }

    std::span<const uint8_t> input_;
    size_t position_ = 0; // next byte to load; runs past the end while zero bytes pad the buffer
    uint64_t buffer_ = 0;
    unsigned available_ = 0;
};

// A canonical Huffman code (RFC 1951 3.2.2).
struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};    // codes of each length
    std::array<uint16_t, kLitLenSymbols> symbol{}; // symbols ordered by code
    std::array<uint16_t, 1u << kFastBits> fast{};  // (symbol << 4) | length, or 0 if longer
};

uint32_t reverse_bits(uint32_t value, unsigned count) {
    uint32_t reversed = 0;
    for (unsigned i = 0; i < count; ++i) {
        reversed = (reversed << 1) | ((value >> i) & 1);
    }
    return reversed;
}

// Builds the code for per-symbol lengths (0 = unused) and returns how many codes of the longest
// length are left unassigned: 0 for a complete code. Over-subscribed lengths are malformed. An
// incomplete code is accepted; a bit pattern it doesn't cover fails when it's decoded.
int build(Huffman& code, std::span<const uint8_t> lengths) {
    code.count.fill(0);
    for (const uint8_t length : lengths) {
        ++code.count[length];
    }
    code.count[0] = 0;
    int left = 1;
    for (unsigned length = 1; length <= kMaxBits; ++length) {
        left = (left << 1) - code.count[length];
        if (left < 0) {
            throw InflateError("over-subscribed Huffman code");
        }
    }

    std::array<uint16_t, kMaxBits + 2> offset{};
    for (unsigned length = 1; length <= kMaxBits; ++length) {
        offset[length + 1] = static_cast<uint16_t>(offset[length] + code.count[length]);
    }
    for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        if (lengths[symbol] != 0) {
            code.symbol[offset[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
        }
    }

    // The first bit of a code is its most significant, and arrives first, so the table is indexed
    // by the code reversed.
    code.fast.fill(0);
    uint32_t next = 0;
    size_t index = 0;
    for (unsigned length = 1; length <= kFastBits; ++length) {
        for (unsigned i = 0; i < code.count[length]; ++i, ++next, ++index) {
            const auto entry = static_cast<uint16_t>((code.symbol[index] << 4) | length);
            for (uint32_t slot = reverse_bits(next, length); slot < (1u << kFastBits);
                 slot += 1u << length) {
                code.fast[slot] = entry;
            }
        }
        next <<= 1;
    }
    return left;
}

uint32_t decode(BitReader& in, const Huffman& code) {
    const uint32_t bits = in.peek(kMaxBits);
    const uint16_t entry = code.fast[bits & ((1u << kFastBits) - 1)];
    if (entry != 0) {
        in.skip(entry & 15u);
        return entry >> 4;
    }
    // Codes of a given length are consecutive integers, so walk the lengths: `first` is the
    // first code of the current length and `index` the position of its symbol.
    int value = 0;
    int first = 0;
    int index = 0;
    for (unsigned length = 1; length <= kMaxBits; ++length) {
        value |= static_cast<int>((bits >> (length - 1)) & 1);
        const int count = code.count[length];
        if (value - first < count) {
            in.skip(length);
            return code.symbol[static_cast<size_t>(index + value - first)];
        }
        index += count;
        first = (first + count) << 1;
        value <<= 1;
    }
    throw InflateError("invalid Huffman code");
}

struct FixedCodes {
    Huffman literal;
    Huffman distance;

    FixedCodes() {
        std::array<uint8_t, kLitLenSymbols> lengths{};
        std::fill(lengths.begin(), lengths.begin() + 144, uint8_t{8});
        std::fill(lengths.begin() + 144, lengths.begin() + 256, uint8_t{9});
        std::fill(lengths.begin() + 256, lengths.begin() + 280, uint8_t{7});
        std::fill(lengths.begin() + 280, lengths.end(), uint8_t{8});
        build(literal, lengths);
        std::array<uint8_t, kDistSymbols> distances{};
        distances.fill(5);
        build(distance, distances);
    }
};

const FixedCodes& fixed_codes() {
    static const FixedCodes codes;
    return codes;
}

void check_room(const std::vector<uint8_t>& out, size_t more, size_t max_output) {
    if (more > max_output - out.size()) {
        throw InflateError("output is larger than the limit");
    }
}

// The literal/length and distance symbols of one compressed block, through end-of-block.
void decode_block(BitReader& in, const Huffman& literal, const Huffman& distance,
                  std::vector<uint8_t>& out, size_t max_output) {
    for (;;) {
        uint32_t symbol = decode(in, literal);
        if (symbol < 256) {
            check_room(out, 1, max_output);
            out.push_back(static_cast<uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) {
            return;
        }
        symbol -= 257;
        if (symbol >= kLengthBase.size()) {
            throw InflateError("invalid length symbol");
        }
        const size_t length = kLengthBase[symbol] + in.read(kLengthExtra[symbol]);
        symbol = decode(in, distance);
        if (symbol >= kDistBase.size()) {
            throw InflateError("invalid distance symbol");
        }
        const size_t back = kDistBase[symbol] + in.read(kDistExtra[symbol]);
        if (back > out.size()) {
            throw InflateError("distance reaches before the start of the output");
        }
        check_room(out, length, max_output);
        // Byte by byte: a copy may overlap the bytes it produces (a run when back < length).
        const size_t at = out.size();
        out.resize(at + length);
        for (size_t i = 0; i < length; ++i) {
            out[at + i] = out[at - back + i];
        }
    }
}

void decode_stored(BitReader& in, std::vector<uint8_t>& out, size_t max_output) {
    in.align();
    const uint32_t length = in.read(16);
    const uint32_t complement = in.read(16);
    if (length != (~complement & 0xFFFF)) {
        throw InflateError("stored block length doesn't match its complement");
    }
    check_room(out, length, max_output);
    in.copy_bytes(length, out);
}

void decode_dynamic(BitReader& in, std::vector<uint8_t>& out, size_t max_output) {
    const size_t literals = in.read(5) + 257;
    const size_t distances = in.read(5) + 1;
    const size_t code_lengths = in.read(4) + 4;
    if (literals > 286 || distances > kDistSymbols) {
        throw InflateError("too many length or distance symbols");
    }

    std::array<uint8_t, kCodeLengthSymbols> order_lengths{};
    for (size_t i = 0; i < code_lengths; ++i) {
        order_lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(in.read(3));
    }
    Huffman length_code;
    if (build(length_code, order_lengths) != 0) {
        throw InflateError("incomplete code length code");
    }

    std::array<uint8_t, 286 + kDistSymbols> lengths{};
    const size_t total = literals + distances;
    for (size_t index = 0; index < total;) {
        const uint32_t symbol = decode(in, length_code);
        if (symbol < 16) {
            lengths[index++] = static_cast<uint8_t>(symbol);
            continue;
        }
        uint8_t repeated = 0;
        size_t times = 0;
        if (symbol == 16) {
            if (index == 0) {
                throw InflateError("length repeat with no previous length");
            }
            repeated = lengths[index - 1];
            times = 3 + in.read(2);
        } else if (symbol == 17) {
            times = 3 + in.read(3);
        } else {
            times = 11 + in.read(7);
        }
        if (index + times > total) {
            throw InflateError("code lengths run past the symbol count");
        }
        std::fill_n(lengths.begin() + static_cast<ptrdiff_t>(index), times, repeated);
        index += times;
    }
    if (lengths[256] == 0) {
        throw InflateError("no end-of-block code");
    }

    Huffman literal;
    Huffman distance;
    build(literal, std::span(lengths).first(literals));
    build(distance, std::span(lengths).subspan(literals, distances));
    decode_block(in, literal, distance, out, max_output);
}

} // namespace

Inflated inflate_raw(std::span<const uint8_t> input, size_t max_output) {
    BitReader in(input);
    Inflated result;
    // The input may run far past the stream (a caller probing an archive passes everything after
    // a candidate header), so the first reservation is capped and the output grows as it decodes.
    result.data.reserve(std::min({max_output, input.size() * 4, kInitialReserve}));
    bool last = false;
    while (!last) {
        last = in.read(1) != 0;
        switch (in.read(2)) {
        case 0: decode_stored(in, result.data, max_output); break;
        case 1:
            decode_block(in, fixed_codes().literal, fixed_codes().distance, result.data,
                         max_output);
            break;
        case 2: decode_dynamic(in, result.data, max_output); break;
        default: throw InflateError("invalid block type");
        }
    }
    result.consumed = in.finish();
    return result;
}

Inflated inflate_zlib(std::span<const uint8_t> input, size_t max_output) {
    if (input.size() < 2) {
        throw InflateError("zlib header missing");
    }
    const uint8_t method = input[0];
    const uint8_t flags = input[1];
    if ((method & 0x0F) != 8) {
        throw InflateError("zlib stream isn't DEFLATE");
    }
    if ((method >> 4) > 7) {
        throw InflateError("zlib window size is out of range");
    }
    if (((method << 8) | flags) % 31 != 0) {
        throw InflateError("zlib header check fails");
    }
    if ((flags & 0x20) != 0) {
        throw InflateError("zlib preset dictionaries aren't supported");
    }
    Inflated result = inflate_raw(input.subspan(2), max_output);
    const size_t trailer = 2 + result.consumed;
    if (input.size() - trailer < 4) {
        throw InflateError("zlib Adler-32 missing");
    }
    const uint32_t expected = (uint32_t{input[trailer]} << 24) |
                              (uint32_t{input[trailer + 1]} << 16) |
                              (uint32_t{input[trailer + 2]} << 8) | input[trailer + 3];
    if (adler32(result.data) != expected) {
        throw InflateError("zlib Adler-32 doesn't match");
    }
    result.consumed = trailer + 4;
    return result;
}

uint32_t adler32(std::span<const uint8_t> data, uint32_t adler) {
    constexpr uint32_t kModulus = 65521;
    // The most bytes that can be summed before `b` could overflow 32 bits (RFC 1950 9.).
    constexpr size_t kRun = 5552;
    uint32_t a = adler & 0xFFFF;
    uint32_t b = adler >> 16;
    for (size_t i = 0; i < data.size();) {
        const size_t end = std::min(data.size(), i + kRun);
        for (; i < end; ++i) {
            a += data[i];
            b += a;
        }
        a %= kModulus;
        b %= kModulus;
    }
    return (b << 16) | a;
}

} // namespace g6
