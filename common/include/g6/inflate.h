#pragma once

// DEFLATE (RFC 1951) and zlib (RFC 1950) decoding, written from the RFCs. The console decompresses
// both in hardware, so runtime/io will need them for the DirectStorage request path, and
// shaderrecomp reads the game's shader archive with them (docs/shaders.md). Malformed or
// truncated input throws InflateError; output never grows past the limit the caller gives.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace g6 {

class InflateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Inflated {
    std::vector<uint8_t> data;
    // Input bytes the stream occupied, up to and including the byte that holds its last bit (and,
    // for zlib, the Adler-32 trailer). Anything after that belongs to the caller.
    size_t consumed = 0;
};

// A raw DEFLATE stream: blocks up to and including the one marked final. Throws InflateError when
// the stream is malformed, ends early, or would decode to more than `max_output` bytes.
Inflated inflate_raw(std::span<const uint8_t> input, size_t max_output);

// A zlib stream: the two-byte header (method 8, no preset dictionary), a DEFLATE stream, and its
// Adler-32, which must match.
Inflated inflate_zlib(std::span<const uint8_t> input, size_t max_output);

// Adler-32 of `data`, continuing from `adler` (1 starts a new checksum).
uint32_t adler32(std::span<const uint8_t> data, uint32_t adler = 1);

} // namespace g6
