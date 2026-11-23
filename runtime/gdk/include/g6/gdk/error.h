#pragma once
// XError, the Game Runtime's error reporting (docs/gdk.md, "XError"). A title registers a callback
// for the errors the runtime reports, and chooses what else happens to each one, with a debugger
// attached or not. Types and signatures are the public GDK's.

#include <cstdint>
#include <windows.h>

namespace g6::gdk {

enum class XErrorOptions : uint32_t {
    None = 0x0,
    OutputDebugStringOnError = 0x1,
    DebugBreakOnError = 0x2,
    FailFastOnError = 0x4,
};

// Returns whether the report goes on to the options' handling.
using XErrorCallback = bool CALLBACK(HRESULT result, const char* message, void* context);

// One report: always to our log, then to the title's callback, then the options that apply.
// `message` may be null.
HRESULT XErrorReport(HRESULT result, const char* message);
// Null removes the callback.
void XErrorSetCallback(XErrorCallback* callback, void* context);
void XErrorSetOptions(XErrorOptions debugger_present, XErrorOptions debugger_not_present);

} // namespace g6::gdk
