// The native loader started after host DLLs with static TLS (docs/execution-model.md, "TLS and
// the TEB"). Host DLLs such as shell32, combase, and gdi32full take TLS indices before the
// loader's donors load, so the donors get higher ones, and loading a donor can make Windows give
// every thread a longer TLS array. The donors load once per process, so the test starts itself
// again as a child process for each number of copies of tls_order.dll to load first. Each child
// loads them, starts the loader, and checks a guest module's static TLS on two threads.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <intrin.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "g6/loader/loader.h"
#include "g6test.h"
#include "tls_order_api.h"

namespace {

// Set in a child's environment to the number of copies it loads first.
constexpr wchar_t kFillersVariable[] = L"G6_TLS_ORDER_FILLERS";
// The most copies a child loads first. Windows 11 grows the array 8 entries at a time (measured),
// so stepping the donors' indices up one at a time over this range moves donors past the end of
// an array three times.
constexpr int kMaxFillers = 24;
// A child's exit code when it passed and Windows replaced its TLS array while the loader started.
constexpr DWORD kExitArrayMoved = 3;

using ValueFn = ULONG (*)();
using BlockFn = void* (*)();

std::filesystem::path guest_path() {
    return G6_TLS_ORDER_DLL;
}

std::filesystem::path filler_path(int number) {
    return guest_path().parent_path() / "fillers" / fmt::format("tls_filler{}.dll", number);
}

void** tls_array() {
    return reinterpret_cast<void**>(__readgsqword(0x58));
}

// The number of copies to load first in a child process, or -1 when this isn't one.
int fillers_to_load() {
    std::wstring text(16, L'\0');
    const DWORD length =
        GetEnvironmentVariableW(kFillersVariable, text.data(), static_cast<DWORD>(text.size()));
    if (length == 0 || length >= text.size()) {
        return -1;
    }
    text.resize(length);
    return std::stoi(text);
}

// Runs this test executable as a child that loads `fillers` copies first, and returns its exit
// code. The child writes to this process's standard handles.
DWORD run_child(int fillers) {
    std::wstring self(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    REQUIRE(length != 0);
    REQUIRE(length < self.size());
    self.resize(length);
    std::wstring command_line = L"\"" + self + L"\" [child]";

    REQUIRE(SetEnvironmentVariableW(kFillersVariable, std::to_wstring(fillers).c_str()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::fflush(stdout);
    const BOOL started = CreateProcessW(self.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                        0, nullptr, nullptr, &startup, &process);
    SetEnvironmentVariableW(kFillersVariable, nullptr);
    REQUIRE(started);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

template <typename T>
T export_of(HMODULE module, const char* name) {
    auto* const function = reinterpret_cast<T>(GetProcAddress(module, name));
    REQUIRE(function != nullptr);
    return function;
}

} // namespace

TEST_CASE("the loader starts after host DLLs with static TLS took the low indices") {
    std::filesystem::create_directories(filler_path(0).parent_path());
    for (int number = 0; number < kMaxFillers; ++number) {
        std::filesystem::copy_file(guest_path(), filler_path(number),
                                   std::filesystem::copy_options::overwrite_existing);
    }
    int moved = 0;
    for (int fillers = 0; fillers <= kMaxFillers; ++fillers) {
        const DWORD code = run_child(fillers);
        INFO(fmt::format("the child that loads {} copies first exited with {:#x}", fillers, code));
        const bool passed = code == 0 || code == kExitArrayMoved;
        CHECK(passed);
        moved += code == kExitArrayMoved ? 1 : 0;
    }
    // At least one child must have had its array replaced while the loader started, or the test
    // missed the case it is for.
    CHECK(moved > 0);
}

// Every check here is a REQUIRE: a passing child ends the process itself to report whether its
// array moved, so a failure must end the test case at once.
TEST_CASE("[child] load copies of tls_order.dll, then start the loader") {
    const int fillers = fillers_to_load();
    if (fillers < 0) {
        SKIP("runs only in the child processes the test above starts");
    }

    // Windows gives each copy an index of its own and a block on this thread, filled from the
    // template.
    std::vector<HMODULE> loaded;
    for (int number = 0; number < fillers; ++number) {
        HMODULE module = LoadLibraryW(filler_path(number).c_str());
        REQUIRE(module != nullptr);
        REQUIRE(export_of<ValueFn>(module, "g6_tls_order_value")() == G6_TLS_ORDER_MAGIC);
        loaded.push_back(module);
    }
    const void* const first_block =
        loaded.empty() ? nullptr : export_of<BlockFn>(loaded.front(), "g6_tls_order_block")();

    bool moved = false;
    {
        g6::loader::LoaderOptions options;
        options.imports.forward_dll("kernel32.dll");
        void* const* const before = tls_array();
        g6::loader::Loader loader(std::move(options));
        moved = tls_array() != before;

        const g6::loader::Module guest = loader.load(guest_path());
        const auto value = guest.export_as<ValueFn>("g6_tls_order_value");
        REQUIRE(value != nullptr);
        REQUIRE(value() == G6_TLS_ORDER_MAGIC);
        ULONG on_new_thread = 0;
        std::thread([&] { on_new_thread = value(); }).join();
        REQUIRE(on_new_thread == G6_TLS_ORDER_MAGIC);
    }
    // A longer array holds the same blocks: Windows moves the pointers, not the blocks.
    if (first_block != nullptr) {
        REQUIRE(export_of<BlockFn>(loaded.front(), "g6_tls_order_block")() == first_block);
    }
    fmt::print(stdout, "    {} copies loaded first; TLS array {}\n", fillers,
               moved ? "replaced while the loader started" : "kept");
    if (moved) {
        std::fflush(stdout);
        std::exit(static_cast<int>(kExitArrayMoved));
    }
}
