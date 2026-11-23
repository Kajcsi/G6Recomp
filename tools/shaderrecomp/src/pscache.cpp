// The driver's pipeline cache (see pscache.h). Written from the layout RageForge reports and
// checked against the user's file (docs/shaders.md); no code is taken from it.

#include "pscache.h"

#include <cstring>
#include <string_view>

#include <fmt/format.h>

#include "dxbc.h"

namespace g6::shader::pscache {
namespace {

constexpr size_t kIdOffset = 0x04;
constexpr size_t kDriverOffset = 0x24;
constexpr size_t kTextSize = 0x20;
constexpr size_t kCountsOffset = 0x50;
constexpr size_t kSlotsOffset = 0x38;
constexpr size_t kRecordWords = 8;

uint64_t read_u64(std::span<const uint8_t> data, size_t offset) {
    return uint64_t{read_u32(data, offset)} | (uint64_t{read_u32(data, offset + 4)} << 32);
}

std::string text_at(std::span<const uint8_t> data, size_t offset) {
    const auto* begin = reinterpret_cast<const char*>(data.data() + offset);
    const size_t length = strnlen(begin, kTextSize);
    return {begin, length};
}

std::span<const uint8_t> range_in(std::span<const uint8_t> area, const Range& range,
                                  std::string_view what, uint32_t index) {
    if (uint64_t{range.offset} + range.size > area.size()) {
        throw FormatError(fmt::format("{} {} runs past its area", what, index));
    }
    return area.subspan(range.offset, range.size);
}

} // namespace

bool is_pipeline_cache(std::span<const uint8_t> data) {
    return data.size() >= 4 && std::memcmp(data.data(), "PS\0\t", 4) == 0;
}

PipelineCache parse(std::span<const uint8_t> data) {
    if (!is_pipeline_cache(data) || data.size() < kHeaderSize) {
        throw FormatError("not a pipeline cache");
    }
    const uint64_t graphics = read_u32(data, kCountsOffset);
    const uint64_t compute = read_u32(data, kCountsOffset + 4);
    const uint64_t metadata = read_u32(data, kCountsOffset + 8);
    const uint64_t shaders = read_u32(data, kCountsOffset + 12);
    const uint64_t metadata_size = read_u32(data, kCountsOffset + 16);
    const uint64_t code_size = read_u32(data, kCountsOffset + 20);

    const uint64_t compute_at = kHeaderSize + graphics * kGraphicsRecordSize;
    const uint64_t metadata_index_at = compute_at + compute * kComputeRecordSize;
    const uint64_t shader_index_at = metadata_index_at + metadata * sizeof(Range);
    const uint64_t metadata_at = shader_index_at + shaders * sizeof(Range);
    const uint64_t code_at = metadata_at + metadata_size;
    if (code_at + code_size != data.size()) {
        throw FormatError(fmt::format("the tables and areas end at {:#x}, the file at {:#x}",
                                      code_at + code_size, data.size()));
    }

    PipelineCache cache;
    cache.id = text_at(data, kIdOffset);
    cache.driver = text_at(data, kDriverOffset);
    cache.graphics.resize(graphics);
    for (size_t i = 0; i < graphics; ++i) {
        const size_t at = kHeaderSize + i * kGraphicsRecordSize;
        GraphicsPipeline& pipeline = cache.graphics[i];
        pipeline.hash = read_u64(data, at);
        pipeline.state = read_u32(data, at + 0x08);
        pipeline.description = read_u32(data, at + 0x0C);
        for (size_t slot = 0; slot < kSlots; ++slot) {
            pipeline.slots[slot] = {read_u32(data, at + kSlotsOffset + slot * 8),
                                    read_u32(data, at + kSlotsOffset + slot * 8 + 4)};
        }
    }
    cache.compute.resize(compute);
    for (size_t i = 0; i < compute; ++i) {
        const size_t at = compute_at + i * kComputeRecordSize;
        cache.compute[i] = {read_u64(data, at), read_u32(data, at + 0x08),
                            read_u32(data, at + 0x0C), read_u32(data, at + 0x10),
                            read_u32(data, at + 0x14)};
    }
    const auto read_ranges = [&](uint64_t at, uint64_t count) {
        std::vector<Range> ranges(count);
        for (size_t i = 0; i < count; ++i) {
            ranges[i] = {read_u32(data, at + i * 8), read_u32(data, at + i * 8 + 4)};
        }
        return ranges;
    };
    cache.metadata = read_ranges(metadata_index_at, metadata);
    cache.shaders = read_ranges(shader_index_at, shaders);
    cache.metadata_area = data.subspan(metadata_at, metadata_size);
    cache.code_area = data.subspan(code_at, code_size);
    return cache;
}

std::span<const uint8_t> metadata_blob(const PipelineCache& cache, uint32_t index) {
    if (index >= cache.metadata.size()) {
        throw FormatError(fmt::format("no metadata blob {}", index));
    }
    return range_in(cache.metadata_area, cache.metadata[index], "metadata blob", index);
}

std::span<const uint8_t> shader_code(const PipelineCache& cache, uint32_t index) {
    if (index >= cache.shaders.size()) {
        throw FormatError(fmt::format("no shader {}", index));
    }
    return range_in(cache.code_area, cache.shaders[index], "shader", index);
}

std::optional<uint32_t> shader_key(std::span<const uint8_t> code) {
    constexpr size_t kFirstLiteral = 8; // after the first v_nop's two words
    if (code.size() < kFirstLiteral + 4) {
        return std::nullopt;
    }
    return read_u32(code, kFirstLiteral);
}

std::optional<Bindings> read_bindings(std::span<const uint8_t> blob) {
    if (blob.size() < 16 || blob.size() % 4 != 0) {
        return std::nullopt;
    }
    const uint32_t kind = read_u32(blob, 0);
    const size_t header_words = kind == kGraphicsBindings ? 4 : kind == kComputeBindings ? 6 : 0;
    const size_t words = blob.size() / 4;
    if (header_words == 0 || words < header_words ||
        read_u32(blob, 4) != blob.size() - 4 * (header_words - 1) ||
        (words - header_words) % kRecordWords != 0) {
        return std::nullopt;
    }
    Bindings bindings;
    bindings.kind = kind;
    for (size_t at = header_words * 4; at < blob.size(); at += kRecordWords * 4) {
        BindingRecord record;
        record.tag = read_u32(blob, at);
        for (size_t i = 0; i < record.words.size(); ++i) {
            record.words[i] = read_u32(blob, at + 4 + i * 4);
        }
        bindings.records.push_back(record);
    }
    return bindings;
}

} // namespace g6::shader::pscache
