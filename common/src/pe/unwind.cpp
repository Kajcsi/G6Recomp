#include "g6/pe/unwind.h"

#include <algorithm>

#include <fmt/format.h>

namespace g6::pe {
namespace {

constexpr uint32_t kRuntimeFunctionSize = 12;
constexpr int kMaxChainDepth = 32;

RuntimeFunction read_entry(const Image& image, uint32_t rva) {
    return {image.read<uint32_t>(rva), image.read<uint32_t>(rva + 4),
            image.read<uint32_t>(rva + 8)};
}

} // namespace

std::vector<RuntimeFunction> read_runtime_functions(const Image& image) {
    std::vector<RuntimeFunction> functions;
    const DataDirectory dir = image.directory(Directory::Exception);
    if (!dir.present()) {
        return functions;
    }
    const uint32_t count = dir.size / kRuntimeFunctionSize;
    functions.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const RuntimeFunction entry = read_entry(image, dir.rva + i * kRuntimeFunctionSize);
        if (entry.begin == 0 && entry.end == 0 && entry.unwind_info == 0) {
            continue; // zero padding at the end of the table
        }
        if (entry.end <= entry.begin) {
            throw FormatError(fmt::format(".pdata entry {} has an empty or inverted range "
                                          "[{:#x}, {:#x})",
                                          i, entry.begin, entry.end));
        }
        functions.push_back(entry);
    }
    return functions;
}

UnwindInfo read_unwind_info(const Image& image, uint32_t rva) {
    if ((rva & 1u) != 0) {
        throw FormatError(fmt::format("unwind info RVA {:#x} is an indirect entry", rva));
    }
    UnwindInfo info;
    const auto header = image.read<uint8_t>(rva);
    info.version = header & 0x7;
    info.flags = header >> 3;
    if (info.version != 1 && info.version != 2) {
        throw FormatError(
            fmt::format("unwind info at {:#x} has unknown version {}", rva, info.version));
    }
    info.prolog_size = image.read<uint8_t>(rva + 1);
    const uint32_t count = image.read<uint8_t>(rva + 2);
    const auto frame = image.read<uint8_t>(rva + 3);
    info.frame_register = frame & 0xF;
    info.frame_offset = uint32_t{static_cast<uint8_t>(frame >> 4)} * 16;

    const auto slot = [&](uint32_t index) { return image.read<uint16_t>(rva + 4 + index * 2); };
    const auto need = [&](uint32_t index, uint32_t slots) {
        if (index + slots > count) {
            throw FormatError(
                fmt::format("unwind info at {:#x}: truncated unwind code {}", rva, index));
        }
    };

    uint32_t i = 0;
    if (info.version >= 2) {
        // Version 2 stores epilog descriptors ahead of the prolog codes. The first one carries
        // the epilog size and whether an epilog sits at the very end of the function; the rest
        // give epilog positions as offsets back from the end (zero entries are padding).
        bool first = true;
        while (i < count) {
            const uint16_t code = slot(i);
            if (((code >> 8) & 0xF) != static_cast<uint8_t>(UnwindOp::Epilog)) {
                break;
            }
            const auto low = static_cast<uint8_t>(code & 0xFF);
            const auto op_info = static_cast<uint8_t>(code >> 12);
            if (first) {
                info.epilog_size = low;
                info.epilog_at_end = (op_info & 1) != 0;
                if (info.epilog_at_end) {
                    info.epilog_offsets_from_end.push_back(info.epilog_size);
                }
                first = false;
            } else if (const uint32_t offset = low | (uint32_t{op_info} << 8); offset != 0) {
                info.epilog_offsets_from_end.push_back(offset);
            }
            ++i;
        }
    }

    while (i < count) {
        const uint16_t raw = slot(i);
        UnwindCode code;
        code.prolog_offset = static_cast<uint8_t>(raw & 0xFF);
        code.op = static_cast<UnwindOp>((raw >> 8) & 0xF);
        code.info = static_cast<uint8_t>(raw >> 12);
        uint32_t used = 1;
        switch (code.op) {
        case UnwindOp::PushNonVolatile:
        case UnwindOp::SetFramePointer: break;
        case UnwindOp::AllocSmall: code.value = uint32_t{code.info} * 8 + 8; break;
        case UnwindOp::AllocLarge:
            if (code.info == 0) {
                need(i, 2);
                code.value = uint32_t{slot(i + 1)} * 8;
                used = 2;
            } else if (code.info == 1) {
                need(i, 3);
                code.value = slot(i + 1) | (uint32_t{slot(i + 2)} << 16);
                used = 3;
            } else {
                throw FormatError(
                    fmt::format("unwind info at {:#x}: bad AllocLarge info {}", rva, code.info));
            }
            break;
        case UnwindOp::SaveNonVolatile:
            need(i, 2);
            code.value = uint32_t{slot(i + 1)} * 8;
            used = 2;
            break;
        case UnwindOp::SaveNonVolatileFar:
        case UnwindOp::SaveXmm128Far:
        case UnwindOp::SpareCode:
            need(i, 3);
            code.value = slot(i + 1) | (uint32_t{slot(i + 2)} << 16);
            used = 3;
            break;
        case UnwindOp::Epilog:
            if (info.version >= 2) {
                throw FormatError(fmt::format("unwind info at {:#x}: epilog code after prolog "
                                              "codes",
                                              rva));
            }
            need(i, 2); // legacy version 1 SaveXmm
            code.value = uint32_t{slot(i + 1)} * 8;
            used = 2;
            break;
        case UnwindOp::SaveXmm128:
            need(i, 2);
            code.value = uint32_t{slot(i + 1)} * 16;
            used = 2;
            break;
        case UnwindOp::PushMachineFrame:
            if (code.info > 1) {
                throw FormatError(fmt::format("unwind info at {:#x}: bad PushMachineFrame info {}",
                                              rva, code.info));
            }
            break;
        default:
            throw FormatError(fmt::format("unwind info at {:#x}: unknown unwind op {}", rva,
                                          static_cast<int>(code.op)));
        }
        info.codes.push_back(code);
        i += used;
    }

    // The code array is padded to an even number of slots before the trailing data.
    const uint32_t tail = rva + 4 + ((count + 1u) & ~1u) * 2;
    if ((info.flags & kUnwindFlagChainInfo) != 0) {
        info.chained = read_entry(image, tail);
    } else if (info.has_handler()) {
        info.handler_rva = image.read<uint32_t>(tail);
        info.handler_data_rva = tail + 4;
    }
    return info;
}

RuntimeFunction resolve_indirect(const Image& image, RuntimeFunction function) {
    for (int depth = 0; function.indirect(); ++depth) {
        if (depth >= kMaxChainDepth) {
            throw FormatError("indirect .pdata entries form a cycle");
        }
        function = read_entry(image, function.unwind_info & ~1u);
    }
    return function;
}

RuntimeFunction primary_function(const Image& image, RuntimeFunction function) {
    for (int depth = 0; depth < kMaxChainDepth; ++depth) {
        if (function.indirect()) {
            function = read_entry(image, function.unwind_info & ~1u);
            continue;
        }
        const UnwindInfo info = read_unwind_info(image, function.unwind_info);
        if (!info.chained) {
            return function;
        }
        function = *info.chained;
    }
    throw FormatError(fmt::format("unwind chain from {:#x} is cyclic or deeper than {}",
                                  function.begin, kMaxChainDepth));
}

const RuntimeFunction* find_runtime_function(std::span<const RuntimeFunction> sorted,
                                             uint32_t rva) {
    const auto it = std::ranges::upper_bound(sorted, rva, {}, &RuntimeFunction::begin);
    if (it == sorted.begin()) {
        return nullptr;
    }
    const RuntimeFunction& candidate = *std::prev(it);
    return candidate.contains(rva) ? &candidate : nullptr;
}

} // namespace g6::pe
