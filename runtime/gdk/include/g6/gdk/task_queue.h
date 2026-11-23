#pragma once

// XTaskQueue, the Game Runtime's callback queue (docs/gdk.md, "XTaskQueue"). The types and
// signatures are the public GDK's, so a guest that calls these through the Game Runtime gets the
// layout it was built against. The behavior follows libHttpClient's open-source (MIT) task queue,
// which the GDK's own documentation points to; no code is taken from it.
//
// A queue has two ports, Work and Completion. Each port has a dispatch mode that decides where
// its callbacks run: on the thread that calls XTaskQueueDispatch (Manual), on host thread-pool
// threads (ThreadPool, or one at a time with SerializedThreadPool), or on the submitting thread
// before the submit returns (Immediate). A composite queue shares ports with other queues.

#include <cstdint>
#include <windows.h>

namespace g6::gdk {

struct XTaskQueueObject;
struct XTaskQueuePortObject;
using XTaskQueueHandle = XTaskQueueObject*;
using XTaskQueuePortHandle = XTaskQueuePortObject*;

enum class XTaskQueueDispatchMode : uint32_t {
    Manual,
    ThreadPool,
    SerializedThreadPool,
    Immediate,
};

enum class XTaskQueuePort : uint32_t {
    Work,
    Completion,
};

struct XTaskQueueRegistrationToken {
    uint64_t token;
};

// `canceled` is true when the queue was terminating or terminated as the callback came due.
using XTaskQueueCallback = void CALLBACK(void* context, bool canceled);
using XTaskQueueMonitorCallback = void CALLBACK(void* context, XTaskQueueHandle queue,
                                                XTaskQueuePort port);
using XTaskQueueTerminatedCallback = void CALLBACK(void* context);

// Errors from XGameErr.h and winerror.h that the SDK headers here don't all define.
inline constexpr HRESULT kInvalidHandle = static_cast<HRESULT>(0x8924010C);
inline constexpr HRESULT kNoTaskQueue = static_cast<HRESULT>(0x800701AB); // ERROR_NO_TASK_QUEUE

HRESULT XTaskQueueCreate(XTaskQueueDispatchMode work_mode, XTaskQueueDispatchMode completion_mode,
                         XTaskQueueHandle* queue);
// A queue whose ports are the given ports of other queues. Callbacks submitted to it run where
// those ports run theirs, and terminating it cancels only its own callbacks.
HRESULT XTaskQueueCreateComposite(XTaskQueuePortHandle work_port,
                                  XTaskQueuePortHandle completion_port, XTaskQueueHandle* queue);
// The port's handle, owned by the queue: it is never closed.
HRESULT XTaskQueueGetPort(XTaskQueueHandle queue, XTaskQueuePort port,
                          XTaskQueuePortHandle* port_handle);
// Another reference to the queue. Each handle is closed once.
HRESULT XTaskQueueDuplicateHandle(XTaskQueueHandle queue, XTaskQueueHandle* duplicate);
void XTaskQueueCloseHandle(XTaskQueueHandle queue);

// Runs at most one callback from the port on the calling thread, waiting up to `timeout_ms` for
// one (INFINITE waits until one arrives or the queue is terminated). Returns whether one ran. A
// port's pending termination also runs here once nothing else is waiting.
bool XTaskQueueDispatch(XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeout_ms);
// Whether the port has nothing waiting, nothing delayed, and nothing running. Private to the
// Game Runtime; kept for its vtable slot.
bool XTaskQueueIsEmpty(XTaskQueueHandle queue, XTaskQueuePort port);

// Terminates the queue: later submits fail with E_ABORT, callbacks still waiting (delayed ones
// and registered waiters included) run as canceled, then the work port finishes, then the
// completion port, and then `callback` runs on the completion port. With `wait` the call returns
// only after that. A queue with a Manual port needs that port dispatched for termination to end.
HRESULT XTaskQueueTerminate(XTaskQueueHandle queue, bool wait, void* context,
                            XTaskQueueTerminatedCallback* callback);

HRESULT XTaskQueueSubmitCallback(XTaskQueueHandle queue, XTaskQueuePort port, void* context,
                                 XTaskQueueCallback* callback);
HRESULT XTaskQueueSubmitDelayedCallback(XTaskQueueHandle queue, XTaskQueuePort port,
                                        uint32_t delay_ms, void* context,
                                        XTaskQueueCallback* callback);

// Queues `callback` each time `wait_handle` is signaled, until unregistered. The queue keeps its
// own duplicate of the handle, so the caller may close theirs.
HRESULT XTaskQueueRegisterWaiter(XTaskQueueHandle queue, XTaskQueuePort port, HANDLE wait_handle,
                                 void* context, XTaskQueueCallback* callback,
                                 XTaskQueueRegistrationToken* token);
void XTaskQueueUnregisterWaiter(XTaskQueueHandle queue, XTaskQueueRegistrationToken token);

// Calls `callback` on the submitting thread whenever a callback is queued on a port this queue
// uses, before it can run. After XTaskQueueUnregisterMonitor returns, the monitor isn't called
// again, unless it is unregistering itself from inside its own call.
HRESULT XTaskQueueRegisterMonitor(XTaskQueueHandle queue, void* context,
                                  XTaskQueueMonitorCallback* callback,
                                  XTaskQueueRegistrationToken* token);
void XTaskQueueUnregisterMonitor(XTaskQueueHandle queue, XTaskQueueRegistrationToken token);

// Holds off the end of the queue's termination while work it doesn't track is outstanding, as
// XAsync does for each call. Fails with E_ABORT once the queue is terminating.
HRESULT XTaskQueueSuspendTermination(XTaskQueueHandle queue);
void XTaskQueueResumeTermination(XTaskQueueHandle queue);

// The process queue, which XAsync uses for a block with no queue. By default it is a queue with
// two ThreadPool ports, made on first use; after XTaskQueueSetCurrentProcessTaskQueue(nullptr)
// there is none and the getter returns false. The handle returned is a new reference.
bool XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue);
void XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue);

} // namespace g6::gdk
