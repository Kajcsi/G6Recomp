#pragma once

// Function discovery: which addresses in the image start a guest function, and where each
// function's code lives. Non-leaf functions come from .pdata (with chained fragments grouped
// under their primary entry). Leaf functions have no .pdata entry, so they are found from
// seeds (entry point, exports, TLS callbacks, the CFG table, relocated code pointers) and by
// following direct calls, tail jumps, and `lea` of padded code addresses out of known functions.

#include <cstdint>
#include <string>
#include <vector>

#include "code_walker.h"
#include "g6/pe/image.h"

namespace g6::recomp {

enum SeedSource : uint32_t {
    kSourcePdata = 1u << 0,
    kSourceEntryPoint = 1u << 1,
    kSourceExport = 1u << 2,
    kSourceTlsCallback = 1u << 3,
    kSourceGuardCf = 1u << 4,
    kSourceRelocation = 1u << 5,
    kSourceDirectCall = 1u << 6,
    kSourceTailCall = 1u << 7,
    // A RIP-relative `lea` of an address that begins after int3 padding, 16-byte aligned: how
    // code takes the address of a function that has no .pdata entry, relocation, or CFG entry.
    kSourceLea = 1u << 8,
};

std::string describe_sources(uint32_t sources);

struct DiscoveredFunction {
    uint32_t start = 0;
    uint32_t end = 0;                   // exclusive end of the primary range
    std::vector<Range> fragments;       // further ranges from chained .pdata entries
    uint32_t sources = 0;               // SeedSource bits
    bool has_unwind_info = false;       // false for leaves; their end comes from the code walk
    std::vector<JumpTable> jump_tables; // switch tables dispatched from this function

    bool contains(uint32_t rva) const;
};

struct FunctionRange {
    Range range;
    uint32_t function_start = 0;
};

struct DiscoveryOptions {
    bool use_guard_cf = true;
    bool use_relocations = true;
    bool follow_calls = true;
};

struct DiscoveryStats {
    size_t pdata_entries = 0;
    size_t pdata_functions = 0;
    size_t chained_fragments = 0;
    size_t leaf_functions = 0;
    size_t leaf_splits = 0; // a leaf that falls through into another was cut at the second entry
    size_t seeds_not_code = 0;
    size_t seeds_mid_function = 0;
    size_t candidates_rejected = 0;
    size_t unwind_errors = 0;
    size_t directory_errors = 0;
    size_t instructions = 0;
    size_t decode_errors = 0;
    size_t fallthroughs = 0;
    size_t import_thunks = 0;
    size_t jump_tables = 0;
    size_t jump_table_targets = 0;    // entries, counting repeated targets each time
    size_t jump_tables_unbounded = 0; // entry count guessed from where valid targets stop
    size_t jump_tables_rejected = 0;  // table-shaped, but an entry was invalid (also unresolved)
    size_t indirect_tail_calls = 0;   // jumps through a pointer loaded from memory
    size_t unresolved_indirect_jumps = 0;
};

struct DiscoveryResult {
    std::vector<DiscoveredFunction> functions; // sorted by start
    std::vector<FunctionRange> ranges;         // every range of every function, sorted by begin
    // `cpuid` and `xgetbv` instructions in the functions' code, sorted. Only instructions the
    // walks decode count, so bytes in data that happen to look like one never do.
    std::vector<uint32_t> cpuid_sites;
    std::vector<uint32_t> xgetbv_sites;
    // Every jump table and byte index table the functions use, sorted. Further walks pass them
    // to CodeWalker::set_table_starts, so an unbounded table stops where the next one begins
    // without a second walk.
    std::vector<uint32_t> table_starts;
    DiscoveryStats stats;

    const DiscoveredFunction* find(uint32_t start) const;
    const DiscoveredFunction* containing(uint32_t rva) const;
};

DiscoveryResult discover_functions(const pe::Image& image, const DiscoveryOptions& options = {});

} // namespace g6::recomp
