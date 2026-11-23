// The default import policy (docs/side-dlls.md, "Default import policy"). Each function below
// is one group of rows in that table; docs/hle-status.md lists the result per DLL.
//
// Forwarding is by API set family, never by "api-ms-win-core-*" as a whole, so that a family
// nobody reviewed stays stubbed. Within forwarded families, functions that open host paths,
// start processes, or load host DLLs are stubbed by name.

#include "g6/imports/policy.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <windows.h>

#include "combase/class_activation.h"
#include "g6/gdk/error.h"
#include "g6/gdk/runtime.h"
#include "g6/imports/command_line.h"
#include "g6/imports/crt_exit.h"
#include "g6/imports/crt_paths.h"
#include "g6/imports/d3d12_xs.h"
#include "g6/imports/files.h"
#include "g6/imports/registry.h"
#include "g6/imports/xmem.h"
#include "g6/loader/crash_report.h"

namespace g6::imports {
namespace {

using loader::ImportRegistry;
using loader::StubResult;
using loader::Symbol;

constexpr uint64_t kAllOnes = ~uint64_t{0};
constexpr uint64_t kErrnoNotImplemented = 40; // ENOSYS

// INVALID_HANDLE_VALUE, with ERROR_CALL_NOT_IMPLEMENTED.
StubResult invalid_handle() {
    return {.value = kAllOnes, .last_error = ERROR_CALL_NOT_IMPLEMENTED};
}

// INVALID_FILE_ATTRIBUTES and INVALID_FILE_SIZE, with ERROR_CALL_NOT_IMPLEMENTED.
StubResult invalid_dword() {
    return {.value = 0xFFFFFFFF, .last_error = ERROR_CALL_NOT_IMPLEMENTED};
}

// The CRT reports failure through its return value and errno, which a stub can't set.
StubResult crt_minus_one() {
    return {.value = kAllOnes, .last_error = std::nullopt};
}
StubResult crt_null() {
    return {.value = 0, .last_error = std::nullopt};
}
StubResult crt_errno() {
    return {.value = kErrnoNotImplemented, .last_error = std::nullopt};
}

void stub_each(ImportRegistry& imports, std::string_view dll,
               std::initializer_list<const char*> names, const StubResult& result) {
    for (const char* name : names) {
        imports.stub(dll, Symbol::by_name(name), result);
    }
}

void forward_each(ImportRegistry& imports, std::string_view dll,
                  std::initializer_list<const char*> names) {
    for (const char* name : names) {
        imports.forward(dll, Symbol::by_name(name));
    }
}

// ---- C runtime ----

// The host's Universal CRT has the console's contract; the console's own vcruntime and msvcp
// (guest modules) sit on top of it.
void add_crt(ImportRegistry& imports) {
    constexpr std::string_view kCrt = "api-ms-win-crt-*";
    imports.forward_dll(kCrt);
    // The OneCore (_o_) copies of everything, path and process functions included. The
    // console's runtime doesn't import them.
    imports.stub_dll("api-ms-win-crt-private-*", StubResult::win32());
    // Starting programs and loading DLLs: never.
    imports.stub_dll("api-ms-win-crt-process-*", crt_minus_one());
    stub_each(imports, kCrt, {"_loaddll", "_getdllprocaddr"}, crt_null());
    stub_each(imports, kCrt, {"system", "_wsystem"}, crt_minus_one());

    // The path functions runtime/io maps (docs/io.md). Every other function that takes a path
    // stays a stub until something imports it.
    ucrtbase::register_paths(imports);
    // The executable's exit handlers go to tables of its own, not the host CRT's process-wide
    // ones (docs/execution-model.md, phase 5, "Exit").
    ucrtbase::register_exit(imports);
    // The guest's own command line, for the CRT's argument functions and GetCommandLine
    // (docs/execution-model.md, phase 5, "Command line").
    register_command_line(imports);
    // The guest's top-level exception filter runs after the crash report
    // (docs/execution-model.md, "Crash reports").
    loader::register_crash_report(imports);

    // The rest of the path functions: those that return -1 on failure ...
    stub_each(imports, kCrt,
              {
                  // filesystem
                  "_chdrive",
                  "_chmod",
                  "_wchmod",
                  "_stat32",
                  "_stat32i64",
                  "_stat64i32",
                  "_wstat32",
                  "_wstat32i64",
                  "_wstat64i32",
                  "_findfirst32",
                  "_findfirst32i64",
                  "_findfirst64",
                  "_findfirst64i32",
                  "_wfindfirst32",
                  "_wfindfirst32i64",
                  "_wfindfirst64",
                  "_wfindfirst64i32",
                  // stdio
                  "_creat",
                  "_wcreat",
                  "_open",
                  "_wopen",
                  "_sopen",
                  "_wsopen",
                  "_pipe",
                  "_pclose",
                  // time
                  "_utime32",
                  "_utime64",
                  "_wutime32",
                  "_wutime64",
              },
              crt_minus_one());
    // ... functions that return a pointer, null on failure ...
    stub_each(imports, kCrt,
              {
                  "_fullpath",
                  "_wfullpath",
                  "_getdcwd",
                  "_wgetdcwd",
                  "_searchenv",
                  "_wsearchenv",
                  "freopen",
                  "_wfreopen",
                  "_popen",
                  "_wpopen",
                  "tmpfile",
                  "tmpnam",
                  "_wtmpnam",
                  "_tempnam",
                  "_wtempnam",
                  "_mktemp",
                  "_wmktemp",
              },
              crt_null());
    // ... and functions that return an error number.
    stub_each(imports, kCrt,
              {
                  "_access_s",
                  "_waccess_s",
                  "_getdiskfree",
                  "_searchenv_s",
                  "_wsearchenv_s",
                  "freopen_s",
                  "_wfreopen_s",
                  "tmpfile_s",
                  "tmpnam_s",
                  "_wtmpnam_s",
                  "_mktemp_s",
                  "_wmktemp_s",
                  "_sopen_s",
                  "_wsopen_s",
                  "_sopen_dispatch",
                  "_wsopen_dispatch",
              },
              crt_errno());
}

// ---- Core API sets ----

void add_core(ImportRegistry& imports) {
    // Same contract as on the console. Thread creation, priorities, ideal processors, and
    // affinity become HLE once runtime/kernel models the console's core layout.
    constexpr std::array kForwarded = {
        "api-ms-win-core-console-*",
        "api-ms-win-core-debug-*",
        "api-ms-win-core-errorhandling-*",
        "api-ms-win-core-fibers-*",
        "api-ms-win-core-handle-*",
        "api-ms-win-core-heap-*",
        "api-ms-win-core-interlocked-*",
        "api-ms-win-core-localization-*",
        "api-ms-win-core-processenvironment-*",
        "api-ms-win-core-processthreads-*",
        "api-ms-win-core-processtopology-*",
        "api-ms-win-core-profile-*",
        "api-ms-win-core-psm-appnotify-*",
        "api-ms-win-core-rtlsupport-*",
        "api-ms-win-core-string-*",
        "api-ms-win-core-synch-*",
        "api-ms-win-core-sysinfo-*",
        "api-ms-win-core-threadpool-*",
        "api-ms-win-core-util-*",
    };
    for (const char* family : kForwarded) {
        imports.forward_dll(family);
    }
    // Never other programs or processes.
    stub_each(imports, "api-ms-win-core-processthreads-*",
              {"CreateProcessA", "CreateProcessW", "CreateProcessAsUserA", "CreateProcessAsUserW",
               "OpenProcess"},
              StubResult::win32());
    // Path searches would look in host directories. The current directory is runtime/io's
    // (add_files).
    stub_each(imports, "api-ms-win-core-processenvironment-*",
              {"SearchPathA", "SearchPathW", "NeedCurrentDirectoryForExePathA",
               "NeedCurrentDirectoryForExePathW"},
              StubResult::win32());

    // The loader functions are the Loader's own HLE. Resource lookups work on a guest module's
    // handle, which is a mapped image like any other.
    forward_each(imports, "api-ms-win-core-libraryloader-*",
                 {"FindResourceA", "FindResourceW", "FindResourceExA", "FindResourceExW",
                  "LoadResource", "LockResource", "SizeofResource", "FreeResource"});

    // The registry: the console-mode key the GDK thunk reads is answered (g6/imports/registry.h);
    // every other key, such as the leap-second lookup in msvcp140_atomic_wait, gets nothing. The
    // stubs return their status rather than setting the last error.
    imports.stub_dll("api-ms-win-core-registry-*",
                     {.value = ERROR_CALL_NOT_IMPLEMENTED, .last_error = std::nullopt});
    kernelbase::register_registry(imports);
}

// ---- Files ----

// Functions that take a path are HLE on runtime/io (g6/imports/files.h), or stubs where nothing
// imports them yet. Functions that take a handle are forwarded: the guest can only have a handle
// from a function we allow. The same rules apply when the game imports them from kernel32 or
// kernelbase directly.
void add_files(ImportRegistry& imports) {
    // The rest of the file API sets is stubbed on purpose: zero with ERROR_CALL_NOT_IMPLEMENTED.
    // kernel32 and kernelbase get no such rule, so whatever else is imported from them stays
    // unreviewed and is logged as such.
    imports.stub_dll("api-ms-win-core-file-*", StubResult::win32());
    for (const char* dll : {"api-ms-win-core-file-*", "kernel32.dll", "kernelbase.dll"}) {
        forward_each(imports, dll,
                     {"FindClose",
                      "FindNextFileA",
                      "FindNextFileW",
                      "FlushFileBuffers",
                      "GetFileInformationByHandle",
                      "GetFileInformationByHandleEx",
                      "GetFileSize",
                      "GetFileSizeEx",
                      "GetFileTime",
                      "GetFileType",
                      "LockFile",
                      "LockFileEx",
                      "ReadFile",
                      "ReadFileEx",
                      "SetEndOfFile",
                      "SetFilePointer",
                      "SetFilePointerEx",
                      "SetFileTime",
                      "UnlockFile",
                      "UnlockFileEx",
                      "WriteFile",
                      "WriteFileEx"});
        // The stubs that need a return value of their own, because zero means success for
        // them.
        stub_each(imports, dll,
                  {"CreateFile3", "FindFirstFileNameW", "FindFirstStreamW",
                   "FindFirstChangeNotificationA", "FindFirstChangeNotificationW",
                   "FindFirstVolumeW"},
                  invalid_handle());
        stub_each(imports, dll, {"GetCompressedFileSizeA", "GetCompressedFileSizeW"},
                  invalid_dword());
    }
    kernelbase::register_files(imports);
}

// ---- COM and WinRT ----

// Apartments, marshaling, task memory, strings, and error info stay in-process and are
// forwarded. Class activation is HLE (combase/class_activation.h).
void add_com(ImportRegistry& imports) {
    imports.forward_dll("api-ms-win-core-com-*");
    imports.forward_dll("api-ms-win-core-winrt-*");
    combase::register_activation(imports);
    stub_each(imports, "api-ms-win-core-com-*",
              {"CoGetInstanceFromFile", "CoGetInstanceFromIStorage"}, StubResult::hresult());

    // The console's oleaut32 is imported by ordinal; bind the two used ordinals by name, so
    // nothing depends on the host's ordinals.
    imports.forward("oleaut32.dll", Symbol::by_ordinal(4), "oleaut32.dll", "SysAllocStringLen");
    imports.forward("oleaut32.dll", Symbol::by_ordinal(6), "oleaut32.dll", "SysFreeString");
    forward_each(imports, "oleaut32.dll", {"SysAllocStringLen", "SysFreeString"});
}

// ---- Console DLLs ----

// The Game Runtime is runtime/gdk's, never the host's (hard rule 5): the entry points
// xgameruntime.lib looks up (docs/gdk.md, "The Game Runtime DLL"). Every other export is
// E_NOTIMPL, and GetProcAddress finds nothing for it, as for RestartApiImpl and RundownApiImpl,
// which only the Gaming Services path in XCurl looks up.
void add_game_runtime(ImportRegistry& imports) {
    constexpr std::string_view kRuntime = "xgameruntime.dll";
    imports.stub_dll(kRuntime, StubResult::hresult());
    const auto hle = [&](const char* name, const void* function) {
        imports.hle(kRuntime, Symbol::by_name(name), function);
    };
    hle("QueryApiImpl", reinterpret_cast<const void*>(&gdk::QueryApiImpl));
    hle("InitializeApiImpl", reinterpret_cast<const void*>(&gdk::InitializeApiImpl));
    hle("InitializeApiImplEx", reinterpret_cast<const void*>(&gdk::InitializeApiImplEx));
    hle("InitializeApiImplEx2", reinterpret_cast<const void*>(&gdk::InitializeApiImplEx2));
    hle("UninitializeApiImpl", reinterpret_cast<const void*>(&gdk::UninitializeApiImpl));
    hle("XErrorReport", reinterpret_cast<const void*>(&gdk::XErrorReport));
}

void add_console(ImportRegistry& imports) {
    // Bink imports XAudio2Create by ordinal 1.
    imports.forward("xaudio2_9.dll", Symbol::by_ordinal(1), "xaudio2_9.dll", "XAudio2Create");
    forward_each(imports, "xaudio2_9.dll", {"XAudio2Create"});

    xmem::register_xmem(imports);
    add_game_runtime(imports);

    // D3D12.X, until runtime/gpu wraps it, but for what the executable calls before the device.
    imports.stub_dll("d3d12_xs.dll", StubResult::hresult());
    d3d12_xs::register_d3d12_xs(imports);

    // Never mapped; our own code takes their place at M3 (docs/side-dlls.md, "Replaced DLLs").
    imports.replace_dll("amd_fidelityfx_upscaler_xbox.dll");
    imports.replace_dll("xboxbvhbuilder.dll");
}

} // namespace

loader::ImportRegistry default_game_policy() {
    ImportRegistry imports;
    add_crt(imports);
    add_core(imports);
    add_files(imports);
    add_com(imports);
    add_console(imports);
    // Networking DLLs and API sets are offline in every registry (hard rule 5).
    return imports;
}

} // namespace g6::imports
