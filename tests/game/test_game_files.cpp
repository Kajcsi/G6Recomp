// Game-file suite (label `game`, local only, never in CI): parses every top-level PE in the
// user's install with each reader, runs function discovery, runs the pre-flight scan
// (docs/scan.md), maps the C++ exception tables (docs/hooks.md), and reads the RTTI
// (docs/re-workflow.md). It prints counts, and for the
// main executable the scan's review notes, which hold addresses and prose only; nothing it reads or
// derives is written anywhere. Skips when G6_GAME_DIR is unset or missing.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cxx_exceptions.h"
#include "function_discovery.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6/pe/unwind.h"
#include "g6test.h"
#include "rtti.h"
#include "scan.h"

namespace fs = std::filesystem;
namespace pe = g6::pe;
namespace recomp = g6::recomp;

namespace {

fs::path game_dir() {
    wchar_t* value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, L"G6_GAME_DIR") != 0 || value == nullptr) {
        return {};
    }
    fs::path path(value);
    std::free(value);
    return path;
}

std::vector<fs::path> top_level(const fs::path& dir, const std::wstring& extension) {
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

// What the scan found in the 1.0.2.1 side DLLs (docs/scan.md), held as a regression check: no
// system instructions, only the TEB fields desktop Windows has at the same offsets, TLS through
// the module's own index, and no code read or written as data.
void check_side_dll_scan(const recomp::ScanReport& scan) {
    CHECK(scan.system.empty());
    for (const recomp::SegmentAccess& access : scan.segments) {
        INFO(recomp::describe(access));
        CHECK(!access.fs);
        CHECK(!access.computed);
        CHECK(recomp::expected_teb_offset(access.offset));
    }
    CHECK(scan.tls_slots.fixed_slot.empty());
    CHECK(scan.tls_slots.other_index.empty());
    CHECK(scan.tls_slots.not_followed.empty());
    CHECK(scan.code_reads.empty());
    CHECK(scan.code_writes.empty());
}

void check_scan(const fs::path& path, const pe::Image& image,
                const recomp::DiscoveryResult& discovery, bool side_dll) {
    const recomp::ScanReport scan = recomp::scan_module(image, discovery);
    INFO(path.filename().string());
    CHECK_EQ(scan.coverage.decode_errors, size_t{0});
    CHECK_EQ(scan.coverage.directory_errors, size_t{0});
    if (side_dll) {
        check_side_dll_scan(scan);
    }
    size_t host_check = 0;
    size_t not_zen2 = 0;
    for (const recomp::FeatureUse& use : scan.features) {
        host_check += use.feature_class == recomp::FeatureClass::kHostCheck ? use.count : 0;
        not_zen2 += use.feature_class == recomp::FeatureClass::kNotZen2 ? use.count : 0;
    }
    const std::vector<std::string> notes = recomp::review_notes(scan);
    fmt::print(stdout,
               "    {:<34} scan: {:>4} host-check {:>4} not-zen2 {:>2} system {:>6} unreached "
               "bytes {:>3} review notes\n",
               "", host_check, not_zen2, scan.system.size(), scan.coverage.unreached_bytes,
               notes.size());
    if (!side_dll) {
        for (const std::string& note : notes) {
            fmt::print(stdout, "      - {}\n", note);
        }
    }
}

// The C++ exception map (docs/hooks.md, "C++ exception functions"): every C++ handler's tables
// read, every continuation FH4 records or a catch funclet returns is listed in the CFG EH
// continuation table when there is one, and every listed continuation inside a function with
// catches is one of those two, so the map misses no catch in the functions the hook engine checks.
void check_cxx(const pe::Image& image, const recomp::DiscoveryResult& discovery) {
    const recomp::CxxExceptionMap map = recomp::map_cxx_exceptions(image, discovery);
    const recomp::CxxExceptionStats& stats = map.stats;
    CHECK_EQ(stats.unreadable, size_t{0});
    CHECK_EQ(stats.stray_continuations, size_t{0});
    size_t unexplained = 0;
    for (const recomp::CxxFunction& function : map.functions) {
        const bool has_catch = std::ranges::any_of(
            function.info.try_blocks, [](const auto& block) { return !block.catches.empty(); });
        if (!map.guard_table || function.funclet || !has_catch) {
            continue;
        }
        for (const uint32_t target : function.continuations) {
            const bool found = std::ranges::binary_search(map.recorded, target) ||
                               std::ranges::binary_search(map.returned, target);
            unexplained += found ? 0 : 1;
        }
    }
    CHECK_EQ(unexplained, size_t{0});
    // A module links one C runtime, so at most one handler only checks the stack cookie.
    CHECK(stats.gs_handlers <= 1);
    fmt::print(stdout,
               "    {:<34} c++: {:>4} fh3 {:>4} fh4 ({} by magic, {} gs-wrapped) {:>3} funclets "
               "{:>3} with catches ({} unverifiable) {:>4} cookie only {:>4} other handlers; "
               "continuations {} guard{} {} recorded {} returned\n",
               "", stats.fh3, stats.fh4, stats.by_magic, stats.gs_wrapped, stats.funclets,
               stats.with_catches, stats.unverifiable, stats.gs_only, stats.other_handlers,
               map.guard.size(), map.guard_table ? "" : " (no table)", map.recorded.size(),
               map.returned.size());
}

// The RTTI scan (tools/x64recomp/src/rtti.h): every locator's class hierarchy reads, and every
// class's type descriptor is among those found by name. In the side DLLs every locator also has a
// vtable.
void check_rtti(const pe::Image& image, bool side_dll) {
    const recomp::RttiReport rtti = recomp::scan_rtti(image);
    INFO(rtti.first_error);
    CHECK_EQ(rtti.bad_locators, size_t{0});
    if (side_dll) {
        CHECK_EQ(rtti.locators_without_vtable, size_t{0});
    }
    size_t unlisted = 0;
    for (const auto& [rva, cls] : rtti.classes) {
        unlisted += std::ranges::binary_search(rtti.type_descriptors, rva) ? 0 : 1;
    }
    CHECK_EQ(unlisted, size_t{0});
    fmt::print(stdout,
               "    {:<34} rtti: {:>5} type descriptors {:>5} classes {:>5} vtables ({} without "
               "one)\n",
               "", rtti.type_descriptors.size(), rtti.classes.size(), rtti.vtables.size(),
               rtti.locators_without_vtable);
}

void check_pe(const fs::path& path, bool side_dll) {
    const pe::Image image = pe::Image::load_file(path);
    CHECK_EQ(image.machine(), pe::kMachineAmd64);
    const auto imports = pe::read_imports(image);
    const auto exports = pe::read_exports(image);
    const auto relocations = pe::read_relocations(image);
    (void)pe::read_tls(image);
    (void)pe::read_load_config(image);
    const auto runtime_functions = pe::read_runtime_functions(image);
    const recomp::DiscoveryResult result = recomp::discover_functions(image);
    CHECK_EQ(result.stats.unwind_errors, size_t{0});
    CHECK_EQ(result.stats.directory_errors, size_t{0});
    const bool entry_found =
        image.entry_point() == 0 || result.find(image.entry_point()) != nullptr;
    CHECK(entry_found);
    // Every resolved table entry must land inside the function that dispatches through it.
    size_t stray_targets = 0;
    for (const recomp::DiscoveredFunction& function : result.functions) {
        for (const recomp::JumpTable& table : function.jump_tables) {
            stray_targets += static_cast<size_t>(std::ranges::count_if(
                table.targets, [&](uint32_t target) { return !function.contains(target); }));
        }
    }
    CHECK_EQ(stray_targets, size_t{0});

    size_t symbols = 0;
    for (const auto& module : imports) {
        symbols += module.symbols.size();
    }
    fmt::print(stdout,
               "    {:<34} {:>3} dlls {:>5} imports {:>5} exports {:>6} relocs {:>6} pdata "
               "{:>6} functions ({} leaves) {:>4} jump tables {:>3} unresolved jumps\n",
               path.filename().string(), imports.size(), symbols, exports.exports.size(),
               relocations.size(), runtime_functions.size(), result.functions.size(),
               result.stats.leaf_functions, result.stats.jump_tables,
               result.stats.unresolved_indirect_jumps);
    check_scan(path, image, result, side_dll);
    check_cxx(image, result);
    check_rtti(image, side_dll);
}

} // namespace

TEST_CASE("game: every top-level DLL parses and discovers cleanly") {
    const fs::path dir = game_dir();
    if (dir.empty() || !fs::is_directory(dir)) {
        SKIP("G6_GAME_DIR is not set or not a directory");
    }
    const std::vector<fs::path> dlls = top_level(dir, L".dll");
    if (dlls.empty()) {
        SKIP("no DLLs in G6_GAME_DIR");
    }
    for (const fs::path& dll : dlls) {
        check_pe(dll, true);
    }
}

TEST_CASE("game: the main executable parses and discovers cleanly") {
    const fs::path dir = game_dir();
    if (dir.empty() || !fs::is_directory(dir)) {
        SKIP("G6_GAME_DIR is not set or not a directory");
    }
    const std::vector<fs::path> executables = top_level(dir, L".exe");
    if (executables.empty()) {
        SKIP("no executable in G6_GAME_DIR");
    }
    for (const fs::path& exe : executables) {
        check_pe(exe, false);
    }
}
