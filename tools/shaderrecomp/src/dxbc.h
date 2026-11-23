#pragma once

// DXBC shader containers and the DXIL program header, the public formats desktop D3D12 shaders use
// (documented by the DirectXShaderCompiler headers). The console's scene shaders come in the same
// container; what the console adds inside it is in console.h. Every read is bounds-checked, and
// malformed input throws FormatError.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace g6::shader {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A four-character code as it reads in memory: "DXIL" is 'D' in the low byte.
constexpr uint32_t fourcc(const char (&text)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(text[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24);
}

// "DXIL" for fourcc("DXIL"); bytes outside printable ASCII show as '?'.
std::string fourcc_text(uint32_t code);

uint32_t read_u32(std::span<const uint8_t> data, size_t offset);
uint16_t read_u16(std::span<const uint8_t> data, size_t offset);

struct DxbcPart {
    uint32_t fourcc = 0;
    std::span<const uint8_t> data;
};

// Header: "DXBC", a 16-byte digest, version 1.0, the total size, the part count, and an offset per
// part. Each part: its fourcc, its size, and its data.
struct Dxbc {
    std::array<uint8_t, 16> digest{};
    uint16_t major = 0;
    uint16_t minor = 0;
    std::vector<DxbcPart> parts;

    // The first part with this fourcc, or nullptr.
    const DxbcPart* find(uint32_t code) const;
};

Dxbc parse_dxbc(std::span<const uint8_t> container);

// Program kinds, from the high half of the DXIL program version.
enum class ProgramKind : uint16_t {
    kPixel = 0,
    kVertex = 1,
    kGeometry = 2,
    kHull = 3,
    kDomain = 4,
    kCompute = 5,
    kLibrary = 6,
    kMesh = 13,
    kAmplification = 14,
};

// "pixel", "vertex", ..., or "kind <n>" for a value with no name here.
std::string kind_name(uint16_t kind);

// The header at the start of a DXIL part: program version and size, then the bitcode header
// ("DXIL", the DXIL version, and the offset and size of the program's bitcode, counted from the
// start of the bitcode header).
struct DxilProgram {
    uint16_t kind = 0;
    uint8_t major = 0;
    uint8_t minor = 0;
    uint32_t dxil_version = 0;
    std::span<const uint8_t> bitcode;

    // LLVM bitcode, bare or in its wrapper.
    bool has_llvm_bitcode() const;
    // The four bytes "NOBC" in place of bitcode, as the console's containers carry.
    bool no_bitcode_marker() const;
};

// The size of the program header, and so where whatever follows it in the part begins.
inline constexpr size_t kDxilProgramHeaderSize = 24;

DxilProgram parse_dxil_program(std::span<const uint8_t> part);

// One element of a signature part (ISG1, OSG1, PSG1): the part holds a count and an offset, then
// 32-byte elements from that offset. Each element names its semantic by an offset to a string in
// the part, which isn't kept: it is text from the shader.
struct SignatureElement {
    uint32_t stream = 0;
    uint32_t semantic_index = 0;
    uint32_t system_value = 0; // 0 for none; kSystemValue* below
    uint32_t component_type = 0;
    uint32_t reg = 0;
    uint8_t mask = 0;    // the components the element has, x in bit 0
    uint8_t rw_mask = 0; // an input's components the shader reads; an output's it never writes
    uint32_t min_precision = 0;
};
inline constexpr uint32_t kSystemValueVertexId = 6;
inline constexpr uint32_t kSystemValueInstanceId = 8;

std::vector<SignatureElement> parse_signature(std::span<const uint8_t> part);
// The elements of a container's ISG1 part; empty when it has none.
std::vector<SignatureElement> input_signature(std::span<const uint8_t> container);

// A geometry shader's part of the PSV0 runtime info (DxilPipelineStateValidation.h in the
// DirectXShaderCompiler): after the part's first word, the size of the runtime info, come the
// input primitive, the output topology, the mask of streams it writes, and whether it writes a
// position.
struct PsvGeometry {
    uint32_t input_primitive = 0; // 1 point, 2 line, 3 triangle, ...
    uint32_t output_topology = 0; // kTopology* below
    uint32_t output_streams = 0;  // bit n: stream n
    bool position = false;
};
inline constexpr uint32_t kTopologyPointList = 1;
inline constexpr uint32_t kTopologyLineStrip = 3;
inline constexpr uint32_t kTopologyTriangleStrip = 5;
// Nothing when the container has no PSV0 part. The runtime info is a union over the stages, so
// what this reads is a geometry shader's only when the shader is one; the caller knows.
std::optional<PsvGeometry> psv_geometry(std::span<const uint8_t> container);

} // namespace g6::shader
