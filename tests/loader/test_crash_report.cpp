// Crash reports (g6/loader/crash_report.h) on an original guest DLL (tests/fixtures/stack_guest):
// the stack walk gets through guest frames, those with unwind data and a leaf without, the report
// names the guest module and the address at its preferred base, the installed filter logs the
// report and hands the exception on, and a filter the guest installs is kept and runs after the
// report. Plan: docs/execution-model.md, "Crash reports".

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "g6/loader/crash_report.h"
#include "g6/loader/loader.h"
#include "g6/log.h"
#include "g6test.h"
#include "stack_guest_api.h"

using g6::loader::CodeAddress;
using g6::loader::crash_report;
using g6::loader::describe_code;
using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::locate_code;
using g6::loader::Module;
using g6::loader::walk_stack;

namespace {

constexpr std::string_view kGuest = "stack_guest.dll";

using StoreFn = int (*)(volatile int*);
using CallFn = int (*)(G6StackCallback, void*);
using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER (*)(LPTOP_LEVEL_EXCEPTION_FILTER);

LoaderOptions make_options() {
    LoaderOptions options;
    options.search_paths.emplace_back(G6_STACK_GUEST_DIR);
    g6::loader::register_crash_report(options.imports);
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

uint64_t rva_of(const Module& module, std::string_view name) {
    return reinterpret_cast<uint64_t>(guest_export<void*>(module, name)) - module.base();
}

// The file name of this test executable, as the walk names its frames.
std::string test_executable() {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), length)).filename().string();
}

bool in_guest(const CodeAddress& frame, uint64_t rva, uint64_t size) {
    return frame.guest && frame.module == kGuest && frame.offset >= rva &&
           frame.offset < rva + size;
}

std::string frames_text(const std::vector<CodeAddress>& frames) {
    std::string text;
    for (const CodeAddress& frame : frames) {
        text += fmt::format("\n  {}", describe_code(frame));
    }
    return text;
}

// ---- Faults captured while their frames are still on the stack ----

// What a filter does with the exception, called from the __except filter expression, before
// anything unwinds.
using Inspect = LONG (*)(EXCEPTION_POINTERS* exception, void* context);

struct Captured {
    std::vector<CodeAddress> frames;
    std::string report;
};

LONG capture(EXCEPTION_POINTERS* exception, void* context) {
    auto* captured = static_cast<Captured*>(context);
    try {
        captured->frames = walk_stack(*exception->ContextRecord);
        captured->report = crash_report(*exception);
    } catch (...) { // NOLINT(bugprone-empty-catch): the checks see empty results
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Runs the guest's store and returns whether it faulted.
bool store_in_guest(StoreFn store, volatile int* address, Inspect inspect, void* context) {
    __try {
        store(address);
        return false;
    } __except (inspect(GetExceptionInformation(), context)) {
        return true;
    }
}

[[noreturn]] __declspec(noinline) void throw_runtime_error() {
    throw std::runtime_error("thrown for test_crash_report");
}

bool throw_in_host(Inspect inspect, void* context) {
    __try {
        throw_runtime_error();
    } __except (inspect(GetExceptionInformation(), context)) {
        return true;
    }
}

// ---- Log capture ----

class LogCapture {
public:
    LogCapture() { g6::log::set_sink(&LogCapture::sink, this); }
    ~LogCapture() { g6::log::set_sink(nullptr, nullptr); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    const std::vector<std::string>& lines() const { return lines_; }

private:
    static void sink(g6::log::Level level, std::string_view category, std::string_view message,
                     void* context) {
        if (level == g6::log::Level::Error && category == "crash") {
            static_cast<LogCapture*>(context)->lines_.emplace_back(message);
        }
    }

    std::vector<std::string> lines_;
};

// ---- The installed filter ----

int g_previous_calls = 0;

LONG WINAPI previous_filter(EXCEPTION_POINTERS* /*exception*/) {
    ++g_previous_calls;
    return EXCEPTION_EXECUTE_HANDLER;
}

struct Installed {
    LPTOP_LEVEL_EXCEPTION_FILTER filter = nullptr;
    LONG result = EXCEPTION_CONTINUE_SEARCH;
};

LONG call_installed(EXCEPTION_POINTERS* exception, void* context) {
    auto* installed = static_cast<Installed*>(context);
    installed->result = installed->filter(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

// What the guest installs as its filter; it counts its calls and returns `g_guest_result`.
int g_guest_filter_calls = 0;
LONG g_guest_result = EXCEPTION_CONTINUE_SEARCH;

LONG WINAPI guest_filter(EXCEPTION_POINTERS* /*exception*/) {
    ++g_guest_filter_calls;
    return g_guest_result;
}

// ---- A walk from inside a host callback that guest code called ----

struct CallbackSeen {
    std::vector<CodeAddress> frames;
    USHORT backtrace = 0;
    std::array<void*, 32> backtrace_frames{};
};

int on_guest_call(void* context) {
    auto* seen = static_cast<CallbackSeen*>(context);
    CONTEXT here{};
    RtlCaptureContext(&here);
    seen->frames = walk_stack(here);
    seen->backtrace = RtlCaptureStackBackTrace(0, static_cast<DWORD>(seen->backtrace_frames.size()),
                                               seen->backtrace_frames.data(), nullptr);
    return 40;
}

} // namespace

TEST_CASE("the walk gets through guest frames to the host code that called them") {
    Loader loader(make_options());
    const Module guest = loader.load(std::string(kGuest));
    CallbackSeen seen;
    CHECK_EQ(guest_export<CallFn>(guest, "g6_stack_call")(&on_guest_call, &seen), 42);
    INFO(fmt::format("walk:{}", frames_text(seen.frames)));

    // The callback, the guest's two frames, then this test.
    REQUIRE(seen.frames.size() >= 4);
    CHECK(!seen.frames[0].guest);
    CHECK_EQ(seen.frames[0].module, test_executable());
    CHECK(seen.frames[1].guest);
    CHECK_EQ(seen.frames[1].module, std::string(kGuest));
    CHECK(in_guest(seen.frames[2], rva_of(guest, "g6_stack_call"), 0x40));
    CHECK(!seen.frames[3].guest);
    CHECK_EQ(seen.frames[3].module, test_executable());

    // RtlCaptureStackBackTrace gives the first guest frame, then stops: it can't unwind it.
    INFO(fmt::format("RtlCaptureStackBackTrace gave {} frames", seen.backtrace));
    REQUIRE(seen.backtrace > 0);
    USHORT first_guest = 0;
    while (first_guest < seen.backtrace &&
           !locate_code(reinterpret_cast<uint64_t>(seen.backtrace_frames[first_guest])).guest) {
        ++first_guest;
    }
    CHECK_EQ(first_guest, static_cast<USHORT>(seen.backtrace - 1));
}

TEST_CASE("a fault in a guest leaf is reported with its guest frames and preferred addresses") {
    Loader loader(make_options());
    const Module guest = loader.load(std::string(kGuest));
    const uint64_t leaf = rva_of(guest, "g6_stack_leaf_store");
    DWORD64 image_base = 0;
    REQUIRE(RtlLookupFunctionEntry(guest.base() + leaf, &image_base, nullptr) == nullptr);

    Captured captured;
    CHECK(store_in_guest(guest_export<StoreFn>(guest, "g6_stack_store"), nullptr, &capture,
                         &captured));
    INFO(captured.report);

    // The leaf, the frame below it, the export, then this test.
    REQUIRE(captured.frames.size() >= 4);
    CHECK(in_guest(captured.frames[0], leaf, 0x10));
    CHECK_EQ(captured.frames[0].preferred, guest.image().image_base() + captured.frames[0].offset);
    CHECK(captured.frames[1].guest);
    CHECK(in_guest(captured.frames[2], rva_of(guest, "g6_stack_store"), 0x40));
    CHECK(!captured.frames[3].guest);
    CHECK_EQ(captured.frames[3].module, test_executable());

    const std::string at = describe_code(captured.frames[0]);
    CHECK_EQ(at, fmt::format("{}+{:#x} [guest {:#x}]", kGuest, captured.frames[0].offset,
                             captured.frames[0].preferred));
    CHECK(captured.report.starts_with("unhandled access violation 0xc0000005 on thread "));
    CHECK(captured.report.find("\n  at " + at + "\n") != std::string::npos);
    CHECK(captured.report.find("\n  writing 0x0000000000000000\n") != std::string::npos);
    CHECK(captured.report.find("\n    #0  " + at + "\n") != std::string::npos);
    CHECK(captured.report.find(fmt::format("rcx {:016x}", 0)) != std::string::npos);
}

TEST_CASE("an address no module holds is shown bare") {
    const CodeAddress nowhere = locate_code(0x10);
    CHECK(nowhere.module.empty());
    CHECK(!nowhere.guest);
    CHECK_EQ(describe_code(nowhere), std::string("0x0000000000000010"));
}

TEST_CASE("a C++ exception's report names its type") {
    Captured captured;
    CHECK(throw_in_host(&capture, &captured));
    INFO(captured.report);
    CHECK(captured.report.starts_with("unhandled C++ exception 0xe06d7363 on thread "));
    CHECK(captured.report.find("\n  type .?AVruntime_error@std@@\n") != std::string::npos);
}

TEST_CASE("the installed filter logs the report, then hands the exception on") {
    Loader loader(make_options());
    const Module guest = loader.load(std::string(kGuest));

    SetUnhandledExceptionFilter(&previous_filter);
    g6::loader::install_crash_report();
    g6::loader::install_crash_report();
    // Reads the process's filter and leaves it installed.
    Installed installed;
    installed.filter = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(installed.filter);
    REQUIRE(installed.filter != nullptr);
    REQUIRE(installed.filter != &previous_filter);

    std::vector<std::string> lines;
    {
        const LogCapture log;
        CHECK(store_in_guest(guest_export<StoreFn>(guest, "g6_stack_store"), nullptr,
                             &call_installed, &installed));
        lines = log.lines();
    }
    CHECK_EQ(g_previous_calls, 1);
    CHECK_EQ(installed.result, LONG{EXCEPTION_EXECUTE_HANDLER});
    REQUIRE(lines.size() == 1);
    INFO(lines[0]);
    CHECK(lines[0].starts_with("unhandled access violation"));
    CHECK(lines[0].find(fmt::format("\n    #0  {}+", kGuest)) != std::string::npos);
}

// After the test above, which installed the report over previous_filter.
TEST_CASE("a guest's filter is kept, and runs after the report") {
    Loader loader(make_options());
    const Module guest = loader.load(std::string(kGuest));
    const auto set_filter = guest_export<SetFilterFn>(guest, "g6_stack_set_filter");
    const auto store = guest_export<StoreFn>(guest, "g6_stack_store");

    CHECK(set_filter(&guest_filter) == nullptr);
    // The process's filter is still the report's.
    Installed installed;
    installed.filter = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(installed.filter);
    REQUIRE(installed.filter != nullptr);
    REQUIRE(installed.filter != &guest_filter);
    REQUIRE(installed.filter != &previous_filter);
    const int previous_calls = g_previous_calls;

    // The guest's filter passes: the one from before the report gets the exception.
    g_guest_result = EXCEPTION_CONTINUE_SEARCH;
    {
        const LogCapture log;
        CHECK(store_in_guest(store, nullptr, &call_installed, &installed));
        CHECK_EQ(log.lines().size(), size_t{1});
    }
    CHECK_EQ(g_guest_filter_calls, 1);
    CHECK_EQ(g_previous_calls, previous_calls + 1);
    CHECK_EQ(installed.result, LONG{EXCEPTION_EXECUTE_HANDLER});

    // The guest's filter decides: nothing after it runs.
    g_guest_result = EXCEPTION_CONTINUE_EXECUTION;
    {
        const LogCapture log;
        CHECK(store_in_guest(store, nullptr, &call_installed, &installed));
        CHECK_EQ(log.lines().size(), size_t{1});
    }
    CHECK_EQ(g_guest_filter_calls, 2);
    CHECK_EQ(g_previous_calls, previous_calls + 1);
    CHECK_EQ(installed.result, LONG{EXCEPTION_CONTINUE_EXECUTION});

    CHECK(set_filter(nullptr) == &guest_filter);
}
