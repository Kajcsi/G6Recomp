#pragma once
// XThread, the Game Runtime's time-sensitive thread marking (docs/gdk.md, "XThread"). A title
// marks the threads that must never block, such as its render thread, and the runtime's
// functions that may block assert that they aren't on one. Signatures are the public GDK's.

#include <windows.h>

namespace g6::gdk {

// Marks or unmarks the calling thread.
HRESULT XThreadSetTimeSensitive(bool time_sensitive);
bool XThreadIsTimeSensitive();
// Logs a call that may block when the calling thread is marked, once per thread. The guest
// reaches this through one Game Runtime thunk, so a caller address would name only the thunk.
// The console reports it as an error; here it never stops the call.
void XThreadAssertNotTimeSensitive();

} // namespace g6::gdk
