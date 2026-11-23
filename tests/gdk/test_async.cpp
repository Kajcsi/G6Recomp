// XAsync (docs/gdk.md, "XAsync"): XAsyncRun, the provider steps and when each runs, results
// with and without a payload, the argument checks, cancel, BeginAlloc's context, the process
// queue, and calls on a terminating queue.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <tuple>

#include "g6/gdk/async.h"
#include "g6test.h"

using namespace g6::gdk;

namespace {

using Mode = XTaskQueueDispatchMode;
using Port = XTaskQueuePort;

XTaskQueueHandle make_queue(Mode work, Mode completion) {
    XTaskQueueHandle queue = nullptr;
    REQUIRE(XTaskQueueCreate(work, completion, &queue) == S_OK);
    return queue;
}

void finish_manual(XTaskQueueHandle queue) {
    while (XTaskQueueDispatch(queue, Port::Work, 0) ||
           XTaskQueueDispatch(queue, Port::Completion, 0)) {
    }
}

struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;
    ~Event() { CloseHandle(handle); }
    void set() const { SetEvent(handle); }
    bool wait(DWORD ms) const { return WaitForSingleObject(handle, ms) == WAIT_OBJECT_0; }
};

// A provider whose steps are counted, and whose behavior each test picks.
struct Probe {
    enum class Begin { Schedule, Nothing, Fail, Pending, CompleteNow };
    enum class Work { CompleteWithPayload, CompleteEmpty, ReturnOk, PendingOnce };
    Begin begin = Begin::Schedule;
    Work work = Work::CompleteWithPayload;
    std::atomic<int> begins{0};
    std::atomic<int> works{0};
    std::atomic<int> results{0};
    std::atomic<int> cancels{0};
    std::atomic<int> cleanups{0};
    Event cleaned;
};

constexpr uint64_t kPayload = 0x00C0FFEE12345678;
const int kIdentity = 0;

HRESULT CALLBACK probe_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* probe = static_cast<Probe*>(data->context);
    switch (op) {
    case XAsyncOp::Begin:
        ++probe->begins;
        switch (probe->begin) {
        case Probe::Begin::Schedule: return XAsyncSchedule(data->async, 0);
        case Probe::Begin::Nothing: return S_OK;
        case Probe::Begin::Fail: return E_OUTOFMEMORY;
        case Probe::Begin::Pending: return E_PENDING;
        case Probe::Begin::CompleteNow: XAsyncComplete(data->async, S_OK, 0); return S_OK;
        }
        return S_OK;
    case XAsyncOp::DoWork: {
        const int works = ++probe->works;
        switch (probe->work) {
        case Probe::Work::CompleteWithPayload:
            XAsyncComplete(data->async, S_OK, sizeof(kPayload));
            return S_OK;
        case Probe::Work::CompleteEmpty: XAsyncComplete(data->async, S_OK, 0); return S_OK;
        case Probe::Work::ReturnOk: return S_OK;
        case Probe::Work::PendingOnce:
            if (works == 1) {
                CHECK(XAsyncSchedule(data->async, 0) == S_OK);
                return E_PENDING;
            }
            XAsyncComplete(data->async, S_OK, 0);
            return S_OK;
        }
        return S_OK;
    }
    case XAsyncOp::GetResult:
        ++probe->results;
        CHECK(data->buffer_size >= sizeof(kPayload));
        std::memcpy(data->buffer, &kPayload, sizeof(kPayload));
        return S_OK;
    case XAsyncOp::Cancel:
        ++probe->cancels;
        XAsyncComplete(data->async, E_ABORT, 0);
        return S_OK;
    case XAsyncOp::Cleanup:
        ++probe->cleanups;
        probe->cleaned.set();
        return S_OK;
    }
    return S_OK;
}

HRESULT begin_probe(XAsyncBlock* block, Probe* probe) {
    return XAsyncBegin(block, probe, &kIdentity, "probe", probe_provider);
}

} // namespace

TEST_CASE("XAsyncRun runs its work on the work port and completes on the completion port") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    struct Seen {
        bool worked = false;
        bool completed = false;
        HRESULT status_in_callback = S_OK;
    } seen;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &seen;
    block.callback = [](XAsyncBlock* async) {
        auto* seen = static_cast<Seen*>(async->context);
        seen->completed = true;
        seen->status_in_callback = XAsyncGetStatus(async, false);
    };
    REQUIRE(XAsyncRun(&block, [](XAsyncBlock* async) -> HRESULT {
                static_cast<Seen*>(async->context)->worked = true;
                return S_OK;
            }) == S_OK);
    CHECK(XAsyncGetStatus(&block, false) == E_PENDING);
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(seen.worked);
    CHECK(XAsyncGetStatus(&block, false) == S_OK);
    CHECK(!seen.completed);
    CHECK(XTaskQueueDispatch(queue, Port::Completion, 0));
    CHECK(seen.completed);
    CHECK(seen.status_in_callback == S_OK);
    CHECK(XAsyncGetStatus(&block, true) == S_OK);

    // A call with no result gives S_OK and nothing, as often as asked.
    size_t used = 99;
    CHECK(XAsyncGetResult(&block, nullptr, 0, nullptr, &used) == S_OK);
    CHECK_EQ(used, size_t{0});
    CHECK(XAsyncGetResult(&block, nullptr, 0, nullptr, &used) == S_OK);

    // The work's failure is the call's.
    seen = {};
    REQUIRE(XAsyncRun(&block, [](XAsyncBlock*) -> HRESULT { return E_ACCESSDENIED; }) == S_OK);
    finish_manual(queue);
    CHECK(seen.completed);
    CHECK(XAsyncGetStatus(&block, false) == E_ACCESSDENIED);
    CHECK(XAsyncGetResult(&block, nullptr, 0, nullptr, &used) == E_ACCESSDENIED);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a result is fetched once, and XAsyncGetStatus waits for the completion routine") {
    XTaskQueueHandle queue = make_queue(Mode::ThreadPool, Mode::ThreadPool);
    Probe probe;
    std::atomic<bool> callback_done{false};
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &callback_done;
    block.callback = [](XAsyncBlock* async) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        static_cast<std::atomic<bool>*>(async->context)->store(true);
    };
    REQUIRE(begin_probe(&block, &probe) == S_OK);
    CHECK(XAsyncGetStatus(&block, true) == S_OK);
    CHECK(callback_done.load());

    size_t size = 0;
    CHECK(XAsyncGetResultSize(&block, &size) == S_OK);
    CHECK_EQ(size, sizeof(kPayload));
    uint64_t value = 0;
    size_t used = 0;
    CHECK(XAsyncGetResult(&block, &kIdentity, sizeof(value), &value, &used) == S_OK);
    CHECK_EQ(value, kPayload);
    CHECK_EQ(used, sizeof(kPayload));
    CHECK_EQ(probe.results.load(), 1);
    CHECK(XAsyncGetResult(&block, &kIdentity, sizeof(value), &value, &used) ==
          E_ILLEGAL_METHOD_CALL);
    CHECK(XAsyncGetResultSize(&block, &size) == S_OK);
    CHECK_EQ(size, size_t{0});

    REQUIRE(probe.cleaned.wait(5000));
    CHECK_EQ(probe.cleanups.load(), 1);
    CHECK_EQ(probe.begins.load(), 1);
    CHECK_EQ(probe.works.load(), 1);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("XAsyncGetResult checks the identity and buffer, and a refused fetch ends the call") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    uint64_t value = 0;
    size_t used = 0;

    Probe pending;
    pending.begin = Probe::Begin::Nothing;
    REQUIRE(begin_probe(&block, &pending) == S_OK);
    CHECK(XAsyncGetResult(&block, &kIdentity, sizeof(value), &value, &used) == E_PENDING);
    CHECK(XAsyncGetStatus(&block, false) == E_PENDING); // still in its call
    XAsyncCancel(&block);
    CHECK(XAsyncGetStatus(&block, false) == E_ABORT);
    finish_manual(queue);

    const int other = 0;
    for (const auto& [identity, buffer, size, expected] :
         {std::tuple{static_cast<const void*>(&other), &value, sizeof(value), E_INVALIDARG},
          std::tuple{static_cast<const void*>(&kIdentity), static_cast<uint64_t*>(nullptr),
                     sizeof(value), E_INVALIDARG},
          // A null buffer is checked before the size.
          std::tuple{static_cast<const void*>(&kIdentity), static_cast<uint64_t*>(nullptr),
                     sizeof(value) - 1, E_INVALIDARG},
          std::tuple{static_cast<const void*>(&kIdentity), &value, sizeof(value) - 1,
                     E_NOT_SUFFICIENT_BUFFER}}) {
        Probe probe;
        REQUIRE(begin_probe(&block, &probe) == S_OK);
        finish_manual(queue);
        CHECK(XAsyncGetStatus(&block, false) == S_OK);
        CHECK(XAsyncGetResult(&block, identity, size, buffer, &used) == expected);
        CHECK_EQ(probe.results.load(), 0);
        CHECK(XAsyncGetResult(&block, &kIdentity, sizeof(value), &value, &used) ==
              E_ILLEGAL_METHOD_CALL);
        CHECK_EQ(probe.cleanups.load(), 1);
    }
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("XAsyncBegin refuses a block that is in a call, and takes it back after") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    Probe first;
    REQUIRE(begin_probe(&block, &first) == S_OK);
    Probe second;
    CHECK(begin_probe(&block, &second) == E_INVALIDARG);
    CHECK_EQ(second.begins.load(), 0);
    finish_manual(queue);
    uint64_t value = 0;
    CHECK(XAsyncGetResult(&block, &kIdentity, sizeof(value), &value, nullptr) == S_OK);
    CHECK(begin_probe(&block, &second) == S_OK);
    finish_manual(queue);
    CHECK(XAsyncGetResult(&block, &kIdentity, sizeof(value), &value, nullptr) == S_OK);
    CHECK_EQ(second.begins.load(), 1);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a failing begin step ends the call while XAsyncBegin succeeds") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    Probe probe;
    probe.begin = Probe::Begin::Fail;
    CHECK(begin_probe(&block, &probe) == S_OK);
    CHECK(XAsyncGetStatus(&block, false) == E_OUTOFMEMORY);
    CHECK(XAsyncGetStatus(&block, true) == E_OUTOFMEMORY);
    CHECK_EQ(probe.cleanups.load(), 1);
    CHECK(XAsyncSchedule(&block, 0) == E_OUTOFMEMORY);

    // Completing inside the begin step works the same way.
    Probe now;
    now.begin = Probe::Begin::CompleteNow;
    CHECK(begin_probe(&block, &now) == S_OK);
    CHECK(XAsyncGetStatus(&block, false) == S_OK);
    CHECK_EQ(now.cleanups.load(), 1);
    CHECK(XAsyncSchedule(&block, 0) == E_INVALIDARG); // the call is over
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("E_PENDING from the begin step keeps the call pending, with no result size") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    Probe probe;
    probe.begin = Probe::Begin::Pending;
    CHECK(begin_probe(&block, &probe) == S_OK);
    CHECK(XAsyncGetStatus(&block, false) == E_PENDING);
    size_t size = 99;
    CHECK(XAsyncGetResultSize(&block, &size) == E_PENDING);
    CHECK_EQ(size, size_t{0});

    // A failed call reports its failure, and no size either.
    XAsyncCancel(&block);
    finish_manual(queue);
    size = 99;
    CHECK(XAsyncGetResultSize(&block, &size) == E_ABORT);
    CHECK_EQ(size, size_t{0});
    CHECK_EQ(probe.cleanups.load(), 1);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("work is scheduled once at a time, and a work step can ask for more") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    Probe probe;
    probe.begin = Probe::Begin::Nothing;
    probe.work = Probe::Work::PendingOnce;
    REQUIRE(begin_probe(&block, &probe) == S_OK);
    CHECK(XAsyncSchedule(&block, 0) == S_OK);
    CHECK(XAsyncSchedule(&block, 0) == E_UNEXPECTED);
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(XAsyncGetStatus(&block, false) == E_PENDING);
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK_EQ(probe.works.load(), 2);
    CHECK(XAsyncGetStatus(&block, false) == S_OK);
    CHECK_EQ(probe.cleanups.load(), 1); // right after the work step that ended the call
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a work step that returns success without completing fails the call") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    Probe probe;
    probe.work = Probe::Work::ReturnOk;
    REQUIRE(begin_probe(&block, &probe) == S_OK);
    finish_manual(queue);
    CHECK(XAsyncGetStatus(&block, false) == E_UNEXPECTED);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("XAsyncCancel asks the provider, once the call is over no more") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    Probe probe;
    probe.begin = Probe::Begin::Nothing;
    REQUIRE(begin_probe(&block, &probe) == S_OK);
    XAsyncCancel(&block);
    CHECK_EQ(probe.cancels.load(), 1);
    CHECK(XAsyncGetStatus(&block, false) == E_ABORT);
    CHECK_EQ(probe.cleanups.load(), 1);
    XAsyncCancel(&block);
    CHECK_EQ(probe.cancels.load(), 1);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("XAsyncBeginAlloc checks its sizes, zeroes the context, and copies the parameters") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XAsyncBlock block{};
    block.queue = queue;
    XAsyncProvider* const provider = [](XAsyncOp op, const XAsyncProviderData* data) -> HRESULT {
        if (op == XAsyncOp::Begin) {
            const auto* bytes = static_cast<const unsigned char*>(data->context);
            bool ok = reinterpret_cast<uintptr_t>(bytes) % alignof(std::max_align_t) == 0;
            for (size_t i = 0; i < 40; ++i) {
                ok = ok && size_t{bytes[i]} == (i < 8 ? 0xA0 + i : 0);
            }
            XAsyncComplete(data->async, ok ? S_OK : E_FAIL, 0);
        }
        return S_OK;
    };
    unsigned char parameters[8];
    for (size_t i = 0; i < sizeof(parameters); ++i) {
        parameters[i] = static_cast<unsigned char>(0xA0 + i);
    }
    CHECK(XAsyncBeginAlloc(&block, nullptr, nullptr, provider, 0, 0, nullptr) == E_INVALIDARG);
    CHECK(XAsyncBeginAlloc(&block, nullptr, nullptr, provider, 40, 8, nullptr) == E_INVALIDARG);
    CHECK(XAsyncBeginAlloc(&block, nullptr, nullptr, provider, 4, 8, parameters) == E_INVALIDARG);
    CHECK(XAsyncBeginAlloc(&block, nullptr, nullptr, provider, 40, 0, parameters) == E_INVALIDARG);
    CHECK(XAsyncBeginAlloc(&block, nullptr, nullptr, provider, 40, 8, parameters) == S_OK);
    CHECK(XAsyncGetStatus(&block, false) == S_OK);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a block without a queue uses the process queue, and fails without one") {
    XAsyncBlock block{};
    REQUIRE(XAsyncRun(&block, [](XAsyncBlock*) -> HRESULT { return S_FALSE; }) == S_OK);
    CHECK(XAsyncGetStatus(&block, true) == S_FALSE);

    XTaskQueueHandle saved = nullptr;
    REQUIRE(XTaskQueueGetCurrentProcessTaskQueue(&saved));
    XTaskQueueSetCurrentProcessTaskQueue(nullptr);
    CHECK(XAsyncRun(&block, [](XAsyncBlock*) -> HRESULT { return S_OK; }) == kNoTaskQueue);
    CHECK(XAsyncGetStatus(&block, false) == kNoTaskQueue);
    XTaskQueueSetCurrentProcessTaskQueue(saved);
    XTaskQueueCloseHandle(saved);
}

TEST_CASE("a terminating queue refuses new calls and cancels the calls it has") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    struct Seen {
        bool worked = false;
        bool completed = false;
    } seen;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &seen;
    block.callback = [](XAsyncBlock* async) {
        static_cast<Seen*>(async->context)->completed = true;
    };
    REQUIRE(XAsyncRun(&block, [](XAsyncBlock* async) -> HRESULT {
                static_cast<Seen*>(async->context)->worked = true;
                return S_OK;
            }) == S_OK);
    Probe idle;
    idle.begin = Probe::Begin::Nothing;
    XAsyncBlock idle_block{};
    idle_block.queue = queue;
    REQUIRE(begin_probe(&idle_block, &idle) == S_OK);
    bool terminated = false;
    CHECK(XTaskQueueTerminate(queue, false, &terminated,
                              [](void* context) { *static_cast<bool*>(context) = true; }) == S_OK);

    XAsyncBlock late{};
    late.queue = queue;
    CHECK(XAsyncRun(&late, [](XAsyncBlock*) -> HRESULT { return S_OK; }) == E_ABORT);
    CHECK(XAsyncGetStatus(&late, false) == E_ABORT);

    // A call begun before can't schedule work after; it ends when its provider cancels it.
    CHECK(XAsyncSchedule(&idle_block, 0) == E_ABORT);
    XAsyncCancel(&idle_block);
    CHECK(XAsyncGetStatus(&idle_block, false) == E_ABORT);
    CHECK_EQ(idle.cleanups.load(), 1);

    // The call holds the termination off until its completion routine has run.
    finish_manual(queue);
    CHECK(!seen.worked);
    CHECK(seen.completed);
    CHECK(terminated);
    CHECK(XAsyncGetStatus(&block, false) == E_ABORT);
    XTaskQueueCloseHandle(queue);
}
