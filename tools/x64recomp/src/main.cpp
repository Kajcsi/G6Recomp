// x64recomp command-line front end: inspects a PE, runs function discovery, the pre-flight scan,
// and the RTTI scan, and emits C++.
// Its output is derived from whatever file it reads, so output for game files stays on the
// user's machine (print it, don't commit it).

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "emitter.h"
#include "function_discovery.h"
#include "g6/entropy.h"
#include "g6/log.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6/pe/unwind.h"
#include "rtti.h"
#include "scan.h"

namespace {

using namespace g6;

constexpr const char* kDirectoryNames[] = {
    "export", "import",       "resource",  "exception", "security",    "basereloc",
    "debug",  "arch",         "globalptr", "tls",       "load-config", "bound-import",
    "iat",    "delay-import", "clr",       "reserved",
};

int usage() {
    fmt::print(stderr,
               "usage: x64recomp <command> <pe-file> [options]\n"
               "\n"
               "commands:\n"
               "  info        headers, sections and data directories\n"
               "  imports     imported DLLs and symbols (regular and delay-load)\n"
               "  exports     exported symbols\n"
               "  functions   function discovery summary\n"
               "              --list          also list every function\n"
               "              --tables        also list every resolved jump table\n"
               "              --no-guard-cf   don't seed from the CFG function table\n"
               "              --no-relocs     don't seed from relocated code pointers\n"
               "              --no-follow     don't follow direct calls and tail jumps\n"
               "  scan        pre-flight scan for native execution (see docs/scan.md)\n"
               "              --sites         list every site, not just the first few\n"
               "              --no-sweep      don't decode .pdata code the walk didn't reach\n"
               "  rtti        MSVC run-time type information: classes, bases, and vtables\n"
               "              --list          also list every class\n"
               "  emit        generate C++ for every function (see docs/codegen.md)\n"
               "              --out <dir>     write functions.h, functions_<n>.cpp, table.cpp\n"
               "              --name <id>     module name; the table is g6_module_<id>\n"
               "              --base <addr>   assigned load address (default: preferred)\n"
               "              --chunks <n>    number of functions_<n>.cpp files (default 1)\n"
               "              --missing <n>   list the n most common untranslated mnemonics\n"
               "              --strict        fail if any instruction is left untranslated\n");
    return 2;
}

// Decimal, or hex with 0x; digit separators (') are allowed.
bool parse_number(std::string_view text, uint64_t& value) {
    int radix = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        radix = 16;
    }
    std::string digits;
    std::ranges::copy_if(text, std::back_inserter(digits), [](char c) { return c != '\''; });
    const char* const end = digits.data() + digits.size();
    const auto [ptr, error] = std::from_chars(digits.data(), end, value, radix);
    return !digits.empty() && error == std::errc() && ptr == end;
}

// x64 code sits near 6 bits per byte; above this it's almost certainly not plain code.
constexpr double kPackedEntropy = 7.5;

std::string section_flags(const pe::Section& section) {
    std::string flags;
    flags += (section.characteristics & pe::kSectionRead) != 0 ? 'r' : '-';
    flags += section.writable() ? 'w' : '-';
    flags += section.executable() ? 'x' : '-';
    return flags;
}

int cmd_info(const pe::Image& image) {
    fmt::print("machine        {:#06x}{}\n", image.machine(),
               image.machine() == pe::kMachineAmd64 ? " (x64)" : "");
    fmt::print("kind           {}\n", image.is_dll() ? "dll" : "exe");
    fmt::print("image base     {:#x}\n", image.image_base());
    fmt::print("size of image  {:#x}\n", image.size_of_image());
    fmt::print("entry point    {:#x}\n", image.entry_point());
    fmt::print("subsystem      {}\n", image.subsystem());
    fmt::print("dll flags      {:#06x}\n", image.dll_characteristics());
    fmt::print("timestamp      {:#010x}\n", image.timestamp());
    fmt::print("stack          reserve {:#x}, commit {:#x}\n", image.stack_reserve(),
               image.stack_commit());

    // Entropy over the bytes the file holds for each section. Code near 8 bits per byte is
    // encrypted or compressed on disk and has to be decrypted before anything can read it.
    fmt::print("\nsections\n");
    for (const pe::Section& section : image.sections()) {
        const uint32_t stored = std::min(section.raw_size, section.mapped_size());
        const double entropy = shannon_entropy(image.bytes(section.virtual_address, stored));
        fmt::print("  {:<10} rva {:#010x}  vsize {:#010x}  raw {:#010x}  {}  entropy {:.2f}{}\n",
                   section.name, section.virtual_address, section.virtual_size, section.raw_size,
                   section_flags(section), entropy,
                   section.executable() && entropy >= kPackedEntropy
                       ? "  (code looks encrypted or compressed)"
                       : "");
    }
    fmt::print("\ndirectories\n");
    for (size_t i = 0; i < pe::kDirectoryCount; ++i) {
        const pe::DataDirectory dir = image.directory(static_cast<pe::Directory>(i));
        if (dir.present()) {
            fmt::print("  {:<13} rva {:#010x}  size {:#x}\n", kDirectoryNames[i], dir.rva,
                       dir.size);
        }
    }
    if (const auto tls = pe::read_tls(image)) {
        fmt::print("\ntls            {} callbacks, zero fill {:#x}\n", tls->callback_rvas.size(),
                   tls->zero_fill_size);
    }
    if (const auto config = pe::read_load_config(image)) {
        fmt::print("load config    size {:#x}, guard flags {:#010x}, {} CFG targets, {} EH "
                   "continuations\n",
                   config->size, config->guard_flags, config->guard_cf_functions.size(),
                   config->guard_eh_continuations.size());
    }
    fmt::print(".pdata         {} entries\n", pe::read_runtime_functions(image).size());
    return 0;
}

int cmd_imports(const pe::Image& image) {
    for (const pe::ImportedModule& module : pe::read_imports(image)) {
        fmt::print("{}{} ({} symbols)\n", module.dll, module.delay_load ? " [delay]" : "",
                   module.symbols.size());
        for (const pe::ImportedSymbol& symbol : module.symbols) {
            if (symbol.ordinal) {
                fmt::print("  iat {:#010x}  ordinal {}\n", symbol.iat_rva, *symbol.ordinal);
            } else {
                fmt::print("  iat {:#010x}  {}\n", symbol.iat_rva, symbol.name);
            }
        }
    }
    return 0;
}

int cmd_exports(const pe::Image& image) {
    const pe::ExportTable table = pe::read_exports(image);
    fmt::print("{} ({} exports)\n", table.dll_name.empty() ? "<unnamed>" : table.dll_name,
               table.exports.size());
    for (const pe::Export& entry : table.exports) {
        const std::string name = entry.name.empty() ? "<ordinal only>" : entry.name;
        if (!entry.forwarder.empty()) {
            fmt::print("  {:>5}  {} -> {}\n", entry.ordinal, name, entry.forwarder);
        } else {
            fmt::print("  {:>5}  {:#010x}  {}\n", entry.ordinal, entry.rva, name);
        }
    }
    return 0;
}

int cmd_functions(const pe::Image& image, const std::vector<std::string_view>& flags) {
    recomp::DiscoveryOptions options;
    bool list = false;
    bool tables = false;
    for (const std::string_view flag : flags) {
        if (flag == "--list") {
            list = true;
        } else if (flag == "--tables") {
            tables = true;
        } else if (flag == "--no-guard-cf") {
            options.use_guard_cf = false;
        } else if (flag == "--no-relocs") {
            options.use_relocations = false;
        } else if (flag == "--no-follow") {
            options.follow_calls = false;
        } else {
            LOG_ERROR(x64recomp, "unknown option {}", flag);
            return usage();
        }
    }
    const recomp::DiscoveryResult result = recomp::discover_functions(image, options);
    const recomp::DiscoveryStats& s = result.stats;
    if (list) {
        for (const recomp::DiscoveredFunction& function : result.functions) {
            fmt::print("{:#010x} {:#010x} {:>3} {}\n", function.start, function.end,
                       function.fragments.size(), recomp::describe_sources(function.sources));
        }
        fmt::print("\n");
    }
    if (tables) {
        for (const recomp::DiscoveredFunction& function : result.functions) {
            for (const recomp::JumpTable& table : function.jump_tables) {
                fmt::print("{:#010x} jump {:#010x} table {:#010x} x{} {:>4} entries{}",
                           function.start, table.jump, table.table, table.entry_size,
                           table.targets.size(), table.bounded ? "" : " (unbounded)");
                if (table.index_table != 0) {
                    fmt::print(" via byte table {:#010x} ({} entries)", table.index_table,
                               table.index_count);
                }
                fmt::print("\n");
            }
        }
        fmt::print("\n");
    }
    fmt::print("functions             {}\n", result.functions.size());
    fmt::print("  from .pdata         {} ({} entries, {} chained fragments)\n", s.pdata_functions,
               s.pdata_entries, s.chained_fragments);
    fmt::print("  leaves              {} ({} import thunks, {} split at a fall-through)\n",
               s.leaf_functions, s.import_thunks, s.leaf_splits);
    fmt::print("instructions walked   {}\n", s.instructions);
    fmt::print("jump tables           {} ({} entries, {} unbounded)\n", s.jump_tables,
               s.jump_table_targets, s.jump_tables_unbounded);
    fmt::print("indirect tail calls   {}\n", s.indirect_tail_calls);
    fmt::print("unresolved jumps      {} ({} rejected tables)\n", s.unresolved_indirect_jumps,
               s.jump_tables_rejected);
    fmt::print("fall-throughs         {}\n", s.fallthroughs);
    fmt::print("rejected              {} candidates, {} mid-function seeds, {} non-code seeds\n",
               s.candidates_rejected, s.seeds_mid_function, s.seeds_not_code);
    fmt::print("errors                {} decode, {} unwind, {} directory\n", s.decode_errors,
               s.unwind_errors, s.directory_errors);
    return 0;
}

int cmd_scan(const pe::Image& image, const std::vector<std::string_view>& flags) {
    recomp::ScanOptions options;
    size_t limit = 4;
    for (const std::string_view flag : flags) {
        if (flag == "--sites") {
            limit = SIZE_MAX;
        } else if (flag == "--no-sweep") {
            options.sweep = false;
        } else {
            LOG_ERROR(x64recomp, "unknown option {}", flag);
            return usage();
        }
    }
    const recomp::ScanReport r = recomp::scan_module(image, options);
    const auto sites = [limit](const recomp::Sites& s) { return recomp::describe_sites(s, limit); };

    fmt::print("module\n");
    fmt::print("  kind                {}, base {:#x}, size {:#x}, dll flags {:#06x}\n",
               r.dll ? "dll" : "exe", r.image_base, r.size_of_image, r.dll_characteristics);
    fmt::print("  relocations         {} 64-bit, {} other{}\n", r.relocations.dir64,
               r.relocations.other, r.relocations.stripped ? " (marked stripped)" : "");
    if (r.tls) {
        fmt::print("  tls                 template {:#x}, zero fill {:#x}, align {}, {} callbacks, "
                   "index {}\n",
                   r.tls->template_size, r.tls->zero_fill, r.tls->alignment, r.tls->callbacks,
                   r.tls->index_rva ? fmt::format("{:#x}", *r.tls->index_rva) : "none");
    } else {
        fmt::print("  tls                 none\n");
    }
    if (r.cfg) {
        fmt::print("  load config         guard flags {:#010x}, {} CFG targets, {} EH "
                   "continuations, check {}, dispatch {}, cookie {}\n",
                   r.cfg->guard_flags, r.cfg->functions, r.cfg->eh_continuations,
                   r.cfg->check_pointer ? "yes" : "no", r.cfg->dispatch_pointer ? "yes" : "no",
                   r.cfg->security_cookie ? "yes" : "no");
    }
    size_t symbols = 0;
    size_t ordinals = 0;
    size_t delayed = 0;
    for (const recomp::ImportFacts& module : r.imports) {
        symbols += module.symbols;
        ordinals += module.by_ordinal;
        delayed += module.delay_load ? 1 : 0;
    }
    fmt::print("  imports             {} DLLs ({} delay-load), {} symbols ({} by ordinal)\n",
               r.imports.size(), delayed, symbols, ordinals);

    const recomp::CxxExceptionStats& x = r.cxx.stats;
    fmt::print("  handlers            {} functions: {} FH3 ({} by magic), {} FH4, {} through a GS "
               "wrapper, {} cookie check only ({} handlers), {} other\n",
               x.handlers, x.fh3, x.by_magic, x.fh4, x.gs_wrapped, x.gs_only, x.gs_handlers,
               x.other_handlers);
    fmt::print("  c++ functions       {} funclets, {} with catches, {} of them unverifiable, {} "
               "unreadable\n",
               x.funclets, x.with_catches, x.unverifiable, x.unreadable);
    fmt::print("  catch continuations {} in the guard table{}, {} recorded by FH4, {} returned "
               "by funclets, {} stray\n",
               r.cxx.guard, r.cxx.guard_table ? "" : " (no table)", r.cxx.recorded, r.cxx.returned,
               x.stray_continuations);

    const recomp::CoverageFacts& c = r.coverage;
    fmt::print("\ncoverage\n");
    fmt::print("  functions           {}\n", c.functions);
    fmt::print("  instructions        {} walked, {} swept\n", c.instructions, c.swept_instructions);
    fmt::print("  .pdata code         {:#x} bytes, {:#x} not reached by the walk\n", c.pdata_bytes,
               c.unreached_bytes);
    fmt::print("  unresolved jumps    {}\n", c.unresolved_jumps);
    fmt::print("  errors              {} decode, {} directory\n", c.decode_errors,
               c.directory_errors);

    fmt::print("\nfeatures\n");
    std::string baseline;
    for (const recomp::FeatureUse& use : r.features) {
        if (use.feature_class == recomp::FeatureClass::kBaseline) {
            baseline += fmt::format("{}{} {}", baseline.empty() ? "" : ", ", use.name, use.count);
            continue;
        }
        std::string host;
        if (const auto here = recomp::host_supports(use.extension)) {
            host = *here ? ", this host: yes" : ", this host: no";
        }
        fmt::print("  {:<18}  {} x{}{}: {}\n", use.name, recomp::describe(use.feature_class),
                   use.count, host, sites(use.sites));
    }
    fmt::print("  baseline            {}\n", baseline.empty() ? "none" : baseline);

    fmt::print("\nfs: and gs: accesses\n");
    for (const recomp::SegmentAccess& access : r.segments) {
        const std::string field =
            access.computed || access.fs ? "" : recomp::teb_field_name(access.offset);
        fmt::print("  {:<22} {:<26} {} reads, {} writes: {}\n", recomp::describe(access), field,
                   access.reads, access.writes, sites(access.sites));
    }
    const recomp::TlsSlotUse& slots = r.tls_slots;
    fmt::print("  tls slots           {} indexed, {} other index, {} fixed, {} not followed; {} "
               "index reads\n",
               slots.indexed.size(), slots.other_index.size(), slots.fixed_slot.size(),
               slots.not_followed.size(), slots.index_reads);

    const auto groups = [&](const char* title, const std::vector<recomp::InstructionUse>& uses) {
        fmt::print("\n{}\n", title);
        if (uses.empty()) {
            fmt::print("  none\n");
        }
        for (const recomp::InstructionUse& use : uses) {
            fmt::print("  {:<22} x{}: {}\n", use.name, use.sites.size(), sites(use.sites));
        }
    };
    groups("system instructions", r.system);
    groups("traps", r.traps);
    groups("timing", r.timing);
    fmt::print("\ncpuid and xgetbv (trapped by the loader)\n");
    fmt::print("  cpuid                  x{}: {}\n", r.cpuid.size(), sites(r.cpuid));
    fmt::print("  xgetbv                 x{}: {}\n", r.xgetbv.size(), sites(r.xgetbv));

    const auto references = [&](const char* title, const std::vector<recomp::CodeReference>& list) {
        fmt::print("\n{} ({})\n", title, list.size());
        size_t shown = 0;
        for (const recomp::CodeReference& reference : list) {
            if (shown++ == limit) {
                fmt::print("  +{} more\n", list.size() - limit);
                break;
            }
            fmt::print("  {:#010x} -> {:#010x}{}\n", reference.site, reference.target,
                       reference.pointer ? " (relocated pointer)"
                       : reference.swept ? " (swept)"
                                         : "");
        }
    };
    references("code reads", r.code_reads);
    references("code writes", r.code_writes);
    references("addresses inside functions", r.code_addresses);

    const std::vector<std::string> notes = recomp::review_notes(r);
    fmt::print("\nreview ({})\n", notes.size());
    for (const std::string& note : notes) {
        fmt::print("  - {}\n", note);
    }
    return 0;
}

int cmd_rtti(const pe::Image& image, const std::vector<std::string_view>& flags) {
    bool list = false;
    for (const std::string_view flag : flags) {
        if (flag == "--list") {
            list = true;
        } else {
            LOG_ERROR(x64recomp, "unknown option {}", flag);
            return usage();
        }
    }
    const recomp::RttiReport r = recomp::scan_rtti(image);
    size_t multiple = 0;
    size_t virtual_bases = 0;
    for (const auto& [rva, cls] : r.classes) {
        multiple += (cls.attributes & recomp::kRttiMultipleInheritance) != 0 ? 1 : 0;
        virtual_bases += (cls.attributes & recomp::kRttiVirtualInheritance) != 0 ? 1 : 0;
    }
    std::multimap<uint32_t, const recomp::RttiVtable*> vtables;
    size_t slots = 0;
    size_t largest = 0;
    for (const recomp::RttiVtable& vtable : r.vtables) {
        vtables.emplace(vtable.type_descriptor, &vtable);
        slots += vtable.slots;
        largest = std::max(largest, vtable.slots);
    }
    fmt::print("type descriptors      {}\n", r.type_descriptors.size());
    fmt::print("locators              {} ({} without a vtable, {} malformed)\n", r.locators,
               r.locators_without_vtable, r.bad_locators);
    fmt::print("polymorphic classes   {} ({} with multiple inheritance, {} with virtual bases)\n",
               r.classes.size(), multiple, virtual_bases);
    fmt::print("vtables               {} ({} slots in all, largest {})\n", r.vtables.size(), slots,
               largest);
    if (!r.first_error.empty()) {
        fmt::print("first error           {}\n", r.first_error);
    }
    if (list) {
        for (const auto& [rva, cls] : r.classes) {
            fmt::print("\n{:#010x}  {}\n", rva, recomp::undecorate_type_name(cls.name));
            for (size_t i = 1; i < cls.bases.size(); ++i) {
                const recomp::RttiBase& base = cls.bases[i];
                fmt::print("  base    {}{}\n", recomp::undecorate_type_name(base.name),
                           base.vbtable_displacement >= 0
                               ? " (virtual)"
                               : fmt::format(" at {:#x}", base.member_displacement));
            }
            const auto [first, last] = vtables.equal_range(rva);
            for (auto it = first; it != last; ++it) {
                fmt::print("  vtable  {:#010x}  offset {:#x}, {} slots\n", it->second->rva,
                           it->second->offset, it->second->slots);
            }
        }
    }
    return r.bad_locators == 0 ? 0 : 1;
}

int cmd_emit(const pe::Image& image, const std::vector<std::string_view>& flags) {
    recomp::EmitOptions options;
    std::filesystem::path out_dir;
    uint64_t missing = 0;
    bool strict = false;
    for (size_t i = 0; i < flags.size(); ++i) {
        const std::string_view flag = flags[i];
        const bool has_value = i + 1 < flags.size();
        uint64_t number = 0;
        if (flag == "--out" && has_value) {
            out_dir = std::filesystem::path(std::string(flags[++i]));
        } else if (flag == "--name" && has_value) {
            options.name = std::string(flags[++i]);
        } else if (flag == "--base" && has_value && parse_number(flags[i + 1], number)) {
            options.base = number;
            ++i;
        } else if (flag == "--chunks" && has_value && parse_number(flags[i + 1], number) &&
                   number > 0 && number <= 4096) {
            options.chunks = static_cast<unsigned>(number);
            ++i;
        } else if (flag == "--missing" && has_value && parse_number(flags[i + 1], number)) {
            missing = number;
            ++i;
        } else if (flag == "--strict") {
            strict = true;
        } else {
            LOG_ERROR(x64recomp, "bad option {}", flag);
            return usage();
        }
    }
    if (!out_dir.empty() && options.name.empty()) {
        LOG_ERROR(x64recomp, "--out needs --name");
        return usage();
    }
    const recomp::DiscoveryResult discovery = recomp::discover_functions(image);
    const recomp::EmitStats s = recomp::emit_module(image, discovery, options, out_dir);
    const auto percent = [](size_t part, size_t whole) {
        return whole == 0 ? 100.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };
    fmt::print("functions             {} ({} fully translated, {:.2f}%)\n", s.functions,
               s.complete_functions, percent(s.complete_functions, s.functions));
    fmt::print("instructions          {} ({} untranslated, {:.3f}% translated)\n", s.instructions,
               s.unimplemented, percent(s.instructions - s.unimplemented, s.instructions));
    fmt::print("decode errors         {}\n", s.decode_errors);
    fmt::print("jump tables           {}\n", s.jump_tables);
    fmt::print("run-time dispatch     {} calls, {} jumps\n", s.indirect_calls, s.indirect_jumps);
    if (missing > 0 && !s.missing.empty()) {
        std::vector<std::pair<std::string, size_t>> sorted(s.missing.begin(), s.missing.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
        fmt::print("untranslated mnemonics ({} distinct)\n", sorted.size());
        for (size_t i = 0; i < sorted.size() && i < missing; ++i) {
            fmt::print("  {:<16} {}\n", sorted[i].first, sorted[i].second);
        }
    }
    if (strict && (s.unimplemented > 0 || s.decode_errors > 0)) {
        LOG_ERROR(x64recomp, "--strict: {} instructions untranslated, {} decode errors",
                  s.unimplemented, s.decode_errors);
        for (const auto& [mnemonic, count] : s.missing) {
            LOG_ERROR(x64recomp, "  {} x{}", mnemonic, count);
        }
        return 1;
    }
    return 0;
}

int run(int argc, wchar_t** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::filesystem::path command_path(argv[1]);
    const std::string command = command_path.string();
    const std::filesystem::path file(argv[2]);
    std::vector<std::string> flag_storage;
    for (int i = 3; i < argc; ++i) {
        flag_storage.push_back(std::filesystem::path(argv[i]).string());
    }
    const std::vector<std::string_view> flags(flag_storage.begin(), flag_storage.end());

    const pe::Image image = pe::Image::load_file(file);
    if (command == "info" && flags.empty()) {
        return cmd_info(image);
    }
    if (command == "imports" && flags.empty()) {
        return cmd_imports(image);
    }
    if (command == "exports" && flags.empty()) {
        return cmd_exports(image);
    }
    if (command == "functions") {
        return cmd_functions(image, flags);
    }
    if (command == "scan") {
        return cmd_scan(image, flags);
    }
    if (command == "rtti") {
        return cmd_rtti(image, flags);
    }
    if (command == "emit") {
        return cmd_emit(image, flags);
    }
    return usage();
}

} // namespace

int wmain(int argc, wchar_t** argv) { // NOLINT(misc-use-internal-linkage): process entry point
    try {
        return run(argc, argv);
    } catch (const pe::FormatError& error) {
        LOG_ERROR(x64recomp, "malformed PE: {}", error.what());
    } catch (const std::exception& error) {
        LOG_ERROR(x64recomp, "{}", error.what());
    }
    return 1;
}
