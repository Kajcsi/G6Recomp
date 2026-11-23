// XTaskQueue (docs/gdk.md, "XTaskQueue"): the four dispatch modes, delayed callbacks,
// termination and what it cancels, monitors, waiters, composite queues, handle checks, and the
// process queue.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "g6/gdk/task_queue.h"
#include "g6test.h"

using namespace g6::gdk;

namespace {

using Mode = XTaskQueueDispatchMode;
using Port = XTaskQueuePort;

// A guest can pass any value, so the checks are tested with values outside the enums.
// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
constexpr auto kBadMode = static_cast<Mode>(4);
// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
constexpr auto kBadPort = static_cast<Port>(2);

XTaskQueueHandle make_queue(Mode work, Mode completion) {
    XTaskQueueHandle queue = nullptr;
    REQUIRE(XTaskQueueCreate(work, completion, &queue) == S_OK);
    REQUIRE(queue != nullptr);
    return queue;
}

// One record per callback run: which, and whether it ran as canceled.
struct Log {
    std::mutex mutex;
    std::vector<int> order;
    std::vector<bool> canceled;
};

struct Item {
    Log* log;
    int id;
};

void CALLBACK record(void* context, bool canceled) {
    auto* item = static_cast<Item*>(context);
    const std::scoped_lock lock(item->log->mutex);
    item->log->order.push_back(item->id);
    item->log->canceled.push_back(canceled);
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

void CALLBACK set_event(void* context, bool /*canceled*/) {
    static_cast<Event*>(context)->set();
}

// Runs the queue's termination to its end on Manual ports.
void finish_manual(XTaskQueueHandle queue) {
    while (XTaskQueueDispatch(queue, Port::Work, 0)) {
    }
    while (XTaskQueueDispatch(queue, Port::Completion, 0)) {
    }
}

} // namespace

TEST_CASE("a manual port runs one callback per dispatch, in order") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    Log log;
    Item items[] = {{&log, 1}, {&log, 2}, {&log, 3}};
    for (Item& item : items) {
        CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &item, record) == S_OK);
    }
    CHECK(!XTaskQueueIsEmpty(queue, Port::Work));
    CHECK(XTaskQueueIsEmpty(queue, Port::Completion));
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK_EQ(log.order.size(), size_t{1});
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(!XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(log.order == std::vector<int>({1, 2, 3}));
    CHECK(std::ranges::none_of(log.canceled, [](bool canceled) { return canceled; }));
    CHECK(XTaskQueueIsEmpty(queue, Port::Work));
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("an immediate port runs the callback before the submit returns") {
    XTaskQueueHandle queue = make_queue(Mode::Immediate, Mode::Immediate);
    struct Seen {
        std::thread::id thread;
        bool ran = false;
    } seen;
    CHECK(XTaskQueueSubmitCallback(queue, Port::Completion, &seen, [](void* context, bool) {
              auto* seen = static_cast<Seen*>(context);
              seen->thread = std::this_thread::get_id();
              seen->ran = true;
          }) == S_OK);
    CHECK(seen.ran);
    CHECK(seen.thread == std::this_thread::get_id());
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a thread-pool port runs callbacks on other threads") {
    XTaskQueueHandle queue = make_queue(Mode::ThreadPool, Mode::ThreadPool);
    struct Seen {
        std::thread::id thread;
        Event done;
    } seen;
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &seen, [](void* context, bool) {
              auto* seen = static_cast<Seen*>(context);
              seen->thread = std::this_thread::get_id();
              seen->done.set();
          }) == S_OK);
    REQUIRE(seen.done.wait(5000));
    CHECK(seen.thread != std::this_thread::get_id());
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a serialized port runs one callback at a time, in order") {
    XTaskQueueHandle queue = make_queue(Mode::SerializedThreadPool, Mode::Manual);
    struct Shared {
        std::atomic<int> running{0};
        std::atomic<int> most{0};
        std::mutex mutex;
        std::vector<int> order;
        Event done;
    } shared;
    struct Job {
        Shared* shared;
        int id;
    };
    constexpr int kJobs = 64;
    std::vector<Job> jobs;
    jobs.reserve(kJobs);
    for (int i = 0; i < kJobs; ++i) {
        jobs.push_back({&shared, i});
    }
    for (Job& job : jobs) {
        REQUIRE(XTaskQueueSubmitCallback(queue, Port::Work, &job, [](void* context, bool) {
                    const auto* job = static_cast<Job*>(context);
                    Shared& shared = *job->shared;
                    const int now = shared.running.fetch_add(1) + 1;
                    int most = shared.most.load();
                    while (now > most && !shared.most.compare_exchange_weak(most, now)) {
                    }
                    std::this_thread::yield();
                    {
                        const std::scoped_lock lock(shared.mutex);
                        shared.order.push_back(job->id);
                    }
                    shared.running.fetch_sub(1);
                    if (job->id == kJobs - 1) {
                        shared.done.set();
                    }
                }) == S_OK);
    }
    REQUIRE(shared.done.wait(5000));
    CHECK_EQ(shared.most.load(), 1);
    std::vector<int> expected(kJobs);
    for (int i = 0; i < kJobs; ++i) {
        expected[i] = i;
    }
    CHECK(shared.order == expected);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("delayed callbacks wait their time and run in the order they are due") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    Log log;
    Item now{&log, 0};
    Item soon{&log, 1};
    Item later{&log, 2};
    const auto start = std::chrono::steady_clock::now();
    CHECK(XTaskQueueSubmitDelayedCallback(queue, Port::Work, 80, &later, record) == S_OK);
    CHECK(XTaskQueueSubmitDelayedCallback(queue, Port::Work, 30, &soon, record) == S_OK);
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &now, record) == S_OK);
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(!XTaskQueueDispatch(queue, Port::Work, 0)); // the others aren't due
    CHECK(!XTaskQueueIsEmpty(queue, Port::Work));
    CHECK(XTaskQueueDispatch(queue, Port::Work, 5000));
    const auto first = std::chrono::steady_clock::now() - start;
    CHECK(XTaskQueueDispatch(queue, Port::Work, 5000));
    const auto second = std::chrono::steady_clock::now() - start;
    CHECK(log.order == std::vector<int>({0, 1, 2}));
    // Timer resolution on Windows is coarse, so only the lower bounds are firm.
    CHECK(first >= std::chrono::milliseconds(25));
    CHECK(second >= std::chrono::milliseconds(70));
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("terminating runs waiting callbacks as canceled, then the work port ends, then the "
          "completion port, then the callback") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    Log log;
    Item work{&log, 1};
    Item delayed{&log, 2};
    Item completion{&log, 3};
    Item terminated{&log, 4};
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &work, record) == S_OK);
    CHECK(XTaskQueueSubmitDelayedCallback(queue, Port::Work, 60000, &delayed, record) == S_OK);
    CHECK(XTaskQueueSubmitCallback(queue, Port::Completion, &completion, record) == S_OK);
    CHECK(XTaskQueueTerminate(queue, false, &terminated,
                              [](void* context) { record(context, false); }) == S_OK);

    // The work port refuses new work at once.
    Item refused{&log, 99};
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &refused, record) == E_ABORT);
    CHECK(XTaskQueueSubmitDelayedCallback(queue, Port::Work, 10, &refused, record) == E_ABORT);
    CHECK(XTaskQueueSuspendTermination(queue) == E_ABORT);
    const Event never;
    XTaskQueueRegistrationToken token{};
    CHECK(XTaskQueueRegisterWaiter(queue, Port::Work, never.handle, &refused, record, &token) ==
          E_ABORT);

    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));  // the delayed one, no longer waiting
    CHECK(!XTaskQueueDispatch(queue, Port::Work, 0)); // the work port's termination runs here
    CHECK(!XTaskQueueDispatch(queue, Port::Work, INFINITE)); // ended: no wait
    CHECK(log.order == std::vector<int>({1, 2}));

    CHECK(XTaskQueueDispatch(queue, Port::Completion, 0));
    CHECK(!XTaskQueueDispatch(queue, Port::Completion, 0));
    CHECK(log.order == std::vector<int>({1, 2, 3, 4}));
    CHECK(log.canceled == std::vector<bool>({true, true, true, false}));
    CHECK(XTaskQueueSubmitCallback(queue, Port::Completion, &refused, record) == E_ABORT);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("Terminate with wait returns once the callback has run") {
    XTaskQueueHandle queue = make_queue(Mode::ThreadPool, Mode::ThreadPool);
    std::atomic<bool> slow_done{false};
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &slow_done, [](void* context, bool) {
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
              static_cast<std::atomic<bool>*>(context)->store(true);
          }) == S_OK);
    std::atomic<bool> terminated{false};
    CHECK(XTaskQueueTerminate(queue, true, &terminated, [](void* context) {
              static_cast<std::atomic<bool>*>(context)->store(true);
          }) == S_OK);
    CHECK(slow_done.load());
    CHECK(terminated.load());
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("SuspendTermination holds termination off until ResumeTermination") {
    XTaskQueueHandle queue = make_queue(Mode::ThreadPool, Mode::ThreadPool);
    REQUIRE(XTaskQueueSuspendTermination(queue) == S_OK);
    Event terminated;
    CHECK(XTaskQueueTerminate(queue, false, &terminated,
                              [](void* context) { set_event(context, false); }) == S_OK);
    CHECK(!terminated.wait(100));
    XTaskQueueResumeTermination(queue);
    CHECK(terminated.wait(5000));
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("monitors hear of each queued callback, through composite queues too, until "
          "unregistered") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    struct Heard {
        XTaskQueueHandle queue = nullptr;
        std::vector<Port> ports;
    } heard;
    heard.queue = queue;
    XTaskQueueRegistrationToken token{};
    REQUIRE(XTaskQueueRegisterMonitor(
                queue, &heard,
                [](void* context, XTaskQueueHandle source, XTaskQueuePort port) {
                    auto* heard = static_cast<Heard*>(context);
                    CHECK(source == heard->queue);
                    heard->ports.push_back(port);
                },
                &token) == S_OK);
    Log log;
    Item item{&log, 0};
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &item, record) == S_OK);
    CHECK(XTaskQueueSubmitCallback(queue, Port::Completion, &item, record) == S_OK);
    CHECK(heard.ports == std::vector<Port>({Port::Work, Port::Completion}));

    // A composite queue on the same ports: its callbacks wake the first queue's monitor, and
    // they run where the first queue's run.
    XTaskQueuePortHandle work_port = nullptr;
    XTaskQueuePortHandle completion_port = nullptr;
    REQUIRE(XTaskQueueGetPort(queue, Port::Work, &work_port) == S_OK);
    REQUIRE(XTaskQueueGetPort(queue, Port::Completion, &completion_port) == S_OK);
    XTaskQueueHandle composite = nullptr;
    REQUIRE(XTaskQueueCreateComposite(work_port, completion_port, &composite) == S_OK);
    Item shared{&log, 7};
    CHECK(XTaskQueueSubmitCallback(composite, Port::Work, &shared, record) == S_OK);
    CHECK_EQ(heard.ports.size(), size_t{3});
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(log.order == std::vector<int>({0, 7}));

    // Terminating the composite cancels only its own callbacks.
    Item mine{&log, 8};
    Item theirs{&log, 9};
    CHECK(XTaskQueueSubmitCallback(composite, Port::Work, &mine, record) == S_OK);
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &theirs, record) == S_OK);
    CHECK(XTaskQueueTerminate(composite, false, nullptr, nullptr) == S_OK);
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(XTaskQueueDispatch(queue, Port::Work, 0));
    CHECK(log.order == std::vector<int>({0, 7, 8, 9}));
    CHECK(log.canceled == std::vector<bool>({false, false, true, false}));

    XTaskQueueUnregisterMonitor(queue, token);
    const size_t before = heard.ports.size();
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &item, record) == S_OK);
    CHECK_EQ(heard.ports.size(), before);
    finish_manual(queue);
    XTaskQueueCloseHandle(composite);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a registered waiter runs once per signal until unregistered") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    Log log;
    Item item{&log, 5};
    XTaskQueueRegistrationToken token{};
    REQUIRE(XTaskQueueRegisterWaiter(queue, Port::Work, signal, &item, record, &token) == S_OK);
    CHECK(!XTaskQueueDispatch(queue, Port::Work, 0));
    SetEvent(signal);
    CHECK(XTaskQueueDispatch(queue, Port::Work, 5000));
    SetEvent(signal);
    CHECK(XTaskQueueDispatch(queue, Port::Work, 5000));
    CHECK_EQ(log.order.size(), size_t{2});
    XTaskQueueUnregisterWaiter(queue, token);
    SetEvent(signal);
    CHECK(!XTaskQueueDispatch(queue, Port::Work, 100));
    CHECK_EQ(log.order.size(), size_t{2});
    CloseHandle(signal);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("terminating gives each registered waiter one last, canceled callback") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    const Event never;
    Log log;
    Item item{&log, 6};
    XTaskQueueRegistrationToken token{};
    REQUIRE(XTaskQueueRegisterWaiter(queue, Port::Work, never.handle, &item, record, &token) ==
            S_OK);
    CHECK(XTaskQueueTerminate(queue, false, nullptr, nullptr) == S_OK);
    finish_manual(queue);
    CHECK(log.order == std::vector<int>({6}));
    CHECK(log.canceled == std::vector<bool>({true}));
    XTaskQueueUnregisterWaiter(queue, token); // gone already: nothing happens
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("handles and arguments are checked") {
    Log log;
    Item item{&log, 0};
    CHECK(XTaskQueueSubmitCallback(nullptr, Port::Work, &item, record) == kInvalidHandle);
    alignas(8) unsigned char not_a_queue[64] = {};
    auto* fake = reinterpret_cast<XTaskQueueHandle>(not_a_queue);
    CHECK(XTaskQueueSubmitCallback(fake, Port::Work, &item, record) == kInvalidHandle);
    CHECK(!XTaskQueueDispatch(nullptr, Port::Work, 0));
    CHECK(!XTaskQueueIsEmpty(fake, Port::Work));
    CHECK(XTaskQueueTerminate(nullptr, false, nullptr, nullptr) == kInvalidHandle);
    XTaskQueueCloseHandle(nullptr);
    XTaskQueueCloseHandle(fake);

    XTaskQueueHandle out = nullptr;
    CHECK(XTaskQueueCreate(kBadMode, Mode::Manual, &out) == E_INVALIDARG);
    CHECK(out == nullptr);
    CHECK(XTaskQueueCreate(Mode::Manual, Mode::Manual, nullptr) == E_POINTER);

    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XTaskQueuePortHandle port = nullptr;
    CHECK(XTaskQueueGetPort(queue, kBadPort, &port) == E_INVALIDARG);
    CHECK(XTaskQueueSubmitCallback(queue, kBadPort, &item, record) == E_INVALIDARG);
    CHECK(XTaskQueueSubmitCallback(queue, Port::Work, &item, nullptr) == E_INVALIDARG);
    CHECK(XTaskQueueDuplicateHandle(queue, nullptr) == E_POINTER);
    XTaskQueueRegistrationToken token{};
    CHECK(XTaskQueueRegisterMonitor(queue, nullptr, nullptr, &token) == E_POINTER);
    CHECK(XTaskQueueRegisterWaiter(queue, Port::Work, nullptr, &item, record, &token) ==
          E_INVALIDARG);
    const Event signal;
    CHECK(XTaskQueueRegisterWaiter(queue, kBadPort, signal.handle, &item, record, &token) ==
          E_INVALIDARG);
    CHECK(XTaskQueueCreateComposite(nullptr, nullptr, &out) == kInvalidHandle);
    XTaskQueueCloseHandle(queue);
}

TEST_CASE("a duplicated handle keeps the queue alive after the first is closed") {
    XTaskQueueHandle queue = make_queue(Mode::Manual, Mode::Manual);
    XTaskQueueHandle duplicate = nullptr;
    REQUIRE(XTaskQueueDuplicateHandle(queue, &duplicate) == S_OK);
    XTaskQueueCloseHandle(queue);
    Log log;
    Item item{&log, 3};
    CHECK(XTaskQueueSubmitCallback(duplicate, Port::Work, &item, record) == S_OK);
    CHECK(XTaskQueueDispatch(duplicate, Port::Work, 0));
    CHECK(log.order == std::vector<int>({3}));
    XTaskQueueCloseHandle(duplicate);
}

TEST_CASE("the process queue is made on first use, and can be replaced or removed") {
    XTaskQueueHandle original = nullptr;
    REQUIRE(XTaskQueueGetCurrentProcessTaskQueue(&original));
    XTaskQueueHandle again = nullptr;
    REQUIRE(XTaskQueueGetCurrentProcessTaskQueue(&again));
    CHECK(again == original);
    XTaskQueueCloseHandle(again);

    // The default runs its callbacks on the thread pool.
    Event ran;
    CHECK(XTaskQueueSubmitCallback(original, Port::Work, &ran, set_event) == S_OK);
    CHECK(ran.wait(5000));

    XTaskQueueSetCurrentProcessTaskQueue(nullptr);
    XTaskQueueHandle none = nullptr;
    CHECK(!XTaskQueueGetCurrentProcessTaskQueue(&none));
    CHECK(none == nullptr);

    XTaskQueueHandle manual = make_queue(Mode::Manual, Mode::Manual);
    XTaskQueueSetCurrentProcessTaskQueue(manual);
    XTaskQueueCloseHandle(manual); // the process keeps its own reference
    XTaskQueueHandle current = nullptr;
    REQUIRE(XTaskQueueGetCurrentProcessTaskQueue(&current));
    CHECK(current == manual);
    Log log;
    Item item{&log, 1};
    CHECK(XTaskQueueSubmitCallback(current, Port::Work, &item, record) == S_OK);
    CHECK(XTaskQueueDispatch(current, Port::Work, 0));
    XTaskQueueCloseHandle(current);

    XTaskQueueSetCurrentProcessTaskQueue(original);
    XTaskQueueCloseHandle(original);
    REQUIRE(XTaskQueueGetCurrentProcessTaskQueue(&current));
    CHECK(current == original);
    XTaskQueueCloseHandle(current);
}
