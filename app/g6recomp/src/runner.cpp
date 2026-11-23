#include "g6/app/runner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <windows.h>
// After windows.h.
#include <bcrypt.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "g6/gdk/game_save.h"
#include "g6/imports/command_line.h"
#include "g6/imports/crt_exit.h"
#include "g6/imports/policy.h"
#include "g6/io/file_system.h"
#include "g6/io/text.h"
#include "g6/loader/crash_report.h"
#include "g6/loader/loader.h"
#include "g6/log.h"

namespace g6::app {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kUsage =
    "usage: g6recomp --game-dir <dir> [--exe <file>] [--data-dir <dir>] [--skip-int3]\n"
    "                [-- <arguments>]\n"
    "\n"
    "  --game-dir <dir>   the install, mounted read-only at G:\n"
    "  --exe <file>       the executable to run: a path, or a file name in the install\n"
    "                     (default: the main executable of the known build there)\n"
    "  --data-dir <dir>   the build's data directory; T: is its temp folder\n"
    "                     (default: %LOCALAPPDATA%\\G6Recomp\\<build ID>)\n"
    "  --skip-int3        step over each int3 in guest code (a soft assert) and log it\n"
    "                     once per address; for finding blockers, never for playing\n"
    "  --                 the rest is the guest's command line, after its program name\n"
    "  --help             this text\n";

std::string utf8(const fs::path& path) {
    return io::utf8(path.native());
}

// A cpuid bit the pre-flight scan's baseline needs (kExtensions in tools/x64recomp/src/scan.cpp).
// MMX, SSE, SSE2, and long mode are in every x64 CPU, AVX2's gathers and the VEX forms of AES come
// with AVX2 and AES, and PREFETCHW is a no-op on the Intel CPUs that predate it.
struct RequiredFeature {
    std::string_view name;
    uint32_t leaf;
    uint32_t subleaf;
    uint8_t reg; // 0-3: eax, ebx, ecx, edx
    uint8_t bit;
};

constexpr uint8_t kEax = 0;
constexpr uint8_t kEbx = 1;
constexpr uint8_t kEcx = 2;
constexpr uint8_t kEdx = 3;

constexpr std::array kRequiredFeatures = {
    RequiredFeature{"SSE3", 1, 0, kEcx, 0},
    RequiredFeature{"PCLMULQDQ", 1, 0, kEcx, 1},
    RequiredFeature{"SSSE3", 1, 0, kEcx, 9},
    RequiredFeature{"FMA", 1, 0, kEcx, 12},
    RequiredFeature{"CMPXCHG16B", 1, 0, kEcx, 13},
    RequiredFeature{"SSE4.1", 1, 0, kEcx, 19},
    RequiredFeature{"SSE4.2", 1, 0, kEcx, 20},
    RequiredFeature{"MOVBE", 1, 0, kEcx, 22},
    RequiredFeature{"POPCNT", 1, 0, kEcx, 23},
    RequiredFeature{"AES", 1, 0, kEcx, 25},
    RequiredFeature{"XSAVE", 1, 0, kEcx, 26},
    RequiredFeature{"AVX", 1, 0, kEcx, 28},
    RequiredFeature{"F16C", 1, 0, kEcx, 29},
    RequiredFeature{"RDRAND", 1, 0, kEcx, 30},
    RequiredFeature{"CLFLUSH", 1, 0, kEdx, 19},
    RequiredFeature{"BMI1", 7, 0, kEbx, 3},
    RequiredFeature{"AVX2", 7, 0, kEbx, 5},
    RequiredFeature{"BMI2", 7, 0, kEbx, 8},
    RequiredFeature{"XSAVEOPT", 0xD, 1, kEax, 0},
    RequiredFeature{"LZCNT", 0x80000001, 0, kEcx, 5},
    RequiredFeature{"RDTSCP", 0x80000001, 0, kEdx, 27},
};

constexpr uint32_t kOsxsaveBit = 27; // leaf 1, ecx
constexpr uint64_t kXcr0SseAvx = 0x6;

// XCR0, the register state Windows saves. Only for a CPU whose OSXSAVE bit is set.
#if defined(__clang__)
__attribute__((target("xsave")))
#endif
uint64_t read_xcr0() {
    return _xgetbv(0);
}

bool same_ascii(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

// File names compare as the file system compares them: ordinal, ignoring case.
bool same_file_name(const fs::path& path, std::string_view name) {
    const std::wstring file_name = path.filename().native();
    const std::wstring wanted = io::widen(name, CP_UTF8);
    return CompareStringOrdinal(file_name.data(), static_cast<int>(file_name.size()), wanted.data(),
                                static_cast<int>(wanted.size()), TRUE) == CSTR_EQUAL;
}

struct AlgorithmCloser {
    void operator()(void* algorithm) const {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm), 0);
    }
};

struct HashDestroyer {
    void operator()(void* hash) const { BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash)); }
};

// --skip-int3. A console build's assert is an int3 that a devkit's debugger stops at; without one,
// Windows ends the process. Many are soft: the code after them carries on. Stepping over them
// shows how far the guest gets, and each address is a finding to fix, not a fix.
LONG CALLBACK skip_guest_int3(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        IsDebuggerPresent() != FALSE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT& context = *exception->ContextRecord;
    // On x64, Windows reports an int3 at its own address.
    const loader::CodeAddress where = loader::locate_code(context.Rip);
    if (!where.guest || *reinterpret_cast<const uint8_t*>(context.Rip) != 0xCC) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::mutex mutex;
    static std::unordered_set<uint64_t> seen;
    bool first = false;
    {
        const std::scoped_lock lock(mutex);
        first = seen.insert(context.Rip).second;
    }
    if (first) {
        LOG_WARN(runner, "skipped the int3 at {} (--skip-int3)", loader::describe_code(where));
    }
    ++context.Rip;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void check_status(NTSTATUS status, std::string_view what) {
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(
            fmt::format("{} failed: {:#010x}", what, static_cast<uint32_t>(status)));
    }
}

} // namespace

std::string_view usage() {
    return kUsage;
}

RunOptions parse_arguments(std::span<const std::wstring> arguments) {
    RunOptions options;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--") {
            options.arguments.assign(arguments.begin() + static_cast<ptrdiff_t>(index) + 1,
                                     arguments.end());
            break;
        }
        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
            continue;
        }
        if (argument == L"--skip-int3") {
            options.skip_int3 = true;
            continue;
        }
        fs::path* target = nullptr;
        if (argument == L"--game-dir") {
            target = &options.game_dir;
        } else if (argument == L"--exe") {
            target = &options.exe;
        } else if (argument == L"--data-dir") {
            target = &options.data_dir;
        } else {
            throw UsageError(fmt::format("unknown argument {}", io::utf8(argument)));
        }
        if (index + 1 == arguments.size() || arguments[index + 1].empty()) {
            throw UsageError(fmt::format("{} needs a value", io::utf8(argument)));
        }
        *target = arguments[++index];
    }
    if (!options.help && options.game_dir.empty()) {
        throw UsageError("--game-dir is required");
    }
    return options;
}

std::string sha256_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(fmt::format("can't read {}", utf8(path)));
    }

    BCRYPT_ALG_HANDLE algorithm_handle = nullptr;
    check_status(
        BCryptOpenAlgorithmProvider(&algorithm_handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
        "BCryptOpenAlgorithmProvider");
    const std::unique_ptr<void, AlgorithmCloser> algorithm(algorithm_handle);
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    check_status(BCryptCreateHash(algorithm_handle, &hash_handle, nullptr, 0, nullptr, 0, 0),
                 "BCryptCreateHash");
    const std::unique_ptr<void, HashDestroyer> hash(hash_handle);

    std::vector<char> buffer(size_t{4} << 20);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0) {
            check_status(BCryptHashData(hash_handle, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count), 0),
                         "BCryptHashData");
        }
    }
    if (file.bad()) {
        throw std::runtime_error(fmt::format("can't read {}", utf8(path)));
    }

    std::array<UCHAR, 32> digest{};
    check_status(BCryptFinishHash(hash_handle, digest.data(), static_cast<ULONG>(digest.size()), 0),
                 "BCryptFinishHash");
    std::string text;
    text.reserve(digest.size() * 2);
    for (const UCHAR byte : digest) {
        text += fmt::format("{:02x}", byte);
    }
    return text;
}

Identified identify_build(const RunOptions& options, std::span<const KnownBuild> builds) {
    fs::path exe = options.exe;
    if (exe.empty()) {
        for (const KnownBuild& build : builds) {
            fs::path candidate = options.game_dir / io::widen(build.exe_name, CP_UTF8);
            if (fs::is_regular_file(candidate)) {
                exe = std::move(candidate);
                break;
            }
        }
        if (exe.empty()) {
            std::string names;
            for (const KnownBuild& build : builds) {
                names += fmt::format("{}{} (build {})", names.empty() ? "" : ", ", build.exe_name,
                                     build.id);
            }
            throw std::runtime_error(fmt::format(
                "{} holds no known build's executable; expected one of: {}. --exe names another",
                utf8(options.game_dir), names));
        }
    } else if (exe.is_relative()) {
        exe = options.game_dir / exe;
    }
    exe = fs::absolute(exe).lexically_normal();
    if (!fs::is_regular_file(exe)) {
        throw std::runtime_error(fmt::format("{} is not a file", utf8(exe)));
    }

    const auto build = std::ranges::find_if(
        builds, [&](const KnownBuild& known) { return same_file_name(exe, known.exe_name); });
    if (build == builds.end()) {
        LOG_WARN(runner, "{} is no known build's executable", utf8(exe));
        return {.exe = exe, .build_id = {}};
    }
    const std::string hash = sha256_file(exe);
    if (build->exe_sha256.empty()) {
        LOG_WARN(runner,
                 "build {} has no executable SHA-256 in known_builds.toml, so {} runs unverified "
                 "(SHA-256 {})",
                 build->id, utf8(exe), hash);
    } else if (!same_ascii(hash, build->exe_sha256)) {
        throw std::runtime_error(
            fmt::format("{} has SHA-256 {}, which is not build {}", utf8(exe), hash, build->id));
    } else {
        LOG_INFO(runner, "{} is build {}", utf8(exe), build->id);
    }
    return {.exe = exe, .build_id = std::string(build->id)};
}

namespace {

// The stack reserve in g6recomp.exe's own header.
uint64_t host_stack_reserve() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfStackReserve;
}

} // namespace

std::array<uint32_t, 4> host_cpuid(uint32_t leaf, uint32_t subleaf) {
    std::array<int, 4> registers{};
    __cpuidex(registers.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
    std::array<uint32_t, 4> result{};
    std::ranges::transform(registers, result.begin(),
                           [](int value) { return static_cast<uint32_t>(value); });
    return result;
}

std::vector<std::string_view> missing_cpu_features(CpuidFunction cpuid, uint64_t xcr0) {
    const uint32_t max_basic = cpuid(0, 0)[0];
    const uint32_t max_extended = cpuid(0x80000000, 0)[0];
    const auto read = [&](uint32_t leaf, uint32_t subleaf) -> std::array<uint32_t, 4> {
        const bool present = leaf >= 0x80000000 ? leaf <= max_extended : leaf <= max_basic;
        return present ? cpuid(leaf, subleaf) : std::array<uint32_t, 4>{};
    };
    std::vector<std::string_view> missing;
    for (const RequiredFeature& feature : kRequiredFeatures) {
        if (((read(feature.leaf, feature.subleaf)[feature.reg] >> feature.bit) & 1) == 0) {
            missing.emplace_back(feature.name);
        }
    }
    const bool saved =
        ((read(1, 0)[kEcx] >> kOsxsaveBit) & 1) != 0 && (xcr0 & kXcr0SseAvx) == kXcr0SseAvx;
    if (!saved) {
        missing.push_back("AVX state saved by Windows");
    }
    return missing;
}

std::vector<std::string_view> missing_host_cpu_features() {
    const bool osxsave = ((host_cpuid(1, 0)[kEcx] >> kOsxsaveBit) & 1) != 0;
    return missing_cpu_features(&host_cpuid, osxsave ? read_xcr0() : 0);
}

[[noreturn]] void run(const RunOptions& requested) {
    // First, before code built for AVX2 (the loader's) runs.
    if (const auto missing = missing_host_cpu_features(); !missing.empty()) {
        throw std::runtime_error(fmt::format(
            "this CPU lacks {}, which the game uses without checking; it needs an AVX2 CPU "
            "(Intel Haswell, AMD Zen, or newer)",
            fmt::join(missing, ", ")));
    }
    loader::install_crash_report();
    if (requested.skip_int3) {
        // Last in line, so the loader's own handlers see every exception first.
        AddVectoredExceptionHandler(0, &skip_guest_int3);
        LOG_WARN(runner, "--skip-int3: int3 in guest code is stepped over, not reported");
    }

    RunOptions options = requested;
    options.game_dir = fs::absolute(options.game_dir).lexically_normal();
    if (!fs::is_directory(options.game_dir)) {
        throw std::runtime_error(fmt::format("{} is not a directory", utf8(options.game_dir)));
    }
    const Identified identified = identify_build(options, kKnownBuilds);

    fs::path data_dir = options.data_dir;
    if (data_dir.empty()) {
        if (identified.build_id.empty()) {
            throw UsageError(
                fmt::format("{} is no known build's executable, so it needs --data-dir",
                            utf8(identified.exe.filename())));
        }
        data_dir = io::user_data_directory(io::widen(identified.build_id, CP_UTF8));
    }
    data_dir = fs::absolute(data_dir).lexically_normal();
    io::FileSystem& file_system = io::active_file_system();
    io::mount_standard_drives(file_system, options.game_dir, data_dir);
    gdk::set_game_save_directory(data_dir / L"saves");
    LOG_INFO(runner, "G: is {} (read-only); the data directory is {}", utf8(options.game_dir),
             utf8(data_dir));

    // The guest sees its modules at their console paths, as its program name among them, so it
    // can open its own files by them. The executable has one only inside the install.
    const std::optional<std::wstring> program =
        file_system.console_path_of(identified.exe.native());
    if (!program || !program->starts_with(L"G:")) {
        throw std::runtime_error(
            fmt::format("{} isn't in the install, {}: the guest sees only the install's files",
                        utf8(identified.exe), utf8(options.game_dir)));
    }
    imports::set_guest_command_line(*program,
                                    imports::join_command_line(*program, options.arguments));
    // The guest's exit ends the host process with its code (the default end).
    imports::ucrtbase::start_guest_process();

    loader::LoaderOptions loader_options;
    loader_options.search_paths.push_back(options.game_dir);
    loader_options.imports = imports::default_game_policy();
    // The entry point's thread gets the reserve g6recomp.exe is linked with, as the threads the
    // guest starts with no stack size do (app/CMakeLists.txt says why).
    loader_options.min_entry_stack_reserve = host_stack_reserve();
    loader_options.guest_path = [](const fs::path& file) {
        return io::active_file_system().console_path_of(file.native()).value_or(file.native());
    };
    // Never destroyed: the process ends while the guest's modules are still mapped, as Windows
    // ends a process without unmapping its images, and other guest threads may still run.
    auto* loader = new loader::Loader(std::move(loader_options));
    const loader::Module executable = loader->load(identified.exe);

    LOG_INFO(runner, "running {} as {}", utf8(identified.exe), io::utf8(*program));
    const uint32_t code = loader->run_entry_point(executable);
    // Windows ends the process when its main thread returns from the entry point.
    LOG_INFO(runner, "the guest's entry point returned {}", code);
    ExitProcess(code);
}

} // namespace g6::app
