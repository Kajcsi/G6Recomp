#include "g6/app/runner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
// After windows.h.
#include <bcrypt.h>

#include <fmt/format.h>

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
    "usage: g6recomp --game-dir <dir> [--exe <file>] [--data-dir <dir>] [-- <arguments>]\n"
    "\n"
    "  --game-dir <dir>   the install, mounted read-only at G:\n"
    "  --exe <file>       the executable to run: a path, or a file name in the install\n"
    "                     (default: the main executable of the known build there)\n"
    "  --data-dir <dir>   the build's data directory; T: is its temp folder\n"
    "                     (default: %LOCALAPPDATA%\\G6Recomp\\<build ID>)\n"
    "  --                 the rest is the guest's command line, after its program name\n"
    "  --help             this text\n";

std::string utf8(const fs::path& path) {
    return io::utf8(path.native());
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

[[noreturn]] void run(const RunOptions& requested) {
    loader::install_crash_report();

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
    io::mount_standard_drives(io::active_file_system(), options.game_dir, data_dir);
    LOG_INFO(runner, "G: is {} (read-only); the data directory is {}", utf8(options.game_dir),
             utf8(data_dir));

    // The guest's program name is the executable's path as its GetModuleFileName reports it.
    const std::wstring& program = identified.exe.native();
    imports::set_guest_command_line(program,
                                    imports::join_command_line(program, options.arguments));
    // The guest's exit ends the host process with its code (the default end).
    imports::ucrtbase::start_guest_process();

    loader::LoaderOptions loader_options;
    loader_options.search_paths.push_back(options.game_dir);
    loader_options.imports = imports::default_game_policy();
    // Never destroyed: the process ends while the guest's modules are still mapped, as Windows
    // ends a process without unmapping its images, and other guest threads may still run.
    auto* loader = new loader::Loader(std::move(loader_options));
    const loader::Module executable = loader->load(identified.exe);

    LOG_INFO(runner, "running {}", utf8(program));
    const uint32_t code = loader->run_entry_point(executable);
    // Windows ends the process when its main thread returns from the entry point.
    LOG_INFO(runner, "the guest's entry point returned {}", code);
    ExitProcess(code);
}

} // namespace g6::app
