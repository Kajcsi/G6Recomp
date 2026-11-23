// The default import policy (docs/side-dlls.md, "Default import policy"): stubs that must not
// look like success, no networking, COM and WinRT activation never forwarded, the two replaced
// side DLLs, and the forwards the side DLLs rely on.

#include <cstdint>
#include <objbase.h>
#include <optional>
#include <roapi.h>
#include <string>
#include <utility>
#include <windows.h>
#include <winstring.h>

#include "g6/gdk/error.h"
#include "g6/gdk/runtime.h"
#include "g6/imports/policy.h"
#include "g6/imports/xmem.h"
#include "g6test.h"

using g6::imports::default_game_policy;
using g6::loader::Binding;
using g6::loader::ImportRegistry;
using g6::loader::Resolution;
using g6::loader::Symbol;

namespace {

constexpr uint64_t kAllOnes = ~uint64_t{0};
constexpr uint64_t kNotImplemented = 0x80004001; // E_NOTIMPL
constexpr std::optional<uint32_t> kNoError;
constexpr std::optional<uint32_t> kCallNotImplemented = ERROR_CALL_NOT_IMPLEMENTED;

struct ExpectedStub {
    const char* dll;
    const char* name;
    uint64_t value;
    std::optional<uint32_t> last_error;
};

const void* host_function(const wchar_t* dll, const char* name) {
    HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module != nullptr ? reinterpret_cast<const void*>(GetProcAddress(module, name))
                             : nullptr;
}

} // namespace

TEST_CASE("stubs in the default policy never look like success") {
    ImportRegistry policy = default_game_policy();
    const ExpectedStub expected[] = {
        // Path functions runtime/io doesn't map yet (docs/io.md, "Functions").
        {"api-ms-win-core-file-l1-2-0.dll", "CreateFile3", kAllOnes, kCallNotImplemented},
        {"kernel32.dll", "FindFirstFileNameW", kAllOnes, kCallNotImplemented},
        {"api-ms-win-core-file-l1-1-0.dll", "GetCompressedFileSizeW", 0xFFFFFFFF,
         kCallNotImplemented},
        {"api-ms-win-core-file-l1-1-0.dll", "GetFinalPathNameByHandleW", 0, kCallNotImplemented},
        {"api-ms-win-core-file-l2-1-0.dll", "CreateHardLinkW", 0, kCallNotImplemented},
        {"api-ms-win-core-file-l2-1-0.dll", "CreateSymbolicLinkW", 0, kCallNotImplemented},
        {"api-ms-win-core-file-l1-1-0.dll", "SetFileInformationByHandle", 0, kCallNotImplemented},
        {"api-ms-win-core-processenvironment-l1-1-0.dll", "SearchPathW", 0, kCallNotImplemented},
        // C runtime paths runtime/io doesn't map yet.
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wfindfirst64", kAllOnes, kNoError},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wchmod", kAllOnes, kNoError},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "_wfullpath", 0, kNoError},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "freopen", 0, kNoError},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "_wsopen_s", 40, kNoError},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "_wopen", kAllOnes, kNoError},
        {"api-ms-win-crt-time-l1-1-0.dll", "_wutime64", kAllOnes, kNoError},
        // Other programs, host DLLs, and the OneCore CRT copies.
        {"api-ms-win-crt-runtime-l1-1-0.dll", "system", kAllOnes, kNoError},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_wsystem", kAllOnes, kNoError},
        {"api-ms-win-crt-process-l1-1-0.dll", "_wspawnv", kAllOnes, kNoError},
        {"api-ms-win-crt-process-l1-1-0.dll", "_execv", kAllOnes, kNoError},
        {"api-ms-win-crt-process-l1-1-0.dll", "_loaddll", 0, kNoError},
        {"api-ms-win-crt-private-l1-1-0.dll", "_o_fopen", 0, kCallNotImplemented},
        {"api-ms-win-core-processthreads-l1-1-0.dll", "CreateProcessW", 0, kCallNotImplemented},
        // The registry returns its status; only reading the console-mode key is HLE.
        {"api-ms-win-core-registry-l1-1-0.dll", "RegSetValueExW", ERROR_CALL_NOT_IMPLEMENTED,
         kNoError},
        {"api-ms-win-core-registry-l1-1-0.dll", "RegEnumKeyExW", ERROR_CALL_NOT_IMPLEMENTED,
         kNoError},
        {"api-ms-win-core-registry-l1-1-0.dll", "RegGetValueW", ERROR_CALL_NOT_IMPLEMENTED,
         kNoError},
        // Activation by file, D3D12.X, and a family nobody reviewed yet.
        {"api-ms-win-core-com-l1-1-0.dll", "CoGetInstanceFromFile", kNotImplemented, kNoError},
        {"d3d12_xs.dll", "D3D12CreateDevice", kNotImplemented, kNoError},
        {"api-ms-win-core-memory-l1-1-0.dll", "VirtualAlloc", 0, kCallNotImplemented},
    };
    for (const ExpectedStub& stub : expected) {
        const Resolution resolution = policy.resolve(stub.dll, Symbol::by_name(stub.name));
        INFO(std::string(stub.dll) + "!" + stub.name);
        CHECK(resolution.binding == Binding::Stub);
        CHECK(resolution.address == nullptr);
        CHECK_EQ(resolution.stub.value, stub.value);
        CHECK(resolution.stub.last_error == stub.last_error);
    }
}

TEST_CASE("the path functions runtime/io maps are HLE, never the host's") {
    ImportRegistry policy = default_game_policy();
    struct Expected {
        const char* dll;
        const char* name;
        const wchar_t* host_dll;
    };
    const Expected expected[] = {
        {"api-ms-win-core-file-l1-1-0.dll", "CreateFileW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "CreateFileA", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-2-0.dll", "CreateFile2", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "FindFirstFileExW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "FindFirstFileW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "GetFileAttributesW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "GetFileAttributesExW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "SetFileAttributesW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "CreateDirectoryW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "RemoveDirectoryW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "DeleteFileW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l2-1-0.dll", "MoveFileExW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l2-1-0.dll", "MoveFileWithProgressW", L"kernelbase.dll"},
        {"api-ms-win-core-file-ansi-l2-1-0.dll", "MoveFileWithProgressA", L"kernelbase.dll"},
        {"api-ms-win-core-kernel32-legacy-l1-1-0.dll", "MoveFileW", L"kernelbase.dll"},
        {"api-ms-win-core-kernel32-legacy-l1-1-0.dll", "MoveFileExA", L"kernelbase.dll"},
        {"api-ms-win-core-file-l2-1-0.dll", "CopyFile2", L"kernelbase.dll"},
        {"api-ms-win-core-file-l2-1-0.dll", "CopyFileExW", L"kernelbase.dll"},
        {"api-ms-win-core-file-ansi-l2-1-0.dll", "CopyFileExA", L"kernelbase.dll"},
        {"api-ms-win-core-file-l2-1-2.dll", "CopyFileW", L"kernelbase.dll"},
        {"api-ms-win-core-kernel32-legacy-l1-1-0.dll", "CopyFileA", L"kernelbase.dll"},
        {"api-ms-win-core-file-l2-1-0.dll", "ReplaceFileW", L"kernelbase.dll"},
        {"api-ms-win-core-file-ansi-l2-1-0.dll", "ReplaceFileA", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "GetDiskFreeSpaceExW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-2-0.dll", "GetTempPathW", L"kernelbase.dll"},
        {"api-ms-win-core-file-l1-1-0.dll", "GetFullPathNameW", L"kernelbase.dll"},
        {"kernel32.dll", "CreateFileW", L"kernelbase.dll"},
        {"kernelbase.dll", "GetFileAttributesA", L"kernelbase.dll"},
        {"api-ms-win-core-processenvironment-l1-1-0.dll", "GetCurrentDirectoryW",
         L"kernelbase.dll"},
        {"api-ms-win-core-processenvironment-l1-1-0.dll", "SetCurrentDirectoryW",
         L"kernelbase.dll"},
        {"kernel32.dll", "SetCurrentDirectoryA", L"kernelbase.dll"},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "fopen", L"ucrtbase.dll"},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "_wfopen", L"ucrtbase.dll"},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "fopen_s", L"ucrtbase.dll"},
        {"api-ms-win-crt-stdio-l1-1-0.dll", "_wfsopen", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wstat64", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_access", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wmkdir", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wrmdir", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "remove", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wrename", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wunlink", L"ucrtbase.dll"},
        {"api-ms-win-crt-environment-l1-1-0.dll", "_wgetcwd", L"ucrtbase.dll"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_wchdir", L"ucrtbase.dll"},
        // The executable's own exit tables (g6/imports/crt_exit.h).
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_crt_atexit", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_crt_at_quick_exit", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_register_thread_local_exe_atexit_callback",
         L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "exit", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "quick_exit", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_exit", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_Exit", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_cexit", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_c_exit", L"ucrtbase.dll"},
        // The guest's own command line (g6/imports/command_line.h).
        {"api-ms-win-core-processenvironment-l1-1-0.dll", "GetCommandLineA", L"kernelbase.dll"},
        {"api-ms-win-core-processenvironment-l1-1-0.dll", "GetCommandLineW", L"kernelbase.dll"},
        {"kernel32.dll", "GetCommandLineW", L"kernelbase.dll"},
        {"kernelbase.dll", "GetCommandLineA", L"kernelbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_configure_narrow_argv", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_configure_wide_argv", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "__p___argc", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "__p___argv", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "__p___wargv", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "__p__acmdln", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "__p__wcmdln", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "__p__pgmptr", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "__p__wpgmptr", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_get_pgmptr", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_get_wpgmptr", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_get_narrow_winmain_command_line", L"ucrtbase.dll"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_get_wide_winmain_command_line", L"ucrtbase.dll"},
        // The guest's top-level exception filter runs after the crash report
        // (g6/loader/crash_report.h).
        {"api-ms-win-core-errorhandling-l1-1-0.dll", "SetUnhandledExceptionFilter",
         L"kernelbase.dll"},
        {"kernel32.dll", "SetUnhandledExceptionFilter", L"kernelbase.dll"},
        {"kernelbase.dll", "SetUnhandledExceptionFilter", L"kernelbase.dll"},
    };
    for (const Expected& item : expected) {
        INFO(std::string(item.dll) + "!" + item.name);
        const Resolution resolution = policy.resolve(item.dll, Symbol::by_name(item.name));
        CHECK(resolution.binding == Binding::Hle);
        CHECK(resolution.address != nullptr);
        CHECK(resolution.address != host_function(item.host_dll, item.name));
    }
}

TEST_CASE("the registry reads the GDK thunk makes, and the xmem queries, are HLE") {
    ImportRegistry policy = default_game_policy();
    // The thunk imports these from the API sets in XCurl and from advapi32 in libHttpClient
    // (both never mapped); the game's executable links the same thunk.
    for (const char* dll : {"api-ms-win-core-registry-l1-1-0.dll",
                            "api-ms-win-core-registry-l2-1-0.dll", "advapi32.dll"}) {
        for (const char* name :
             {"RegOpenKeyExW", "RegOpenKeyW", "RegQueryValueExW", "RegCloseKey"}) {
            INFO(std::string(dll) + "!" + name);
            const Resolution resolution = policy.resolve(dll, Symbol::by_name(name));
            CHECK(resolution.binding == Binding::Hle);
            CHECK(resolution.address != nullptr);
            CHECK(resolution.address != host_function(L"advapi32.dll", name));
        }
    }
    for (const char* name :
         {"XMemVirtualQuery", "XMemGetWorkingSetStatistics", "XMemTransferMemory"}) {
        INFO(name);
        const Resolution resolution = policy.resolve("xmem.dll", Symbol::by_name(name));
        CHECK(resolution.binding == Binding::Hle);
        CHECK(resolution.address != nullptr);
    }
}

TEST_CASE("the xmem functions the executable calls first are HLE") {
    ImportRegistry policy = default_game_policy();
    // docs/boot.md, "Our runs": each failure among these is a fatal assert.
    for (const char* name :
         {"XMemVirtualAlloc", "XMemAllocDefault", "XMemFreeDefault", "XMemIsToolsMemAvailable"}) {
        INFO(name);
        const Resolution resolution = policy.resolve("xmem.dll", Symbol::by_name(name));
        CHECK(resolution.binding == Binding::Hle);
        CHECK(resolution.address != nullptr);
    }
}

TEST_CASE("D3D12ConfigureVirtualMemoryX is HLE, before the device") {
    ImportRegistry policy = default_game_policy();
    const Resolution resolution =
        policy.resolve("d3d12_xs.dll", Symbol::by_name("D3D12ConfigureVirtualMemoryX"));
    CHECK(resolution.binding == Binding::Hle);
    CHECK(resolution.address != nullptr);
}

TEST_CASE("the app-state notifications are the host's") {
    // The Game Runtime's start-up registers for them.
    ImportRegistry policy = default_game_policy();
    const Resolution resolution =
        policy.resolve("api-ms-win-core-psm-appnotify-l1-1-0.dll",
                       Symbol::by_name("RegisterAppStateChangeNotification"));
    CHECK(resolution.binding == Binding::Forward);
}

TEST_CASE("GameInputCreate is our stand-in, and the rest of GameInput an HRESULT stub") {
    ImportRegistry policy = default_game_policy();
    // GameInputCreate is ordinal 1, as on the host; the stand-in is ours, never the host's.
    Resolution resolution = policy.resolve("gameinput.dll", Symbol::by_ordinal(1));
    CHECK(resolution.binding == Binding::Hle);
    CHECK(resolution.address != nullptr);
    CHECK(resolution.address != host_function(L"gameinput.dll", "GameInputCreate"));
    // Anything else in it is an HRESULT stub, never a zero that reads as S_OK.
    resolution = policy.resolve("gameinput.dll", Symbol::by_ordinal(2));
    CHECK(resolution.binding == Binding::Stub);
    CHECK_EQ(resolution.stub.value, kNotImplemented);
}

TEST_CASE("DStorageGetFactory is ours, and the rest of xcrdapi an HRESULT stub") {
    ImportRegistry policy = default_game_policy();
    Resolution resolution = policy.resolve("xcrdapi.dll", Symbol::by_name("DStorageGetFactory"));
    CHECK(resolution.binding == Binding::Hle);
    CHECK(resolution.address != nullptr);
    resolution = policy.resolve("XCRDAPI.dll", Symbol::by_name("DStorageGetFactoryX"));
    CHECK(resolution.binding == Binding::Stub);
    CHECK_EQ(resolution.stub.value, kNotImplemented);
}

TEST_CASE("the default policy forwards nothing to a networking DLL") {
    ImportRegistry policy = default_game_policy();
    for (const char* dll : {"ws2_32.dll", "winhttp.dll", "wininet.dll", "xcurl.dll",
                            "libHttpClient.GDK.dll", "api-ms-win-net-isolation-l1-1-0.dll"}) {
        INFO(dll);
        CHECK(policy.dll_binding(dll) == Binding::Offline);
        const Resolution resolution = policy.resolve(dll, Symbol::by_name("connect"));
        CHECK(resolution.binding == Binding::Offline);
        CHECK(resolution.address == nullptr);
    }
}

TEST_CASE("the Game Runtime is never the host's") {
    // A PC with the Gaming Services has its own xgameruntime.dll, which talks to Xbox services;
    // the guest gets runtime/gdk instead (hard rule 5).
    ImportRegistry policy = default_game_policy();
    CHECK(policy.dll_binding("xgameruntime.dll") != Binding::Forward);
    CHECK(!policy.host_provides("xgameruntime.dll"));
    CHECK(policy.knows("xgameruntime.dll"));

    // The entry points xgameruntime.lib looks up are runtime/gdk's (docs/gdk.md, "The Game
    // Runtime DLL").
    const std::pair<const char*, const void*> entry_points[] = {
        {"QueryApiImpl", reinterpret_cast<const void*>(&g6::gdk::QueryApiImpl)},
        {"InitializeApiImpl", reinterpret_cast<const void*>(&g6::gdk::InitializeApiImpl)},
        {"InitializeApiImplEx", reinterpret_cast<const void*>(&g6::gdk::InitializeApiImplEx)},
        {"InitializeApiImplEx2", reinterpret_cast<const void*>(&g6::gdk::InitializeApiImplEx2)},
        {"UninitializeApiImpl", reinterpret_cast<const void*>(&g6::gdk::UninitializeApiImpl)},
        {"XErrorReport", reinterpret_cast<const void*>(&g6::gdk::XErrorReport)},
    };
    for (const auto& [name, function] : entry_points) {
        INFO(name);
        const Resolution resolution = policy.resolve("xgameruntime.dll", Symbol::by_name(name));
        CHECK(resolution.binding == Binding::Hle);
        CHECK(resolution.address == function);
    }

    // Nothing else: GetProcAddress finds no address, and an import gets E_NOTIMPL.
    for (const char* name : {"XGameRuntimeInitialize", "XTaskQueueCreate", "XAsyncRun",
                             "RestartApiImpl", "RundownApiImpl", "GetApiVersionImpl"}) {
        INFO(name);
        const Resolution resolution = policy.resolve("xgameruntime.dll", Symbol::by_name(name));
        CHECK(resolution.binding == Binding::Stub);
        CHECK(resolution.address == nullptr);
        CHECK_EQ(resolution.stub.value, kNotImplemented);
    }
}

TEST_CASE("COM and WinRT activation is never forwarded") {
    ImportRegistry policy = default_game_policy();
    for (const char* dll : {"api-ms-win-core-com-l1-1-0.dll", "combase.dll", "ole32.dll"}) {
        for (const char* name : {"CoCreateInstance", "CoCreateInstanceEx",
                                 "CoCreateInstanceFromApp", "CoGetClassObject"}) {
            INFO(std::string(dll) + "!" + name);
            const Resolution resolution = policy.resolve(dll, Symbol::by_name(name));
            CHECK(resolution.binding == Binding::Hle);
            CHECK(resolution.address != nullptr);
            CHECK(resolution.address != host_function(L"combase.dll", name));
        }
    }
    for (const char* dll : {"api-ms-win-core-winrt-l1-1-0.dll", "combase.dll"}) {
        for (const char* name : {"RoGetActivationFactory", "RoActivateInstance"}) {
            INFO(std::string(dll) + "!" + name);
            const Resolution resolution = policy.resolve(dll, Symbol::by_name(name));
            CHECK(resolution.binding == Binding::Hle);
            CHECK(resolution.address != host_function(L"combase.dll", name));
        }
    }

    // A host class is refused as if it weren't registered, and the out pointer cleared. The
    // CLSID is the global interface table's, registered on every Windows.
    using CreateFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    const auto create = reinterpret_cast<CreateFn>(const_cast<void*>(
        policy.resolve("api-ms-win-core-com-l1-1-0.dll", Symbol::by_name("CoCreateInstance"))
            .address));
    constexpr CLSID kStdGlobalInterfaceTable = {
        0x00000323, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    void* object = reinterpret_cast<void*>(uintptr_t{1});
    CHECK_EQ(create(kStdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, &object),
             REGDB_E_CLASSNOTREG);
    CHECK(object == nullptr);

    using FactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
    const auto get_factory = reinterpret_cast<FactoryFn>(const_cast<void*>(
        policy
            .resolve("api-ms-win-core-winrt-l1-1-0.dll", Symbol::by_name("RoGetActivationFactory"))
            .address));
    constexpr wchar_t kUri[] = L"Windows.Foundation.Uri";
    HSTRING_HEADER header{};
    HSTRING name = nullptr;
    REQUIRE(SUCCEEDED(WindowsCreateStringReference(kUri, ARRAYSIZE(kUri) - 1, &header, &name)));
    void* factory = reinterpret_cast<void*>(uintptr_t{1});
    CHECK_EQ(get_factory(name, IID_IUnknown, &factory), REGDB_E_CLASSNOTREG);
    CHECK(factory == nullptr);
}

TEST_CASE("the side DLLs the policy replaces") {
    const ImportRegistry policy = default_game_policy();
    CHECK(policy.is_replaced("amd_fidelityfx_upscaler_xbox.dll"));
    CHECK(policy.is_replaced("XboxBvhBuilder.dll"));
    for (const char* mapped : {"Bink2Scarlett.dll", "amd_fidelityfx_loader_xbox.dll",
                               "vcruntime140.dll", "msvcp140.dll", "concrt140.dll"}) {
        INFO(mapped);
        CHECK(!policy.is_replaced(mapped));
    }
}

TEST_CASE("what the side DLLs rely on is forwarded or implemented") {
    ImportRegistry policy = default_game_policy();
    struct Expected {
        const char* dll;
        const char* name;
    };
    const Expected forwarded[] = {
        {"api-ms-win-crt-stdio-l1-1-0.dll", "fwrite"},
        {"api-ms-win-crt-filesystem-l1-1-0.dll", "_lock_file"},
        {"api-ms-win-crt-heap-l1-1-0.dll", "_aligned_malloc"},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_beginthreadex"},
        {"api-ms-win-core-synch-l1-2-0.dll", "WaitOnAddress"},
        {"api-ms-win-core-processthreads-l1-1-0.dll", "CreateThread"},
        {"api-ms-win-core-file-l1-1-0.dll", "ReadFile"},
        {"api-ms-win-core-file-l1-1-0.dll", "FindNextFileW"},
        {"api-ms-win-core-libraryloader-l1-2-0.dll", "FindResourceExW"},
        {"api-ms-win-core-com-l1-1-0.dll", "CoTaskMemAlloc"},
        {"api-ms-win-core-winrt-string-l1-1-0.dll", "WindowsCreateString"},
        {"api-ms-win-core-winrt-l1-1-0.dll", "RoInitialize"},
    };
    for (const Expected& entry : forwarded) {
        INFO(std::string(entry.dll) + "!" + entry.name);
        const Resolution resolution = policy.resolve(entry.dll, Symbol::by_name(entry.name));
        CHECK(resolution.binding == Binding::Forward);
        CHECK(resolution.address != nullptr);
    }

    // Console ordinals, bound by name.
    auto resolution = policy.resolve("oleaut32.dll", Symbol::by_ordinal(4));
    CHECK(resolution.binding == Binding::Forward);
    CHECK(resolution.address == host_function(L"oleaut32.dll", "SysAllocStringLen"));
    resolution = policy.resolve("oleaut32.dll", Symbol::by_ordinal(6));
    CHECK(resolution.address == host_function(L"oleaut32.dll", "SysFreeString"));
    resolution = policy.resolve("XAudio2_9.dll", Symbol::by_ordinal(1));
    CHECK(resolution.binding == Binding::Forward);
    CHECK(resolution.address == host_function(L"xaudio2_9.dll", "XAudio2Create"));
    // Other ordinals stay stubs.
    CHECK(policy.resolve("oleaut32.dll", Symbol::by_ordinal(2)).binding == Binding::Stub);

    resolution = policy.resolve("xmem.dll", Symbol::by_name("XMemAlloc"));
    CHECK(resolution.binding == Binding::Hle);
    CHECK(resolution.address == reinterpret_cast<const void*>(&g6::imports::xmem::XMemAlloc));
    CHECK(policy.resolve("xmem.dll", Symbol::by_name("XMemFree")).binding == Binding::Hle);
}
