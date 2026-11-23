#pragma once

// The runner (docs/execution-model.md, phase 5, "The runner"): what g6recomp.exe does with its
// command line. It finds the executable to run and the build it belongs to, mounts the console's
// drives on the install and the build's data directory (docs/io.md), gives the guest its command
// line, loads the executable under the default import policy, and runs its entry point.

#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "g6/app/known_builds.h"

namespace g6::app {

struct RunOptions {
    std::filesystem::path game_dir;
    // A path, or a file name in game_dir. Empty: the executable of the known build that game_dir
    // holds.
    std::filesystem::path exe;
    // Empty: the build's user data directory, %LOCALAPPDATA%\G6Recomp\<build ID>.
    std::filesystem::path data_dir;
    // The guest's arguments, after its program name.
    std::vector<std::wstring> arguments;
    bool help = false;
};

// A command line g6recomp can't use. main prints it with the usage text.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string_view usage();

// Reads g6recomp's arguments, without its own program name. Everything after `--` is the
// guest's. Throws UsageError.
RunOptions parse_arguments(std::span<const std::wstring> arguments);

// The SHA-256 of a file's contents, in lower-case hex. Throws std::runtime_error when the file
// can't be read.
std::string sha256_file(const std::filesystem::path& path);

struct Identified {
    std::filesystem::path exe; // absolute
    // The build whose executable this is, or empty when its file name is no known build's.
    std::string build_id;
};

// Finds the executable and its build. An executable named as a known build's must have that
// build's SHA-256; when the build has none recorded, it runs unverified, with a warning that
// gives its hash. Throws std::runtime_error when there is no executable or its hash differs.
Identified identify_build(const RunOptions& options, std::span<const KnownBuild> builds);

// Runs the guest process, and ends the host process with the guest's exit code: when the guest
// calls exit, as the C runtime ends its own process (g6/imports/crt_exit.h), or when its entry
// point returns, as Windows ends a process whose main thread returns from it. Returns only by
// throwing: UsageError when an executable that is no known build's has no data directory given,
// and std::runtime_error (kernel::LoadError among them) when the guest can't be started.
[[noreturn]] void run(const RunOptions& options);

} // namespace g6::app
