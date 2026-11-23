#pragma once

// The console driver's pipeline cache in shaders.rpf (docs/shaders.md): a file that starts with
// `PS\0\t` and holds, for every pipeline the game ships precompiled, its shaders as final machine
// code and per-shader binding tables. RageForge reported the layout; the parts used here are
// checked against the user's file. Malformed input throws FormatError.
//
// Layout: a 0x80-byte header, then graphics pipeline records (0x70 bytes each), compute pipeline
// records (0x18 bytes), the metadata index and the shader index (an offset and a size each,
// relative to their areas), the metadata area, and the code area, which runs to the end.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace g6::shader::pscache {

inline constexpr uint32_t kNone = 0xFFFFFFFF;
inline constexpr size_t kSlots = 7;
inline constexpr size_t kHeaderSize = 0x80;
inline constexpr size_t kGraphicsRecordSize = 0x70;
inline constexpr size_t kComputeRecordSize = 0x18;

struct Range {
    uint32_t offset = 0;
    uint32_t size = 0;
};

// One shader of a pipeline and the metadata blob that holds its bindings.
struct Slot {
    uint32_t shader = kNone;
    uint32_t bindings = kNone;
};

struct GraphicsPipeline {
    uint64_t hash = 0;
    uint32_t state = kNone;       // metadata: register state
    uint32_t description = kNone; // metadata: starts with a hash; names the input layout, if any
    std::array<Slot, kSlots> slots{};
};

struct ComputePipeline {
    uint64_t hash = 0;
    uint32_t state = kNone;       // metadata: register state, thread-group size included
    uint32_t description = kNone; // metadata: starts with a hash
    uint32_t shader = kNone;
    uint32_t bindings = kNone; // metadata
};

struct PipelineCache {
    std::string id;     // 0x04, text
    std::string driver; // 0x24, the driver version, text
    std::vector<GraphicsPipeline> graphics;
    std::vector<ComputePipeline> compute;
    std::vector<Range> metadata;
    std::vector<Range> shaders;
    std::span<const uint8_t> metadata_area;
    std::span<const uint8_t> code_area;
};

bool is_pipeline_cache(std::span<const uint8_t> data);

// Parses the header and tables. The spans point into `data`, which must outlive the result.
PipelineCache parse(std::span<const uint8_t> data);

// A metadata blob or a shader's code; throws FormatError when the index or range is bad.
std::span<const uint8_t> metadata_blob(const PipelineCache& cache, uint32_t index);
std::span<const uint8_t> shader_code(const PipelineCache& cache, uint32_t index);

// What ties a cache shader to the shader records it was compiled from: the code opens with three
// v_nop instructions that carry 32-bit literals, and the first literal equals the key of the
// matching records' native programs (fssm_key in console.h). Nothing when the code is too short.
std::optional<uint32_t> shader_key(std::span<const uint8_t> code);

// A binding blob: a header (four words for a graphics shader, six for a compute shader), then
// 32-byte records. The second header word is the blob's size counted from the last header word.
inline constexpr uint32_t kGraphicsBindings = 2;
inline constexpr uint32_t kComputeBindings = 0x403;
inline constexpr uint32_t kRecordResource = 0x41; // word 1: the resource kind
inline constexpr uint32_t kRecord42 = 0x42;
inline constexpr uint32_t kRecord43 = 0x43;

struct BindingRecord {
    uint32_t tag = 0;
    std::array<uint32_t, 7> words{};
};

struct Bindings {
    uint32_t kind = 0; // kGraphicsBindings or kComputeBindings
    std::vector<BindingRecord> records;
};

// The blob's records, or nothing when it isn't a binding blob whose size word and records fit.
std::optional<Bindings> read_bindings(std::span<const uint8_t> blob);

} // namespace g6::shader::pscache
