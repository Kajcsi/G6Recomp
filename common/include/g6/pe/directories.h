#pragma once

// Readers for the PE data directories the recompiler and runtime need. Each reader returns an
// empty result when the directory is absent and throws FormatError when it is malformed.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "g6/pe/image.h"

namespace g6::pe {

// ---- Imports (regular and delay-load) ----

struct ImportedSymbol {
    std::string name;                // empty when imported by ordinal only
    std::optional<uint16_t> ordinal; // set when imported by ordinal
    uint16_t hint = 0;
    uint32_t iat_rva = 0; // the address slot the loader fills in
};

struct ImportedModule {
    std::string dll;
    bool delay_load = false;
    // Delay-load only: RVA of the slot where the delay helper stores the DLL's module handle.
    uint32_t module_handle_rva = 0;
    std::vector<ImportedSymbol> symbols;
};

std::vector<ImportedModule> read_imports(const Image& image);

// ---- Exports ----

struct Export {
    uint32_t ordinal = 0;
    std::string name;      // empty when exported by ordinal only
    uint32_t rva = 0;      // 0 for forwarders
    std::string forwarder; // "dll.symbol" or "dll.#ordinal" when forwarded
};

struct ExportTable {
    std::string dll_name;
    std::vector<Export> exports; // unused ordinal slots are omitted
};

ExportTable read_exports(const Image& image);

// ---- Base relocations ----

inline constexpr uint8_t kRelocAbsolute = 0;
inline constexpr uint8_t kRelocHighLow = 3;
inline constexpr uint8_t kRelocDir64 = 10;

struct Relocation {
    uint32_t rva = 0;
    uint8_t type = 0;
};

// Every relocation entry except IMAGE_REL_BASED_ABSOLUTE padding.
std::vector<Relocation> read_relocations(const Image& image);

// ---- TLS ----

struct TlsDirectory {
    uint64_t raw_data_start_va = 0;
    uint64_t raw_data_end_va = 0;
    uint64_t index_va = 0;
    uint64_t callbacks_va = 0;
    uint32_t zero_fill_size = 0;
    uint32_t characteristics = 0;
    std::vector<uint32_t> callback_rvas;
};

std::optional<TlsDirectory> read_tls(const Image& image);

// ---- Load config (security cookie, Control Flow Guard tables) ----

struct LoadConfig {
    uint32_t size = 0;
    uint64_t security_cookie_va = 0;
    uint64_t guard_cf_check_va = 0;
    uint64_t guard_cf_dispatch_va = 0;
    uint32_t guard_flags = 0;
    // Valid indirect-call targets from the CFG function table. Good function-start seeds.
    std::vector<uint32_t> guard_cf_functions;
    // Valid exception continuation targets (EH continuation metadata). Mid-function addresses,
    // not function starts.
    std::vector<uint32_t> guard_eh_continuations;
};

std::optional<LoadConfig> read_load_config(const Image& image);

} // namespace g6::pe
