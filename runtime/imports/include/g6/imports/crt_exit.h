#pragma once

// The guest executable's exit tables (docs/execution-model.md, phase 5, "Exit"), for imports from
// api-ms-win-crt-*. The guest's CRT is the host's ucrtbase.dll, whose atexit table is
// process-wide: an executable's registrations would land among the host's own, and its exit
// would run the host's handlers too. So the functions that register and run exit handlers work
// on tables of the guest's own, with the UCRT's rules as measured on the host:
//
// * _crt_atexit, which atexit and an executable's static destructors reach, and
//   _crt_at_quick_exit add to the guest's two tables. _register_thread_local_exe_atexit_callback
//   keeps the executable's thread_local destructor callback; a second call ends the process with
//   a fast fail, as the UCRT's does.
// * exit and _cexit call that callback with DLL_PROCESS_DETACH for the calling thread, then run
//   the atexit table newest first. quick_exit runs the at-quick-exit table. _exit, _Exit, and
//   _c_exit run nothing. A handler registered while a table runs runs next, and each handler
//   runs once.
// * exit, quick_exit, _exit, and _Exit then end the guest process; _cexit and _c_exit return.
//
// Every one of them takes the exit lock first. The thread that holds it may call them again from
// a handler; any other thread waits, for good once the process is ending.

#include <cstdint>
#include <windows.h>

namespace g6::loader {
class ImportRegistry;
}

// The names are the CRT's own, as CLAUDE.md asks; they live in this namespace only.
// NOLINTBEGIN(bugprone-reserved-identifier)
namespace g6::imports::ucrtbase {

using AtExitFunction = void(__cdecl*)();

// How the guest process ends: called with the exit code on the thread that ends it, after the
// tables ran. It must not return. The default is ExitProcess, which ends the host process with
// the guest's code, as the UCRT ends its own; the UCRT then flushes the guest's streams.
using EndProcess = void (*)(int code);

// Starts a new guest process: empties the tables, forgets the callback and the exit lock's
// owner, and sets how the process ends (nullptr for ExitProcess). Call it before the
// executable's entry point runs, while no guest code is running the functions below.
void start_guest_process(EndProcess end = nullptr);

// 0 on success, -1 for a null function (the UCRT faults on one) or when memory runs out.
int _crt_atexit(AtExitFunction function);
int _crt_at_quick_exit(AtExitFunction function);
void _register_thread_local_exe_atexit_callback(PIMAGE_TLS_CALLBACK callback);

[[noreturn]] void exit(int code);
[[noreturn]] void quick_exit(int code);
[[noreturn]] void _exit(int code);
[[noreturn]] void _Exit(int code);
void _cexit();
void _c_exit();

// Binds the functions above for the CRT API sets.
void register_exit(loader::ImportRegistry& imports);

} // namespace g6::imports::ucrtbase
// NOLINTEND(bugprone-reserved-identifier)
