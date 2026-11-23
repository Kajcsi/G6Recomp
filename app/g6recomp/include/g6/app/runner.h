#pragma once

// The runner (docs/execution-model.md, phase 5, "The runner"): what g6recomp.exe does with its
// command line. It finds the executable to run and the build it belongs to, mounts the console's
// drives on the install and the build's data directory (docs/io.md), gives the guest its command
// line, loads the executable under the default import policy, and runs its entry point.

#include <array>
#include <cstdint>
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
    // Step over int3 in guest code instead of ending the process (a diagnostic; see usage()).
    bool skip_int3 = false;
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

// `cpuid` with eax = leaf and ecx = subleaf: eax, ebx, ecx, and edx.
using CpuidFunction = std::array<uint32_t, 4> (*)(uint32_t leaf, uint32_t subleaf);

std::array<uint32_t, 4> host_cpuid(uint32_t leaf, uint32_t subleaf);

// The features of the pre-flight scan's baseline (docs/scan.md) that `cpuid` doesn't report, by
// name, in a fixed order; empty when it reports all of them. The game uses these without asking,
// so a host that lacks one would stop at an illegal instruction somewhere in its code. AVX also
// needs Windows to save the AVX registers: the OSXSAVE bit, and bits 1 and 2 of `xcr0`.
std::vector<std::string_view> missing_cpu_features(CpuidFunction cpuid, uint64_t xcr0);

// missing_cpu_features for this host.
std::vector<std::string_view> missing_host_cpu_features();

// Runs the guest process, and ends the host process with the guest's exit code: when the guest
// calls exit, as the C runtime ends its own process (g6/imports/crt_exit.h), or when its entry
// point returns, as Windows ends a process whose main thread returns from it. Returns only by
// throwing: UsageError when an executable that is no known build's has no data directory given,
// and std::runtime_error (kernel::LoadError among them) when the guest can't be started, the
// host CPU among the reasons (missing_host_cpu_features).
[[noreturn]] void run(const RunOptions& options);

} // namespace g6::app
