// Original guest DLL for the native loader tests (tests/loader/test_loader.cpp). Built with the
// static C runtime, so its exception handling, TLS support, and startup code are guest code too,
// and with Control Flow Guard, like a shipped binary. The loader maps it itself; Windows never
// sees it as a module.
//
// It exercises what the loader must get right: imports from made-up console DLLs by name, by
// ordinal, delay-loaded, and missing; static TLS with a TLS callback and a relocated pointer in
// the template; DllMain notifications; C++ exceptions and SEH across guest and host frames;
// fibers; the HLE loader functions, including a threadpool callback that unloads its own module;
// relocations; and the security cookie.

#include <cstdint>
#include <windows.h>

#include "loader_guest_api.h"

#define GUEST_EXPORT extern "C" __declspec(dllexport)

extern "C" {
// g6fakeos.dll and g6fakedelay.dll exist only as import libraries (see the .def files).
__declspec(dllimport) int g6fake_add(int a, int b);
__declspec(dllimport) int g6fake_bound_ordinal(int a);
__declspec(dllimport) int g6fake_missing(void);
__declspec(dllimport) long g6fake_hresult(void);
__declspec(dllimport) int g6fake_missing_ordinal(void);
__declspec(dllimport) int g6fakedelay_value(void);

// The linker's name for the image's own headers.
extern IMAGE_DOS_HEADER __ImageBase; // NOLINT(bugprone-reserved-identifier)
}

namespace {

G6GuestCounts g_counts;
uintptr_t g_cookie_at_tls_attach;
volatile LONG g_guards_destroyed;
volatile LONG g_finally_ran;

void count(long* counter) {
    InterlockedIncrement(counter);
}

} // namespace

// ---- Relocations ----

GUEST_EXPORT int g6_guest_global = 1234;
// A pointer in initialized data: only a base relocation makes it right at another base.
GUEST_EXPORT int* g6_guest_global_pointer = &g6_guest_global;

GUEST_EXPORT void* g6_guest_image_base(void) {
    return &__ImageBase;
}

GUEST_EXPORT int g6_guest_relocated_ok(void) {
    return g6_guest_global_pointer == &g6_guest_global ? 1 : 0;
}

// ---- Static TLS ----

static __declspec(thread) int t_value = G6_GUEST_TLS_INITIAL;
// The template holds a pointer, which the loader's relocations must have fixed.
static __declspec(thread) int* t_pointer = &g6_guest_global;
static __declspec(thread) int t_callback_seen = 0;

GUEST_EXPORT int g6_guest_tls_get(void) {
    return t_value;
}

GUEST_EXPORT void g6_guest_tls_set(int value) {
    t_value = value;
}

GUEST_EXPORT int* g6_guest_tls_pointer(void) {
    return t_pointer;
}

GUEST_EXPORT void* g6_guest_tls_address(void) {
    return &t_value;
}

// Whether this thread's THREAD_ATTACH TLS callback ran, and ran after its block was filled.
GUEST_EXPORT int g6_guest_tls_callback_seen(void) {
    return t_callback_seen;
}

static void NTAPI on_tls_callback(PVOID, DWORD reason, PVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        count(&g_counts.tls_process_attach);
        // Before any C runtime code: only the loader can have set the cookie by now.
        g_cookie_at_tls_attach = __security_cookie;
        break;
    case DLL_THREAD_ATTACH:
        count(&g_counts.tls_thread_attach);
        t_callback_seen = 1;
        break;
    case DLL_THREAD_DETACH: count(&g_counts.tls_thread_detach); break;
    default: break;
    }
}

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g6_guest_tls_callback")
#pragma section(".CRT$XLF", long, read)
// A const pointer, so it lands in the read-only callback array.
extern "C" __declspec(allocate(".CRT$XLF")) void(NTAPI* const g6_guest_tls_callback)(
    PVOID, DWORD, PVOID) = on_tls_callback;

// ---- DllMain ----

// The C runtime's DLL entry point calls DllMain by name.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) { // NOLINT(misc-use-internal-linkage)
    switch (reason) {
    case DLL_PROCESS_ATTACH: count(&g_counts.dll_process_attach); break;
    case DLL_THREAD_ATTACH: count(&g_counts.dll_thread_attach); break;
    case DLL_THREAD_DETACH: count(&g_counts.dll_thread_detach); break;
    default: break;
    }
    return TRUE;
}

// Asks for no more thread notifications, as a DLL may from its DllMain.
GUEST_EXPORT int g6_guest_disable_thread_calls(void) {
    return DisableThreadLibraryCalls(reinterpret_cast<HMODULE>(&__ImageBase)) ? 1 : 0;
}

GUEST_EXPORT void g6_guest_counts(G6GuestCounts* out) {
    *out = g_counts;
}

GUEST_EXPORT uint64_t g6_guest_cookie_at_tls_attach(void) {
    return g_cookie_at_tls_attach;
}

GUEST_EXPORT uint64_t g6_guest_security_cookie(void) {
    return __security_cookie;
}

// ---- Imports from the made-up console DLLs ----

GUEST_EXPORT int g6_guest_fake_add(int a, int b) {
    return g6fake_add(a, b);
}

GUEST_EXPORT int g6_guest_fake_ordinal(int a) {
    return g6fake_bound_ordinal(a);
}

// The stub's return value in the high half, the last error it left in the low half.
GUEST_EXPORT uint64_t g6_guest_call_missing(void) {
    SetLastError(ERROR_SUCCESS);
    const auto value = static_cast<uint32_t>(g6fake_missing());
    return (uint64_t{value} << 32) | GetLastError();
}

GUEST_EXPORT uint64_t g6_guest_call_missing_ordinal(void) {
    SetLastError(ERROR_SUCCESS);
    const auto value = static_cast<uint32_t>(g6fake_missing_ordinal());
    return (uint64_t{value} << 32) | GetLastError();
}

GUEST_EXPORT long g6_guest_call_hresult(void) {
    return g6fake_hresult();
}

GUEST_EXPORT int g6_guest_delay_value(void) {
    return g6fakedelay_value();
}

// ---- C++ exceptions ----

namespace {

struct GuestError {
    int code;
};

struct Guard {
    Guard() = default;
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() { InterlockedIncrement(&g_guards_destroyed); }
};

__declspec(noinline) void throw_with_guard(int code) {
    const Guard guard;
    throw GuestError{code};
}

} // namespace

// Throws a GuestError through a guest frame that holds a guard.
GUEST_EXPORT void g6_guest_throw(int code) {
    throw_with_guard(code);
}

// Calls the host, which is expected to throw a GuestError back through its own frames.
GUEST_EXPORT int g6_guest_catch_through(G6HostCallback callback, void* context) {
    try {
        callback(context);
    } catch (const GuestError& error) {
        return error.code;
    }
    return -1;
}

GUEST_EXPORT long g6_guest_guards_destroyed(void) {
    return g_guards_destroyed;
}

GUEST_EXPORT long g6_guest_finally_ran(void) {
    return g_finally_ran;
}

extern "C" void g6_guest_note_finally(void) {
    InterlockedIncrement(&g_finally_ran);
}

// ---- Fibers ----

namespace {

struct FiberState {
    void* main = nullptr;
    int expected_tls = 0;
    int runs = 0;
    int tls_ok = 0;
    int caught = 0;
};

void WINAPI fiber_main(void* parameter) {
    auto* state = static_cast<FiberState*>(parameter);
    for (;;) {
        ++state->runs;
        // Fibers share their thread's TLS.
        if (t_value == state->expected_tls) {
            ++state->tls_ok;
        }
        if (state->runs == 1) {
            // An exception thrown and caught on the fiber's own stack.
            try {
                throw_with_guard(state->runs);
            } catch (const GuestError&) {
                ++state->caught;
            }
        }
        SwitchToFiber(state->main);
    }
}

} // namespace

// Switches to a guest fiber `rounds` times. Returns runs * 10000 + tls_ok * 100 + caught, or a
// negative value when a fiber call fails. The thread must not be a fiber yet.
GUEST_EXPORT int g6_guest_fiber_ping_pong(int rounds, int expected_tls) {
    FiberState state;
    state.expected_tls = expected_tls;
    state.main = ConvertThreadToFiber(nullptr);
    if (state.main == nullptr) {
        return -1;
    }
    void* fiber = CreateFiber(0, fiber_main, &state);
    if (fiber == nullptr) {
        ConvertFiberToThread();
        return -2;
    }
    for (int i = 0; i < rounds; ++i) {
        SwitchToFiber(fiber);
    }
    DeleteFiber(fiber);
    ConvertFiberToThread();
    return state.runs * 10000 + state.tls_ok * 100 + state.caught;
}

// ---- The loader functions, which the loader binds to its HLE versions ----

GUEST_EXPORT void* g6_guest_get_module_handle(const wchar_t* name, unsigned long* error) {
    HMODULE module = GetModuleHandleW(name);
    *error = GetLastError();
    return module;
}

GUEST_EXPORT void* g6_guest_get_proc_address(void* module, const char* name, unsigned long* error) {
    FARPROC function = GetProcAddress(static_cast<HMODULE>(module), name);
    *error = GetLastError();
    return reinterpret_cast<void*>(function);
}

GUEST_EXPORT void* g6_guest_load_library(const wchar_t* name, unsigned long* error) {
    HMODULE module = LoadLibraryW(name);
    *error = GetLastError();
    return module;
}

GUEST_EXPORT unsigned long g6_guest_module_file_name(void* module, wchar_t* buffer,
                                                     unsigned long size) {
    return GetModuleFileNameW(static_cast<HMODULE>(module), buffer, size);
}

namespace {

volatile LONG g_pool_runs;

// What a DLL does to stay loaded while its threadpool callbacks run (the console's msvcp140
// does it for its task scheduler): it takes a reference to itself and asks the pool to drop it
// once the callback returns.
void CALLBACK pool_work(PTP_CALLBACK_INSTANCE instance, void*, PTP_WORK) {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<const wchar_t*>(&pool_work), &self)) {
        FreeLibraryWhenCallbackReturns(instance, self);
        InterlockedIncrement(&g_pool_runs);
    }
}

} // namespace

// Runs `rounds` threadpool callbacks that each unload this module when they return, and returns
// how many ran.
GUEST_EXPORT int g6_guest_pool_unload(int rounds) {
    InterlockedExchange(&g_pool_runs, 0);
    PTP_WORK work = CreateThreadpoolWork(pool_work, nullptr, nullptr);
    if (work == nullptr) {
        return -1;
    }
    for (int i = 0; i < rounds; ++i) {
        SubmitThreadpoolWork(work);
    }
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
    return static_cast<int>(InterlockedCompareExchange(&g_pool_runs, 0, 0));
}
