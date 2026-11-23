#include "g6/gdk/async.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>

#include "g6/log.h"

// Our own structure; the behavior follows libHttpClient's MIT XAsync (docs/gdk.md, "XAsync").
//
// A call is a Call object. The caller's block points to it while the call is attached, and the
// provider works on a copy of that block inside the Call, whose queue is the call's own reference
// to the queue. The call ends ("detaches") when it completes with no result to fetch, when it
// fails, or when its result is fetched; the blocks then keep only the final status. One global
// lock guards the blocks' state; provider steps, task-queue calls, and caller callbacks all run
// outside it.

namespace g6::gdk {
namespace {

struct Call;

// What XAsyncBlock::internal holds. Tags mark a block in a call, a block whose result was
// fetched, and a provider's copy; zero is a block that was never used or whose call ended.
struct BlockState {
    Call* call;
    HRESULT status;
    uint32_t tag;
};
static_assert(sizeof(BlockState) <= sizeof(XAsyncBlock::internal));

constexpr uint32_t kAttached = 0x6C415847;  // "GXAl"
constexpr uint32_t kRetrieved = 0x72415847; // "GXAr"
constexpr uint32_t kCopy = 0x63415847;      // "GXAc"

// Where the provider's Cleanup step runs: at the call's last release, right after the work step
// that ended it, or nowhere while a Cancel step is under way (cleanup then waits for the release).
enum class CleanupAt : uint32_t { Release, AfterWork, InCancel, Done };

std::mutex g_lock;

struct Call {
    std::atomic<uint32_t> refs{1}; // the first is the caller's block, while attached
    XAsyncBlock* user = nullptr;
    XAsyncBlock copy{};
    XAsyncProviderData data{};
    XAsyncProvider* provider = nullptr;
    const void* identity = nullptr;
    const char* identity_name = nullptr;
    XTaskQueueHandle queue = nullptr;
    std::atomic<bool> scheduled{false};
    std::atomic<CleanupAt> cleanup{CleanupAt::Release};
    std::unique_ptr<std::max_align_t[]> owned_context;

    // Guarded by g_lock, as is data.buffer_size until the result is fetched.
    bool attached = false;
    HRESULT status = E_PENDING;

    // XAsyncGetStatus waits for this: the completion routine has returned.
    std::mutex wait_mutex;
    std::condition_variable wait_done;
    bool finished = false;

    Call() = default;
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    Call(Call&&) = delete;
    Call& operator=(Call&&) = delete;

    void add_ref() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
    // Releases a reference that can't be the last, because the caller holds another.
    void release_held() { refs.fetch_sub(1, std::memory_order_release); }

private:
    ~Call() {
        if (provider != nullptr && cleanup.exchange(CleanupAt::Done) != CleanupAt::Done) {
            provider(XAsyncOp::Cleanup, &data);
        }
        if (queue != nullptr) {
            XTaskQueueCloseHandle(queue);
        }
    }
};

BlockState load(const XAsyncBlock* block) {
    BlockState state{};
    std::memcpy(&state, block->internal, sizeof(state));
    return state;
}

void store(XAsyncBlock* block, const BlockState& state) {
    std::memset(block->internal, 0, sizeof(block->internal));
    std::memcpy(block->internal, &state, sizeof(state));
}

struct View {
    Call* call = nullptr;
    HRESULT status = S_OK;
    bool retrieved = false;
};

// What a block shows: its call's state while attached (the caller's block and the provider's copy
// alike), otherwise the status it kept. g_lock held.
View view_of(const XAsyncBlock* block) {
    const BlockState state = load(block);
    if ((state.tag == kAttached || state.tag == kCopy) && state.call != nullptr) {
        return {.call = state.call, .status = state.call->status};
    }
    if (state.tag == kAttached) {
        return {.status = E_PENDING}; // XAsyncBegin is still setting the call up
    }
    return {.status = state.status, .retrieved = state.tag == kRetrieved};
}

// Takes a reference on the block's call, if it has one, under g_lock.
View hold(const XAsyncBlock* block) {
    const std::scoped_lock lock(g_lock);
    const View view = view_of(block);
    if (view.call != nullptr) {
        view.call->add_ref();
    }
    return view;
}

// Detaches the call from both blocks, which keep the final status. g_lock held. The caller then
// releases the reference the caller's block held.
void detach_locked(Call* call, bool retrieved) {
    call->attached = false;
    store(call->user, {.call = nullptr, .status = call->status, .tag = retrieved ? kRetrieved : 0});
    store(&call->copy, {.call = nullptr, .status = call->status, .tag = kCopy});
}

void cleanup_at(Call* call, CleanupAt where) {
    if (call->cleanup.compare_exchange_strong(where, CleanupAt::Done)) {
        call->provider(XAsyncOp::Cleanup, &call->data);
    }
}

void finish_wait(Call* call) {
    bool first = false;
    {
        const std::scoped_lock lock(call->wait_mutex);
        first = !call->finished;
        call->finished = true;
    }
    call->wait_done.notify_all();
    if (first) {
        XTaskQueueResumeTermination(call->queue);
    }
}

void CALLBACK on_completion(void* context, bool /*canceled*/) {
    // Runs even when the queue is terminating, so the caller can free what the call used.
    auto* call = static_cast<Call*>(context);
    if (call->copy.callback != nullptr) {
        call->copy.callback(call->user);
    }
    finish_wait(call);
    call->release();
}

void signal_completion(Call* call) {
    if (call->copy.callback == nullptr) {
        finish_wait(call);
        return;
    }
    call->add_ref();
    const HRESULT result =
        XTaskQueueSubmitCallback(call->queue, XTaskQueuePort::Completion, call, on_completion);
    if (FAILED(result)) {
        LOG_ERROR(gdk, "XAsync: {} could not queue its completion routine ({:#010x})",
                  call->identity_name != nullptr ? call->identity_name : "a call",
                  static_cast<uint32_t>(result));
        call->release_held();
        finish_wait(call);
    }
}

void CALLBACK on_work(void* context, bool canceled) {
    auto* call = static_cast<Call*>(context); // the reference XAsyncSchedule took
    call->scheduled = false;
    bool attached = false;
    {
        const std::scoped_lock lock(g_lock);
        attached = call->attached;
    }
    if (attached) {
        if (canceled) {
            // The queue is terminating and won't run this call's work again.
            XAsyncCancel(&call->copy);
            if (XAsyncGetStatus(&call->copy, false) != E_ABORT) {
                XAsyncComplete(&call->copy, E_ABORT, 0);
            }
        } else {
            HRESULT result = call->provider(XAsyncOp::DoWork, &call->data);
            // E_PENDING asks for more work. Anything else ends the call; a success should have
            // come through XAsyncComplete, which makes this one a no-op.
            if (result != E_PENDING) {
                if (SUCCEEDED(result)) {
                    result = E_UNEXPECTED;
                }
                XAsyncComplete(&call->copy, result, 0);
            }
        }
        cleanup_at(call, CleanupAt::AfterWork);
    }
    call->release();
}

HRESULT begin(XAsyncBlock* async, void* context, const void* identity, const char* identity_name,
              XAsyncProvider* provider, std::unique_ptr<std::max_align_t[]> owned_context) {
    if (async == nullptr || provider == nullptr) {
        return E_INVALIDARG;
    }
    {
        const std::scoped_lock lock(g_lock);
        if (load(async).tag == kAttached) {
            return E_INVALIDARG;
        }
        store(async, {.call = nullptr, .status = E_PENDING, .tag = kAttached});
    }

    auto* call = new Call;
    call->user = async;
    HRESULT result = S_OK;
    if (async->queue == nullptr) {
        if (!XTaskQueueGetCurrentProcessTaskQueue(&call->queue)) {
            result = kNoTaskQueue;
        }
    } else {
        result = XTaskQueueDuplicateHandle(async->queue, &call->queue);
    }
    if (SUCCEEDED(result)) {
        result = XTaskQueueSuspendTermination(call->queue); // E_ABORT once terminating
    }
    if (FAILED(result)) {
        {
            const std::scoped_lock lock(g_lock);
            store(async, {.call = nullptr, .status = result, .tag = 0});
        }
        call->release();
        return result;
    }

    call->copy = *async;
    call->copy.queue = call->queue;
    store(&call->copy, {.call = call, .status = E_PENDING, .tag = kCopy});
    call->owned_context = std::move(owned_context);
    call->data.async = &call->copy;
    call->data.context = call->owned_context != nullptr ? call->owned_context.get() : context;
    call->provider = provider;
    call->identity = identity;
    call->identity_name = identity_name;
    {
        const std::scoped_lock lock(g_lock);
        call->attached = true;
        store(async, {.call = call, .status = E_PENDING, .tag = kAttached});
    }

    // A failing begin step fails the call, not XAsyncBegin, so the caller still sees it end.
    call->add_ref();
    const HRESULT begun = provider(XAsyncOp::Begin, &call->data);
    if (FAILED(begun)) {
        XAsyncComplete(async, begun, 0);
    }
    call->release();
    return S_OK;
}

HRESULT CALLBACK run_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork: {
        auto* work = reinterpret_cast<XAsyncWork*>(data->context);
        XAsyncComplete(data->async, work(data->async), 0);
        return S_OK;
    }
    case XAsyncOp::GetResult:
    case XAsyncOp::Cancel:
    case XAsyncOp::Cleanup: return S_OK;
    }
    return S_OK;
}

} // namespace

HRESULT XAsyncGetStatus(XAsyncBlock* async, bool wait) {
    if (async == nullptr) {
        return E_INVALIDARG;
    }
    const View view = hold(async);
    HRESULT status = view.status;
    if (wait) {
        if (view.call == nullptr) {
            // Ended already, or never begun.
            return status == E_PENDING ? E_INVALIDARG : status;
        }
        {
            std::unique_lock lock(view.call->wait_mutex);
            view.call->wait_done.wait(lock, [&] { return view.call->finished; });
        }
        const std::scoped_lock lock(g_lock);
        status = view_of(async).status;
    }
    if (view.call != nullptr) {
        view.call->release();
    }
    return status;
}

HRESULT XAsyncGetResultSize(XAsyncBlock* async, size_t* buffer_size) {
    if (async == nullptr || buffer_size == nullptr) {
        return E_INVALIDARG;
    }
    const std::scoped_lock lock(g_lock);
    const View view = view_of(async);
    *buffer_size = view.call != nullptr ? view.call->data.buffer_size : 0;
    return view.status;
}

void XAsyncCancel(XAsyncBlock* async) {
    if (async == nullptr) {
        return;
    }
    const View view = hold(async);
    Call* call = view.call;
    if (call == nullptr) {
        return;
    }
    // Only one Cancel step at a time, and none once cleanup is due or done.
    CleanupAt expected = CleanupAt::Release;
    if (call->cleanup.compare_exchange_strong(expected, CleanupAt::InCancel)) {
        call->provider(XAsyncOp::Cancel, &call->data);
        expected = CleanupAt::InCancel;
        call->cleanup.compare_exchange_strong(expected, CleanupAt::Release);
    }
    call->release();
}

HRESULT XAsyncRun(XAsyncBlock* async, XAsyncWork* work) {
    return XAsyncBegin(async, reinterpret_cast<void*>(work),
                       reinterpret_cast<const void*>(&XAsyncRun), "XAsyncRun", run_provider);
}

HRESULT XAsyncBegin(XAsyncBlock* async, void* context, const void* identity,
                    const char* identity_name, XAsyncProvider* provider) {
    return begin(async, context, identity, identity_name, provider, nullptr);
}

HRESULT XAsyncBeginAlloc(XAsyncBlock* async, const void* identity, const char* identity_name,
                         XAsyncProvider* provider, size_t context_size, size_t parameter_block_size,
                         void* parameter_block) {
    if (context_size == 0) {
        return E_INVALIDARG;
    }
    if (parameter_block_size != 0
            ? parameter_block == nullptr || parameter_block_size > context_size
            : parameter_block != nullptr) {
        return E_INVALIDARG;
    }
    constexpr size_t kUnit = sizeof(std::max_align_t);
    auto owned = std::make_unique<std::max_align_t[]>((context_size + kUnit - 1) / kUnit);
    if (parameter_block_size != 0) {
        std::memcpy(owned.get(), parameter_block, parameter_block_size);
    }
    return begin(async, nullptr, identity, identity_name, provider, std::move(owned));
}

HRESULT XAsyncSchedule(XAsyncBlock* async, uint32_t delay_ms) {
    if (async == nullptr) {
        return E_INVALIDARG;
    }
    const View view = hold(async);
    Call* call = view.call;
    HRESULT result = S_OK;
    if (FAILED(view.status) && view.status != E_PENDING) {
        result = view.status; // the call failed already
    } else if (call == nullptr) {
        result = E_INVALIDARG; // the call is over
    } else if (call->scheduled.exchange(true)) {
        result = E_UNEXPECTED; // its work is queued already
    } else {
        // The work callback takes over this reference.
        result = XTaskQueueSubmitDelayedCallback(call->queue, XTaskQueuePort::Work, delay_ms, call,
                                                 on_work);
        if (SUCCEEDED(result)) {
            return S_OK;
        }
        call->scheduled = false;
    }
    if (call != nullptr) {
        call->release();
    }
    return result;
}

void XAsyncComplete(XAsyncBlock* async, HRESULT result, size_t required_buffer_size) {
    if (async == nullptr || result == E_PENDING) {
        return;
    }
    Call* call = nullptr;
    bool ended = false;
    {
        const std::scoped_lock lock(g_lock);
        call = view_of(async).call;
        if (call == nullptr || call->status != E_PENDING) {
            return; // only the first completion counts
        }
        call->status = result;
        // No result to fetch: the call ends now. A failure has none, whatever size it names.
        if (required_buffer_size == 0 || FAILED(result)) {
            required_buffer_size = 0;
            detach_locked(call, false);
            ended = true;
        }
        call->data.buffer_size = required_buffer_size;
        call->add_ref();
    }
    signal_completion(call);
    if (ended) {
        // Clean the provider up as soon as its work step returns, or at the last release.
        CleanupAt expected = CleanupAt::Release;
        call->cleanup.compare_exchange_strong(expected, CleanupAt::AfterWork);
        call->release_held(); // the caller's block's reference
    }
    call->release();
}

HRESULT XAsyncGetResult(XAsyncBlock* async, const void* identity, size_t buffer_size, void* buffer,
                        size_t* buffer_used) {
    if (async == nullptr) {
        return E_INVALIDARG;
    }
    const View view = hold(async);
    Call* call = view.call;
    HRESULT result = view.status;
    if (SUCCEEDED(result)) {
        if (view.retrieved) {
            return E_ILLEGAL_METHOD_CALL; // fetched once already; the call is gone
        }
        if (call == nullptr) {
            if (buffer_used != nullptr) {
                *buffer_used = 0; // a call with no result
            }
        } else if (identity != call->identity) {
            LOG_WARN(gdk, "XAsyncGetResult: the block was begun by {}, not this function",
                     call->identity_name != nullptr ? call->identity_name : "another provider");
            result = E_INVALIDARG;
        } else if (call->data.buffer_size == 0) {
            result = kNotSupported;
        } else if (buffer == nullptr) {
            result = E_INVALIDARG;
        } else if (buffer_size < call->data.buffer_size) {
            result = E_NOT_SUFFICIENT_BUFFER;
        } else {
            if (buffer_used != nullptr) {
                *buffer_used = call->data.buffer_size;
            }
            call->data.buffer_size = buffer_size;
            call->data.buffer = buffer;
            result = call->provider(XAsyncOp::GetResult, &call->data);
        }
    }
    // Any answer but E_PENDING ends the call, failures included: the result is offered once.
    if (call != nullptr && result != E_PENDING) {
        bool ended = false;
        {
            const std::scoped_lock lock(g_lock);
            if (call->attached) {
                detach_locked(call, true);
                ended = true;
            }
        }
        if (ended) {
            call->release_held(); // the caller's block's reference
        }
    }
    if (call != nullptr) {
        call->release();
    }
    return result;
}

} // namespace g6::gdk
