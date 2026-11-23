#include <fmt/format.h>

#include "g6/pe/directories.h"

namespace g6::pe {

std::vector<Relocation> read_relocations(const Image& image) {
    std::vector<Relocation> relocations;
    const DataDirectory dir = image.directory(Directory::BaseReloc);
    if (!dir.present()) {
        return relocations;
    }
    const uint64_t end = uint64_t{dir.rva} + dir.size;
    uint64_t block = dir.rva;
    while (block + 8 <= end) {
        const auto block_rva = static_cast<uint32_t>(block);
        const auto page = image.read<uint32_t>(block_rva);
        const auto block_size = image.read<uint32_t>(block_rva + 4);
        if (block_size == 0) {
            break; // some linkers pad the directory with zeros
        }
        if (block_size < 8 || block + block_size > end) {
            throw FormatError(
                fmt::format("relocation block at {:#x} has bad size {:#x}", block, block_size));
        }
        const uint32_t count = (block_size - 8) / 2;
        for (uint32_t i = 0; i < count; ++i) {
            const auto entry = image.read<uint16_t>(block_rva + 8 + i * 2);
            const auto type = static_cast<uint8_t>(entry >> 12);
            if (type != kRelocAbsolute) {
                relocations.push_back({page + (entry & 0x0FFFu), type});
            }
        }
        block += block_size;
    }
    return relocations;
}

} // namespace g6::pe
