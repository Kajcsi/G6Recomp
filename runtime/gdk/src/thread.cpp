#include "g6/gdk/thread.h"

#include "g6/log.h"

namespace g6::gdk {
namespace {

thread_local bool t_time_sensitive = false;
thread_local bool t_reported = false;

} // namespace

HRESULT XThreadSetTimeSensitive(bool time_sensitive) {
    t_time_sensitive = time_sensitive;
    return S_OK;
}

bool XThreadIsTimeSensitive() {
    return t_time_sensitive;
}

void XThreadAssertNotTimeSensitive() {
    if (t_time_sensitive && !t_reported) {
        t_reported = true;
        LOG_WARN(gdk,
                 "XThreadAssertNotTimeSensitive: thread {} is marked time-sensitive and called a "
                 "function that may block",
                 GetCurrentThreadId());
    }
}

} // namespace g6::gdk
