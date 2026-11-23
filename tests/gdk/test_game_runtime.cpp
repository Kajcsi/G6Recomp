// The Game Runtime DLL (docs/gdk.md, "The Game Runtime DLL"): QueryApiImpl's answers, the
// service objects' IUnknown slots, the functions at the threading, error, and feature slots,
// called through the vtables as the guest's thunks call them, and the initializers.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "g6/gdk/async.h"
#include "g6/gdk/error.h"
#include "g6/gdk/runtime.h"
#include "g6/gdk/task_queue.h"
#include "g6/gdk/thread.h"
#include "g6test.h"

using namespace g6::gdk;

namespace {

constexpr GUID kUnknownId = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kOtherId = {
    0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};

void* service(const GUID& id) {
    void* object = nullptr;
    REQUIRE(QueryApiImpl(&id, &id, &object) == S_OK);
    REQUIRE(object != nullptr);
    return object;
}

// Calls a slot as a thunk does: the object first, then the function's arguments.
template <typename Result, typename... Args>
Result call(void* object, int slot, Args... args) {
    const void* const* vtable = *static_cast<const void* const* const*>(object);
    using Function = Result (*)(void*, Args...);
    return reinterpret_cast<Function>(const_cast<void*>(vtable[slot]))(object, args...);
}

// Threading slots (docs/gdk.md, "Slots").
constexpr int kAsyncGetStatus = 3;
constexpr int kAsyncRun = 6;
constexpr int kTaskQueueCreate = 12;
constexpr int kTaskQueueDispatch = 16;
constexpr int kTaskQueueCloseHandle = 17;
constexpr int kTaskQueueSubmitCallback = 18;
constexpr int kThreadSetTimeSensitive = 27;
constexpr int kThreadAssertNotTimeSensitive = 29;
constexpr int kThreadIsTimeSensitive = 30;

void CALLBACK count_callback(void* context, bool canceled) {
    if (!canceled) {
        static_cast<std::atomic<int>*>(context)->fetch_add(1);
    }
}

HRESULT CALLBACK work_returns_value(XAsyncBlock* async) {
    *static_cast<int*>(async->context) = 42;
    return S_FALSE;
}

struct ErrorSeen {
    HRESULT result = S_OK;
    std::string message;
    int calls = 0;
};

bool CALLBACK record_error(HRESULT result, const char* message, void* context) {
    auto* seen = static_cast<ErrorSeen*>(context);
    seen->result = result;
    seen->message = message;
    ++seen->calls;
    return false;
}

} // namespace

TEST_CASE("QueryApiImpl gives an object for each service and nothing else") {
    void* object = reinterpret_cast<void*>(1);
    CHECK(QueryApiImpl(&kThreadingId, &kThreadingId, nullptr) == E_POINTER);
    CHECK(QueryApiImpl(nullptr, &kThreadingId, &object) == E_POINTER);
    CHECK(object == nullptr);

    // A service we don't provide is "class not registered", which xgameruntime.lib turns into
    // its own error.
    object = reinterpret_cast<void*>(1);
    CHECK(QueryApiImpl(&kOtherId, &kOtherId, &object) == REGDB_E_CLASSNOTREG);
    CHECK(object == nullptr);
    object = reinterpret_cast<void*>(1);
    CHECK(QueryApiImpl(&kThreadingId, &kErrorId, &object) == E_NOINTERFACE);
    CHECK(object == nullptr);

    const void* const threading = service(kThreadingId);
    CHECK(QueryApiImpl(&kThreadingId, &kUnknownId, &object) == S_OK);
    CHECK(object == threading);
    CHECK(service(kErrorId) != threading);
    CHECK(service(kFeatureId) != threading);
}

TEST_CASE("the service objects answer IUnknown") {
    void* const error = service(kErrorId);
    void* object = nullptr;
    CHECK(call<HRESULT>(error, 0, &kErrorId, &object) == S_OK);
    CHECK(object == error);
    CHECK(call<HRESULT>(error, 0, &kUnknownId, &object) == S_OK);
    CHECK(object == error);
    CHECK(call<HRESULT>(error, 0, &kThreadingId, &object) == E_NOINTERFACE);
    CHECK(object == nullptr);
    CHECK(call<HRESULT>(error, 0, &kErrorId, static_cast<void**>(nullptr)) == E_POINTER);
    // Static objects: the counts never reach zero.
    CHECK(call<ULONG>(error, 1) >= 1);
    CHECK(call<ULONG>(error, 2) >= 1);
}

TEST_CASE("task queues and async calls through the threading slots") {
    void* const threading = service(kThreadingId);

    XTaskQueueHandle queue = nullptr;
    REQUIRE(call<HRESULT>(threading, kTaskQueueCreate, XTaskQueueDispatchMode::Manual,
                          XTaskQueueDispatchMode::Manual, &queue) == S_OK);
    std::atomic<int> ran{0};
    CHECK(call<HRESULT>(threading, kTaskQueueSubmitCallback, queue, XTaskQueuePort::Work,
                        static_cast<void*>(&ran), &count_callback) == S_OK);
    CHECK(call<bool>(threading, kTaskQueueDispatch, queue, XTaskQueuePort::Work, uint32_t{0}));
    CHECK_EQ(ran.load(), 1);
    CHECK(!call<bool>(threading, kTaskQueueDispatch, queue, XTaskQueuePort::Work, uint32_t{0}));

    // XAsyncRun on the queue: the work runs on the work port, and its result is the status.
    int value = 0;
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &value;
    CHECK(call<HRESULT>(threading, kAsyncRun, &async, &work_returns_value) == S_OK);
    CHECK(call<HRESULT>(threading, kAsyncGetStatus, &async, false) == E_PENDING);
    while (
        call<bool>(threading, kTaskQueueDispatch, queue, XTaskQueuePort::Work, uint32_t{0}) ||
        call<bool>(threading, kTaskQueueDispatch, queue, XTaskQueuePort::Completion, uint32_t{0})) {
    }
    CHECK(call<HRESULT>(threading, kAsyncGetStatus, &async, true) == S_FALSE);
    CHECK_EQ(value, 42);
    call<void>(threading, kTaskQueueCloseHandle, queue);
}

TEST_CASE("time-sensitive threads through the threading slots") {
    void* const threading = service(kThreadingId);
    CHECK(!call<bool>(threading, kThreadIsTimeSensitive));
    CHECK(call<HRESULT>(threading, kThreadSetTimeSensitive, true) == S_OK);
    CHECK(call<bool>(threading, kThreadIsTimeSensitive));
    CHECK(XThreadIsTimeSensitive());
    // Logged, never stops the call.
    call<void>(threading, kThreadAssertNotTimeSensitive);

    // The mark is per thread.
    bool other = true;
    std::thread([&] { other = XThreadIsTimeSensitive(); }).join();
    CHECK(!other);

    CHECK(call<HRESULT>(threading, kThreadSetTimeSensitive, false) == S_OK);
    CHECK(!call<bool>(threading, kThreadIsTimeSensitive));
}

TEST_CASE("the unknown threading slots fail") {
    void* const threading = service(kThreadingId);
    CHECK(call<HRESULT>(threading, 8) == E_NOTIMPL);
    CHECK(call<HRESULT>(threading, 28) == E_NOTIMPL);
}

TEST_CASE("errors reach the callback set through the error slots") {
    void* const error = service(kErrorId);
    ErrorSeen seen;
    call<void>(error, 4, &record_error, static_cast<void*>(&seen));
    call<void>(error, 5, XErrorOptions::None, XErrorOptions::None);

    CHECK(XErrorReport(E_OUTOFMEMORY, "out of memory") == S_OK);
    CHECK_EQ(seen.calls, 1);
    CHECK(seen.result == E_OUTOFMEMORY);
    CHECK_EQ(seen.message, std::string("out of memory"));
    // A report without a message passes an empty one.
    CHECK(XErrorReport(E_FAIL, nullptr) == S_OK);
    CHECK_EQ(seen.calls, 2);
    CHECK(seen.message.empty());
    // Slot 3 is unknown and changes nothing.
    call<void>(error, 3, uint64_t{1}, uint64_t{2});

    // Without a callback, reports only reach the log.
    call<void>(error, 4, static_cast<XErrorCallback*>(nullptr), static_cast<void*>(&seen));
    CHECK(XErrorReport(E_FAIL, "no callback") == S_OK);
    CHECK_EQ(seen.calls, 2);
}

TEST_CASE("every feature is available, as on the console") {
    void* const feature = service(kFeatureId);
    for (const uint32_t number : {0U, 16U, 21U, 200U}) {
        INFO(std::to_string(number));
        CHECK(call<bool>(feature, 3, number));
    }
    CHECK(XGameRuntimeIsFeatureAvailable(17));
}

TEST_CASE("the initializers succeed") {
    CHECK(InitializeApiImplEx2(0x0000633600002712, 0x000065F400001832, 2, nullptr) == S_OK);
    CHECK(InitializeApiImplEx(0x0000633600002712, 0x000065F400001832, 2) == S_OK);
    CHECK(InitializeApiImpl(0x6336, 0x2712) == S_OK);
    CHECK(UninitializeApiImpl() == S_OK);
}
