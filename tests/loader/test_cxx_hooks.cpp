// The hook engine's C++ exception rules (docs/hooks.md, "C++ exception functions") on an original
// guest (tests/fixtures/eh_guest), built by clang-cl (FH3) and cl.exe (FH4), each with and
// without an EH continuation table: what the exception map reads from each build, which
// mid-function hooks the engine accepts and refuses, and that exceptions still reach their catch
// blocks past an accepted hook. cl.exe's builds also have a function whose only handler checks
// the stack cookie (docs/hooks.md, "Functions under a cookie check alone").
//
// Sites are found from the tables at run time rather than hard-coded, since each compiler lays
// the functions out differently.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "code_walker.h"
#include "cxx_exceptions.h"
#include "function_discovery.h"
#include "g6/kernel/image_loader.h"
#include "g6/loader/loader.h"
#include "g6/loader/patches.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6test.h"

using g6::kernel::LoadError;
using g6::loader::HookContext;
using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::Module;
using g6::loader::ModulePatches;
using g6::recomp::CxxFunction;

namespace {

constexpr uint32_t kWindow = 5; // a patch jump's size

struct Variant {
    std::string_view name;
    bool fh4 = false;
    bool ehcont = false;

    std::string dll() const { return fmt::format("eh_guest_{}.dll", name); }
};

std::vector<Variant> variants() {
    std::vector<Variant> list{{"fh3", false, false}, {"fh3_ehcont", false, true}};
    if (G6_EH_GUEST_FH4 != 0) {
        list.push_back({"fh4", true, false});
        list.push_back({"fh4_ehcont", true, true});
    }
    return list;
}

// One build of the guest as the engine sees it, read from the file.
// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's map move constructor allocates.
struct Guest {
    g6::pe::Image image;
    g6::recomp::DiscoveryResult discovery;
    g6::recomp::CxxExceptionMap map;
    std::map<std::string, uint32_t, std::less<>> exports;

    uint32_t rva_of(std::string_view name) const {
        const auto it = exports.find(name);
        if (it == exports.end()) {
            throw std::runtime_error(fmt::format("eh_guest doesn't export {}", name));
        }
        return it->second;
    }

    const CxxFunction& cxx(std::string_view name) const {
        const CxxFunction* function = map.find(rva_of(name));
        if (function == nullptr) {
            g6::test::report_failure(__FILE__, __LINE__,
                                     fmt::format("{} has no C++ exception tables", name));
            throw g6::test::RequireFailed{};
        }
        return *function;
    }

    // The instruction starts in the function at `start` that its code reaches, from the entry
    // and from each catch continuation inside it, as the patcher finds them.
    std::vector<uint32_t> instruction_starts(uint32_t start) const { return walk(start, false); }

    // The return addresses of the calls in the function at `start`.
    std::vector<uint32_t> return_addresses(uint32_t start) const { return walk(start, true); }

private:
    std::vector<uint32_t> walk(uint32_t start, bool after_calls) const {
        const g6::recomp::DiscoveredFunction* function = discovery.find(start);
        if (function == nullptr) {
            return {};
        }
        std::vector<g6::recomp::Range> ranges{{function->start, function->end}};
        ranges.insert(ranges.end(), function->fragments.begin(), function->fragments.end());
        std::vector<uint32_t> entries{start};
        for (const uint32_t continuation : map.continuations) {
            if (function->contains(continuation)) {
                entries.push_back(continuation);
            }
        }
        std::vector<uint32_t> found;
        g6::recomp::CodeWalker(image).walk(
            entries, ranges,
            [&](uint32_t rva) { return rva != start && discovery.find(rva) != nullptr; },
            [&](uint32_t rva, const ZydisDecodedInstruction& instruction) {
                if (!after_calls) {
                    found.push_back(rva);
                } else if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
                    found.push_back(rva + instruction.length);
                }
            });
        std::ranges::sort(found);
        const auto [first, last] = std::ranges::unique(found);
        found.erase(first, last);
        return found;
    }
};

Guest read_guest(const Variant& variant) {
    Guest guest{
        g6::pe::Image::load_file(std::string(G6_EH_GUEST_DIR) + "/" + variant.dll()), {}, {}, {}};
    guest.discovery = g6::recomp::discover_functions(guest.image);
    guest.map = g6::recomp::map_cxx_exceptions(guest.image, guest.discovery);
    for (const g6::pe::Export& entry : g6::pe::read_exports(guest.image).exports) {
        if (!entry.name.empty()) {
            guest.exports.emplace(entry.name, entry.rva);
        }
    }
    return guest;
}

std::filesystem::path system_directory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    return {std::wstring(buffer.data(), length)};
}

// The guest's C++ runtime is the host's vcruntime140.dll and vcruntime140_1.dll, mapped as guest
// modules rather than forwarded: the C++ handlers find the module that threw through
// RtlPcToFileHeader, which only the loader's HLE answers for guest modules. Every DLL in a search
// path is a guest file, so System32 can't be one; the two are copied to a folder of their own.
const std::filesystem::path& guest_runtime_directory() {
    static const std::filesystem::path folder = [] {
        std::filesystem::path path = std::filesystem::path(G6_EH_GUEST_DIR) / "host_vcruntime";
        std::filesystem::create_directories(path);
        for (const char* name : {"vcruntime140.dll", "vcruntime140_1.dll"}) {
            std::filesystem::copy_file(system_directory() / name, path / name,
                                       std::filesystem::copy_options::update_existing);
        }
        return path;
    }();
    return folder;
}

// The universal CRT and the system DLLs are forwarded.
LoaderOptions make_options(const Variant& variant, ModulePatches patches) {
    LoaderOptions options;
    options.search_paths.emplace_back(G6_EH_GUEST_DIR);
    options.search_paths.push_back(guest_runtime_directory());
    options.imports.forward_dll("kernel32.dll");
    options.imports.forward_dll("ntdll.dll");
    options.imports.forward_dll("api-ms-win-core-*");
    options.imports.forward_dll("api-ms-win-crt-*");
    options.patches.emplace(variant.dll(), std::move(patches));
    return options;
}

// The load's refusal, or nullopt when the guest loaded.
std::optional<std::string> refusal(const Variant& variant, ModulePatches patches) {
    Loader loader(make_options(variant, std::move(patches)));
    try {
        loader.load(variant.dll());
    } catch (const LoadError& error) {
        return std::string(error.what());
    }
    return std::nullopt;
}

bool contains(const std::optional<std::string>& message, std::string_view text) {
    return message && message->find(text) != std::string::npos;
}

// ---- The hook ----

int g_calls = 0;

void hook_count(HookContext& /*context*/) {
    ++g_calls;
}

using IntFn = int (*)(int);

template <typename T>
T guest_export(const Module& module, std::string_view name) {
    void* address = module.find_export(name);
    REQUIRE(address != nullptr);
    return reinterpret_cast<T>(address);
}

// The sites in `name` whose mid-function hook the engine refuses with a message containing
// `reason`, trying every instruction start but the entry.
std::vector<uint32_t> refused_sites(const Variant& variant, const Guest& guest,
                                    std::string_view name, std::string_view reason,
                                    bool handler_checked) {
    const uint32_t start = guest.rva_of(name);
    std::vector<uint32_t> sites;
    for (const uint32_t site : guest.instruction_starts(start)) {
        if (site == start) {
            continue;
        }
        ModulePatches patches;
        patches.mid(site, &hook_count).handler_checked = handler_checked;
        if (contains(refusal(variant, std::move(patches)), reason)) {
            sites.push_back(site);
        }
    }
    return sites;
}

} // namespace

TEST_CASE("the exception map reads each build's tables") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        const Guest guest = read_guest(variant);
        const auto format = variant.fh4 ? g6::pe::CxxEhFormat::kFh4 : g6::pe::CxxEhFormat::kFh3;
        CHECK_EQ(guest.map.stats.unreadable, 0u);
        CHECK_EQ(guest.map.stats.stray_continuations, 0u);
        CHECK_EQ(guest.map.guard_table, variant.ehcont);

        // ehg_catch: one try block with one catch, which resumes inside the function. The entry
        // is outside every state.
        const CxxFunction& catcher = guest.cxx("ehg_catch");
        CHECK(catcher.info.format == format);
        CHECK(!catcher.funclet);
        CHECK(catcher.verifiable);
        REQUIRE(catcher.info.try_blocks.size() == 1);
        CHECK_EQ(catcher.info.try_blocks[0].catches.size(), 1u);
        CHECK_EQ(catcher.info.state_at(catcher.start), -1);
        REQUIRE(!catcher.continuations.empty());
        const g6::recomp::DiscoveredFunction* body = guest.discovery.find(catcher.start);
        REQUIRE(body != nullptr);
        for (const uint32_t continuation : catcher.continuations) {
            CHECK(body->contains(continuation));
            // Each source this build has names it.
            if (variant.ehcont) {
                CHECK(std::ranges::binary_search(guest.map.guard, continuation));
            }
            if (variant.fh4) {
                CHECK(std::ranges::binary_search(guest.map.recorded, continuation));
            } else {
                CHECK(std::ranges::binary_search(guest.map.returned, continuation));
            }
        }

        // The catch funclet runs in the parent's states. FH3 gives it the parent's handler.
        const uint32_t funclet = catcher.info.try_blocks[0].catches[0].handler;
        if (const CxxFunction* entry = guest.map.find(funclet)) {
            CHECK(entry->funclet);
            CHECK(!entry->verifiable);
        } else {
            CHECK(variant.fh4);
        }

        // ehg_live: no try block, but a state with an object to destroy.
        const CxxFunction& live = guest.cxx("ehg_live");
        CHECK(live.verifiable);
        CHECK(live.info.try_blocks.empty());
        CHECK(live.info.states >= 1);

        // ehg_rethrow's catch never resumes, so only the guard table vouches for it.
        CHECK_EQ(guest.cxx("ehg_rethrow").verifiable, variant.ehcont);
    }
}

TEST_CASE("each build runs unpatched on the host's C++ runtime") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        Loader loader(make_options(variant, {}));
        const Module module = loader.load(variant.dll());
        const auto catcher = guest_export<IntFn>(module, "ehg_catch");
        const auto catch_buffer = guest_export<IntFn>(module, "ehg_catch_buffer");
        const auto catch_live = guest_export<IntFn>(module, "ehg_catch_live");
        const auto catch_rethrown = guest_export<IntFn>(module, "ehg_catch_rethrown");
        const auto cleanups = guest_export<int (*)()>(module, "ehg_cleanups");
        CHECK_EQ(catcher(5), 106);
        CHECK_EQ(catcher(-3), 94);
        // ehg_buffer passes maybe_throw a byte of its buffer: 6 for 3, and -16 for -40.
        CHECK_EQ(catch_buffer(3), 106);
        CHECK_EQ(catch_buffer(-40), -16);
        const int before = cleanups();
        CHECK_EQ(catch_live(1), 1002);
        CHECK_EQ(catch_live(-7), -7);
        CHECK_EQ(catch_rethrown(2), 3);
        CHECK_EQ(catch_rethrown(-4), -1004);
        CHECK_EQ(cleanups(), before + 3);
    }
}

TEST_CASE("a hook at a C++ function's entry is accepted, and exceptions still reach the catch") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        const Guest guest = read_guest(variant);
        ModulePatches patches;
        patches.mid(guest.rva_of("ehg_catch"), &hook_count);
        patches.mid(guest.rva_of("ehg_live"), &hook_count);
        Loader loader(make_options(variant, std::move(patches)));
        const Module module = loader.load(variant.dll());
        const auto catcher = guest_export<IntFn>(module, "ehg_catch");
        const auto catch_live = guest_export<IntFn>(module, "ehg_catch_live");
        const auto cleanups = guest_export<int (*)()>(module, "ehg_cleanups");

        g_calls = 0;
        CHECK_EQ(catcher(5), 106);
        CHECK_EQ(catcher(-3), 94); // caught, and resumed at the continuation
        CHECK_EQ(g_calls, 2);

        const int before = cleanups();
        CHECK_EQ(catch_live(1), 1002);
        CHECK_EQ(catch_live(-7), -7); // thrown through ehg_live, whose cleanup still runs
        CHECK_EQ(g_calls, 4);
        CHECK_EQ(cleanups(), before + 2);
    }
}

TEST_CASE("a window in a try block or where objects are live is refused") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        const Guest guest = read_guest(variant);

        const std::vector<uint32_t> in_try =
            refused_sites(variant, guest, "ehg_catch", "inside the try block", false);
        CHECK(!in_try.empty());
        const std::vector<uint32_t> live =
            refused_sites(variant, guest, "ehg_live", "where objects are live", false);
        CHECK(!live.empty());

        // A hand check overrides both. The sites whose windows hold no call or epilogue, and
        // don't start at a call's return address, then load, and exceptions still reach the
        // catch, since every call stays in place. Which sites those are depends on the compiler,
        // so only their count is checked.
        std::vector<uint32_t> sites = in_try;
        sites.insert(sites.end(), live.begin(), live.end());
        size_t loaded = 0;
        size_t ran = 0;
        for (const uint32_t site : sites) {
            INFO(fmt::format("site {:#x}", site));
            ModulePatches patches;
            patches.mid(site, &hook_count).handler_checked = true;
            Loader loader(make_options(variant, std::move(patches)));
            std::optional<Module> module;
            try {
                module = loader.load(variant.dll());
            } catch (const LoadError&) {
                continue; // refused by another rule
            }
            ++loaded;
            g_calls = 0;
            CHECK_EQ(guest_export<IntFn>(*module, "ehg_catch")(5), 106);
            CHECK_EQ(guest_export<IntFn>(*module, "ehg_live")(1), 1002);
            CHECK_EQ(guest_export<IntFn>(*module, "ehg_catch")(-3), 94);
            CHECK_EQ(guest_export<IntFn>(*module, "ehg_catch_live")(-7), -7);
            ran += g_calls > 0 ? 1 : 0;
        }
        CHECK(loaded > 0);
        CHECK(ran > 0);
    }
}

TEST_CASE("a window over a catch continuation is refused even after a hand check") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        const Guest guest = read_guest(variant);
        const CxxFunction& catcher = guest.cxx("ehg_catch");
        const std::vector<uint32_t> returns = guest.return_addresses(catcher.start);
        size_t candidates = 0;
        for (const uint32_t site : guest.instruction_starts(catcher.start)) {
            const bool covers = std::ranges::any_of(catcher.continuations, [&](uint32_t target) {
                return target > site && target < site + kWindow;
            });
            // A window at a return address is refused before this rule is checked.
            if (!covers || std::ranges::binary_search(returns, site)) {
                continue;
            }
            ++candidates;
            ModulePatches patches;
            patches.mid(site, &hook_count).handler_checked = true;
            const std::optional<std::string> message = refusal(variant, std::move(patches));
            INFO(fmt::format("site {:#x}: {}", site, message.value_or("loaded")));
            CHECK(contains(message, "a catch block resumes inside the window"));
        }
        CHECK(candidates > 0);
    }
}

TEST_CASE("a window at a call's return address is refused even after a hand check") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        const Guest guest = read_guest(variant);
        size_t candidates = 0;
        for (const std::string_view name : {"ehg_catch", "ehg_live", "ehg_buffer"}) {
            const uint32_t start = guest.rva_of(name);
            const std::vector<uint32_t> starts = guest.instruction_starts(start);
            for (const uint32_t site : guest.return_addresses(start)) {
                if (!std::ranges::binary_search(starts, site)) {
                    continue; // after a call that doesn't return
                }
                ++candidates;
                ModulePatches patches;
                patches.mid(site, &hook_count).handler_checked = true;
                const std::optional<std::string> message = refusal(variant, std::move(patches));
                INFO(fmt::format("{} at {:#x}: {}", name, site, message.value_or("loaded")));
                CHECK(contains(message, "return address of the call"));
            }
        }
        CHECK(candidates > 0);
    }
}

TEST_CASE("a function under a stack cookie check alone takes hooks without a hand check") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        const Guest guest = read_guest(variant);
        const uint32_t buffer = guest.rva_of("ehg_buffer");
        // cl.exe gives ehg_buffer __GSHandlerCheck. clang-cl checks the cookie without a handler.
        CHECK_EQ(guest.map.is_gs_only(buffer), variant.fh4);
        CHECK_EQ(guest.map.stats.gs_only, variant.fh4 ? 1u : 0u);
        CHECK_EQ(guest.map.stats.gs_handlers, variant.fh4 ? 1u : 0u);

        // Every window in the function either loads or is refused by a rule that has nothing to
        // do with the handler. Exceptions thrown through it still reach the caller's catch.
        size_t loaded = 0;
        size_t ran = 0;
        for (const uint32_t site : guest.instruction_starts(buffer)) {
            ModulePatches patches;
            patches.mid(site, &hook_count);
            Loader loader(make_options(variant, std::move(patches)));
            std::optional<Module> module;
            try {
                module = loader.load(variant.dll());
            } catch (const LoadError& error) {
                INFO(fmt::format("site {:#x}: {}", site, error.what()));
                CHECK(std::string_view(error.what()).find("handler_checked") ==
                      std::string_view::npos);
                continue;
            }
            ++loaded;
            INFO(fmt::format("site {:#x}", site));
            g_calls = 0;
            CHECK_EQ(guest_export<IntFn>(*module, "ehg_catch_buffer")(3), 106);
            CHECK_EQ(guest_export<IntFn>(*module, "ehg_catch_buffer")(-40), -16);
            // A window on clang-cl's cookie-failure path loads but never runs.
            ran += g_calls >= 1 ? 1 : 0;
        }
        // The entry and at least one window inside, and each of them ran.
        CHECK(loaded > 1);
        CHECK(ran > 1);
        CHECK(loaded - ran <= 1);
    }
}

TEST_CASE("a catch with no known continuation, or a funclet, keeps the hand check") {
    for (const Variant& variant : variants()) {
        INFO(std::string(variant.name));
        const Guest guest = read_guest(variant);
        const uint32_t rethrow = guest.rva_of("ehg_rethrow");

        ModulePatches patches;
        patches.mid(rethrow, &hook_count);
        const std::optional<std::string> message = refusal(variant, std::move(patches));
        if (variant.ehcont) {
            CHECK(!message);
        } else {
            CHECK(contains(message, "continuation isn't known"));
        }

        {
            ModulePatches checked;
            checked.mid(rethrow, &hook_count).handler_checked = true;
            Loader loader(make_options(variant, std::move(checked)));
            const Module module = loader.load(variant.dll());
            g_calls = 0;
            CHECK_EQ(guest_export<IntFn>(module, "ehg_catch_rethrown")(2), 3);
            CHECK_EQ(guest_export<IntFn>(module, "ehg_catch_rethrown")(-4), -1004);
            CHECK_EQ(g_calls, 2);
        }

        const uint32_t funclet = guest.cxx("ehg_catch").info.try_blocks.at(0).catches.at(0).handler;
        if (guest.map.find(funclet) != nullptr) {
            ModulePatches in_funclet;
            in_funclet.mid(funclet, &hook_count);
            CHECK(contains(refusal(variant, std::move(in_funclet)), "funclet"));
        }
    }
}
