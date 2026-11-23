#pragma once

// Crash reports for guest code (docs/execution-model.md, "Crash reports"): where an address lies,
// a walk of the stack through guest and host frames, and the report the runtime logs for an
// exception no frame handled.
//
// The walk unwinds with RtlLookupFunctionEntry and RtlVirtualUnwind, which see the function
// tables the loader registers for guest modules. RtlCaptureStackBackTrace doesn't: it stops at
// the first frame in a module the Windows loader didn't load.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace g6::loader {

// Where an address lies.
struct CodeAddress {
    uint64_t address = 0;
    // The file name of the module that holds the address, or empty when none does.
    std::string module;
    // From the module's base.
    uint64_t offset = 0;
    // A module the active Loader mapped. For those, `preferred` is the address at the module's
    // preferred base, which is what the analysis tools and REA show; 0 otherwise.
    bool guest = false;
    uint64_t preferred = 0;
};

CodeAddress locate_code(uint64_t address);

// "game.exe+0x1234 [guest 0x140001234]" for a guest module, "ntdll.dll+0x5678" for a host one,
// and the bare address when no module holds it.
std::string describe_code(const CodeAddress& where);

// Walks the calling thread's stack from `context`, newest frame first, at most `max_frames`
// frames. A function without unwind data is a leaf, whose return address is at the stack
// pointer. The walk stops at a null return address, at a stack pointer that doesn't grow or
// leaves the thread's stack, and at unwind data that can't be followed.
std::vector<CodeAddress> walk_stack(const CONTEXT& context, size_t max_frames = 64);

// What the runtime logs for an exception: its code and name, where it happened, what an access
// violation touched, the type of a C++ exception, the registers, and the stack. The exception
// must have happened on the calling thread.
std::string crash_report(const EXCEPTION_POINTERS& exception);

// Installs a top-level exception filter that logs crash_report at error level for an exception
// no frame handled, then hands the exception to the filter that was there before, so the
// process ends, or a debugger gets it, as it would have. A stack overflow is reported from a
// thread of its own, since little of the faulting thread's stack is left. Installing it again
// does nothing.
void install_crash_report();

} // namespace g6::loader
