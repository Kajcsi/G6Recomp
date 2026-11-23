#pragma once

// Module-level code emission: every discovered function of one PE becomes C++ (see
// docs/codegen.md), plus a table that registers them with the runtime's dispatcher. Output for
// game files is derived from the game and stays on the user's machine.

#include <cstdint>
#include <filesystem>
#include <string>

#include "function_discovery.h"
#include "function_emitter.h"
#include "g6/pe/image.h"

namespace g6::recomp {

struct EmitOptions {
    uint64_t base = 0;   // assigned load address; 0 keeps the preferred base
    std::string name;    // module name, used for the table symbol g6_module_<name>
    unsigned chunks = 1; // number of functions_<n>.cpp files
};

// Generates code for every function in `discovery`. With a non-empty `out_dir`, writes
// functions.h, functions_<n>.cpp, and table.cpp there (a file whose content is unchanged is
// left alone, so builds don't redo work).
EmitStats emit_module(const pe::Image& image, const DiscoveryResult& discovery,
                      const EmitOptions& options, const std::filesystem::path& out_dir);

} // namespace g6::recomp
