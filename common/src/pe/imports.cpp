#include <fmt/format.h>

#include "g6/pe/directories.h"

namespace g6::pe {
namespace {

constexpr uint32_t kImportDescriptorSize = 20;
constexpr uint32_t kDelayDescriptorSize = 32;
constexpr uint64_t kOrdinalFlag = 1ull << 63;
constexpr size_t kMaxModules = 1u << 16;
constexpr size_t kMaxSymbolsPerModule = 1u << 20;

// Reads a null-terminated array of 64-bit thunks. `va_based` is for old-style delay-load
// descriptors whose name-table entries are virtual addresses rather than RVAs.
std::vector<ImportedSymbol> read_thunks(const Image& image, uint32_t lookup_rva, uint32_t iat_rva,
                                        bool va_based) {
    std::vector<ImportedSymbol> symbols;
    for (uint32_t i = 0;; ++i) {
        if (i >= kMaxSymbolsPerModule) {
            throw FormatError("import thunk array is not terminated");
        }
        const auto thunk = image.read<uint64_t>(lookup_rva + i * 8);
        if (thunk == 0) {
            break;
        }
        ImportedSymbol symbol;
        symbol.iat_rva = iat_rva + i * 8;
        if ((thunk & kOrdinalFlag) != 0) {
            symbol.ordinal = static_cast<uint16_t>(thunk & 0xFFFF);
        } else {
            const uint32_t hint_name =
                va_based ? image.va_to_rva(thunk) : static_cast<uint32_t>(thunk & 0x7FFFFFFF);
            symbol.hint = image.read<uint16_t>(hint_name);
            symbol.name = image.read_cstring(hint_name + 2);
        }
        symbols.push_back(std::move(symbol));
    }
    return symbols;
}

void read_regular(const Image& image, std::vector<ImportedModule>& modules) {
    const DataDirectory dir = image.directory(Directory::Import);
    if (!dir.present()) {
        return;
    }
    for (uint32_t desc = dir.rva;; desc += kImportDescriptorSize) {
        if (modules.size() >= kMaxModules) {
            throw FormatError("import directory is not terminated");
        }
        const auto lookup = image.read<uint32_t>(desc + 0);
        const auto name = image.read<uint32_t>(desc + 12);
        const auto iat = image.read<uint32_t>(desc + 16);
        if (name == 0 && iat == 0) {
            break;
        }
        ImportedModule module;
        module.dll = image.read_cstring(name);
        // Without an import lookup table the IAT holds the unbound thunks.
        module.symbols = read_thunks(image, lookup != 0 ? lookup : iat, iat, false);
        modules.push_back(std::move(module));
    }
}

void read_delay(const Image& image, std::vector<ImportedModule>& modules) {
    const DataDirectory dir = image.directory(Directory::DelayImport);
    if (!dir.present()) {
        return;
    }
    for (uint32_t desc = dir.rva;; desc += kDelayDescriptorSize) {
        if (modules.size() >= kMaxModules) {
            throw FormatError("delay import directory is not terminated");
        }
        const auto attributes = image.read<uint32_t>(desc + 0);
        const auto name = image.read<uint32_t>(desc + 4);
        const auto module_handle = image.read<uint32_t>(desc + 8);
        const auto iat = image.read<uint32_t>(desc + 12);
        const auto names = image.read<uint32_t>(desc + 16);
        if (name == 0) {
            break;
        }
        // Attribute bit 0 set means the descriptor holds RVAs; clear means old-style VAs.
        const bool rva_based = (attributes & 1) != 0;
        const auto to_rva = [&](uint32_t value) {
            return rva_based ? value : image.va_to_rva(image.image_base() + value);
        };
        ImportedModule module;
        module.delay_load = true;
        module.dll = image.read_cstring(to_rva(name));
        module.module_handle_rva = module_handle != 0 ? to_rva(module_handle) : 0;
        module.symbols = read_thunks(image, to_rva(names), to_rva(iat), !rva_based);
        modules.push_back(std::move(module));
    }
}

} // namespace

std::vector<ImportedModule> read_imports(const Image& image) {
    std::vector<ImportedModule> modules;
    read_regular(image, modules);
    read_delay(image, modules);
    return modules;
}

} // namespace g6::pe
