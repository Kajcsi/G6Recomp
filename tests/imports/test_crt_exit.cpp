// The guest executable's exit tables (g6/imports/crt_exit.h) under the native loader, on an
// original /MD executable (tests/fixtures/exit_guest). Each way it can end runs its own handlers
// in the order the host UCRT runs them, and ends with its own code. The test's end function ends
// the guest's main thread instead of the process, so run_entry_point returns the code; a handler
// that had gone to the host CRT's table would never have run before that. Ending the thread
// destroys its thread_local object, as any thread end does, so a log shows that after the end
// when the exit left it alone.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <windows.h>

#include <fmt/format.h>

#include "exit_guest_api.h"
#include "g6/imports/crt_exit.h"
#include "g6/loader/loader.h"
#include "g6test.h"

using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::Module;
namespace ucrtbase = g6::imports::ucrtbase;

namespace {

using Note = void (*)(char);

std::atomic<Note> g_note{nullptr};

// Records the end in the guest's log and ends the guest's main thread with the code.
[[noreturn]] void end_thread(int code) {
    if (Note note = g_note.load()) {
        note(G6_EXIT_EVENT_END);
    }
    ExitThread(static_cast<DWORD>(code));
}

std::filesystem::path system_directory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    return {std::wstring(buffer.data(), length)};
}

// The guest's C++ runtime is the host's vcruntime140.dll, mapped as a guest module from a copy
// in a folder of its own, as in test_cxx_hooks.
const std::filesystem::path& guest_runtime_directory() {
    static const std::filesystem::path folder = [] {
        std::filesystem::path path = std::filesystem::path(G6_EXIT_GUEST_DIR) / "host_vcruntime";
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
    options.search_paths.emplace_back(G6_EXIT_GUEST_DIR);
    options.search_paths.push_back(guest_runtime_directory());
    options.imports.forward_dll("kernel32.dll");
    options.imports.forward_dll("api-ms-win-core-*");
    options.imports.forward_dll("api-ms-win-crt-*");
    ucrtbase::register_exit(options.imports);
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

struct Ending {
    uint32_t code = 0;
    std::string log;
};

// Loads a fresh copy of the guest, runs it in `mode` as a new guest process, and returns its
// exit code and log.
Ending run_guest(int mode) {
    Loader loader(make_options());
    const Module exe = loader.load("exit_guest.exe");
    *guest_export<int*>(exe, "g6_exit_mode") = mode;
    g_note = guest_export<Note>(exe, "g6_exit_note");
    ucrtbase::start_guest_process(&end_thread);

    Ending ending;
    ending.code = loader.run_entry_point(exe);
    ending.log = guest_export<const char*>(exe, "g6_exit_log");

    g_note = nullptr;
    ucrtbase::start_guest_process();
    return ending;
}

} // namespace

TEST_CASE("returning from main runs the thread_local destructor, then the handlers newest first") {
    // The CRT's startup code calls exit with main's result. A handler registered by a handler
    // runs next; the static object, registered before main, runs last.
    const Ending ending = run_guest(G6_EXIT_RETURN);
    CHECK_EQ(ending.code, uint32_t{3});
    CHECK_EQ(ending.log, std::string("MT3AL1D|"));
}

TEST_CASE("exit runs the same cleanup") {
    const Ending ending = run_guest(G6_EXIT_EXIT);
    CHECK_EQ(ending.code, uint32_t{4});
    CHECK_EQ(ending.log, std::string("MT3AL1D|"));
}

TEST_CASE("quick_exit runs only the at_quick_exit handlers") {
    const Ending ending = run_guest(G6_EXIT_QUICK);
    CHECK_EQ(ending.code, uint32_t{5});
    CHECK_EQ(ending.log, std::string("MQ|T"));
}

TEST_CASE("_exit runs no handler") {
    const Ending ending = run_guest(G6_EXIT_PLAIN);
    CHECK_EQ(ending.code, uint32_t{6});
    CHECK_EQ(ending.log, std::string("M|T"));
}

TEST_CASE("_cexit runs the cleanup and returns, and exit after it runs nothing twice") {
    const Ending ending = run_guest(G6_EXIT_CEXIT);
    CHECK_EQ(ending.code, uint32_t{7});
    CHECK_EQ(ending.log, std::string("MT3AL1DC|"));
}

TEST_CASE("each guest process starts with empty tables") {
    // Runs that register handlers and end without running them leave nothing for the next.
    for (int round = 0; round < 2; ++round) {
        INFO(fmt::format("round {}", round));
        const Ending plain = run_guest(G6_EXIT_PLAIN);
        CHECK_EQ(plain.log, std::string("M|T"));
        const Ending quick = run_guest(G6_EXIT_QUICK);
        CHECK_EQ(quick.log, std::string("MQ|T"));
    }
    const Ending ending = run_guest(G6_EXIT_EXIT);
    CHECK_EQ(ending.log, std::string("MT3AL1D|"));
}

TEST_CASE("a null handler is refused") {
    ucrtbase::start_guest_process(&end_thread);
    CHECK_EQ(ucrtbase::_crt_atexit(nullptr), -1);
    CHECK_EQ(ucrtbase::_crt_at_quick_exit(nullptr), -1);
    ucrtbase::start_guest_process();
}
