#pragma once

// XAsync, the Game Runtime's asynchronous call model (docs/gdk.md, "XAsync"). The caller owns an
// XAsyncBlock for the life of a call; a provider does the work through the XAsyncOp steps and runs
// its work steps on the block's task queue. Types and signatures are the public GDK's, and the
// behavior follows libHttpClient's open-source (MIT) implementation; no code is taken from it.

#include <cstddef>
#include <cstdint>
#include <windows.h>

#include "g6/gdk/task_queue.h"

namespace g6::gdk {

struct XAsyncBlock;

using XAsyncCompletionRoutine = void CALLBACK(XAsyncBlock* async);
using XAsyncWork = HRESULT CALLBACK(XAsyncBlock* async);

struct XAsyncBlock {
    // The queue the call's work and completion run on; null means the process queue.
    XTaskQueueHandle queue;
    void* context;
    // Runs on the completion port once the call is over. Optional: without it, poll or wait with
    // XAsyncGetStatus.
    XAsyncCompletionRoutine* callback;
    // The call's state while it runs, and its status after. Zero it before the first call.
    unsigned char internal[sizeof(void*) * 4];
};
static_assert(sizeof(XAsyncBlock) == 56);

enum class XAsyncOp : uint32_t {
    Begin,
    DoWork,
    GetResult,
    Cancel,
    Cleanup,
};

struct XAsyncProviderData {
    // The provider's view of the call: a copy of the caller's block that the call owns.
    XAsyncBlock* async;
    // At GetResult, the size of `buffer`; before that, the size the result needs.
    size_t buffer_size;
    void* buffer;
    void* context;
};

using XAsyncProvider = HRESULT CALLBACK(XAsyncOp op, const XAsyncProviderData* data);

// Errors from winerror.h that the SDK headers here don't define.
inline constexpr HRESULT kNotSupported = static_cast<HRESULT>(0x80070032); // ERROR_NOT_SUPPORTED

// The call's status: E_PENDING while it runs, then its result. With `wait`, returns only once the
// call is over and its completion routine has returned.
HRESULT XAsyncGetStatus(XAsyncBlock* async, bool wait);
// The status, and the size XAsyncGetResult needs (0 while pending, after a failure, or when there
// is no result to fetch).
HRESULT XAsyncGetResultSize(XAsyncBlock* async, size_t* buffer_size);
// Asks the provider to cancel. A provider that can't runs to its end.
void XAsyncCancel(XAsyncBlock* async);
// Runs `work` on the queue's work port; its return value becomes the call's result.
HRESULT XAsyncRun(XAsyncBlock* async, XAsyncWork* work);

// For providers. XAsyncBegin starts a call: E_INVALIDARG when the block is already in a call,
// kNoTaskQueue when there is no queue to use, and E_ABORT when the queue is terminating. A begin
// step that fails ends the call with that failure while XAsyncBegin returns S_OK.
HRESULT XAsyncBegin(XAsyncBlock* async, void* context, const void* identity,
                    const char* identity_name, XAsyncProvider* provider);
// XAsyncBegin with `context_size` zeroed bytes as the provider's context, owned by the call, and
// the first `parameter_block_size` of them copied from `parameter_block`. Private to the Game
// Runtime in libHttpClient's design; no known vtable slot reaches it yet (docs/gdk.md, "Slots").
HRESULT XAsyncBeginAlloc(XAsyncBlock* async, const void* identity, const char* identity_name,
                         XAsyncProvider* provider, size_t context_size, size_t parameter_block_size,
                         void* parameter_block);
// Queues the provider's work step on the work port after `delay_ms`. Once per call at a time: a
// second before the first has run fails with E_UNEXPECTED.
HRESULT XAsyncSchedule(XAsyncBlock* async, uint32_t delay_ms);
// Ends the call with `result`, and a result of `required_buffer_size` bytes for XAsyncGetResult
// (none for a failure). E_PENDING ends nothing. Only the first call counts.
void XAsyncComplete(XAsyncBlock* async, HRESULT result, size_t required_buffer_size);
// Fetches the result through the provider's GetResult step, which ends the call; a call with no
// result gives S_OK and 0 bytes as often as asked. Fetching a result a second time fails with
// E_ILLEGAL_METHOD_CALL.
HRESULT XAsyncGetResult(XAsyncBlock* async, const void* identity, size_t buffer_size, void* buffer,
                        size_t* buffer_used);

} // namespace g6::gdk
