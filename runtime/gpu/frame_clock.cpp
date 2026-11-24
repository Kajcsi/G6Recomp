// The console's frame clock (frame_clock.h).

#include "frame_clock.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "g6/log.h"

namespace g6::gpu::detail {
namespace {

constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
// The most handles one wait can take, less the clock's own.
constexpr uint32_t kMostHandles = MAXIMUM_WAIT_OBJECTS - 1;
// Tokens are 48 bits wide and never 0.
constexpr uint64_t kTokenMask = 0xFFFF'FFFF'FFFFULL;

int64_t now() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

// A notice the first time something the clock doesn't model yet comes up.
void note_once(std::atomic<bool>& seen, const char* what, uint64_t value) {
    if (!seen.exchange(true, std::memory_order_relaxed)) {
        LOG_WARN(d3d12, "frame clock: {} {:#x}", what, value);
    }
}

} // namespace

FrameClock::FrameClock() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    frequency_ = frequency.QuadPart;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (timer_ == nullptr) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (Event& event : events_) {
        event.signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (timer_ == nullptr || wake_ == nullptr ||
        std::ranges::any_of(events_, [](const Event& event) { return event.signal == nullptr; })) {
        throw std::runtime_error("cannot create the frame clock's timer and events");
    }
}

FrameClock::~FrameClock() {
    {
        const std::scoped_lock lock(mutex_);
        stop_ = true;
    }
    SetEvent(wake_);
    if (thread_.joinable()) {
        thread_.join();
    }
    for (Event& event : events_) {
        close_extra(event);
        CloseHandle(event.signal);
    }
    CloseHandle(wake_);
    CloseHandle(timer_);
}

int64_t FrameClock::ticks(uint64_t microseconds) const {
    return static_cast<int64_t>(microseconds * static_cast<uint64_t>(frequency_) / 1'000'000);
}

void FrameClock::close_extra(Event& event) {
    for (HANDLE handle : event.extra) {
        CloseHandle(handle);
    }
    event.extra.clear();
}

HRESULT FrameClock::set_interval(uint32_t length_us) {
    {
        const std::scoped_lock lock(mutex_);
        if (length_us == 0) {
            interval_ = 0;
            running_ = false;
            for (Event& event : events_) {
                event.scheduled = false;
                close_extra(event);
            }
        } else if (const int64_t interval = ticks(length_us); interval != interval_) {
            interval_ = interval;
            base_ = now();
            for (Event& event : events_) {
                event.last = 0;
            }
        }
    }
    SetEvent(wake_);
    return S_FALSE;
}

HRESULT FrameClock::schedule(uint32_t type, uint32_t offset_us, const void* handles,
                             uint32_t flags) {
    static std::atomic<bool> other_flags{false};
    if (type >= kEventTypes) {
        return E_INVALIDARG;
    }
    if ((flags & ~kUnschedule) != 0) {
        note_once(other_flags, "scheduling flags not modelled:", flags);
    }
    // The title's handles, duplicated so that the clock can signal them after the title closes
    // its own.
    std::vector<HANDLE> extra;
    if (handles != nullptr && (flags & kUnschedule) == 0) {
        uint32_t count = 0;
        const HANDLE* list = nullptr;
        std::memcpy(&count, handles, sizeof(count));
        std::memcpy(static_cast<void*>(&list), static_cast<const uint8_t*>(handles) + 8,
                    sizeof(list));
        if (count > kMostHandles || (count != 0 && list == nullptr)) {
            return E_INVALIDARG;
        }
        for (uint32_t i = 0; i < count; ++i) {
            HANDLE copy = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), list[i], GetCurrentProcess(), &copy, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS)) {
                const DWORD error = GetLastError();
                for (HANDLE made : extra) {
                    CloseHandle(made);
                }
                return HRESULT_FROM_WIN32(error);
            }
            extra.push_back(copy);
        }
    }

    HRESULT result = S_FALSE;
    {
        const std::scoped_lock lock(mutex_);
        if (interval_ == 0) {
            for (HANDLE made : extra) {
                CloseHandle(made);
            }
            return E_INVALIDARG; // no interval set
        }
        Event& event = events_[type];
        close_extra(event);
        if ((flags & kUnschedule) != 0) {
            event.scheduled = false;
        } else {
            event.offset = ticks(offset_us) % interval_;
            event.scheduled = true;
            event.extra = std::move(extra);
            if (!running_) {
                running_ = true;
                base_ = now();
                for (Event& each : events_) {
                    each.last = 0;
                }
                result = kStartedClock;
            }
            if (!thread_.joinable()) {
                thread_ = std::thread([this] { run(); });
            }
        }
    }
    SetEvent(wake_);
    return result;
}

HRESULT FrameClock::wait(uint32_t type, uint32_t timeout_ms, const void* handles, uint32_t flags,
                         uint64_t* token) {
    static std::atomic<bool> other_flags{false};
    static std::atomic<bool> with_handles{false};
    if (type >= kEventTypes) {
        return E_INVALIDARG;
    }
    if (handles != nullptr) {
        note_once(with_handles, "a wait with the title's handles is not modelled, block at",
                  reinterpret_cast<uintptr_t>(handles));
        return E_NOTIMPL;
    }
    if ((flags & ~kTokenOnTimeout) != 0) {
        note_once(other_flags, "wait flags not modelled:", flags);
    }
    HANDLE signal = nullptr;
    {
        const std::scoped_lock lock(mutex_);
        if (!events_[type].scheduled) {
            return E_INVALIDARG;
        }
        signal = events_[type].signal;
    }
    const DWORD waited = WaitForSingleObject(signal, timeout_ms);
    if (waited == WAIT_FAILED) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const bool came = waited == WAIT_OBJECT_0;
    if (type == 0 && token != nullptr && *token == 0 && (came || (flags & kTokenOnTimeout) != 0)) {
        const std::scoped_lock lock(mutex_);
        uint64_t next = next_token_++ & kTokenMask;
        if (next == 0) {
            next = next_token_++ & kTokenMask;
        }
        *token = next;
    }
    return came ? S_OK : S_FALSE;
}

// The next time `event` is due: the first after its last occurrence, or its first since the
// start.
int64_t FrameClock::first_due(const Event& event) const {
    const int64_t first = base_ + event.offset;
    if (event.last < first) {
        return first;
    }
    return first + (((event.last - first) / interval_) + 1) * interval_;
}

void FrameClock::fire(Event& event) const {
    SetEvent(event.signal);
    for (HANDLE handle : event.extra) {
        if (!SetEvent(handle)) {
            ReleaseSemaphore(handle, 1, nullptr);
        }
    }
}

void FrameClock::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::unique_lock lock(mutex_);
    while (!stop_) {
        int64_t soonest = kNever;
        if (running_ && interval_ > 0) {
            const int64_t time = now();
            for (Event& event : events_) {
                if (!event.scheduled) {
                    continue;
                }
                int64_t due = first_due(event);
                if (due <= time) {
                    // A late clock signals once, for the latest occurrence, not once per frame
                    // it missed.
                    event.last = due + (((time - due) / interval_) * interval_);
                    fire(event);
                    due = event.last + interval_;
                }
                soonest = std::min(soonest, due);
            }
        }
        lock.unlock();
        if (soonest == kNever) {
            WaitForSingleObject(wake_, INFINITE);
        } else {
            const int64_t left = std::max<int64_t>(soonest - now(), 0);
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<int64_t>(left * 10'000'000 / frequency_, 1);
            SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
            const std::array<HANDLE, 2> both = {wake_, timer_};
            WaitForMultipleObjects(static_cast<DWORD>(both.size()), both.data(), FALSE, INFINITE);
        }
        lock.lock();
    }
}

} // namespace g6::gpu::detail
