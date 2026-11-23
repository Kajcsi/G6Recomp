// The native loader on original guest modules (tests/fixtures/loader_guest): imports by policy,
// static TLS from several threads, SEH and C++ exceptions across guest and host frames, fibers,
// the HLE loader functions, relocations, the security cookie, and an executable's entry point.
// Plan: docs/execution-model.md, phase 1.

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "g6/kernel/image_loader.h"
#include "g6/loader/loader.h"
#include "g6/log.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6test.h"
#include "loader_guest_api.h"

using g6::kernel::LoadError;
using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::Module;
using g6::loader::StubResult;
using g6::loader::Symbol;

namespace {

constexpr uint64_t kDefaultSecurityCookie = 0x00002B992DDFA232;
constexpr int kDelayValue = 4321;

// ---- Host implementations the registry binds the made-up console imports to ----

int host_fake_add(int a, int b) {
    return a + b + 1000;
}

int host_fake_ordinal(int a) {
    return a * 3;
}

int host_delay_value() {
    return kDelayValue;
}

constexpr int kReplacementValue = 70;

// Our implementation of the replaced fixture DLL's one export.
int host_replacement_value() {
    return kReplacementValue;
}

std::filesystem::path guest_path(std::string_view name) {
    return std::filesystem::path(G6_GUEST_DIR) / name;
}

LoaderOptions make_options() {
    LoaderOptions options;
    options.search_paths.emplace_back(G6_GUEST_DIR);
    options.imports.forward_dll("kernel32.dll");
    options.imports.forward_dll("api-ms-win-core-*");
    options.imports.forward_dll("ntdll.dll");
    options.imports.hle("g6fakeos.dll", Symbol::by_name("g6fake_add"),
                        reinterpret_cast<const void*>(&host_fake_add));
    options.imports.hle("g6fakeos.dll", Symbol::by_ordinal(G6_FAKE_ORDINAL_BOUND),
                        reinterpret_cast<const void*>(&host_fake_ordinal));
    options.imports.stub("g6fakeos.dll", Symbol::by_name("g6fake_hresult"), StubResult::hresult());
    options.imports.hle("g6fakedelay.dll", Symbol::by_name("g6fakedelay_value"),
                        reinterpret_cast<const void*>(&host_delay_value));
    return options;
}

// An export of a guest module as a function pointer; fails the test case when it is missing.
template <typename T>
T guest_function(const Module& module, std::string_view name) {
    void* address = module.find_export(name);
    if (address == nullptr) {
        g6::test::report_failure(__FILE__, __LINE__,
                                 fmt::format("{} doesn't export {}", module.name(), name));
        throw g6::test::RequireFailed{};
    }
    return reinterpret_cast<T>(address);
}

G6GuestCounts counts_of(const Module& guest) {
    G6GuestCounts counts{};
    guest_function<void (*)(G6GuestCounts*)>(guest, "g6_guest_counts")(&counts);
    return counts;
}

// ---- Host frames for the exception tests ----

std::atomic<int> g_host_guards_destroyed{0};

struct HostGuard {
    HostGuard() = default;
    HostGuard(const HostGuard&) = delete;
    HostGuard& operator=(const HostGuard&) = delete;
    ~HostGuard() { ++g_host_guards_destroyed; }
};

struct ThrowThrough {
    void (*guest_throw)(int);
    int code;
};

// Called by guest code; calls guest code that throws a C++ exception through this frame.
void host_throw_through(void* context) {
    const auto* call = static_cast<const ThrowThrough*>(context);
    const HostGuard guard;
    call->guest_throw(call->code);
}

struct RaiseThrough {
    void (*guest_raise)();
};

// Called by guest code; calls guest code that raises an SEH exception through this frame.
void host_raise_through(void* context) {
    static_cast<const RaiseThrough*>(context)->guest_raise();
}

// ---- Log capture ----

class LogCapture {
public:
    LogCapture() { g6::log::set_sink(&LogCapture::sink, this); }
    ~LogCapture() { g6::log::set_sink(nullptr, nullptr); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    size_t count_containing(std::string_view text) const {
        const std::scoped_lock lock(mutex_);
        size_t count = 0;
        for (const std::string& line : lines_) {
            if (line.find(text) != std::string::npos) {
                ++count;
            }
        }
        return count;
    }

private:
    static void sink(g6::log::Level, std::string_view, std::string_view message, void* context) {
        auto* self = static_cast<LogCapture*>(context);
        const std::scoped_lock lock(self->mutex_);
        self->lines_.emplace_back(message);
    }

    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

} // namespace

TEST_CASE("imports bind by policy") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    CHECK(!guest.is_executable());
    CHECK_EQ(guest.name(), std::string("loader_guest.dll"));
    CHECK(guest_function<int (*)()>(guest, "g6_guest_relocated_ok")() == 1);
    CHECK(guest_function<void* (*)()>(guest, "g6_guest_image_base")() ==
          reinterpret_cast<void*>(guest.base()));

    // HLE by name and by ordinal.
    CHECK_EQ(guest_function<int (*)(int, int)>(guest, "g6_guest_fake_add")(2, 3), 1005);
    CHECK_EQ(guest_function<int (*)(int)>(guest, "g6_guest_fake_ordinal")(7), 21);

    // A delay-loaded import is bound at load time, so the delay helper never runs.
    CHECK_EQ(guest_function<int (*)()>(guest, "g6_guest_delay_value")(), kDelayValue);
    bool found_delay = false;
    for (const g6::pe::ImportedModule& imported : g6::pe::read_imports(guest.image())) {
        if (!imported.delay_load) {
            continue;
        }
        CHECK_EQ(g6::loader::normalize_dll_name(imported.dll), std::string("g6fakedelay.dll"));
        for (const g6::pe::ImportedSymbol& symbol : imported.symbols) {
            const auto* slot = reinterpret_cast<void* const*>(guest.base() + symbol.iat_rva);
            CHECK(*slot == reinterpret_cast<void*>(&host_delay_value));
            found_delay = true;
        }
        if (imported.module_handle_rva != 0) {
            const auto* handle =
                reinterpret_cast<void* const*>(guest.base() + imported.module_handle_rva);
            CHECK(*handle != nullptr);
        }
    }
    CHECK(found_delay);

    // The loader maps the module itself; Windows never sees it.
    CHECK(GetModuleHandleW(L"loader_guest.dll") == nullptr);
    REQUIRE(loader.find("LOADER_GUEST").has_value());
    CHECK(*loader.find("LOADER_GUEST") == guest);
    REQUIRE(loader.module_at(guest.base() + 0x1000).has_value());
    CHECK(*loader.module_at(guest.base() + 0x1000) == guest);
    CHECK_EQ(loader.modules().size(), size_t{1});

    // Loading it again returns the same module.
    CHECK(loader.load("loader_guest.dll") == guest);

    // DllMain and the TLS callback each saw PROCESS_ATTACH once.
    const G6GuestCounts counts = counts_of(guest);
    CHECK_EQ(counts.dll_process_attach, 1L);
    CHECK_EQ(counts.tls_process_attach, 1L);
}

TEST_CASE("unimplemented imports log once and fail") {
    const LogCapture log;
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");

    const auto call_missing = guest_function<uint64_t (*)()>(guest, "g6_guest_call_missing");
    const auto call_missing_ordinal =
        guest_function<uint64_t (*)()>(guest, "g6_guest_call_missing_ordinal");
    const auto call_hresult = guest_function<long (*)()>(guest, "g6_guest_call_hresult");

    // Zero, with ERROR_CALL_NOT_IMPLEMENTED: the stub's value is in the high half.
    CHECK_EQ(call_missing(), uint64_t{ERROR_CALL_NOT_IMPLEMENTED});
    CHECK_EQ(call_missing(), uint64_t{ERROR_CALL_NOT_IMPLEMENTED});
    CHECK_EQ(call_missing_ordinal(), uint64_t{ERROR_CALL_NOT_IMPLEMENTED});
    CHECK_EQ(static_cast<uint32_t>(call_hresult()), uint32_t{0x80004001}); // E_NOTIMPL

    CHECK_EQ(
        log.count_containing(
            "unimplemented import g6fakeos.dll!g6fake_missing called from loader_guest.dll+0x"),
        size_t{1});
    CHECK_EQ(log.count_containing("unimplemented import g6fakeos.dll!g6fakeos_ord9 called from "
                                  "loader_guest.dll+0x"),
             size_t{1});
    CHECK_EQ(log.count_containing("unimplemented import g6fakeos.dll!g6fake_hresult"), size_t{1});
    CHECK_EQ(log.count_containing("mapped loader_guest.dll at"), size_t{1});
}

TEST_CASE("static TLS on threads that started before and after the load") {
    Loader loader(make_options());

    // A thread that starts before the module is loaded and reads its TLS afterwards.
    struct Early {
        std::atomic<const Module*> guest{nullptr};
        int value = 0;
        int* pointer = nullptr;
        int callback_seen = -1;
    } early;
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(go != nullptr);
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(started != nullptr);
    std::thread early_thread([&early, go, started] {
        SetEvent(started);
        WaitForSingleObject(go, INFINITE);
        const Module* guest = early.guest.load();
        if (guest == nullptr) {
            return;
        }
        early.value = guest_function<int (*)()>(*guest, "g6_guest_tls_get")();
        early.pointer = guest_function<int* (*)()>(*guest, "g6_guest_tls_pointer")();
        early.callback_seen = guest_function<int (*)()>(*guest, "g6_guest_tls_callback_seen")();
    });
    struct Release {
        HANDLE go;
        std::thread& thread;
        Release(HANDLE event, std::thread& waiting) : go(event), thread(waiting) {}
        Release(const Release&) = delete;
        Release& operator=(const Release&) = delete;
        ~Release() {
            SetEvent(go);
            if (thread.joinable()) {
                thread.join();
            }
            CloseHandle(go);
        }
    } const release{go, early_thread};
    // Windows delivers a new thread's THREAD_ATTACH notifications on that thread when it first
    // runs, not when it is created. Until the thread is running it would still see the donor
    // DLL the load brings in, so wait for it.
    WaitForSingleObject(started, INFINITE);
    CloseHandle(started);

    const Module guest = loader.load("loader_guest.dll");
    const auto tls_get = guest_function<int (*)()>(guest, "g6_guest_tls_get");
    const auto tls_set = guest_function<void (*)(int)>(guest, "g6_guest_tls_set");
    const auto tls_pointer = guest_function<int* (*)()>(guest, "g6_guest_tls_pointer");
    const auto tls_address = guest_function<void* (*)()>(guest, "g6_guest_tls_address");
    const auto callback_seen = guest_function<int (*)()>(guest, "g6_guest_tls_callback_seen");
    // The template holds a pointer to this, relocated like any other.
    auto* const global = static_cast<int*>(guest.find_export("g6_guest_global"));
    REQUIRE(global != nullptr);
    CHECK_EQ(*global, 1234);

    early.guest.store(&guest);
    SetEvent(go);
    early_thread.join();
    CHECK_EQ(early.value, G6_GUEST_TLS_INITIAL);
    CHECK(early.pointer == global);
    // A module loaded after a thread started gets no THREAD_ATTACH for it, as on Windows.
    CHECK_EQ(early.callback_seen, 0);

    // This thread, which loaded the module.
    CHECK_EQ(tls_get(), G6_GUEST_TLS_INITIAL);
    CHECK(tls_pointer() == global);
    const void* const main_address = tls_address();

    // A thread that starts after the load: its block is filled before the TLS callback runs.
    const G6GuestCounts before = counts_of(guest);
    int late_initial = 0;
    int late_after_set = 0;
    int* late_pointer = nullptr;
    void* late_address = nullptr;
    int late_callback_seen = 0;
    std::thread late([&] {
        late_initial = tls_get();
        late_pointer = tls_pointer();
        late_callback_seen = callback_seen();
        tls_set(99);
        late_after_set = tls_get();
        late_address = tls_address();
    });
    late.join();
    const G6GuestCounts after = counts_of(guest);

    CHECK_EQ(late_initial, G6_GUEST_TLS_INITIAL);
    CHECK_EQ(late_after_set, 99);
    CHECK(late_pointer == global);
    CHECK_EQ(late_callback_seen, 1);
    CHECK(late_address != nullptr);
    CHECK(late_address != main_address);
    // The other thread's write stayed on that thread.
    CHECK_EQ(tls_get(), G6_GUEST_TLS_INITIAL);

    // At least the one thread attached and detached (Windows may start threads of its own).
    CHECK(after.dll_thread_attach >= before.dll_thread_attach + 1);
    CHECK(after.dll_thread_detach >= before.dll_thread_detach + 1);
    CHECK(after.tls_thread_attach >= before.tls_thread_attach + 1);
    CHECK(after.tls_thread_detach >= before.tls_thread_detach + 1);
}

TEST_CASE("a module with static TLS keeps its thread notifications") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    const auto disable = guest_function<int (*)()>(guest, "g6_guest_disable_thread_calls");
    const auto callback_seen = guest_function<int (*)()>(guest, "g6_guest_tls_callback_seen");

    // Windows accepts DisableThreadLibraryCalls from such a module and ignores it, because its
    // TLS callbacks set up thread_local variables on every new thread.
    CHECK_EQ(disable(), 1);
    const G6GuestCounts before = counts_of(guest);
    int seen = 0;
    std::thread thread([&] { seen = callback_seen(); });
    thread.join();
    const G6GuestCounts after = counts_of(guest);

    CHECK_EQ(seen, 1);
    CHECK(after.tls_thread_attach >= before.tls_thread_attach + 1);
    CHECK(after.dll_thread_attach >= before.dll_thread_attach + 1);
    CHECK(after.dll_thread_detach >= before.dll_thread_detach + 1);
}

TEST_CASE("a guest C++ exception unwinds through a host frame to a guest catch") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    const auto guest_throw = guest_function<void (*)(int)>(guest, "g6_guest_throw");
    const auto catch_through =
        guest_function<int (*)(G6HostCallback, void*)>(guest, "g6_guest_catch_through");
    const auto guards_destroyed = guest_function<long (*)()>(guest, "g6_guest_guards_destroyed");

    // Both frames' destructors run.
    const int host_guards = g_host_guards_destroyed.load();
    ThrowThrough call{guest_throw, 17};
    CHECK_EQ(catch_through(&host_throw_through, &call), 17);
    CHECK_EQ(g_host_guards_destroyed.load(), host_guards + 1);
    CHECK_EQ(guards_destroyed(), 1L);
}

TEST_CASE("host code catches a guest C++ exception") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    const auto guest_throw = guest_function<void (*)(int)>(guest, "g6_guest_throw");
    const auto guards_destroyed = guest_function<long (*)()>(guest, "g6_guest_guards_destroyed");

    bool caught = false;
    try {
        guest_throw(5);
    } catch (...) {
        caught = true;
    }
    CHECK(caught);
    CHECK_EQ(guards_destroyed(), 1L);
}

TEST_CASE("a guest SEH exception unwinds through a host frame to a guest handler") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    const auto seh_raise = guest_function<void (*)()>(guest, "g6_guest_seh_raise");
    const auto seh_catch_through =
        guest_function<DWORD (*)(G6HostCallback, void*)>(guest, "g6_guest_seh_catch_through");
    const auto finally_ran = guest_function<long (*)()>(guest, "g6_guest_finally_ran");

    // The guest's __finally runs on the way.
    RaiseThrough raise{seh_raise};
    CHECK_EQ(seh_catch_through(&host_raise_through, &raise), DWORD{G6_GUEST_SEH_CODE});
    CHECK_EQ(finally_ran(), 1L);
}

TEST_CASE("a hardware exception in guest code reaches a guest handler") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    CHECK_EQ(guest_function<int (*)()>(guest, "g6_guest_fault_caught")(), 1);
}

TEST_CASE("guest fibers keep their thread's TLS and their own exceptions") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    const auto tls_set = guest_function<void (*)(int)>(guest, "g6_guest_tls_set");
    const auto ping_pong = guest_function<int (*)(int, int)>(guest, "g6_guest_fiber_ping_pong");

    int result = 0;
    std::thread thread([&] {
        tls_set(77);
        result = ping_pong(5, 77);
    });
    thread.join();
    // 5 runs, TLS right in all of them, one exception caught on the fiber's stack.
    CHECK_EQ(result, 50501);
}

TEST_CASE("the HLE loader functions see guest modules and the policy") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    using GetModuleHandleFn = void* (*)(const wchar_t*, unsigned long*);
    using GetProcAddressFn = void* (*)(void*, const char*, unsigned long*);
    using ModuleFileNameFn = unsigned long (*)(void*, wchar_t*, unsigned long);
    const auto get_module_handle =
        guest_function<GetModuleHandleFn>(guest, "g6_guest_get_module_handle");
    const auto get_proc_address =
        guest_function<GetProcAddressFn>(guest, "g6_guest_get_proc_address");
    const auto load_library = guest_function<GetModuleHandleFn>(guest, "g6_guest_load_library");
    const auto module_file_name =
        guest_function<ModuleFileNameFn>(guest, "g6_guest_module_file_name");
    auto* const base = reinterpret_cast<void*>(guest.base());
    unsigned long error = 0;

    // Guest modules, by any spelling of the name.
    CHECK(get_module_handle(L"loader_guest.dll", &error) == base);
    CHECK(get_module_handle(L"LOADER_GUEST", &error) == base);
    CHECK(load_library(L"loader_guest.dll", &error) == base);

    // Their exports, by name and by ordinal.
    const void* const fake_add = guest.find_export("g6_guest_fake_add");
    REQUIRE(fake_add != nullptr);
    CHECK(get_proc_address(base, "g6_guest_fake_add", &error) == fake_add);
    uint32_t ordinal = 0;
    for (const g6::pe::Export& entry : g6::pe::read_exports(guest.image()).exports) {
        if (entry.name == "g6_guest_fake_add") {
            ordinal = entry.ordinal;
        }
    }
    REQUIRE(ordinal != 0);
    CHECK(get_proc_address(base, MAKEINTRESOURCEA(ordinal), &error) == fake_add);
    CHECK(guest.find_export(static_cast<uint16_t>(ordinal)) == fake_add);
    CHECK(get_proc_address(base, "g6_no_such_export", &error) == nullptr);
    CHECK_EQ(error, static_cast<unsigned long>(ERROR_PROC_NOT_FOUND));

    // Networking DLLs don't exist (hard rule 5).
    for (const wchar_t* name : {L"ws2_32.dll", L"winhttp.dll", L"WININET"}) {
        CHECK(get_module_handle(name, &error) == nullptr);
        CHECK_EQ(error, static_cast<unsigned long>(ERROR_MOD_NOT_FOUND));
        CHECK(load_library(name, &error) == nullptr);
        CHECK_EQ(error, static_cast<unsigned long>(ERROR_MOD_NOT_FOUND));
    }

    // A host DLL is a pseudo handle that resolves through the policy, never the real module.
    const HMODULE host_kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* const kernel32 = get_module_handle(L"kernel32.dll", &error);
    REQUIRE(kernel32 != nullptr);
    CHECK(kernel32 != host_kernel32);
    CHECK(load_library(L"KERNEL32", &error) == kernel32);
    CHECK(get_proc_address(kernel32, "GetCurrentThreadId", &error) ==
          reinterpret_cast<void*>(GetProcAddress(host_kernel32, "GetCurrentThreadId")));
    const void* const hle_get_proc_address = get_proc_address(kernel32, "GetProcAddress", &error);
    CHECK(hle_get_proc_address != nullptr);
    CHECK(hle_get_proc_address !=
          reinterpret_cast<void*>(GetProcAddress(host_kernel32, "GetProcAddress")));

    // A console DLL the registry implements.
    void* const fake_os = get_module_handle(L"g6fakeos.dll", &error);
    REQUIRE(fake_os != nullptr);
    CHECK(fake_os != kernel32);
    CHECK(get_proc_address(fake_os, "g6fake_add", &error) ==
          reinterpret_cast<void*>(&host_fake_add));
    CHECK(get_proc_address(fake_os, MAKEINTRESOURCEA(G6_FAKE_ORDINAL_BOUND), &error) ==
          reinterpret_cast<void*>(&host_fake_ordinal));
    CHECK(get_proc_address(fake_os, "g6fake_missing", &error) == nullptr);
    CHECK_EQ(error, static_cast<unsigned long>(ERROR_PROC_NOT_FOUND));

    // A DLL nobody knows.
    CHECK(get_module_handle(L"g6nothing.dll", &error) == nullptr);
    CHECK_EQ(error, static_cast<unsigned long>(ERROR_MOD_NOT_FOUND));
    CHECK(load_library(L"g6nothing.dll", &error) == nullptr);
    CHECK_EQ(error, static_cast<unsigned long>(ERROR_MOD_NOT_FOUND));

    // The file name is the guest file's.
    wchar_t buffer[MAX_PATH] = {};
    const unsigned long length = module_file_name(base, buffer, MAX_PATH);
    CHECK_EQ(std::filesystem::path(std::wstring(buffer, length)), guest.path());
}

TEST_CASE("a threadpool callback can ask to unload its own guest module") {
    // Windows' pool would unload the handle through its own loader, which doesn't know guest
    // modules. Guest modules stay mapped until the Loader goes, so the request changes nothing.
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    using GetModuleHandleFn = void* (*)(const wchar_t*, unsigned long*);
    using GetProcAddressFn = void* (*)(void*, const char*, unsigned long*);
    const auto get_module_handle =
        guest_function<GetModuleHandleFn>(guest, "g6_guest_get_module_handle");
    const auto get_proc_address =
        guest_function<GetProcAddressFn>(guest, "g6_guest_get_proc_address");
    unsigned long error = 0;

    // The guest's import is bound to the loader's HLE, not to Windows' pool.
    void* const kernel32 = get_module_handle(L"kernel32", &error);
    REQUIRE(kernel32 != nullptr);
    const void* const bound = get_proc_address(kernel32, "FreeLibraryWhenCallbackReturns", &error);
    CHECK(bound != nullptr);
    CHECK(bound != reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                          "FreeLibraryWhenCallbackReturns")));

    const auto pool_unload = guest_function<int (*)(int)>(guest, "g6_guest_pool_unload");
    CHECK_EQ(pool_unload(8), 8);
    // The module is still mapped and runs.
    CHECK_EQ(pool_unload(1), 1);
    CHECK_EQ(guest_function<int (*)(int, int)>(guest, "g6_guest_fake_add")(1, 2), 1003);
}

TEST_CASE("modules load away from their preferred base") {
    const g6::pe::Image image = g6::pe::Image::load_file(guest_path("loader_guest.dll"));
    // Take the preferred range; if something else holds it already, that works as well.
    void* const blocker = VirtualAlloc(reinterpret_cast<void*>(image.image_base()),
                                       image.size_of_image(), MEM_RESERVE, PAGE_NOACCESS);
    {
        Loader loader(make_options());
        const Module guest = loader.load("loader_guest.dll");
        CHECK(guest.base() != image.image_base());
        CHECK(guest_function<int (*)()>(guest, "g6_guest_relocated_ok")() == 1);
        CHECK(guest_function<void* (*)()>(guest, "g6_guest_image_base")() ==
              reinterpret_cast<void*>(guest.base()));
        CHECK(guest_function<int* (*)()>(guest, "g6_guest_tls_pointer")() ==
              guest.find_export("g6_guest_global"));
        CHECK_EQ(guest_function<int (*)(int, int)>(guest, "g6_guest_fake_add")(1, 1), 1002);

        // Exceptions still find the relocated exception table and ThrowInfo.
        const auto guest_throw = guest_function<void (*)(int)>(guest, "g6_guest_throw");
        ThrowThrough call{guest_throw, 23};
        CHECK_EQ(guest_function<int (*)(G6HostCallback, void*)>(guest, "g6_guest_catch_through")(
                     &host_throw_through, &call),
                 23);
    }
    if (blocker != nullptr) {
        VirtualFree(blocker, 0, MEM_RELEASE);
    }
}

TEST_CASE("modules load at an explicit base") {
    const g6::pe::Image image = g6::pe::Image::load_file(guest_path("loader_guest.dll"));
    Loader loader(make_options());
    // Find a free range, then hand it to the loader. Top down, where heap growth won't take it
    // in the meantime.
    void* const probe =
        VirtualAlloc(nullptr, image.size_of_image(), MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
    REQUIRE(probe != nullptr);
    VirtualFree(probe, 0, MEM_RELEASE);

    const Module guest =
        loader.load(guest_path("loader_guest.dll"), reinterpret_cast<uint64_t>(probe));
    CHECK(guest.base() == reinterpret_cast<uint64_t>(probe));
    CHECK(guest_function<int (*)()>(guest, "g6_guest_relocated_ok")() == 1);
    CHECK_EQ(guest_function<int (*)()>(guest, "g6_guest_tls_get")(), G6_GUEST_TLS_INITIAL);
    CHECK_EQ(guest.path(), guest_path("loader_guest.dll"));
}

TEST_CASE("an executable runs its entry point") {
    const LogCapture log;
    Loader loader(make_options());
    const Module exe = loader.load("loader_guest_exe.exe");
    CHECK(exe.is_executable());
    // This host doesn't lend TLS index 0 (test_exe_tls covers one that does), so the executable's
    // TLS goes through a donor, which its code, built without /GA, handles.
    CHECK_EQ(log.count_containing("loader_guest_exe.exe is an executable with static TLS, but gets "
                                  "TLS index"),
             size_t{1});
    // Its DLL came along.
    REQUIRE(loader.find("loader_guest.dll").has_value());
    CHECK_EQ(loader.modules().size(), size_t{2});
    // Own TLS, the DLL's TLS, the stack reserve, the PEB, and GetModuleHandle(NULL).
    CHECK_EQ(loader.run_entry_point(exe), uint32_t{0x1F});
    CHECK_THROWS_AS(loader.run_entry_point(*loader.find("loader_guest.dll")),
                    std::invalid_argument);
}

TEST_CASE("each module gets a random security cookie before it runs") {
    Loader loader(make_options());
    const Module guest = loader.load("loader_guest.dll");
    const uint64_t cookie =
        guest_function<uint64_t (*)()>(guest, "g6_guest_cookie_at_tls_attach")();
    CHECK(cookie != 0);
    CHECK(cookie != kDefaultSecurityCookie);
    CHECK_EQ(cookie >> 48, uint64_t{0});
    // The C runtime kept it.
    CHECK_EQ(guest_function<uint64_t (*)()>(guest, "g6_guest_security_cookie")(), cookie);
}

TEST_CASE("loads that can't succeed throw") {
    Loader loader(make_options());
    CHECK_THROWS_AS(loader.load("g6_no_such_module.dll"), LoadError);
    CHECK_THROWS_AS(loader.load("ws2_32.dll"), LoadError);
    CHECK_THROWS_AS(loader.load("WinHttp"), LoadError);
    CHECK(loader.modules().empty());
    // The loader still works afterwards.
    const Module guest = loader.load("loader_guest.dll");
    CHECK_EQ(guest_function<int (*)(int, int)>(guest, "g6_guest_fake_add")(0, 0), 1000);
}

TEST_CASE("an imported DLL in the search path is mapped unless it is replaced") {
    Loader loader(make_options());
    const Module importer = loader.load("replaced_importer.dll");
    CHECK(loader.find("g6replaced.dll").has_value());
    CHECK_EQ(loader.modules().size(), size_t{2});
    CHECK_EQ(guest_function<int (*)()>(importer, "g6_importer_call")(), G6_REPLACED_ORIGINAL);
}

TEST_CASE("a replaced DLL without an implementation is never mapped") {
    const LogCapture log;
    LoaderOptions options = make_options();
    options.imports.replace_dll("g6replaced.dll");
    Loader loader(std::move(options));

    // The importer loads; its import from the replaced DLL is a stub.
    const Module importer = loader.load("replaced_importer.dll");
    CHECK(!loader.find("g6replaced.dll").has_value());
    CHECK_EQ(loader.modules().size(), size_t{1});
    SetLastError(0);
    const int value = guest_function<int (*)()>(importer, "g6_importer_call")();
    const DWORD error_after_call = GetLastError();
    CHECK_EQ(value, 0);
    CHECK_EQ(error_after_call, DWORD{ERROR_CALL_NOT_IMPLEMENTED});
    CHECK_EQ(log.count_containing("unimplemented import g6replaced.dll!g6replaced_value"),
             size_t{1});

    // Neither the host nor the guest can load it, by name or by path.
    CHECK_THROWS_AS(loader.load("g6replaced.dll"), LoadError);
    CHECK_THROWS_AS(loader.load(guest_path("G6REPLACED.DLL")), LoadError);
    const Module guest = loader.load("loader_guest.dll");
    using GetModuleHandleFn = void* (*)(const wchar_t*, unsigned long*);
    const auto load_library = guest_function<GetModuleHandleFn>(guest, "g6_guest_load_library");
    const auto get_module_handle =
        guest_function<GetModuleHandleFn>(guest, "g6_guest_get_module_handle");
    unsigned long error = 0;
    CHECK(load_library(L"g6replaced.dll", &error) == nullptr);
    CHECK_EQ(error, static_cast<unsigned long>(ERROR_MOD_NOT_FOUND));
    CHECK(get_module_handle(L"g6replaced", &error) == nullptr);
    CHECK_EQ(error, static_cast<unsigned long>(ERROR_MOD_NOT_FOUND));
    CHECK(!loader.find("g6replaced.dll").has_value());
    CHECK_EQ(loader.modules().size(), size_t{2});
}

TEST_CASE("HLE rules stand in for a replaced DLL") {
    LoaderOptions options = make_options();
    options.imports.replace_dll("g6replaced.dll");
    options.imports.hle("g6replaced.dll", Symbol::by_name("g6replaced_value"),
                        reinterpret_cast<const void*>(&host_replacement_value));
    Loader loader(std::move(options));

    const Module importer = loader.load("replaced_importer.dll");
    CHECK(!loader.find("g6replaced.dll").has_value());
    CHECK_EQ(guest_function<int (*)()>(importer, "g6_importer_call")(), kReplacementValue);

    // The guest gets a pseudo handle that resolves to our function, as for any registry DLL.
    const Module guest = loader.load("loader_guest.dll");
    using GetModuleHandleFn = void* (*)(const wchar_t*, unsigned long*);
    using GetProcAddressFn = void* (*)(void*, const char*, unsigned long*);
    const auto load_library = guest_function<GetModuleHandleFn>(guest, "g6_guest_load_library");
    const auto get_module_handle =
        guest_function<GetModuleHandleFn>(guest, "g6_guest_get_module_handle");
    const auto get_proc_address =
        guest_function<GetProcAddressFn>(guest, "g6_guest_get_proc_address");
    unsigned long error = 0;
    void* const replaced = load_library(L"g6replaced.dll", &error);
    REQUIRE(replaced != nullptr);
    CHECK(!loader.module_at(reinterpret_cast<uint64_t>(replaced)).has_value());
    CHECK(get_module_handle(L"G6REPLACED", &error) == replaced);
    CHECK(get_proc_address(replaced, "g6replaced_value", &error) ==
          reinterpret_cast<void*>(&host_replacement_value));
    CHECK(get_proc_address(replaced, "g6replaced_other", &error) == nullptr);
    CHECK_EQ(error, static_cast<unsigned long>(ERROR_PROC_NOT_FOUND));
    CHECK_EQ(loader.modules().size(), size_t{2});
}

TEST_CASE("only one loader exists at a time") {
    Loader first(make_options());
    CHECK_THROWS_AS(Loader(make_options()), std::logic_error);
    const Module guest = first.load("loader_guest.dll");
    CHECK_EQ(guest_function<int (*)(int, int)>(guest, "g6_guest_fake_add")(1, 2), 1003);
}

// ---- Process notifications, compared with Windows' own loader (init_guest.dll) ----

namespace {

constexpr int kInitCapacity = 64;

// Notifications as init_sink.dll encodes them (G6_INIT_RECORD).
using InitRecords = std::vector<int>;

// Where init_guest.dll's imports from init_sink.dll go under the native loader. Its TLS callback
// may run on any thread that starts or exits while it is loaded.
std::mutex g_init_mutex;
InitRecords g_init_records;
std::atomic<int> g_init_refuse{0};

void host_init_record(int routine, unsigned long reason) {
    const std::scoped_lock lock(g_init_mutex);
    g_init_records.push_back(G6_INIT_RECORD(routine, static_cast<int>(reason)));
}

int host_init_refuse() {
    return g_init_refuse.load();
}

// The process notifications in order, as text: thread notifications depend on unrelated threads.
std::string describe_process(const InitRecords& records) {
    std::string text;
    for (const int record : records) {
        const int reason = record & 0xFFFF;
        if (reason != DLL_PROCESS_ATTACH && reason != DLL_PROCESS_DETACH) {
            continue;
        }
        text += text.empty() ? "" : ", ";
        text += (record >> 16) == G6_INIT_TLS_CALLBACK ? "tls " : "entry ";
        text += reason == DLL_PROCESS_ATTACH ? "attach" : "detach";
    }
    return text;
}

struct WindowsInit {
    std::string loaded; // a load and FreeLibrary
    std::string failed; // a load whose entry point fails
    DWORD failed_error = 0;
};

// What Windows' own loader does with init_guest.dll. The sink is loaded first, so it outlives
// both loads of the guest.
WindowsInit windows_init() {
    const std::wstring sink_path = guest_path("init_sink.dll").wstring();
    const std::wstring guest_file = guest_path("init_guest.dll").wstring();
    HMODULE const sink = LoadLibraryExW(sink_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE(sink != nullptr);
    const auto set_refuse =
        reinterpret_cast<void (*)(int)>(GetProcAddress(sink, "g6_init_set_refuse"));
    const auto read = reinterpret_cast<int (*)(int*, int)>(GetProcAddress(sink, "g6_init_read"));
    REQUIRE(set_refuse != nullptr);
    REQUIRE(read != nullptr);
    const auto take = [&] {
        std::array<int, kInitCapacity> buffer{};
        const int count = std::min(read(buffer.data(), kInitCapacity), kInitCapacity);
        return describe_process(InitRecords(buffer.begin(), buffer.begin() + count));
    };

    WindowsInit result;
    set_refuse(1);
    HMODULE const failed =
        LoadLibraryExW(guest_file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    result.failed_error = GetLastError();
    CHECK(failed == nullptr);
    result.failed = take();

    set_refuse(0);
    HMODULE const loaded =
        LoadLibraryExW(guest_file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        FreeLibrary(loaded);
    }
    result.loaded = take();
    FreeLibrary(sink);
    return result;
}

// The same under the native loader, with init_sink.dll replaced by the host functions above.
// The guest's detach notifications run when the loader goes away.
std::string native_init(bool refuse, bool& load_failed) {
    LoaderOptions options = make_options();
    options.imports.replace_dll("init_sink.dll");
    options.imports.hle("init_sink.dll", Symbol::by_name("g6_init_record"),
                        reinterpret_cast<const void*>(&host_init_record));
    options.imports.hle("init_sink.dll", Symbol::by_name("g6_init_refuse"),
                        reinterpret_cast<const void*>(&host_init_refuse));
    {
        const std::scoped_lock lock(g_init_mutex);
        g_init_records.clear();
    }
    g_init_refuse = refuse ? 1 : 0;
    load_failed = false;
    {
        Loader loader(std::move(options));
        try {
            (void)loader.load("init_guest.dll");
        } catch (const LoadError&) {
            load_failed = true;
        }
    }
    const std::scoped_lock lock(g_init_mutex);
    return describe_process(g_init_records);
}

} // namespace

TEST_CASE("TLS callbacks and the entry point get Windows' notifications, also on a failed load") {
    const WindowsInit windows = windows_init();
    INFO("Windows: loaded: " + windows.loaded + "; failed: " + windows.failed);
    CHECK_EQ(windows.failed_error, DWORD{ERROR_DLL_INIT_FAILED});

    bool load_failed = false;
    CHECK_EQ(native_init(false, load_failed), windows.loaded);
    CHECK(!load_failed);
    CHECK_EQ(native_init(true, load_failed), windows.failed);
    CHECK(load_failed);
}
