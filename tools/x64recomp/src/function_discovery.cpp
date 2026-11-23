#include "function_discovery.h"

#include <algorithm>
#include <map>
#include <set>

#include "g6/log.h"
#include "g6/pe/directories.h"
#include "g6/pe/unwind.h"

namespace g6::recomp {
namespace {

using pe::Image;

struct SourceName {
    uint32_t bit;
    const char* name;
};

constexpr SourceName kSourceNames[] = {
    {kSourcePdata, "pdata"},     {kSourceEntryPoint, "entry"}, {kSourceExport, "export"},
    {kSourceTlsCallback, "tls"}, {kSourceGuardCf, "guard-cf"}, {kSourceRelocation, "reloc"},
    {kSourceDirectCall, "call"}, {kSourceTailCall, "tail"},
};

// One past the end of the executable section holding `rva`, or 0 when it is not code.
uint32_t executable_section_end(const Image& image, uint32_t rva) {
    const pe::Section* section = image.section_at(rva);
    if (section == nullptr || !section->executable()) {
        return 0;
    }
    return section->virtual_address + section->mapped_size();
}

const FunctionRange* find_range(const std::vector<FunctionRange>& sorted, uint32_t rva) {
    const auto it = std::ranges::upper_bound(
        sorted, rva, {}, [](const FunctionRange& entry) { return entry.range.begin; });
    if (it == sorted.begin()) {
        return nullptr;
    }
    const FunctionRange& candidate = *std::prev(it);
    return candidate.range.contains(rva) ? &candidate : nullptr;
}

std::vector<FunctionRange> ranges_of(const std::map<uint32_t, DiscoveredFunction>& functions) {
    std::vector<FunctionRange> ranges;
    for (const auto& [start, function] : functions) {
        ranges.push_back({{function.start, function.end}, start});
        for (const Range& fragment : function.fragments) {
            ranges.push_back({fragment, start});
        }
    }
    std::ranges::sort(ranges, [](const FunctionRange& a, const FunctionRange& b) {
        return a.range.begin < b.range.begin;
    });
    return ranges;
}

// Groups .pdata entries by the primary function they unwind through. Chained entries (cold
// code split out of a function, or prolog/epilog fragments) become fragments of that function.
std::map<uint32_t, DiscoveredFunction> group_pdata(const Image& image, DiscoveryStats& stats) {
    std::map<uint32_t, DiscoveredFunction> functions;
    std::vector<pe::RuntimeFunction> entries;
    try {
        entries = pe::read_runtime_functions(image);
    } catch (const pe::FormatError& error) {
        ++stats.directory_errors;
        LOG_WARN(discovery, "exception directory unreadable: {}", error.what());
        return functions;
    }
    stats.pdata_entries = entries.size();

    for (const pe::RuntimeFunction& entry : entries) {
        pe::RuntimeFunction primary = entry;
        try {
            primary = pe::primary_function(image, entry);
        } catch (const pe::FormatError& error) {
            // Keep the entry as a function of its own; its range is still known code.
            ++stats.unwind_errors;
            LOG_DEBUG(discovery, "unwind data for [{:#x}, {:#x}): {}", entry.begin, entry.end,
                      error.what());
        }
        DiscoveredFunction& function = functions[primary.begin];
        if (function.sources == 0) {
            function.start = primary.begin;
            function.end = primary.end;
            function.sources = kSourcePdata;
            function.has_unwind_info = true;
        }
        if (entry.begin != primary.begin) {
            function.fragments.push_back({entry.begin, entry.end});
            ++stats.chained_fragments;
        }
    }
    for (auto& [start, function] : functions) {
        std::ranges::sort(function.fragments,
                          [](const Range& a, const Range& b) { return a.begin < b.begin; });
    }
    stats.pdata_functions = functions.size();
    return functions;
}

template <typename Reader>
auto read_directory(const char* name, DiscoveryStats& stats, Reader&& reader)
    -> decltype(reader()) {
    try {
        return reader();
    } catch (const pe::FormatError& error) {
        ++stats.directory_errors;
        LOG_WARN(discovery, "{} directory unreadable: {}", name, error.what());
        return {};
    }
}

} // namespace

std::string describe_sources(uint32_t sources) {
    std::string text;
    for (const SourceName& source : kSourceNames) {
        if ((sources & source.bit) != 0) {
            if (!text.empty()) {
                text += '+';
            }
            text += source.name;
        }
    }
    return text.empty() ? "none" : text;
}

bool DiscoveredFunction::contains(uint32_t rva) const {
    if (rva >= start && rva < end) {
        return true;
    }
    return std::ranges::any_of(fragments,
                               [rva](const Range& fragment) { return fragment.contains(rva); });
}

const DiscoveredFunction* DiscoveryResult::find(uint32_t start) const {
    const auto it = std::ranges::lower_bound(functions, start, {}, &DiscoveredFunction::start);
    return it != functions.end() && it->start == start ? &*it : nullptr;
}

const DiscoveredFunction* DiscoveryResult::containing(uint32_t rva) const {
    const FunctionRange* range = find_range(ranges, rva);
    return range != nullptr ? find(range->function_start) : nullptr;
}

DiscoveryResult discover_functions(const Image& image, const DiscoveryOptions& options) {
    DiscoveryResult result;
    DiscoveryStats& stats = result.stats;

    // 1. Non-leaf functions from the exception directory.
    std::map<uint32_t, DiscoveredFunction> functions = group_pdata(image, stats);
    const std::vector<FunctionRange> covered = ranges_of(functions);

    std::map<uint32_t, DiscoveredFunction> leaves;
    std::map<uint32_t, uint32_t> candidates; // possible leaf starts -> SeedSource bits

    // The accepted leaf whose range strictly contains `rva` (not at its start), if any.
    const auto leaf_containing = [&](uint32_t rva) -> DiscoveredFunction* {
        auto it = leaves.upper_bound(rva);
        if (it == leaves.begin()) {
            return nullptr;
        }
        --it;
        return rva > it->second.start && rva < it->second.end ? &it->second : nullptr;
    };
    // Sorts an address that something claims is a function start into: a known function (add
    // the source), not code, the middle of a .pdata function, or a leaf candidate to verify.
    // Candidates inside an accepted leaf are settled when the candidate is verified.
    const auto classify = [&](uint32_t rva, uint32_t sources) {
        if (auto it = functions.find(rva); it != functions.end()) {
            it->second.sources |= sources;
            return;
        }
        if (auto it = leaves.find(rva); it != leaves.end()) {
            it->second.sources |= sources;
            return;
        }
        if (!image.is_executable(rva)) {
            ++stats.seeds_not_code;
            LOG_DEBUG(discovery, "seed {:#x} ({}) is not in an executable section", rva,
                      describe_sources(sources));
            return;
        }
        if (find_range(covered, rva) != nullptr) {
            ++stats.seeds_mid_function;
            LOG_DEBUG(discovery, "seed {:#x} ({}) lands inside a known function", rva,
                      describe_sources(sources));
            return;
        }
        candidates[rva] |= sources;
    };

    // 2. Seeds from the image's own metadata.
    if (image.entry_point() != 0) {
        classify(image.entry_point(), kSourceEntryPoint);
    }
    const pe::ExportTable exports =
        read_directory("export", stats, [&] { return pe::read_exports(image); });
    for (const pe::Export& entry : exports.exports) {
        // Exported data (objects, tables) is normal and not a seed problem, so skip it quietly.
        if (entry.rva != 0 && entry.forwarder.empty() && image.is_executable(entry.rva)) {
            classify(entry.rva, kSourceExport);
        }
    }
    const auto tls = read_directory("TLS", stats, [&] { return pe::read_tls(image); });
    if (tls) {
        for (const uint32_t callback : tls->callback_rvas) {
            classify(callback, kSourceTlsCallback);
        }
    }
    if (options.use_guard_cf) {
        const auto config =
            read_directory("load config", stats, [&] { return pe::read_load_config(image); });
        if (config) {
            for (const uint32_t target : config->guard_cf_functions) {
                classify(target, kSourceGuardCf);
            }
        }
    }
    if (options.use_relocations) {
        // Absolute pointers into code (vtables, callback tables, `mov reg, imm64` of a function
        // address). Most relocations point at data and are skipped without being counted.
        const auto relocations =
            read_directory("relocation", stats, [&] { return pe::read_relocations(image); });
        for (const pe::Relocation& relocation : relocations) {
            if (relocation.type != pe::kRelocDir64 || !image.contains(relocation.rva, 8)) {
                continue;
            }
            const auto target = image.try_va_to_rva(image.read<uint64_t>(relocation.rva));
            if (target && image.is_executable(*target)) {
                classify(*target, kSourceRelocation);
            }
        }
    }

    // cpuid and xgetbv sites seen by the latest walk; kept only when the walk is accepted.
    std::vector<uint32_t> walk_cpuid;
    std::vector<uint32_t> walk_xgetbv;
    const InstructionVisitor collect_sites = [&](uint32_t rva,
                                                 const ZydisDecodedInstruction& insn) {
        if (insn.mnemonic == ZYDIS_MNEMONIC_CPUID) {
            walk_cpuid.push_back(rva);
        } else if (insn.mnemonic == ZYDIS_MNEMONIC_XGETBV) {
            walk_xgetbv.push_back(rva);
        }
    };
    const auto keep_sites = [&] {
        result.cpuid_sites.insert(result.cpuid_sites.end(), walk_cpuid.begin(), walk_cpuid.end());
        result.xgetbv_sites.insert(result.xgetbv_sites.end(), walk_xgetbv.begin(),
                                   walk_xgetbv.end());
        walk_cpuid.clear();
        walk_xgetbv.clear();
    };

    // 3. Walk every .pdata function for direct calls and tail jumps out of it.
    const CodeWalker walker(image);
    for (auto& [start, function] : functions) {
        std::vector<Range> ranges{{function.start, function.end}};
        ranges.insert(ranges.end(), function.fragments.begin(), function.fragments.end());
        const uint32_t entry = start;
        const WalkResult walk = walker.walk(
            std::span<const uint32_t>(&entry, 1), ranges,
            [&, own = start](uint32_t rva) {
                return rva != own && (functions.contains(rva) || leaves.contains(rva));
            },
            collect_sites);
        keep_sites();
        stats.instructions += walk.instructions;
        stats.decode_errors += walk.decode_errors;
        stats.fallthroughs += walk.fallthrough_targets.size();
        stats.unresolved_indirect_jumps += walk.indirect_jumps;
        stats.jump_tables_rejected += walk.rejected_tables;
        stats.indirect_tail_calls += walk.indirect_tail_calls;
        function.jump_tables = walk.jump_tables;
        if (options.follow_calls) {
            for (const uint32_t target : walk.call_targets) {
                classify(target, kSourceDirectCall);
            }
            for (const uint32_t target : walk.tail_targets) {
                classify(target, kSourceTailCall);
            }
        }
    }

    // 4. Verify leaf candidates, lowest address first. A leaf may run up to the next known
    // function or fragment (or the end of its section); it is accepted when every path from its
    // entry decodes and ends in a return, a trap, a tail jump, or a fall-through into a known
    // function.
    //
    // Lowest-first means a leaf that falls through into another leaf (hand-written code does
    // this) is first accepted with both bodies. When the second entry comes up it lands inside
    // the first leaf, which is then split there, but only if the first leaf, re-walked with the
    // new boundary, reaches the second entry solely by falling or jumping to it exactly, and
    // the second never jumps back into the first. Otherwise the entry is a bogus seed in the
    // middle of a function and is rejected.
    std::set<uint32_t> boundaries;
    for (const FunctionRange& range : covered) {
        boundaries.insert(range.range.begin);
    }
    const auto walk_leaf = [&](uint32_t start, uint32_t limit, const InstructionVisitor& visit) {
        const Range range{start, limit};
        return walker.walk(
            std::span<const uint32_t>(&start, 1), std::span<const Range>(&range, 1),
            [&](uint32_t rva) {
                return rva != start && (functions.contains(rva) || leaves.contains(rva));
            },
            visit);
    };
    while (!candidates.empty()) {
        const auto node = candidates.extract(candidates.begin());
        const uint32_t start = node.key();
        const uint32_t sources = node.mapped();
        uint32_t limit = executable_section_end(image, start);
        if (const auto next = boundaries.upper_bound(start); next != boundaries.end()) {
            limit = std::min(limit, *next);
        }
        DiscoveredFunction* container = leaf_containing(start);
        if (container != nullptr) {
            limit = std::min(limit, container->end);
        }
        walk_cpuid.clear();
        walk_xgetbv.clear();
        const WalkResult walk = walk_leaf(start, limit, collect_sites);
        const bool valid = walk.ok() && !walk.first_is_trap && walk.instructions != 0;
        if (container != nullptr) {
            // Re-walk the container as if `start` were already a function. Its sites were kept
            // when it was accepted, and this walk covers a part of the same code.
            const uint32_t old_end = container->end;
            leaves.emplace(start, DiscoveredFunction{start, start, {}, sources, false});
            const WalkResult before = walk_leaf(container->start, start, {});
            leaves.erase(start);
            const auto crosses = [&](uint32_t target) {
                return target > container->start && target < old_end && target != start;
            };
            const bool splits = valid && before.ok() &&
                                std::ranges::none_of(before.tail_targets, crosses) &&
                                std::ranges::none_of(walk.tail_targets, crosses);
            if (!splits) {
                ++stats.seeds_mid_function;
                LOG_DEBUG(discovery, "seed {:#x} ({}) lands inside leaf {:#x}", start,
                          describe_sources(sources), container->start);
                continue;
            }
            container->end = before.extent_end;
            container->jump_tables = before.jump_tables;
            ++stats.leaf_splits;
            ++stats.fallthroughs;
        }
        if (!valid) {
            ++stats.candidates_rejected;
            LOG_DEBUG(discovery,
                      "rejected leaf candidate {:#x} ({}): {} decode errors, {} paths off the end, "
                      "{} bad targets{}",
                      start, describe_sources(sources), walk.decode_errors, walk.ran_off_end,
                      walk.bad_targets, walk.first_is_trap ? ", starts with a trap" : "");
            continue;
        }
        DiscoveredFunction leaf;
        leaf.start = start;
        leaf.end = walk.extent_end;
        leaf.sources = sources;
        leaf.jump_tables = walk.jump_tables;
        leaves.emplace(start, std::move(leaf));
        boundaries.insert(start);
        keep_sites();

        stats.instructions += walk.instructions;
        stats.fallthroughs += walk.fallthrough_targets.size();
        stats.unresolved_indirect_jumps += walk.indirect_jumps;
        stats.jump_tables_rejected += walk.rejected_tables;
        stats.indirect_tail_calls += walk.indirect_tail_calls;
        if (walk.instructions == 1 && walk.import_jump_slots.size() == 1) {
            ++stats.import_thunks;
        }
        if (options.follow_calls) {
            for (const uint32_t target : walk.call_targets) {
                classify(target, kSourceDirectCall);
            }
            for (const uint32_t target : walk.tail_targets) {
                classify(target, kSourceTailCall);
            }
        }
    }
    stats.leaf_functions = leaves.size();

    // 5. Merge into one sorted list.
    result.functions.reserve(functions.size() + leaves.size());
    for (auto& [start, function] : functions) {
        result.functions.push_back(std::move(function));
    }
    for (auto& [start, leaf] : leaves) {
        result.functions.push_back(std::move(leaf));
    }
    std::ranges::sort(
        result.functions,
        [](const DiscoveredFunction& a, const DiscoveredFunction& b) { return a.start < b.start; });
    for (const DiscoveredFunction& function : result.functions) {
        stats.jump_tables += function.jump_tables.size();
        for (const JumpTable& table : function.jump_tables) {
            stats.jump_table_targets += table.targets.size();
            stats.jump_tables_unbounded += table.bounded ? 0 : 1;
        }
        result.ranges.push_back({{function.start, function.end}, function.start});
        for (const Range& fragment : function.fragments) {
            result.ranges.push_back({fragment, function.start});
        }
    }
    std::ranges::sort(result.ranges, [](const FunctionRange& a, const FunctionRange& b) {
        return a.range.begin < b.range.begin;
    });
    for (std::vector<uint32_t>* sites : {&result.cpuid_sites, &result.xgetbv_sites}) {
        std::ranges::sort(*sites);
        const auto [first, last] = std::ranges::unique(*sites);
        sites->erase(first, last);
    }
    return result;
}

} // namespace g6::recomp
