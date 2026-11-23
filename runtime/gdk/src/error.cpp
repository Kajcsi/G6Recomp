#include "g6/gdk/error.h"

#include <mutex>

#include "g6/log.h"

// Every report reaches our log first, whatever the title asked for. The options start as None,
// so nothing else happens until the title sets them, and fail-fast is logged but never acted on:
// during bring-up a reported error must not end the game before the log shows why.

namespace g6::gdk {
namespace {

struct State {
    std::mutex mutex;
    XErrorCallback* callback = nullptr;
    void* context = nullptr;
    XErrorOptions debugger_present = XErrorOptions::None;
    XErrorOptions debugger_not_present = XErrorOptions::None;
};

State& state() {
    static State instance;
    return instance;
}

bool has(XErrorOptions options, XErrorOptions flag) {
    return (static_cast<uint32_t>(options) & static_cast<uint32_t>(flag)) != 0;
}

} // namespace

HRESULT XErrorReport(HRESULT result, const char* message) {
    const char* const text = message != nullptr ? message : "";
    LOG_WARN(gdk, "XErrorReport {:#010x}: {}", static_cast<uint32_t>(result), text);

    State& error = state();
    XErrorCallback* callback = nullptr;
    void* context = nullptr;
    XErrorOptions options = XErrorOptions::None;
    {
        const std::scoped_lock lock(error.mutex);
        callback = error.callback;
        context = error.context;
        options =
            IsDebuggerPresent() != FALSE ? error.debugger_present : error.debugger_not_present;
    }
    // The callback runs outside the lock, so it may set another callback or report again.
    if (callback != nullptr && !callback(result, text, context)) {
        return S_OK;
    }
    if (has(options, XErrorOptions::OutputDebugStringOnError)) {
        OutputDebugStringA(text);
        OutputDebugStringA("\n");
    }
    if (has(options, XErrorOptions::DebugBreakOnError) && IsDebuggerPresent() != FALSE) {
        DebugBreak();
    }
    if (has(options, XErrorOptions::FailFastOnError)) {
        LOG_ERROR(gdk, "XErrorReport {:#010x}: the title asks to fail fast; continuing",
                  static_cast<uint32_t>(result));
    }
    return S_OK;
}

void XErrorSetCallback(XErrorCallback* callback, void* context) {
    State& error = state();
    const std::scoped_lock lock(error.mutex);
    error.callback = callback;
    error.context = callback != nullptr ? context : nullptr;
}

void XErrorSetOptions(XErrorOptions debugger_present, XErrorOptions debugger_not_present) {
    State& error = state();
    const std::scoped_lock lock(error.mutex);
    error.debugger_present = debugger_present;
    error.debugger_not_present = debugger_not_present;
}

} // namespace g6::gdk
