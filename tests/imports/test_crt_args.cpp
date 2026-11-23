// The guest's command line (g6/imports/command_line.h) under the native loader, on an original
// /MD executable and a DLL it imports (tests/fixtures/args_guest). The executable's startup code
// gets argc and argv from the CRT's argument functions, so main sees the guest's command line,
// not the test's; the DLL sees it already while the loader attaches it. As in test_crt_exit, the
// end function ends the guest's main thread, so run_entry_point returns the exit code.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "args_guest_api.h"
#include "g6/imports/command_line.h"
#include "g6/imports/crt_exit.h"
#include "g6/io/text.h"
#include "g6/loader/loader.h"
#include "g6test.h"

using g6::imports::join_command_line;
using g6::imports::set_guest_command_line;
using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::Module;
namespace ucrtbase = g6::imports::ucrtbase;

namespace {

[[noreturn]] void end_thread(int code) {
    ExitThread(static_cast<DWORD>(code));
}

std::filesystem::path system_directory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    return {std::wstring(buffer.data(), length)};
}

// The guest's C++ runtime is the host's vcruntime140.dll, mapped as a guest module from a copy
// in a folder of its own, as in test_crt_exit.
const std::filesystem::path& guest_runtime_directory() {
    static const std::filesystem::path folder = [] {
        std::filesystem::path path = std::filesystem::path(G6_ARGS_GUEST_DIR) / "host_vcruntime";
        std::filesystem::create_directories(path);
        std::filesystem::copy_file(system_directory() / "vcruntime140.dll",
                                   path / "vcruntime140.dll",
                                   std::filesystem::copy_options::update_existing);
        return path;
    }();
    return folder;
}

// The system and the rest of the universal CRT are forwarded.
LoaderOptions make_options() {
    LoaderOptions options;
    options.search_paths.emplace_back(G6_ARGS_GUEST_DIR);
    options.search_paths.push_back(guest_runtime_directory());
    options.imports.forward_dll("kernel32.dll");
    options.imports.forward_dll("api-ms-win-core-*");
    options.imports.forward_dll("api-ms-win-crt-*");
    ucrtbase::register_exit(options.imports);
    g6::imports::register_command_line(options.imports);
    return options;
}

template <typename T>
T guest_export(const Module& module, std::string_view name) {
    const T address = module.export_as<T>(name);
    if (address == nullptr) {
        g6::test::report_failure(__FILE__, __LINE__,
                                 fmt::format("{} doesn't export {}", module.name(), name));
        throw g6::test::RequireFailed{};
    }
    return address;
}

std::wstring program_path() {
    return (std::filesystem::path(G6_ARGS_GUEST_DIR) / "args_guest.exe").make_preferred().wstring();
}

struct Seen {
    uint32_t code = 0;
    int argc = 0;
    std::string argv;
    std::wstring command_line;
    std::string command_line_a;
    std::string winmain;
    std::string program;
    std::wstring at_attach;
};

// Starts a guest process with `line` as its command line, runs it, and returns what it saw.
Seen run_guest(std::wstring_view line) {
    set_guest_command_line(program_path(), line);
    Loader loader(make_options());
    const Module exe = loader.load("args_guest.exe");
    ucrtbase::start_guest_process(&end_thread);

    Seen seen;
    seen.code = loader.run_entry_point(exe);
    seen.argc = *guest_export<const int*>(exe, "g6_args_argc");
    seen.argv = guest_export<const char*>(exe, "g6_args_argv");
    seen.command_line = guest_export<const wchar_t*>(exe, "g6_args_command_line");
    seen.command_line_a = guest_export<const char*>(exe, "g6_args_command_line_a");
    seen.winmain = guest_export<const char*>(exe, "g6_args_winmain");
    seen.program = guest_export<const char*>(exe, "g6_args_program");
    seen.at_attach = guest_export<const wchar_t*>(exe, "g6_args_at_attach_copy");

    ucrtbase::start_guest_process();
    return seen;
}

std::string joined(const std::vector<std::string>& arguments) {
    std::string text;
    for (const std::string& argument : arguments) {
        text += argument;
        text += static_cast<char>(G6_ARGS_SEPARATOR);
    }
    return text;
}

} // namespace

TEST_CASE("main gets the guest's arguments, and the command-line functions its line") {
    const std::wstring program = program_path();
    const std::vector<std::wstring> arguments = {L"-x", L"two words", LR"(quote"d)", LR"(dir\)"};
    const std::wstring line = join_command_line(program, arguments);
    INFO(fmt::format("line {}", g6::io::utf8(line)));

    const Seen seen = run_guest(line);
    CHECK_EQ(seen.code, uint32_t{0});
    CHECK_EQ(seen.argc, 5);
    CHECK_EQ(seen.argv,
             joined({g6::io::narrow(program), "-x", "two words", R"(quote"d)", R"(dir\)"}));
    CHECK(seen.command_line == line);
    CHECK_EQ(seen.command_line_a, g6::io::narrow(line));
    CHECK_EQ(seen.winmain, std::string(R"(-x "two words" "quote\"d" dir\)"));
    CHECK_EQ(seen.program, g6::io::narrow(program));
}

TEST_CASE("a DLL sees the guest's command line while the loader attaches it") {
    const std::wstring line = join_command_line(program_path(), std::vector<std::wstring>{L"-a"});
    const Seen seen = run_guest(line);
    CHECK(seen.at_attach == line);
}

TEST_CASE("each guest process sees the command line set for it") {
    const Seen first = run_guest(L"first.exe one");
    CHECK_EQ(first.argv, std::string("first.exe|one|"));
    const Seen second = run_guest(L"second.exe two three");
    CHECK_EQ(second.argc, 3);
    CHECK_EQ(second.argv, std::string("second.exe|two|three|"));
    CHECK_EQ(second.winmain, std::string("two three"));
    // The program name is the executable's path, whatever argv[0] says.
    CHECK_EQ(second.program, g6::io::narrow(program_path()));
}
