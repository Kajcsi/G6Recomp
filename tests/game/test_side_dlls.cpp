// Game-file suite (label `game`, local only, never in CI): the side DLLs under the default import
// policy (docs/side-dlls.md, "Tests"). Each DLL the loader maps is loaded and initialized on its
// own, and every import that ended up a stub must be one the policy chose; the replaced and
// networking DLLs are never mapped; and crt_guest, an original /MD fixture, runs on the console's
// C++ runtime. It prints counts and the names of public API functions only. Skips when
// G6_GAME_DIR is unset or missing.

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "crt_guest_api.h"
#include "g6/imports/policy.h"
#include "g6/kernel/image_loader.h"
#include "g6/loader/loader.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6test.h"

namespace fs = std::filesystem;
namespace pe = g6::pe;
using g6::imports::default_game_policy;
using g6::kernel::LoadError;
using g6::loader::Binding;
using g6::loader::ImportRegistry;
using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::Module;
using g6::loader::Resolution;
using g6::loader::Symbol;

namespace {

// The side DLLs the loader maps (docs/side-dlls.md, "Which DLLs are mapped").
constexpr const char* kMappedDlls[] = {
    "vcruntime140.dll",         "vcruntime140_1.dll", "msvcp140.dll",
    "msvcp140_1.dll",           "msvcp140_2.dll",     "msvcp140_atomic_wait.dll",
    "msvcp140_codecvt_ids.dll", "concrt140.dll",      "vcomp140.dll",
    "vccorlib140.dll",          "Bink2Scarlett.dll",  "amd_fidelityfx_loader_xbox.dll",
};
constexpr const char* kReplacedDlls[] = {"XboxBvhBuilder.dll", "amd_fidelityfx_upscaler_xbox.dll"};
constexpr const char* kNetworkingDlls[] = {"XCurl.dll", "libHttpClient.GDK.dll"};
// The console runtime DLLs crt_guest's imports must bind to.
constexpr const char* kConsoleRuntime[] = {
    "vcruntime140.dll", "vcruntime140_1.dll",       "msvcp140.dll",  "msvcp140_1.dll",
    "msvcp140_2.dll",   "msvcp140_atomic_wait.dll", "concrt140.dll", "vcomp140.dll",
};

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

fs::path require_game_dir() {
    fs::path dir = game_dir();
    if (dir.empty() || !fs::is_directory(dir)) {
        SKIP("G6_GAME_DIR is not set or not a directory");
    }
    return dir;
}

LoaderOptions game_options(const fs::path& dir) {
    LoaderOptions options;
    options.search_paths.push_back(dir);
    options.imports = default_game_policy();
    return options;
}

// Where an import slot points.
enum class Target { Guest, Host, Runtime, Stub };

Target classify(const Loader& loader, uint64_t address) {
    if (loader.module_at(address)) {
        return Target::Guest;
    }
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &owner) == 0) {
        return Target::Stub; // the loader's stub thunks live outside every module
    }
    // HLE, ours or the loader's, is linked into this test.
    return owner == GetModuleHandleW(nullptr) ? Target::Runtime : Target::Host;
}

// Whether the policy stubs an import on purpose: a stub rule, an offline rule, or a replaced
// DLL. Anything else (deny by default, a function the host lacks, an export a guest module
// lacks) means the policy needs a decision.
bool deliberate_stub(const Resolution& resolution) {
    return resolution.binding == Binding::Offline ||
           resolution.note == "stubbed by a function rule" || resolution.note == "stubbed DLL" ||
           resolution.note == "replaced DLL without an implementation yet";
}

struct Counts {
    size_t guest = 0;
    size_t host = 0;
    size_t runtime = 0;
    size_t stub = 0;
};

struct Stubs {
    // The deliberate stubs of the module under test, as "dll!symbol (why)".
    std::vector<std::string> deliberate;
    // The stubs the policy didn't choose, in any mapped module, as "module: dll!symbol (why)".
    std::vector<std::string> unexpected;
};

// Classifies every import of every mapped module.
Stubs find_stubs(const Loader& loader, ImportRegistry& policy, std::string_view tested,
                 Counts& counts) {
    Stubs stubs;
    const std::string tested_key = g6::loader::normalize_dll_name(tested);
    for (const Module& module : loader.modules()) {
        const bool under_test = g6::loader::normalize_dll_name(module.name()) == tested_key;
        for (const pe::ImportedModule& imported : pe::read_imports(module.image())) {
            const std::string key = g6::loader::normalize_dll_name(imported.dll);
            for (const pe::ImportedSymbol& entry : imported.symbols) {
                const Symbol symbol = entry.name.empty()
                                          ? Symbol::by_ordinal(entry.ordinal.value_or(0))
                                          : Symbol::by_name(entry.name);
                const uint64_t slot =
                    *reinterpret_cast<const uint64_t*>(module.base() + entry.iat_rva);
                switch (classify(loader, slot)) {
                case Target::Guest: ++counts.guest; continue;
                case Target::Host: ++counts.host; continue;
                case Target::Runtime: ++counts.runtime; continue;
                case Target::Stub: ++counts.stub; break;
                }
                const std::string name =
                    fmt::format("{}!{}", key, g6::loader::symbol_display_name(key, symbol));
                if (loader.find(key)) {
                    stubs.unexpected.push_back(fmt::format(
                        "{}: {} (the guest module doesn't export it)", module.name(), name));
                    continue;
                }
                const Resolution resolution = policy.resolve(key, symbol);
                if (!deliberate_stub(resolution)) {
                    stubs.unexpected.push_back(
                        fmt::format("{}: {} ({})", module.name(), name, resolution.note));
                } else if (under_test) {
                    stubs.deliberate.push_back(fmt::format("{} ({})", name, resolution.note));
                }
            }
        }
    }
    return stubs;
}

void report(const Stubs& stubs) {
    for (const std::string& line : stubs.deliberate) {
        fmt::print(stdout, "      stub {}\n", line);
    }
    for (const std::string& line : stubs.unexpected) {
        fmt::print(stdout, "      UNEXPECTED stub {}\n", line);
    }
}

} // namespace

TEST_CASE("game: each side DLL maps and initializes under the default policy") {
    const fs::path dir = require_game_dir();
    ImportRegistry policy = default_game_policy();
    for (const char* dll : kMappedDlls) {
        INFO(dll);
        if (!fs::is_regular_file(dir / dll)) {
            CHECK(fs::is_regular_file(dir / dll));
            continue;
        }
        Loader loader(game_options(dir));
        loader.load(dll); // maps its dependencies and runs their initialization
        REQUIRE(loader.find(dll).has_value());
        for (const Module& module : loader.modules()) {
            CHECK(!policy.is_replaced(module.name()));
            CHECK(!g6::loader::is_networking_dll(module.name()));
        }
        Counts counts;
        const Stubs stubs = find_stubs(loader, policy, dll, counts);
        fmt::print(stdout,
                   "    {:<32} {:>2} modules: {:>4} from guest modules, {:>4} host, {:>3} runtime "
                   "HLE, {:>3} stubs\n",
                   dll, loader.modules().size(), counts.guest, counts.host, counts.runtime,
                   counts.stub);
        report(stubs);
        CHECK(stubs.unexpected.empty());
    } // each Loader unmaps its modules here
}

TEST_CASE("game: replaced and networking DLLs are never mapped") {
    const fs::path dir = require_game_dir();
    Loader loader(game_options(dir));
    for (const auto& list :
         {std::vector<const char*>(std::begin(kReplacedDlls), std::end(kReplacedDlls)),
          std::vector<const char*>(std::begin(kNetworkingDlls), std::end(kNetworkingDlls))}) {
        for (const char* dll : list) {
            INFO(dll);
            CHECK(fs::is_regular_file(dir / dll));
            CHECK_THROWS_AS(loader.load(dll), LoadError);
            CHECK_THROWS_AS(loader.load(dir / dll), LoadError);
        }
    }
    CHECK(loader.modules().empty());
}

// Runs last and never destroys its Loader, not even at exit: the console's OpenMP and ConcRT
// runtimes keep worker threads in their own code after the calls return, so their modules must
// stay mapped until the process is gone.
TEST_CASE("game: crt_guest runs on the console's C++ runtime") {
    const fs::path dir = require_game_dir();
    static Loader* kept = nullptr;
    if (kept != nullptr) {
        SKIP("ran already in this process");
    }
    kept = new Loader(game_options(dir)); // NOLINT(cppcoreguidelines-owning-memory): never freed
    Loader& loader = *kept;
    const Module guest = loader.load(fs::path(G6_CRT_GUEST_DIR) / "crt_guest.dll");

    // Every runtime import binds to the console's copy, and none is missing from it.
    std::vector<std::string> problems;
    for (const pe::ImportedModule& imported : pe::read_imports(guest.image())) {
        const std::string key = g6::loader::normalize_dll_name(imported.dll);
        bool console_runtime = false;
        for (const char* runtime : kConsoleRuntime) {
            console_runtime = console_runtime || key == g6::loader::normalize_dll_name(runtime);
        }
        if (!console_runtime) {
            continue;
        }
        const std::optional<Module> provider = loader.find(key);
        if (!provider) {
            problems.push_back(fmt::format("{} isn't mapped from the install", key));
            continue;
        }
        for (const pe::ImportedSymbol& entry : imported.symbols) {
            const uint64_t slot = *reinterpret_cast<const uint64_t*>(guest.base() + entry.iat_rva);
            const std::optional<Module> owner = loader.module_at(slot);
            if (!owner) {
                problems.push_back(fmt::format("{}!{} isn't exported by the console's copy", key,
                                               entry.name.empty()
                                                   ? fmt::format("#{}", entry.ordinal.value_or(0))
                                                   : entry.name));
            }
        }
    }
    for (const std::string& problem : problems) {
        fmt::print(stdout, "      {}\n", problem);
    }
    REQUIRE(problems.empty());
    // Its system imports are API sets, as in console code, and get no stub the policy didn't
    // choose either.
    ImportRegistry policy = default_game_policy();
    Counts counts;
    const Stubs stubs = find_stubs(loader, policy, "crt_guest.dll", counts);
    report(stubs);
    REQUIRE(stubs.unexpected.empty());

    const auto exceptions = guest.export_as<CrtGuestExceptionsFn>("crt_guest_exceptions");
    const auto throw_error = guest.export_as<CrtGuestThrowFn>("crt_guest_throw");
    const auto containers = guest.export_as<CrtGuestContainersFn>("crt_guest_containers");
    const auto threads = guest.export_as<CrtGuestThreadsFn>("crt_guest_threads");
    const auto format = guest.export_as<CrtGuestFormatFn>("crt_guest_format");
    const auto atomic_wait = guest.export_as<CrtGuestAtomicWaitFn>("crt_guest_atomic_wait");
    const auto openmp = guest.export_as<CrtGuestOpenMpFn>("crt_guest_openmp");
    const auto ppl = guest.export_as<CrtGuestPplFn>("crt_guest_ppl");
    REQUIRE(exceptions != nullptr);
    REQUIRE(throw_error != nullptr);
    REQUIRE(containers != nullptr);
    REQUIRE(threads != nullptr);
    REQUIRE(format != nullptr);
    REQUIRE(atomic_wait != nullptr);
    REQUIRE(openmp != nullptr);
    REQUIRE(ppl != nullptr);

    CHECK_EQ(exceptions(), kCrtGuestExceptions);
    bool caught = false;
    try {
        throw_error("from the guest");
    } catch (const std::exception& error) {
        caught = std::string(error.what()) == "from the guest";
    }
    CHECK(caught);
    CHECK_EQ(containers(), kCrtGuestContainers);
    CHECK_EQ(threads(8), int64_t{36});

    char text[64] = {};
    CHECK_EQ(format(1234567, text, sizeof(text)), 14);
    CHECK_EQ(std::string(text), std::string("1,234,567 1.23"));

    CHECK_EQ(atomic_wait(200), 400);

    int team = 0;
    CHECK_EQ(openmp(1000, &team), int64_t{500500});
    CHECK(team >= 1);
    fmt::print(stdout, "    OpenMP team of {} threads ({} logical processors)\n", team,
               std::thread::hardware_concurrency());

    CHECK_EQ(ppl(1000), kCrtGuestTaskResult * 1000000 + 500500);
}
