// The runner (g6/app/runner.h, docs/execution-model.md, phase 5, "The runner"). Its argument
// parsing, hashing, and build identification run in this process; the runner itself runs as
// g6recomp.exe in a child process, since it ends the process with the guest's exit code. The
// guest is an original executable (tests/fixtures/runner_guest) whose universal CRT is DLL
// imports, in a folder that stands in for an install.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>

#include "g6/app/runner.h"
#include "g6/imports/command_line.h"
#include "g6test.h"
#include "runner_guest_api.h"

namespace fs = std::filesystem;
using g6::app::Identified;
using g6::app::identify_build;
using g6::app::KnownBuild;
using g6::app::parse_arguments;
using g6::app::RunOptions;
using g6::app::sha256_file;
using g6::app::UsageError;

namespace {

constexpr const wchar_t* kArgument1 = L"" G6_RUNNER_ARGUMENT_1;
constexpr const wchar_t* kArgument2 = L"" G6_RUNNER_ARGUMENT_2;

constexpr const char* kAbcSha256 =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// A fresh folder under the host's temp directory, removed again at the end.
class TempFolder {
public:
    TempFolder() {
        static std::atomic<int> counter{0};
        root_ =
            fs::temp_directory_path() / ("g6runner-test-" + std::to_string(GetCurrentProcessId()) +
                                         "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(root_);
    }
    // remove_all with an error code throws only std::bad_alloc, which ends the test run anyway.
    ~TempFolder() { // NOLINT(bugprone-exception-escape)
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const fs::path& path() const { return root_; }

private:
    fs::path root_;
};

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// An install for runner_guest.exe: the executable and its data file.
void make_install(const fs::path& install) {
    fs::create_directories(install);
    fs::copy_file(fs::path(G6_RUNNER_GUEST_DIR) / "runner_guest.exe", install / "runner_guest.exe");
    write_file(install / G6_RUNNER_DATA_FILE, G6_RUNNER_DATA);
}

// Runs g6recomp.exe with `arguments` and returns its exit code.
uint32_t run_g6recomp(const std::vector<std::wstring>& arguments) {
    const std::wstring program = fs::path(G6_RUNNER_EXE).make_preferred().native();
    std::wstring line = g6::imports::join_command_line(program, arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process) == 0) {
        g6::test::report_failure(__FILE__, __LINE__, "CreateProcessW failed for g6recomp.exe");
        throw g6::test::RequireFailed{};
    }
    CloseHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, 60'000);
    DWORD code = 0;
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        g6::test::report_failure(__FILE__, __LINE__, "g6recomp.exe didn't end within a minute");
        throw g6::test::RequireFailed{};
    }
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code;
}

} // namespace

TEST_CASE("The runner's options and the guest's arguments are read from its command line") {
    const std::vector<std::wstring> arguments = {L"--game-dir", L"C:\\install", L"--exe",
                                                 L"main.exe",   L"--data-dir",  L"D:\\data",
                                                 L"--",         L"--exe",       L"two words"};
    const RunOptions options = parse_arguments(arguments);
    CHECK(options.game_dir == fs::path(L"C:\\install"));
    CHECK(options.exe == fs::path(L"main.exe"));
    CHECK(options.data_dir == fs::path(L"D:\\data"));
    REQUIRE(options.arguments.size() == 2);
    CHECK(options.arguments[0] == L"--exe");
    CHECK(options.arguments[1] == L"two words");
    CHECK(!options.help);

    const RunOptions minimal = parse_arguments(std::vector<std::wstring>{L"--game-dir", L"x"});
    CHECK(minimal.exe.empty());
    CHECK(minimal.data_dir.empty());
    CHECK(minimal.arguments.empty());

    CHECK(parse_arguments(std::vector<std::wstring>{L"--help"}).help);
}

TEST_CASE("A command line the runner can't use is a usage error") {
    using Arguments = std::vector<std::wstring>;
    CHECK_THROWS_AS(parse_arguments(Arguments{}), UsageError);
    CHECK_THROWS_AS(parse_arguments(Arguments{L"--exe", L"main.exe"}), UsageError);
    CHECK_THROWS_AS(parse_arguments(Arguments{L"--game-dir"}), UsageError);
    CHECK_THROWS_AS(parse_arguments(Arguments{L"--game-dir", L""}), UsageError);
    CHECK_THROWS_AS(parse_arguments(Arguments{L"--game-dir", L"x", L"--fast"}), UsageError);
    CHECK_THROWS_AS(parse_arguments(Arguments{L"--game-dir", L"x", L"extra"}), UsageError);
}

TEST_CASE("A file's SHA-256 is its contents' digest in lower-case hex") {
    const TempFolder folder;
    write_file(folder.path() / "abc.bin", "abc");
    write_file(folder.path() / "empty.bin", "");
    CHECK_EQ(sha256_file(folder.path() / "abc.bin"), std::string(kAbcSha256));
    CHECK_EQ(sha256_file(folder.path() / "empty.bin"),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_THROWS_AS(sha256_file(folder.path() / "none.bin"), std::runtime_error);
}

TEST_CASE("The runner finds a known build's executable and checks its hash") {
    const TempFolder folder;
    write_file(folder.path() / "known.exe", "abc");
    write_file(folder.path() / "unhashed.exe", "abc");
    write_file(folder.path() / "other.exe", "abc");
    const std::array<KnownBuild, 2> builds = {{
        {.id = "9.0.0.1", .exe_name = "known.exe", .exe_sha256 = kAbcSha256},
        {.id = "9.0.0.2", .exe_name = "unhashed.exe", .exe_sha256 = ""},
    }};

    RunOptions options;
    options.game_dir = folder.path();
    const Identified found = identify_build(options, builds);
    CHECK(found.exe == folder.path() / "known.exe");
    CHECK_EQ(found.build_id, std::string("9.0.0.1"));

    // File names match as the file system matches them, ignoring case.
    options.exe = L"KNOWN.EXE";
    CHECK_EQ(identify_build(options, builds).build_id, std::string("9.0.0.1"));

    // A build with no hash recorded runs unverified.
    options.exe = L"unhashed.exe";
    CHECK_EQ(identify_build(options, builds).build_id, std::string("9.0.0.2"));

    // An executable that is no known build's has no build.
    options.exe = folder.path() / "other.exe";
    const Identified other = identify_build(options, builds);
    CHECK(other.exe == folder.path() / "other.exe");
    CHECK(other.build_id.empty());

    // A known build's name with other contents is refused.
    write_file(folder.path() / "known.exe", "abd");
    options.exe.clear();
    CHECK_THROWS_AS(identify_build(options, builds), std::runtime_error);

    options.exe = L"none.exe";
    CHECK_THROWS_AS(identify_build(options, builds), std::runtime_error);

    // With no executable named, the install must hold a known build's.
    fs::remove(folder.path() / "known.exe");
    fs::remove(folder.path() / "unhashed.exe");
    options.exe.clear();
    CHECK_THROWS_AS(identify_build(options, builds), std::runtime_error);
}

TEST_CASE("g6recomp runs a guest with the install at G: and the data directory's temp at T:") {
    const TempFolder folder;
    const fs::path install = folder.path() / "install";
    const fs::path data = folder.path() / "data";
    make_install(install);

    const uint32_t code =
        run_g6recomp({L"--game-dir", install.native(), L"--exe", L"runner_guest.exe", L"--data-dir",
                      data.native(), L"--", kArgument1, kArgument2});
    CHECK_EQ(code, uint32_t{G6_RUNNER_OK});
    CHECK_EQ(read_file(data / "temp" / G6_RUNNER_COPY_FILE), std::string(G6_RUNNER_DATA));
    CHECK(!fs::exists(install / G6_RUNNER_COPY_FILE));
}

TEST_CASE("g6recomp refuses what it can't run, without starting the guest") {
    const TempFolder folder;
    const fs::path install = folder.path() / "install";
    make_install(install);
    write_file(install / "not_a_pe.exe", "abc");

    // No usable command line, and no data directory for an executable of no known build.
    CHECK_EQ(run_g6recomp({}), uint32_t{2});
    CHECK_EQ(run_g6recomp({L"--game-dir", install.native(), L"--exe", L"runner_guest.exe"}),
             uint32_t{2});
    // An executable outside the install, where the guest couldn't see it.
    fs::create_directories(folder.path() / "elsewhere");
    fs::copy_file(install / "runner_guest.exe", folder.path() / "elsewhere" / "runner_guest.exe");
    CHECK_EQ(run_g6recomp({L"--game-dir", install.native(), L"--exe",
                           (folder.path() / "elsewhere" / "runner_guest.exe").native(),
                           L"--data-dir", (folder.path() / "data").native()}),
             uint32_t{1});
    // An install with no known build's executable, and a file the loader can't map.
    CHECK_EQ(run_g6recomp({L"--game-dir", install.native()}), uint32_t{1});
    CHECK_EQ(run_g6recomp({L"--game-dir", install.native(), L"--exe", L"not_a_pe.exe",
                           L"--data-dir", (folder.path() / "data").native()}),
             uint32_t{1});
    CHECK_EQ(run_g6recomp({L"--help"}), uint32_t{0});
}
