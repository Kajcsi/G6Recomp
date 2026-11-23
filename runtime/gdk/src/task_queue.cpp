#include "g6/gdk/task_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// The handle types are opaque in the header; here they are the bases of Queue and Port, so a
// handle is the object's own address and its signature tells a live one from anything else.
namespace g6::gdk {
struct XTaskQueueObject {
    uint32_t signature = 0;
};
struct XTaskQueuePortObject {
    uint32_t signature = 0;
};
} // namespace g6::gdk

namespace g6::gdk {
namespace {

constexpr uint32_t kQueueSignature = 0x31555154; // "TQU1"
constexpr uint32_t kPortSignature = 0x31505154;  // "TQP1"
constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

HRESULT last_error() {
    return HRESULT_FROM_WIN32(GetLastError());
}

class Port;
class Queue;
struct Waiter;

// A queue's view of one port. Callbacks run as canceled once it leaves Active, and submits fail
// from Terminating on.
enum class SlotState : uint32_t { Active, Canceled, Terminating, Terminated };

struct Termination;

struct Slot {
    Queue* queue = nullptr;
    Port* port = nullptr;
    XTaskQueuePort type = XTaskQueuePort::Work;
    std::atomic<SlotState> state{SlotState::Active};
    // Guarded by the port's mutex: work that holds the termination off (XAsync calls), and the
    // termination waiting for it to end.
    uint32_t suspends = 0;
    std::vector<std::shared_ptr<Termination>> parked;
};

// One XTaskQueueTerminate. It ends the work slot, then the completion slot, then calls back.
struct Termination {
    Queue* queue = nullptr; // a reference, released at the end
    XTaskQueuePort stage = XTaskQueuePort::Work;
    void* context = nullptr;
    XTaskQueueTerminatedCallback* callback = nullptr;
    std::mutex mutex;
    std::condition_variable done_changed;
    bool done = false;
};

// A callback waiting to run. It holds a reference on its slot's queue, and a waiter's callback
// holds one on the waiter.
struct Entry {
    Slot* slot = nullptr;
    XTaskQueueCallback* callback = nullptr;
    void* context = nullptr;
    Waiter* waiter = nullptr;
};

enum class WaiterState : uint32_t { Registered, Canceled, Removed };

// A registered wait handle. The registration holds one reference and a reference on the queue.
struct Waiter {
    std::atomic<uint32_t> refs{1};
    Slot* slot = nullptr;
    HANDLE handle = nullptr; // our duplicate
    PTP_WAIT wait = nullptr;
    XTaskQueueCallback* callback = nullptr;
    void* context = nullptr;
    // Guarded by the port's mutex.
    WaiterState state = WaiterState::Registered;
    bool queued = false;

    Waiter() = default;
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    Waiter(Waiter&&) = delete;
    Waiter& operator=(Waiter&&) = delete;

    void add_ref() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

private:
    ~Waiter() {
        if (wait != nullptr) {
            CloseThreadpoolWait(wait);
        }
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
};

// Callbacks running on this thread, so a teardown started from inside one doesn't wait for
// itself to return.
thread_local const Port* t_timer_port = nullptr;
thread_local const Waiter* t_waiter = nullptr;

std::atomic<uint64_t> g_next_waiter_token{0};

// A dispatch engine: callbacks that are ready, callbacks waiting for their time, wait
// registrations, and terminations waiting for the port to go idle. Queues share it through slots.
class Port : public XTaskQueuePortObject {
public:
    explicit Port(XTaskQueueDispatchMode mode) : mode_(mode) { signature = kPortSignature; }
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;
    Port(Port&&) = delete;
    Port& operator=(Port&&) = delete;

    HRESULT init();

    void add_ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    bool try_add_ref() {
        uint32_t refs = refs_.load(std::memory_order_relaxed);
        while (refs != 0) {
            if (refs_.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
    void release() {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    void attach(Slot* slot);
    void detach(Slot* slot);

    HRESULT submit(Slot& slot, uint32_t delay_ms, void* context, XTaskQueueCallback* callback);
    HRESULT register_waiter(Slot& slot, HANDLE handle, void* context, XTaskQueueCallback* callback,
                            uint64_t* token);
    bool unregister_waiter(const Queue* queue, uint64_t token);
    HRESULT suspend_termination(Slot& slot);
    void resume_termination(Slot& slot);
    void terminate(Slot& slot, std::shared_ptr<Termination> termination);
    bool dispatch(Slot& slot, uint32_t timeout_ms);
    bool empty();

private:
    ~Port();

    static void CALLBACK on_work(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK);
    static void CALLBACK on_timer(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER);
    static void CALLBACK on_wait(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT);

    void work_item();
    void timer_fired();
    void waiter_signaled(Waiter* waiter);
    bool drain_one();
    void run_terminations();
    void notify(size_t items);
    void arm_timer_locked();
    void finish_waiter(Waiter* waiter);
    // Moves the slot's parked terminations to the port once nothing holds them off; returns how
    // many moved. Port mutex held.
    size_t release_parked_locked(Slot& slot);
    static void stop_waiter(Waiter* waiter);
    static void advance(const std::shared_ptr<Termination>& termination);

    const XTaskQueueDispatchMode mode_;
    std::atomic<uint32_t> refs_{1};
    PTP_WORK work_ = nullptr;
    PTP_TIMER timer_ = nullptr;
    std::atomic<uint32_t> serial_owners_{0}; // SerializedThreadPool: workers counted for the owner

    std::mutex mutex_;
    std::condition_variable ready_changed_;
    std::deque<Entry> ready_;
    std::multimap<uint64_t, Entry> delayed_; // by due time; equal times keep their order
    uint64_t timer_due_ = kNever;
    std::map<uint64_t, Waiter*> waiters_;
    std::vector<std::shared_ptr<Termination>> terminations_;
    std::vector<Slot*> attached_;
    uint32_t running_ = 0;
};

struct Monitor {
    uint64_t token = 0;
    void* context = nullptr;
    XTaskQueueMonitorCallback* callback = nullptr;
    // Guarded by the queue's monitor mutex.
    bool removed = false;
    uint32_t busy = 0; // calls under way
};

thread_local std::vector<const Monitor*> t_monitor_calls;

class Queue : public XTaskQueueObject {
public:
    Queue() { signature = kQueueSignature; }
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;

    // Takes a reference on each port.
    void init(Port* work_port, Port* completion_port) {
        work.queue = this;
        work.type = XTaskQueuePort::Work;
        work.port = work_port;
        completion.queue = this;
        completion.type = XTaskQueuePort::Completion;
        completion.port = completion_port;
        work_port->add_ref();
        completion_port->add_ref();
        work_port->attach(&work);
        completion_port->attach(&completion);
    }

    Slot* slot(XTaskQueuePort port) {
        switch (port) {
        case XTaskQueuePort::Work: return &work;
        case XTaskQueuePort::Completion: return &completion;
        }
        return nullptr;
    }

    void add_ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    bool try_add_ref() {
        uint32_t refs = refs_.load(std::memory_order_relaxed);
        while (refs != 0) {
            if (refs_.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
    void release() {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    bool has_monitors() const { return monitor_count_.load(std::memory_order_acquire) != 0; }

    HRESULT register_monitor(void* context, XTaskQueueMonitorCallback* callback,
                             XTaskQueueRegistrationToken* token) {
        if (callback == nullptr || token == nullptr) {
            return E_POINTER;
        }
        auto monitor = std::make_shared<Monitor>();
        monitor->context = context;
        monitor->callback = callback;
        const std::scoped_lock lock(monitor_mutex_);
        monitor->token = ++next_monitor_token_;
        monitors_.push_back(std::move(monitor));
        monitor_count_.fetch_add(1, std::memory_order_release);
        token->token = monitors_.back()->token;
        return S_OK;
    }

    void unregister_monitor(uint64_t token) {
        std::unique_lock lock(monitor_mutex_);
        const auto it = std::ranges::find_if(
            monitors_, [token](const auto& monitor) { return monitor->token == token; });
        if (it == monitors_.end()) {
            return;
        }
        const std::shared_ptr<Monitor> monitor = std::move(*it);
        monitors_.erase(it);
        monitor_count_.fetch_sub(1, std::memory_order_release);
        monitor->removed = true;
        // Wait out calls already under way, except this thread's own.
        const auto own = static_cast<uint32_t>(std::ranges::count(t_monitor_calls, monitor.get()));
        monitor_done_.wait(lock, [&] { return monitor->busy == own; });
    }

    void call_monitors(XTaskQueuePort port) {
        std::vector<std::shared_ptr<Monitor>> monitors;
        {
            const std::scoped_lock lock(monitor_mutex_);
            monitors = monitors_;
        }
        // Each call counts as busy only while it runs, so a monitor that unregisters another
        // waits only for calls under way on other threads.
        for (const auto& monitor : monitors) {
            {
                const std::scoped_lock lock(monitor_mutex_);
                if (monitor->removed) {
                    continue;
                }
                ++monitor->busy;
            }
            t_monitor_calls.push_back(monitor.get());
            monitor->callback(monitor->context, this, port);
            t_monitor_calls.pop_back();
            {
                const std::scoped_lock lock(monitor_mutex_);
                --monitor->busy;
            }
            monitor_done_.notify_all();
        }
    }

    Slot work;
    Slot completion;

private:
    ~Queue() {
        signature = 0;
        work.port->detach(&work);
        completion.port->detach(&completion);
        work.port->release();
        completion.port->release();
    }

    std::atomic<uint32_t> refs_{1};
    std::mutex monitor_mutex_;
    std::condition_variable monitor_done_;
    std::vector<std::shared_ptr<Monitor>> monitors_;
    uint64_t next_monitor_token_ = 0;
    std::atomic<uint32_t> monitor_count_{0};
};

Slot& termination_slot(const Termination& termination) {
    return termination.stage == XTaskQueuePort::Work ? termination.queue->work
                                                     : termination.queue->completion;
}

// ---- Port ----

HRESULT Port::init() {
    if (mode_ == XTaskQueueDispatchMode::ThreadPool ||
        mode_ == XTaskQueueDispatchMode::SerializedThreadPool) {
        work_ = CreateThreadpoolWork(on_work, this, nullptr);
        if (work_ == nullptr) {
            return last_error();
        }
    }
    timer_ = CreateThreadpoolTimer(on_timer, this, nullptr);
    if (timer_ == nullptr) {
        return last_error();
    }
    return S_OK;
}

Port::~Port() {
    signature = 0;
    if (timer_ != nullptr) {
        SetThreadpoolTimer(timer_, nullptr, 0, 0);
        if (t_timer_port != this) {
            WaitForThreadpoolTimerCallbacks(timer_, TRUE);
        }
        CloseThreadpoolTimer(timer_);
    }
    // Every submitted work item holds a reference, so none is outstanding unless this is its own
    // last release, and closing from inside the callback is allowed.
    if (work_ != nullptr) {
        CloseThreadpoolWork(work_);
    }
}

void Port::attach(Slot* slot) {
    const std::scoped_lock lock(mutex_);
    attached_.push_back(slot);
}

void Port::detach(Slot* slot) {
    const std::scoped_lock lock(mutex_);
    std::erase(attached_, slot);
}

HRESULT Port::submit(Slot& slot, uint32_t delay_ms, void* context, XTaskQueueCallback* callback) {
    if (callback == nullptr) {
        return E_INVALIDARG;
    }
    {
        const std::scoped_lock lock(mutex_);
        if (slot.state.load() >= SlotState::Terminating) {
            return E_ABORT;
        }
        slot.queue->add_ref();
        const Entry entry{.slot = &slot, .callback = callback, .context = context};
        if (delay_ms != 0) {
            delayed_.emplace(now_ms() + delay_ms, entry);
            arm_timer_locked();
            return S_OK;
        }
        ready_.push_back(entry);
    }
    notify(1);
    return S_OK;
}

HRESULT Port::register_waiter(Slot& slot, HANDLE handle, void* context,
                              XTaskQueueCallback* callback, uint64_t* token) {
    if (slot.state.load() >= SlotState::Terminating) {
        return E_ABORT;
    }
    auto* waiter = new Waiter;
    waiter->slot = &slot;
    waiter->callback = callback;
    waiter->context = context;
    HANDLE process = GetCurrentProcess();
    if (DuplicateHandle(process, handle, process, &waiter->handle, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == 0) {
        const HRESULT error = last_error();
        waiter->release();
        return error;
    }
    waiter->wait = CreateThreadpoolWait(on_wait, waiter, nullptr);
    if (waiter->wait == nullptr) {
        const HRESULT error = last_error();
        waiter->release();
        return error;
    }
    const std::scoped_lock lock(mutex_);
    if (slot.state.load() >= SlotState::Terminating) {
        waiter->release();
        return E_ABORT;
    }
    *token = ++g_next_waiter_token;
    waiters_.emplace(*token, waiter);
    slot.queue->add_ref();
    SetThreadpoolWait(waiter->wait, waiter->handle, nullptr);
    return S_OK;
}

bool Port::unregister_waiter(const Queue* queue, uint64_t token) {
    Waiter* waiter = nullptr;
    {
        const std::scoped_lock lock(mutex_);
        const auto it = waiters_.find(token);
        if (it == waiters_.end() || it->second->slot->queue != queue) {
            return false;
        }
        waiter = it->second;
        waiter->state = WaiterState::Removed;
        waiters_.erase(it);
    }
    stop_waiter(waiter);
    Queue* owner = waiter->slot->queue;
    waiter->release();
    owner->release();
    return true;
}

void Port::stop_waiter(Waiter* waiter) {
    SetThreadpoolWait(waiter->wait, nullptr, nullptr);
    if (t_waiter != waiter) {
        WaitForThreadpoolWaitCallbacks(waiter->wait, TRUE);
    }
}

HRESULT Port::suspend_termination(Slot& slot) {
    const std::scoped_lock lock(mutex_);
    if (slot.state.load() >= SlotState::Terminating) {
        return E_ABORT;
    }
    ++slot.suspends;
    return S_OK;
}

void Port::resume_termination(Slot& slot) {
    size_t scheduled = 0;
    {
        const std::scoped_lock lock(mutex_);
        if (slot.suspends == 0) {
            return; // unbalanced; nothing to resume
        }
        --slot.suspends;
        scheduled = release_parked_locked(slot);
    }
    if (scheduled != 0) {
        notify(scheduled);
    }
}

size_t Port::release_parked_locked(Slot& slot) {
    if (slot.suspends != 0 || slot.parked.empty()) {
        return 0;
    }
    const size_t count = slot.parked.size();
    for (auto& termination : slot.parked) {
        terminations_.push_back(std::move(termination));
    }
    slot.parked.clear();
    return count;
}

void Port::terminate(Slot& slot, std::shared_ptr<Termination> termination) {
    size_t queued = 0;
    std::vector<Waiter*> retired;
    {
        const std::scoped_lock lock(mutex_);
        SlotState state = slot.state.load();
        while (state < SlotState::Terminating &&
               !slot.state.compare_exchange_weak(state, SlotState::Terminating)) {
        }
        // Delayed callbacks run now, as canceled, in the order they were due.
        for (auto it = delayed_.begin(); it != delayed_.end();) {
            if (it->second.slot == &slot) {
                ready_.push_back(it->second);
                it = delayed_.erase(it);
                ++queued;
            } else {
                ++it;
            }
        }
        arm_timer_locked();
        // Each registered waiter gets its callback once more, canceled, unless it is waiting
        // already.
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            Waiter* waiter = it->second;
            if (waiter->slot != &slot) {
                ++it;
                continue;
            }
            waiter->state = WaiterState::Canceled;
            if (!waiter->queued) {
                waiter->queued = true;
                waiter->add_ref();
                slot.queue->add_ref();
                ready_.push_back(Entry{.slot = &slot,
                                       .callback = waiter->callback,
                                       .context = waiter->context,
                                       .waiter = waiter});
                ++queued;
            }
            retired.push_back(waiter);
            it = waiters_.erase(it);
        }
        if (slot.suspends == 0) {
            terminations_.push_back(std::move(termination));
            ++queued;
        } else {
            slot.parked.push_back(std::move(termination));
        }
    }
    for (Waiter* waiter : retired) {
        stop_waiter(waiter);
        Queue* owner = waiter->slot->queue;
        waiter->release();
        owner->release();
    }
    if (queued != 0) {
        notify(queued);
    }
}

bool Port::dispatch(Slot& slot, uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (drain_one()) {
            return true;
        }
        std::unique_lock lock(mutex_);
        const auto wake = [&] {
            return !ready_.empty() || (!terminations_.empty() && running_ == 0) ||
                   slot.state.load() == SlotState::Terminated;
        };
        if (slot.state.load() == SlotState::Terminated && ready_.empty()) {
            return false;
        }
        if (timeout_ms == INFINITE) {
            ready_changed_.wait(lock, wake);
        } else if (!ready_changed_.wait_until(lock, deadline, wake)) {
            return false;
        }
    }
}

bool Port::empty() {
    const std::scoped_lock lock(mutex_);
    return ready_.empty() && delayed_.empty() && running_ == 0;
}

void Port::notify(size_t items) {
    add_ref(); // a monitor or an immediate callback may close the last handle to the queue
    ready_changed_.notify_all();
    if (work_ != nullptr) {
        for (size_t i = 0; i < items; ++i) {
            add_ref(); // released by the work item
            SubmitThreadpoolWork(work_);
        }
    }
    // Monitors of every queue on this port hear of each item.
    std::vector<std::pair<Queue*, XTaskQueuePort>> watchers;
    {
        const std::scoped_lock lock(mutex_);
        for (Slot* slot : attached_) {
            if (slot->queue->has_monitors() && slot->queue->try_add_ref()) {
                watchers.emplace_back(slot->queue, slot->type);
            }
        }
    }
    for (const auto& [queue, type] : watchers) {
        for (size_t i = 0; i < items; ++i) {
            queue->call_monitors(type);
        }
        queue->release();
    }
    if (mode_ == XTaskQueueDispatchMode::Immediate) {
        for (size_t i = 0; i < items; ++i) {
            drain_one();
        }
    }
    release();
}

void Port::arm_timer_locked() {
    const uint64_t due = delayed_.empty() ? kNever : delayed_.begin()->first;
    if (due == timer_due_) {
        return;
    }
    timer_due_ = due;
    if (due == kNever) {
        SetThreadpoolTimer(timer_, nullptr, 0, 0);
        return;
    }
    const uint64_t now = now_ms();
    const uint64_t wait_ms = due > now ? due - now : 0;
    // A negative due time is relative, in 100 ns units; at least one unit, so "now" is never
    // read as an absolute time.
    ULARGE_INTEGER relative{};
    relative.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(wait_ms * 10000 + 1));
    FILETIME due_time{.dwLowDateTime = relative.LowPart, .dwHighDateTime = relative.HighPart};
    SetThreadpoolTimer(timer_, &due_time, 0, 0);
}

void CALLBACK Port::on_timer(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
    auto* port = static_cast<Port*>(context);
    if (!port->try_add_ref()) {
        return; // the port is going away, so nothing is delayed on it
    }
    t_timer_port = port;
    port->timer_fired();
    port->release();
    t_timer_port = nullptr;
}

void Port::timer_fired() {
    size_t moved = 0;
    {
        const std::scoped_lock lock(mutex_);
        timer_due_ = kNever;
        const uint64_t now = now_ms();
        while (!delayed_.empty() && delayed_.begin()->first <= now) {
            ready_.push_back(delayed_.begin()->second);
            delayed_.erase(delayed_.begin());
            ++moved;
        }
        arm_timer_locked(); // also re-arms a timer that fired early
    }
    if (moved != 0) {
        notify(moved);
    }
}

void CALLBACK Port::on_wait(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
    auto* waiter = static_cast<Waiter*>(context);
    waiter->add_ref();
    const Waiter* outer = t_waiter;
    t_waiter = waiter;
    waiter->slot->port->waiter_signaled(waiter);
    t_waiter = outer;
    waiter->release();
}

void Port::waiter_signaled(Waiter* waiter) {
    {
        const std::scoped_lock lock(mutex_);
        if (waiter->state != WaiterState::Registered || waiter->queued) {
            return;
        }
        waiter->queued = true;
        waiter->add_ref();
        waiter->slot->queue->add_ref();
        ready_.push_back(Entry{.slot = waiter->slot,
                               .callback = waiter->callback,
                               .context = waiter->context,
                               .waiter = waiter});
    }
    notify(1);
}

void Port::finish_waiter(Waiter* waiter) {
    {
        const std::scoped_lock lock(mutex_);
        waiter->queued = false;
        // Wait for the next signal only after this one's callback has run.
        if (waiter->state == WaiterState::Registered) {
            SetThreadpoolWait(waiter->wait, waiter->handle, nullptr);
        }
    }
    waiter->release();
}

void CALLBACK Port::on_work(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
    auto* port = static_cast<Port*>(context);
    port->work_item();
    port->release();
}

void Port::work_item() {
    if (mode_ != XTaskQueueDispatchMode::SerializedThreadPool) {
        drain_one();
        return;
    }
    // The first worker in owns the port and drains it; later ones leave their count behind,
    // and the owner keeps going until it has drained for each of them.
    if (serial_owners_.fetch_add(1) == 0) {
        while (drain_one() || serial_owners_.fetch_sub(1) != 1) {
        }
    }
}

// Callers hold a reference on the port: XTaskQueueDispatch through the queue, notify its own,
// and each thread-pool work item one of its own.
bool Port::drain_one() {
    Entry entry;
    bool found = false;
    bool call = false;
    {
        const std::scoped_lock lock(mutex_);
        if (!ready_.empty()) {
            entry = ready_.front();
            ready_.pop_front();
            found = true;
            ++running_;
            // An unregistered waiter's callback that was already waiting is dropped.
            call = entry.waiter == nullptr || entry.waiter->state != WaiterState::Removed;
        }
    }
    if (found) {
        if (call) {
            entry.callback(entry.context, entry.slot->state.load() != SlotState::Active);
        }
        if (entry.waiter != nullptr) {
            finish_waiter(entry.waiter);
        }
        {
            const std::scoped_lock lock(mutex_);
            --running_;
        }
        entry.slot->queue->release();
    }
    run_terminations();
    return found;
}

void Port::run_terminations() {
    std::vector<std::shared_ptr<Termination>> due;
    {
        const std::scoped_lock lock(mutex_);
        if (terminations_.empty() || !ready_.empty() || running_ != 0) {
            return;
        }
        due.swap(terminations_);
        for (const auto& termination : due) {
            termination_slot(*termination).state.store(SlotState::Terminated);
        }
    }
    ready_changed_.notify_all();
    for (const auto& termination : due) {
        advance(termination);
    }
}

void Port::advance(const std::shared_ptr<Termination>& termination) {
    if (termination->stage == XTaskQueuePort::Work) {
        termination->stage = XTaskQueuePort::Completion;
        Slot& completion = termination->queue->completion;
        completion.port->terminate(completion, termination);
        return;
    }
    if (termination->callback != nullptr) {
        termination->callback(termination->context);
    }
    Queue* queue = termination->queue;
    {
        const std::scoped_lock lock(termination->mutex);
        termination->done = true;
    }
    termination->done_changed.notify_all();
    queue->release();
}

// ---- Handles and the process queue ----

Queue* queue_of(XTaskQueueHandle handle) {
    if (handle == nullptr || handle->signature != kQueueSignature) {
        return nullptr;
    }
    return static_cast<Queue*>(handle);
}

Port* port_of(XTaskQueuePortHandle handle) {
    if (handle == nullptr || handle->signature != kPortSignature) {
        return nullptr;
    }
    return static_cast<Port*>(handle);
}

bool valid_mode(XTaskQueueDispatchMode mode) {
    return static_cast<uint32_t>(mode) <= static_cast<uint32_t>(XTaskQueueDispatchMode::Immediate);
}

HRESULT create_port(XTaskQueueDispatchMode mode, Port** port) {
    auto* created = new Port(mode);
    const HRESULT result = created->init();
    if (FAILED(result)) {
        created->release();
        return result;
    }
    *port = created;
    return S_OK;
}

struct ProcessQueue {
    std::mutex mutex;
    Queue* default_queue = nullptr; // made on first use and kept
    Queue* queue = nullptr;         // what XTaskQueueSetCurrentProcessTaskQueue set
    bool set = false;
};

ProcessQueue& process_queue() {
    static auto* const instance = new ProcessQueue; // never destroyed: used until exit
    return *instance;
}

} // namespace

HRESULT XTaskQueueCreate(XTaskQueueDispatchMode work_mode, XTaskQueueDispatchMode completion_mode,
                         XTaskQueueHandle* queue) {
    if (queue == nullptr) {
        return E_POINTER;
    }
    *queue = nullptr;
    if (!valid_mode(work_mode) || !valid_mode(completion_mode)) {
        return E_INVALIDARG;
    }
    Port* work = nullptr;
    HRESULT result = create_port(work_mode, &work);
    if (FAILED(result)) {
        return result;
    }
    Port* completion = nullptr;
    result = create_port(completion_mode, &completion);
    if (FAILED(result)) {
        work->release();
        return result;
    }
    auto* created = new Queue;
    created->init(work, completion);
    work->release();
    completion->release();
    *queue = created;
    return S_OK;
}

HRESULT XTaskQueueCreateComposite(XTaskQueuePortHandle work_port,
                                  XTaskQueuePortHandle completion_port, XTaskQueueHandle* queue) {
    if (queue == nullptr) {
        return E_POINTER;
    }
    *queue = nullptr;
    Port* work = port_of(work_port);
    Port* completion = port_of(completion_port);
    if (work == nullptr || completion == nullptr) {
        return kInvalidHandle;
    }
    auto* created = new Queue;
    created->init(work, completion);
    *queue = created;
    return S_OK;
}

HRESULT XTaskQueueGetPort(XTaskQueueHandle queue, XTaskQueuePort port,
                          XTaskQueuePortHandle* port_handle) {
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return kInvalidHandle;
    }
    const Slot* slot = target->slot(port);
    if (slot == nullptr) {
        return E_INVALIDARG;
    }
    if (port_handle == nullptr) {
        return E_POINTER;
    }
    *port_handle = slot->port;
    return S_OK;
}

HRESULT XTaskQueueDuplicateHandle(XTaskQueueHandle queue, XTaskQueueHandle* duplicate) {
    if (duplicate == nullptr) {
        return E_POINTER;
    }
    *duplicate = nullptr;
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return kInvalidHandle;
    }
    target->add_ref();
    *duplicate = target;
    return S_OK;
}

void XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    if (Queue* target = queue_of(queue)) {
        target->release();
    }
}

bool XTaskQueueDispatch(XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeout_ms) {
    Queue* target = queue_of(queue);
    Slot* slot = target == nullptr ? nullptr : target->slot(port);
    if (slot == nullptr) {
        return false;
    }
    // A callback may close the caller's handle, and the port runs on until this returns.
    target->add_ref();
    const bool ran = slot->port->dispatch(*slot, timeout_ms);
    target->release();
    return ran;
}

bool XTaskQueueIsEmpty(XTaskQueueHandle queue, XTaskQueuePort port) {
    Queue* target = queue_of(queue);
    Slot* slot = target == nullptr ? nullptr : target->slot(port);
    return slot != nullptr && slot->port->empty();
}

HRESULT XTaskQueueTerminate(XTaskQueueHandle queue, bool wait, void* context,
                            XTaskQueueTerminatedCallback* callback) {
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return kInvalidHandle;
    }
    auto termination = std::make_shared<Termination>();
    target->add_ref();
    termination->queue = target;
    termination->context = context;
    termination->callback = callback;
    for (Slot* slot : {&target->work, &target->completion}) {
        SlotState active = SlotState::Active;
        slot->state.compare_exchange_strong(active, SlotState::Canceled);
    }
    target->work.port->terminate(target->work, termination);
    if (wait) {
        std::unique_lock lock(termination->mutex);
        termination->done_changed.wait(lock, [&] { return termination->done; });
    }
    return S_OK;
}

HRESULT XTaskQueueSubmitCallback(XTaskQueueHandle queue, XTaskQueuePort port, void* context,
                                 XTaskQueueCallback* callback) {
    return XTaskQueueSubmitDelayedCallback(queue, port, 0, context, callback);
}

HRESULT XTaskQueueSubmitDelayedCallback(XTaskQueueHandle queue, XTaskQueuePort port,
                                        uint32_t delay_ms, void* context,
                                        XTaskQueueCallback* callback) {
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return kInvalidHandle;
    }
    Slot* slot = target->slot(port);
    if (slot == nullptr) {
        return E_INVALIDARG;
    }
    return slot->port->submit(*slot, delay_ms, context, callback);
}

HRESULT XTaskQueueRegisterWaiter(XTaskQueueHandle queue, XTaskQueuePort port, HANDLE wait_handle,
                                 void* context, XTaskQueueCallback* callback,
                                 XTaskQueueRegistrationToken* token) {
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return kInvalidHandle;
    }
    Slot* slot = target->slot(port);
    if (slot == nullptr || wait_handle == nullptr || callback == nullptr || token == nullptr) {
        return E_INVALIDARG;
    }
    return slot->port->register_waiter(*slot, wait_handle, context, callback, &token->token);
}

void XTaskQueueUnregisterWaiter(XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return;
    }
    if (!target->work.port->unregister_waiter(target, token.token) &&
        target->completion.port != target->work.port) {
        target->completion.port->unregister_waiter(target, token.token);
    }
}

HRESULT XTaskQueueRegisterMonitor(XTaskQueueHandle queue, void* context,
                                  XTaskQueueMonitorCallback* callback,
                                  XTaskQueueRegistrationToken* token) {
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return kInvalidHandle;
    }
    return target->register_monitor(context, callback, token);
}

void XTaskQueueUnregisterMonitor(XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
    if (Queue* target = queue_of(queue)) {
        target->unregister_monitor(token.token);
    }
}

HRESULT XTaskQueueSuspendTermination(XTaskQueueHandle queue) {
    Queue* target = queue_of(queue);
    if (target == nullptr) {
        return kInvalidHandle;
    }
    return target->work.port->suspend_termination(target->work);
}

void XTaskQueueResumeTermination(XTaskQueueHandle queue) {
    if (Queue* target = queue_of(queue)) {
        target->work.port->resume_termination(target->work);
    }
}

bool XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue) {
    if (queue == nullptr) {
        return false;
    }
    *queue = nullptr;
    ProcessQueue& process = process_queue();
    const std::scoped_lock lock(process.mutex);
    Queue* current = process.queue;
    if (!process.set) {
        if (process.default_queue == nullptr) {
            XTaskQueueHandle created = nullptr;
            if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                        XTaskQueueDispatchMode::ThreadPool, &created))) {
                return false;
            }
            process.default_queue = queue_of(created);
        }
        current = process.default_queue;
    }
    if (current == nullptr) {
        return false;
    }
    current->add_ref();
    *queue = current;
    return true;
}

void XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue) {
    Queue* next = queue_of(queue);
    if (next != nullptr) {
        next->add_ref();
    }
    Queue* previous = nullptr;
    {
        ProcessQueue& process = process_queue();
        const std::scoped_lock lock(process.mutex);
        previous = process.queue;
        process.queue = next;
        process.set = true;
    }
    if (previous != nullptr) {
        previous->release();
    }
}

} // namespace g6::gdk
