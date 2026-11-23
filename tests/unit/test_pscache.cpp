// shaderrecomp's reading of the console driver's pipeline cache (tools/shaderrecomp/src/pscache.h)
// on a cache built here: one graphics and one compute pipeline, four metadata blobs, and two
// shaders of made-up bytes.

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dxbc.h"
#include "g6test.h"
#include "pscache.h"

namespace sh = g6::shader;
namespace ps = g6::shader::pscache;

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}
void put64(Bytes& out, uint64_t value) {
    put32(out, static_cast<uint32_t>(value));
    put32(out, static_cast<uint32_t>(value >> 32));
}
void set32(Bytes& out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}
void put_words(Bytes& out, std::initializer_list<uint32_t> words) {
    for (const uint32_t word : words) {
        put32(out, word);
    }
}
// A 32-byte binding record: its tag and seven words, the first given.
void put_record(Bytes& out, uint32_t tag, uint32_t first) {
    put_words(out, {tag, first, 0, 0, 0, 0, 0, 0});
}

// Graphics bindings: a four-word header whose second word is the size from the last header word.
Bytes graphics_bindings() {
    Bytes out;
    put_words(out, {ps::kGraphicsBindings, 16 + 3 * 32 - 12, 0, 0});
    put_record(out, ps::kRecordResource, 0x300);
    put_record(out, ps::kRecord42, 0);
    put_record(out, ps::kRecord43, 0);
    return out;
}

// Compute bindings: a six-word header.
Bytes compute_bindings() {
    Bytes out;
    put_words(out, {ps::kComputeBindings, 24 + 32 - 20, 0, 0, 0, 0});
    put_record(out, ps::kRecord43, 0);
    return out;
}

constexpr uint32_t kGraphicsState = 0;
constexpr uint32_t kGraphicsDescription = 1;
constexpr uint32_t kGraphicsBindingsBlob = 2;
constexpr uint32_t kComputeBindingsBlob = 3;

Bytes sample_cache() {
    const std::array<Bytes, 4> blobs = {Bytes(8, 0xA0), Bytes(12, 0xA1), graphics_bindings(),
                                        compute_bindings()};
    const std::array<Bytes, 2> shaders = {Bytes(16, 0xC0), Bytes(12, 0xC1)};
    size_t metadata_size = 0;
    for (const Bytes& blob : blobs) {
        metadata_size += blob.size();
    }
    size_t code_size = 0;
    for (const Bytes& shader : shaders) {
        code_size += shader.size();
    }

    Bytes out(ps::kHeaderSize, 0);
    std::copy_n("PS\0\t", 4, out.begin());
    std::copy_n("0x12345678", 10, out.begin() + 0x04);
    std::copy_n("1.2.3", 5, out.begin() + 0x24);
    set32(out, 0x50, 1);
    set32(out, 0x54, 1);
    set32(out, 0x58, static_cast<uint32_t>(blobs.size()));
    set32(out, 0x5C, static_cast<uint32_t>(shaders.size()));
    set32(out, 0x60, static_cast<uint32_t>(metadata_size));
    set32(out, 0x64, static_cast<uint32_t>(code_size));

    // The graphics pipeline: pixel shader 1 in slot 0 and vertex shader 0 in slot 1.
    put64(out, 0x1111222233334444);
    put_words(out, {kGraphicsState, kGraphicsDescription});
    out.resize(out.size() + 0x28, 0x5A); // hashes
    put_words(out, {1, kGraphicsBindingsBlob, 0, ps::kNone});
    for (size_t slot = 2; slot < ps::kSlots; ++slot) {
        put_words(out, {ps::kNone, ps::kNone});
    }
    // The compute pipeline: shader 0 again.
    put64(out, 0x5555666677778888);
    put_words(out, {ps::kNone, ps::kNone, 0, kComputeBindingsBlob});

    uint32_t at = 0;
    for (const Bytes& blob : blobs) {
        put_words(out, {at, static_cast<uint32_t>(blob.size())});
        at += static_cast<uint32_t>(blob.size());
    }
    at = 0;
    for (const Bytes& shader : shaders) {
        put_words(out, {at, static_cast<uint32_t>(shader.size())});
        at += static_cast<uint32_t>(shader.size());
    }
    for (const Bytes& blob : blobs) {
        out.insert(out.end(), blob.begin(), blob.end());
    }
    for (const Bytes& shader : shaders) {
        out.insert(out.end(), shader.begin(), shader.end());
    }
    return out;
}

} // namespace

TEST_CASE("pscache parses the header, pipelines, and indexes") {
    const Bytes data = sample_cache();
    REQUIRE(ps::is_pipeline_cache(data));
    const ps::PipelineCache cache = ps::parse(data);
    CHECK_EQ(cache.id, std::string("0x12345678"));
    CHECK_EQ(cache.driver, std::string("1.2.3"));

    REQUIRE(cache.graphics.size() == 1);
    const ps::GraphicsPipeline& graphics = cache.graphics[0];
    CHECK_EQ(graphics.hash, uint64_t{0x1111222233334444});
    CHECK_EQ(graphics.state, kGraphicsState);
    CHECK_EQ(graphics.description, kGraphicsDescription);
    CHECK_EQ(graphics.slots[0].shader, uint32_t{1});
    CHECK_EQ(graphics.slots[0].bindings, kGraphicsBindingsBlob);
    CHECK_EQ(graphics.slots[1].shader, uint32_t{0});
    CHECK_EQ(graphics.slots[1].bindings, ps::kNone);
    CHECK_EQ(graphics.slots[6].shader, ps::kNone);

    REQUIRE(cache.compute.size() == 1);
    CHECK_EQ(cache.compute[0].hash, uint64_t{0x5555666677778888});
    CHECK_EQ(cache.compute[0].state, ps::kNone);
    CHECK_EQ(cache.compute[0].shader, uint32_t{0});
    CHECK_EQ(cache.compute[0].bindings, kComputeBindingsBlob);

    CHECK_EQ(cache.metadata.size(), size_t{4});
    CHECK_EQ(ps::metadata_blob(cache, kGraphicsDescription).size(), size_t{12});
    CHECK_EQ(ps::metadata_blob(cache, kGraphicsDescription)[0], uint8_t{0xA1});
    const std::span<const uint8_t> code = ps::shader_code(cache, 1);
    CHECK_EQ(code.size(), size_t{12});
    CHECK_EQ(code[0], uint8_t{0xC1});
    // The code area ends the file.
    CHECK(code.data() + code.size() == data.data() + data.size());
}

TEST_CASE("pscache reads binding blobs") {
    const Bytes data = sample_cache();
    const ps::PipelineCache cache = ps::parse(data);

    const std::optional<ps::Bindings> graphics =
        ps::read_bindings(ps::metadata_blob(cache, kGraphicsBindingsBlob));
    REQUIRE(graphics.has_value());
    CHECK_EQ(graphics->kind, ps::kGraphicsBindings);
    REQUIRE(graphics->records.size() == 3);
    CHECK_EQ(graphics->records[0].tag, ps::kRecordResource);
    CHECK_EQ(graphics->records[0].words[0], uint32_t{0x300});
    CHECK_EQ(graphics->records[2].tag, ps::kRecord43);

    const std::optional<ps::Bindings> compute =
        ps::read_bindings(ps::metadata_blob(cache, kComputeBindingsBlob));
    REQUIRE(compute.has_value());
    CHECK_EQ(compute->kind, ps::kComputeBindings);
    CHECK_EQ(compute->records.size(), size_t{1});

    // Blobs that aren't bindings, or whose size word or records don't fit.
    CHECK(!ps::read_bindings(ps::metadata_blob(cache, kGraphicsState)).has_value());
    Bytes blob = graphics_bindings();
    set32(blob, 4, 96);
    CHECK(!ps::read_bindings(blob).has_value());
    blob = graphics_bindings();
    set32(blob, 0, 3);
    CHECK(!ps::read_bindings(blob).has_value());
    blob = graphics_bindings();
    put32(blob, 0);
    set32(blob, 4, 104);
    CHECK(!ps::read_bindings(blob).has_value());
}

TEST_CASE("pscache rejects malformed caches") {
    Bytes data = sample_cache();
    data.push_back(0);
    CHECK_THROWS_AS(ps::parse(data), sh::FormatError);

    data = sample_cache();
    data[0] = 'Q';
    CHECK(!ps::is_pipeline_cache(data));
    CHECK_THROWS_AS(ps::parse(data), sh::FormatError);

    data = sample_cache();
    const ps::PipelineCache cache = ps::parse(data);
    CHECK_THROWS_AS(ps::shader_code(cache, 2), sh::FormatError);
    CHECK_THROWS_AS(ps::metadata_blob(cache, 4), sh::FormatError);

    // A shader whose range runs past the code area: the second shader index entry's size.
    const size_t shader_index =
        ps::kHeaderSize + ps::kGraphicsRecordSize + ps::kComputeRecordSize + 4 * 8;
    set32(data, shader_index + 8 + 4, 13);
    CHECK_THROWS_AS(ps::shader_code(ps::parse(data), 1), sh::FormatError);
}
