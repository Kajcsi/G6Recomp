#pragma once

// The console's frame clock (docs/d3d12x.md, "The frame clock"). A console title paces its frames
// by a clock the device runs, not by a swap chain: it sets a frame interval, schedules events at
// offsets into each interval, and waits for them. Here a thread of the clock's own runs it on a
// high-resolution timer, not locked to the display.

#include <array>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

namespace g6::gpu::detail {

class FrameClock {
public:
    // Event types: 0 is the frame's origin, 1 and 2 are later points in each frame.
    static constexpr uint32_t kEventTypes = 3;
    // What scheduling returns when it starts the clock, and otherwise (xcl's report).
    static constexpr HRESULT kStartedClock = 0x10000000;
    // Scheduling flag: take the event off the clock.
    static constexpr uint32_t kUnschedule = 1;
    // Wait flag: hand out a token for the origin even when the wait times out.
    static constexpr uint32_t kTokenOnTimeout = 2;

    FrameClock();
    ~FrameClock();
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;
    FrameClock(FrameClock&&) = delete;
    FrameClock& operator=(FrameClock&&) = delete;

    // The interval in microseconds. 0 stops the clock and takes every event off it; a new length
    // restarts the frames from now.
    HRESULT set_interval(uint32_t length_us);

    // Puts event `type` on the clock at `offset_us` into each frame, or takes it off with
    // kUnschedule. `handles` is null or the title's block of handles to signal with the event:
    // a count at +0 and a pointer to the handles at +8. The first event scheduled starts the
    // clock.
    HRESULT schedule(uint32_t type, uint32_t offset_us, const void* handles, uint32_t flags);

    // Waits up to `timeout_ms` for the next event of `type`: S_OK when it came, S_FALSE when the
    // wait timed out. A wait for the origin hands out a frame token in `token` unless the title
    // passes one in.
    HRESULT wait(uint32_t type, uint32_t timeout_ms, const void* handles, uint32_t flags,
                 uint64_t* token);

private:
    struct Event {
        bool scheduled = false;
        int64_t offset = 0;        // ticks into each frame
        int64_t last = 0;          // the last time it was signalled, 0 for none since the start
        HANDLE signal = nullptr;   // auto-reset, set at each occurrence
        std::vector<HANDLE> extra; // the title's handles, duplicated
    };

    void run();
    void fire(Event& event) const;
    [[nodiscard]] int64_t first_due(const Event& event) const;
    static void close_extra(Event& event);
    [[nodiscard]] int64_t ticks(uint64_t microseconds) const;

    std::mutex mutex_;
    int64_t frequency_ = 0;
    int64_t interval_ = 0; // ticks; 0 while no interval is set
    int64_t base_ = 0;     // when the frames started
    bool running_ = false;
    bool stop_ = false;
    std::array<Event, kEventTypes> events_{};
    uint64_t next_token_ = 1;
    HANDLE timer_ = nullptr;
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

} // namespace g6::gpu::detail
