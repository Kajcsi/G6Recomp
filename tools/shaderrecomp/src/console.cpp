// The console's additions to a shader container (see console.h).

#include "console.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "g6/inflate.h"

namespace g6::shader {

std::optional<Xdat> read_xdat(std::span<const uint8_t> dxil_part) {
    constexpr size_t kAt = kDxilProgramHeaderSize;
    constexpr size_t kEntrySize = 12;
    if (dxil_part.size() < kAt + 12 || read_u32(dxil_part, kAt) != fourcc("XDAT")) {
        return std::nullopt;
    }
    const uint32_t count = read_u32(dxil_part, kAt + 4);
    if (count > (dxil_part.size() - kAt - 12) / kEntrySize) {
        throw FormatError(fmt::format("XDAT table of {} entries doesn't fit", count));
    }
    Xdat xdat{.unknown = read_u32(dxil_part, kAt + 8), .entries = {}};
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = kAt + 12 + (kEntrySize * i);
        const uint32_t offset = read_u32(dxil_part, at + 4);
        const uint32_t size = read_u32(dxil_part, at + 8);
        if (offset > dxil_part.size() || size > dxil_part.size() - offset) {
            throw FormatError(fmt::format("XDAT entry {} runs past the end of the part", i));
        }
        xdat.entries.push_back({read_u32(dxil_part, at), dxil_part.subspan(offset, size)});
    }
    return xdat;
}

NativeProgramHeader read_native_program_header(std::span<const uint8_t> xbch) {
    if (xbch.size() < kNativeProgramHeaderSize || read_u32(xbch, 0) != fourcc("XBCH")) {
        throw FormatError("native program entry has no XBCH header");
    }
    NativeProgramHeader header;
    header.version = read_u32(xbch, 4);
    header.compressed_size = read_u32(xbch, 8);
    header.size = read_u32(xbch, 16);
    if (header.compressed_size != xbch.size() - kNativeProgramHeaderSize) {
        throw FormatError(fmt::format("XBCH says {:#x} compressed bytes, the entry holds {:#x}",
                                      header.compressed_size,
                                      xbch.size() - kNativeProgramHeaderSize));
    }
    return header;
}

std::vector<uint8_t> unpack_native_program(std::span<const uint8_t> xbch) {
    const NativeProgramHeader header = read_native_program_header(xbch);
    Inflated program;
    try {
        program = inflate_zlib(xbch.subspan(kNativeProgramHeaderSize), header.size);
    } catch (const InflateError& error) {
        throw FormatError(fmt::format("native program doesn't inflate: {}", error.what()));
    }
    if (program.data.size() != header.size) {
        throw FormatError(fmt::format("native program is {:#x} bytes, XBCH says {:#x}",
                                      program.data.size(), header.size));
    }
    if (read_u32(program.data, 0) != kFssmMagic) {
        throw FormatError("native program isn't an FSSM image");
    }
    return std::move(program.data);
}

uint16_t fssm_version(std::span<const uint8_t> fssm) {
    if (read_u32(fssm, 0) != kFssmMagic) {
        throw FormatError("not an FSSM image");
    }
    return read_u16(fssm, 4);
}

std::span<const uint8_t> fssm_code(std::span<const uint8_t> fssm) {
    constexpr size_t kCodeOffsetAt = 0xC0;
    constexpr size_t kCodeSizeAt = 0xC8;
    constexpr size_t kCodeSkip = 0x28;
    if (fssm.size() < kCodeSizeAt + 4 || read_u32(fssm, 0) != kFssmMagic) {
        throw FormatError("not an FSSM image");
    }
    const size_t offset = read_u32(fssm, kCodeOffsetAt);
    const size_t size = read_u32(fssm, kCodeSizeAt);
    if (size < kCodeSkip || offset > fssm.size() || size > fssm.size() - offset) {
        throw FormatError(fmt::format("FSSM code at {:#x}, {:#x} bytes, doesn't fit in {:#x}",
                                      offset, size, fssm.size()));
    }
    return fssm.subspan(offset + kCodeSkip, size - kCodeSkip);
}

uint32_t fssm_key(std::span<const uint8_t> fssm) {
    constexpr size_t kCodeOffsetAt = 0xC0;
    constexpr size_t kKeyAt = 0x18;
    fssm_code(fssm); // checks that the range, which is longer than kKeyAt + 4, fits
    return read_u32(fssm, read_u32(fssm, kCodeOffsetAt) + kKeyAt);
}

std::string hardware_stage_name(uint8_t stage) {
    constexpr std::array<std::string_view, 7> kNames = {"LS", "HS", "ES", "GS", "VS", "PS", "CS"};
    return stage < kNames.size() ? std::string(kNames.at(stage)) : fmt::format("{}", stage);
}

std::vector<FssmStage> fssm_stages(std::span<const uint8_t> fssm) {
    constexpr size_t kCodeOffsetAt = 0xC0;
    constexpr size_t kFirst = 0x100;
    constexpr size_t kStride = 0x88;
    constexpr uint64_t kTag = 0xC00002;
    if (fssm.size() < 4 || read_u32(fssm, 0) != kFssmMagic) {
        throw FormatError("not an FSSM image");
    }
    std::vector<FssmStage> stages;
    if (fssm.size() < kCodeOffsetAt + 4) {
        return stages;
    }
    const size_t code = std::min<size_t>(read_u32(fssm, kCodeOffsetAt), fssm.size());
    for (size_t at = kFirst; at + 8 <= code; at += kStride) {
        const uint64_t word = read_u32(fssm, at) | (uint64_t{read_u32(fssm, at + 4)} << 32);
        if (word >> 16 != kTag) {
            break;
        }
        stages.push_back(
            {.api = static_cast<uint8_t>(word >> 8), .hardware = static_cast<uint8_t>(word)});
    }
    return stages;
}

FssmSetup fssm_setup(std::span<const uint8_t> fssm) {
    constexpr size_t kSetupAt = 0xD4;
    if (fssm.size() < kSetupAt + 4 || read_u32(fssm, 0) != kFssmMagic) {
        throw FormatError("not an FSSM image");
    }
    const uint32_t word = read_u32(fssm, kSetupAt);
    return {.gs = (word & 1) != 0,
            .fetch = ((word >> 2) & 1) != 0,
            .wave32 = ((word >> 5) & 1) != 0,
            .user_sgprs = static_cast<uint8_t>(word >> 16),
            .fetch_sgpr = static_cast<uint8_t>(word >> 24)};
}

std::optional<FssmRegisterCounts> fssm_register_counts(std::span<const uint8_t> fssm) {
    constexpr size_t kCountsAt = 0x110;
    if (fssm_stages(fssm).empty() || fssm.size() < kCountsAt + 4) {
        return std::nullopt;
    }
    const uint32_t word = read_u32(fssm, kCountsAt);
    return FssmRegisterCounts{.vgprs = word & 0x3FF, .sgprs = word >> 20};
}

std::optional<uint32_t> fssm_rsrc2(std::span<const uint8_t> fssm) {
    const std::vector<FssmStage> stages = fssm_stages(fssm);
    if (stages.empty()) {
        return std::nullopt;
    }
    size_t at = 0;
    switch (stages.front().hardware) {
    case kHardwareCs:
    case kHardwareGs: at = 0x138; break;
    case kHardwarePs: at = 0x23C; break;
    case kHardwareVs:
    case kHardwareEs: at = 0x2A0; break;
    default: return std::nullopt;
    }
    if (fssm.size() < at + 4) {
        return std::nullopt;
    }
    return read_u32(fssm, at);
}

GsRsrc2 gs_rsrc2(uint32_t word) {
    return {.user_sgprs = (word >> 1) & 0x1F,
            .es_vgpr_components = (word >> 16) & 3,
            .lds_size = (word >> 19) & 0xFF};
}

std::optional<std::array<uint8_t, 4>> fssm_gs_out_prim_types(std::span<const uint8_t> fssm) {
    constexpr size_t kTypesAt = 0x13C;
    const std::vector<FssmStage> stages = fssm_stages(fssm);
    if (stages.empty() || stages.front().hardware != kHardwareGs || fssm.size() < kTypesAt + 4) {
        return std::nullopt;
    }
    const uint32_t word = read_u32(fssm, kTypesAt);
    constexpr std::array<unsigned, 4> kShifts = {0, 8, 16, 22};
    std::array<uint8_t, 4> types{};
    for (size_t stream = 0; stream < types.size(); ++stream) {
        types.at(stream) = static_cast<uint8_t>((word >> kShifts.at(stream)) & 0x3F);
    }
    return types;
}

std::optional<uint8_t> gs_out_prim_type(uint32_t topology) {
    switch (topology) {
    case kTopologyPointList: return uint8_t{0};
    case kTopologyLineStrip: return uint8_t{1};
    case kTopologyTriangleStrip: return uint8_t{2};
    default: return std::nullopt;
    }
}

std::optional<uint32_t> fssm_vgpr_components(std::span<const uint8_t> fssm) {
    constexpr size_t kComponentsAt = 0x2BC;
    const std::vector<FssmStage> stages = fssm_stages(fssm);
    if (stages.empty() || fssm.size() < kComponentsAt + 4) {
        return std::nullopt;
    }
    switch (stages.front().hardware) {
    case kHardwareVs:
    case kHardwareEs:
    case kHardwareMesh: return read_u32(fssm, kComponentsAt);
    default: return std::nullopt;
    }
}

std::optional<VertexVgprs> vertex_vgprs(uint8_t hardware) {
    uint32_t first = 0;
    switch (hardware) {
    case kHardwareVs: first = 0; break;
    case kHardwareEs: first = kMergedGsVgprs; break;
    default: return std::nullopt;
    }
    constexpr uint32_t kInstanceId = 3; // after the vertex ID and two user VGPRs
    return VertexVgprs{first, first + kInstanceId, first + kInstanceId + 1};
}

std::vector<FetchedElement> fetch_layout(std::span<const SignatureElement> inputs,
                                         uint32_t first_fetched) {
    std::vector<size_t> read;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].system_value == 0 && inputs[i].rw_mask != 0) {
            read.push_back(i);
        }
    }
    std::ranges::stable_sort(read, {}, [&](size_t i) { return inputs[i].reg; });
    std::vector<FetchedElement> layout;
    layout.reserve(read.size());
    constexpr uint32_t kVgprsPerElement = 4;
    for (size_t k = 0; k < read.size(); ++k) {
        layout.push_back({read[k], first_fetched + (kVgprsPerElement * static_cast<uint32_t>(k))});
    }
    return layout;
}

uint32_t fssm_layout(std::span<const uint8_t> fssm) {
    constexpr size_t kLayoutAt = 0x10;
    if (read_u32(fssm, 0) != kFssmMagic) {
        throw FormatError("not an FSSM image");
    }
    return read_u32(fssm, kLayoutAt);
}

std::optional<uint32_t> fssm_ps_input_enable(std::span<const uint8_t> fssm) {
    constexpr size_t kEnableAt = 0x248;
    const std::vector<FssmStage> stages = fssm_stages(fssm);
    if (stages.empty() || stages.front().hardware != kHardwarePs || fssm.size() < kEnableAt + 4) {
        return std::nullopt;
    }
    return read_u32(fssm, kEnableAt);
}

namespace {

constexpr uint32_t kMaxStageTableRecords = 32;

// The records of a stage table of `stride`-byte records at `first`, counted by the word at
// `count_at`, or nothing when the image is too short or the count is out of range.
std::optional<std::span<const uint8_t>> stage_table(std::span<const uint8_t> fssm, size_t first,
                                                    size_t stride, size_t count_at) {
    if (fssm.size() < count_at + 4 || fssm_layout(fssm) != kFssmLayout) {
        return std::nullopt;
    }
    const uint32_t count = read_u32(fssm, count_at);
    if (count > kMaxStageTableRecords) {
        return std::nullopt;
    }
    return fssm.subspan(first, stride * count);
}

} // namespace

std::optional<std::vector<PsInputRecord>> fssm_ps_input_table(std::span<const uint8_t> fssm) {
    constexpr size_t kFirst = 0x138;
    constexpr size_t kStride = 8;
    constexpr size_t kCountAt = 0x238;
    const std::vector<FssmStage> stages = fssm_stages(fssm);
    if (stages.empty() || stages.front().hardware != kHardwarePs) {
        return std::nullopt;
    }
    const std::optional<std::span<const uint8_t>> table =
        stage_table(fssm, kFirst, kStride, kCountAt);
    if (!table) {
        return std::nullopt;
    }
    std::vector<PsInputRecord> records;
    for (size_t at = 0; at < table->size(); at += kStride) {
        const std::span<const uint8_t> r = table->subspan(at, kStride);
        records.push_back({.kind = r[0],
                           .reg = r[1],
                           .mask = r[2],
                           .attribute = r[3],
                           .byte4 = r[4],
                           .byte5 = r[5],
                           .byte6 = r[6],
                           .flags = r[7]});
    }
    return records;
}

std::optional<std::vector<VsOutputRecord>> fssm_vs_output_table(std::span<const uint8_t> fssm) {
    constexpr size_t kFirst = 0x1DC;
    constexpr size_t kStride = 6;
    constexpr size_t kCountAt = 0x29C;
    const std::vector<FssmStage> stages = fssm_stages(fssm);
    if (std::ranges::none_of(
            stages, [](const FssmStage& stage) { return stage.hardware == kHardwareVs; })) {
        return std::nullopt;
    }
    const std::optional<std::span<const uint8_t>> table =
        stage_table(fssm, kFirst, kStride, kCountAt);
    if (!table) {
        return std::nullopt;
    }
    std::vector<VsOutputRecord> records;
    for (size_t at = 0; at < table->size(); at += kStride) {
        const std::span<const uint8_t> r = table->subspan(at, kStride);
        records.push_back({.kind = r[0], .reg = r[1], .param = r[2], .extra = {r[3], r[4], r[5]}});
    }
    return records;
}

PsInputs ps_inputs(uint32_t enable) {
    // VGPRs per enable bit, in register order (see console.h).
    constexpr std::array<uint32_t, 16> kSizes = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    constexpr uint32_t kPullModel = 3;
    constexpr uint32_t kLastBarycentric = 6;
    constexpr uint32_t kPositionX = 8;
    constexpr uint32_t kFrontFace = 12;
    PsInputs inputs;
    for (uint32_t bit = 0; bit < kSizes.size(); ++bit) {
        if ((enable >> bit & 1) == 0) {
            continue;
        }
        if (bit <= kLastBarycentric && bit != kPullModel) {
            inputs.barycentrics |= 3u << inputs.vgprs;
        } else if (bit >= kPositionX && bit < kPositionX + 4) {
            inputs.position[bit - kPositionX] = inputs.vgprs;
        } else if (bit == kFrontFace) {
            inputs.front_face = inputs.vgprs;
        }
        inputs.vgprs += kSizes.at(bit);
    }
    return inputs;
}

} // namespace g6::shader
