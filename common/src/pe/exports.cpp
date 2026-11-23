#include <fmt/format.h>

#include "g6/pe/directories.h"

namespace g6::pe {
namespace {
constexpr uint32_t kMaxExports = 1u << 20;
}

ExportTable read_exports(const Image& image) {
    ExportTable table;
    const DataDirectory dir = image.directory(Directory::Export);
    if (!dir.present()) {
        return table;
    }
    const auto name_rva = image.read<uint32_t>(dir.rva + 12);
    const auto ordinal_base = image.read<uint32_t>(dir.rva + 16);
    const auto function_count = image.read<uint32_t>(dir.rva + 20);
    const auto name_count = image.read<uint32_t>(dir.rva + 24);
    const auto functions = image.read<uint32_t>(dir.rva + 28);
    const auto names = image.read<uint32_t>(dir.rva + 32);
    const auto name_ordinals = image.read<uint32_t>(dir.rva + 36);
    if (function_count > kMaxExports || name_count > kMaxExports) {
        throw FormatError("export directory has an implausible number of entries");
    }
    if (name_rva != 0) {
        table.dll_name = image.read_cstring(name_rva);
    }

    std::vector<Export> slots(function_count);
    for (uint32_t i = 0; i < function_count; ++i) {
        slots[i].ordinal = ordinal_base + i;
        slots[i].rva = image.read<uint32_t>(functions + i * 4);
    }
    for (uint32_t i = 0; i < name_count; ++i) {
        const auto index = image.read<uint16_t>(name_ordinals + i * 2);
        if (index >= function_count) {
            throw FormatError(fmt::format("export name {} points at ordinal slot {} of {}", i,
                                          index, function_count));
        }
        slots[index].name = image.read_cstring(image.read<uint32_t>(names + i * 4));
    }
    for (Export& slot : slots) {
        if (slot.rva == 0) {
            continue; // unused ordinal
        }
        // An address inside the export directory itself is a forwarder string.
        if (slot.rva >= dir.rva && slot.rva - dir.rva < dir.size) {
            slot.forwarder = image.read_cstring(slot.rva);
            slot.rva = 0;
        }
        table.exports.push_back(std::move(slot));
    }
    return table;
}

} // namespace g6::pe
