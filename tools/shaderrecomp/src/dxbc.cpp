// DXBC containers and DXIL program headers (see dxbc.h).

#include "dxbc.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace g6::shader {

std::string fourcc_text(uint32_t code) {
    std::string text;
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<char>((code >> (8 * i)) & 0xFF);
        text += c >= 0x20 && c < 0x7F ? c : '?';
    }
    return text;
}

uint32_t read_u32(std::span<const uint8_t> data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) {
        throw FormatError(
            fmt::format("read of 4 bytes at {:#x} is past the end ({:#x})", offset, data.size()));
    }
    uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

uint16_t read_u16(std::span<const uint8_t> data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 2) {
        throw FormatError(
            fmt::format("read of 2 bytes at {:#x} is past the end ({:#x})", offset, data.size()));
    }
    uint16_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

const DxbcPart* Dxbc::find(uint32_t code) const {
    const auto it = std::ranges::find(parts, code, &DxbcPart::fourcc);
    return it == parts.end() ? nullptr : &*it;
}

Dxbc parse_dxbc(std::span<const uint8_t> container) {
    constexpr size_t kHeaderSize = 32;
    if (container.size() < kHeaderSize || read_u32(container, 0) != fourcc("DXBC")) {
        throw FormatError("not a DXBC container");
    }
    const uint32_t total = read_u32(container, 24);
    if (total < kHeaderSize || total > container.size()) {
        throw FormatError(fmt::format("container size {:#x} doesn't fit the {:#x} bytes given",
                                      total, container.size()));
    }
    const std::span<const uint8_t> data = container.first(total);
    Dxbc dxbc;
    std::copy_n(data.begin() + 4, dxbc.digest.size(), dxbc.digest.begin());
    dxbc.major = read_u16(data, 20);
    dxbc.minor = read_u16(data, 22);
    const uint32_t count = read_u32(data, 28);
    if (count > (total - kHeaderSize) / 4) {
        throw FormatError(fmt::format("{} parts don't fit a {:#x}-byte container", count, total));
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = read_u32(data, kHeaderSize + (4 * size_t{i}));
        const uint32_t size = read_u32(data, size_t{offset} + 4);
        if (size > total - offset - 8) {
            throw FormatError(
                fmt::format("part {} at {:#x} runs past the end of the container", i, offset));
        }
        dxbc.parts.push_back({read_u32(data, offset), data.subspan(size_t{offset} + 8, size)});
    }
    return dxbc;
}

std::string kind_name(uint16_t kind) {
    switch (static_cast<ProgramKind>(kind)) {
    case ProgramKind::kPixel: return "pixel";
    case ProgramKind::kVertex: return "vertex";
    case ProgramKind::kGeometry: return "geometry";
    case ProgramKind::kHull: return "hull";
    case ProgramKind::kDomain: return "domain";
    case ProgramKind::kCompute: return "compute";
    case ProgramKind::kLibrary: return "library";
    case ProgramKind::kMesh: return "mesh";
    case ProgramKind::kAmplification: return "amplification";
    }
    return fmt::format("kind {}", kind);
}

bool DxilProgram::has_llvm_bitcode() const {
    constexpr std::array<uint8_t, 4> kBare = {'B', 'C', 0xC0, 0xDE};
    constexpr std::array<uint8_t, 4> kWrapper = {0xDE, 0xC0, 0x17, 0x0B};
    return bitcode.size() >= 4 && (std::ranges::equal(bitcode.first(4), kBare) ||
                                   std::ranges::equal(bitcode.first(4), kWrapper));
}

bool DxilProgram::no_bitcode_marker() const {
    return bitcode.size() == 4 && read_u32(bitcode, 0) == fourcc("NOBC");
}

DxilProgram parse_dxil_program(std::span<const uint8_t> part) {
    if (part.size() < kDxilProgramHeaderSize) {
        throw FormatError("DXIL part is smaller than its program header");
    }
    const uint32_t version = read_u32(part, 0);
    if (read_u32(part, 8) != fourcc("DXIL")) {
        throw FormatError("DXIL program header has no bitcode header");
    }
    DxilProgram program;
    program.kind = static_cast<uint16_t>(version >> 16);
    program.major = static_cast<uint8_t>((version >> 4) & 0xF);
    program.minor = static_cast<uint8_t>(version & 0xF);
    program.dxil_version = read_u32(part, 12);
    // Bitcode offsets count from the bitcode header, which starts 8 bytes into the part.
    const uint64_t offset = uint64_t{read_u32(part, 16)} + 8;
    const uint32_t size = read_u32(part, 20);
    if (offset > part.size() || size > part.size() - offset) {
        throw FormatError("DXIL bitcode runs past the end of its part");
    }
    program.bitcode = part.subspan(static_cast<size_t>(offset), size);
    return program;
}

std::vector<SignatureElement> parse_signature(std::span<const uint8_t> part) {
    constexpr size_t kElementSize = 32;
    const uint32_t count = read_u32(part, 0);
    const uint32_t offset = read_u32(part, 4);
    if (offset > part.size() || count > (part.size() - offset) / kElementSize) {
        throw FormatError(fmt::format("{} signature elements at {:#x} don't fit a {:#x}-byte part",
                                      count, offset, part.size()));
    }
    std::vector<SignatureElement> elements;
    elements.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = offset + (kElementSize * i);
        SignatureElement element;
        element.stream = read_u32(part, at);
        element.semantic_index = read_u32(part, at + 8);
        element.system_value = read_u32(part, at + 12);
        element.component_type = read_u32(part, at + 16);
        element.reg = read_u32(part, at + 20);
        element.mask = part[at + 24];
        element.rw_mask = part[at + 25];
        element.min_precision = read_u32(part, at + 28);
        elements.push_back(element);
    }
    return elements;
}

std::vector<SignatureElement> input_signature(std::span<const uint8_t> container) {
    const Dxbc dxbc = parse_dxbc(container);
    const DxbcPart* part = dxbc.find(fourcc("ISG1"));
    return part == nullptr ? std::vector<SignatureElement>{} : parse_signature(part->data);
}

std::optional<PsvGeometry> psv_geometry(std::span<const uint8_t> container) {
    constexpr size_t kGeometrySize = 13; // three 32-bit words and a byte
    const Dxbc dxbc = parse_dxbc(container);
    const DxbcPart* part = dxbc.find(fourcc("PSV0"));
    if (part == nullptr) {
        return std::nullopt;
    }
    const std::span<const uint8_t> data = part->data;
    const uint32_t size = read_u32(data, 0);
    if (size < kGeometrySize || size > data.size() - 4) {
        throw FormatError(fmt::format("PSV0 runtime info of {:#x} bytes in a {:#x}-byte part", size,
                                      data.size()));
    }
    return PsvGeometry{.input_primitive = read_u32(data, 4),
                       .output_topology = read_u32(data, 8),
                       .output_streams = read_u32(data, 12),
                       .position = data[16] != 0};
}

} // namespace g6::shader
