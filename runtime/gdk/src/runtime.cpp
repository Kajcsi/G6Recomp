#include "g6/gdk/runtime.h"

#include <array>
#include <atomic>
#include <intrin.h>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>

#include "g6/gdk/async.h"
#include "g6/gdk/error.h"
#include "g6/gdk/task_queue.h"
#include "g6/gdk/thread.h"
#include "g6/log.h"

// The service objects are COM-style: a pointer to a vtable whose first three slots are
// IUnknown's, and each later slot a function that takes the object, then the public function's
// arguments. The slot of each function was checked on the console's libHttpClient and XCurl:
// how many arguments each thunk passes and whether it uses the result (docs/gdk.md, "Slots").

namespace g6::gdk {
namespace {

struct Object {
    const void* const* vtable;
};

// IUnknown's interface ID, here so that nothing needs uuid.lib.
constexpr GUID kUnknownId = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

// The slot function for a public function: the same signature with the object in front.
template <auto Function>
struct Method;

template <typename Result, typename... Args, Result (*Function)(Args...)>
struct Method<Function> {
    static Result call(const Object* /*self*/, Args... args) { return Function(args...); }
};

template <auto Function>
const void* method() noexcept {
    return reinterpret_cast<const void*>(&Method<Function>::call);
}

std::string guid_text(const GUID& id) {
    return fmt::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1], id.Data4[2],
                       id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
}

// ---- IUnknown ----

const GUID& interface_of(const Object* self);

HRESULT query_interface(const Object* self, const GUID* interface_id, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    if (interface_id != nullptr &&
        (*interface_id == kUnknownId || *interface_id == interface_of(self))) {
        *object = const_cast<Object*>(self);
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG add_ref(const Object* /*self*/) {
    return 1;
}

ULONG release(const Object* /*self*/) {
    return 1;
}

// ---- Slots nothing has explained yet ----

// A slot whose function is unknown: logged once with the caller, then E_NOTIMPL. The arguments,
// if any, are left alone.
template <int Slot>
HRESULT unknown_slot(const Object* self) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        LOG_WARN(gdk,
                 "xgameruntime.dll: interface {} slot {} is unknown, called from {}; "
                 "returning E_NOTIMPL",
                 guid_text(interface_of(self)), Slot, _ReturnAddress());
    }
    return E_NOTIMPL;
}

// Error interface slot 3, which the thunks call with two arguments and no result.
void error_slot_3(const Object* /*self*/, uint64_t first, uint64_t second) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        LOG_WARN(gdk,
                 "xgameruntime.dll: error interface slot 3 is unknown, called from {} with "
                 "{:#x}, {:#x}; ignored",
                 _ReturnAddress(), first, second);
    }
}

// ---- The services ----

// XThreadingImpl: XAsync, XTaskQueue, and XThread. Slots 8 and 28 have no known function; no
// thunk calls slot 8, and the slot 28 thunk passes no arguments and returns a 32-bit value.
const std::array<const void*, 31> kThreadingSlots = {
    reinterpret_cast<const void*>(&query_interface),
    reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),
    method<&XAsyncGetStatus>(),
    method<&XAsyncGetResultSize>(),
    method<&XAsyncCancel>(),
    method<&XAsyncRun>(),
    method<&XAsyncBegin>(),
    reinterpret_cast<const void*>(&unknown_slot<8>),
    method<&XAsyncSchedule>(),
    method<&XAsyncComplete>(),
    method<&XAsyncGetResult>(),
    method<&XTaskQueueCreate>(),
    method<&XTaskQueueCreateComposite>(),
    method<&XTaskQueueGetPort>(),
    method<&XTaskQueueDuplicateHandle>(),
    method<&XTaskQueueDispatch>(),
    method<&XTaskQueueCloseHandle>(),
    method<&XTaskQueueSubmitCallback>(),
    method<&XTaskQueueSubmitDelayedCallback>(),
    method<&XTaskQueueRegisterWaiter>(),
    method<&XTaskQueueUnregisterWaiter>(),
    method<&XTaskQueueTerminate>(),
    method<&XTaskQueueRegisterMonitor>(),
    method<&XTaskQueueUnregisterMonitor>(),
    method<&XTaskQueueGetCurrentProcessTaskQueue>(),
    method<&XTaskQueueSetCurrentProcessTaskQueue>(),
    method<&XThreadSetTimeSensitive>(),
    reinterpret_cast<const void*>(&unknown_slot<28>),
    method<&XThreadAssertNotTimeSensitive>(),
    method<&XThreadIsTimeSensitive>(),
};

// XErrorImpl.
const std::array<const void*, 6> kErrorSlots = {
    reinterpret_cast<const void*>(&query_interface),
    reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),
    reinterpret_cast<const void*>(&error_slot_3),
    method<&XErrorSetCallback>(),
    method<&XErrorSetOptions>(),
};

// The feature query.
const std::array<const void*, 4> kFeatureSlots = {
    reinterpret_cast<const void*>(&query_interface),
    reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),
    method<&XGameRuntimeIsFeatureAvailable>(),
};

const Object kThreading{kThreadingSlots.data()};
const Object kError{kErrorSlots.data()};
const Object kFeature{kFeatureSlots.data()};

struct Service {
    const GUID& id;
    const Object& object;
};

const std::array<Service, 3> kServices = {{
    {kThreadingId, kThreading},
    {kErrorId, kError},
    {kFeatureId, kFeature},
}};

const GUID& interface_of(const Object* self) {
    for (const Service& service : kServices) {
        if (&service.object == self) {
            return service.id;
        }
    }
    return kUnknownId;
}

// Classes QueryApiImpl has reported as missing.
struct Reported {
    std::mutex mutex;
    std::set<std::string> classes;
};

bool first_report(const GUID& runtime_class) {
    static Reported reported;
    const std::scoped_lock lock(reported.mutex);
    return reported.classes.insert(guid_text(runtime_class)).second;
}

std::atomic<uint64_t> g_reported_features{0};
std::atomic<bool> g_reported_init{false};

void report_init(const char* name, uint64_t version, uint64_t os_version, uint32_t flags,
                 const void* options) {
    if (!g_reported_init.exchange(true)) {
        LOG_INFO(gdk, "xgameruntime.dll: {}: version {}.{}, OS version {}.{}, flags {:#x}{}", name,
                 version >> 32, version & 0xFFFFFFFF, os_version >> 32, os_version & 0xFFFFFFFF,
                 flags, options != nullptr ? ", with options" : "");
    }
}

} // namespace

HRESULT QueryApiImpl(const GUID* runtime_class, const GUID* interface_id, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (runtime_class == nullptr || interface_id == nullptr) {
        return E_POINTER;
    }
    for (const Service& service : kServices) {
        if (*runtime_class == service.id) {
            return query_interface(&service.object, interface_id, object);
        }
    }
    if (first_report(*runtime_class)) {
        LOG_WARN(gdk, "QueryApiImpl: no service {} yet (interface {}), called from {}",
                 guid_text(*runtime_class), guid_text(*interface_id), _ReturnAddress());
    }
    return REGDB_E_CLASSNOTREG;
}

HRESULT InitializeApiImpl(uint32_t version_high, uint32_t version_low) {
    report_init("InitializeApiImpl", (uint64_t{version_high} << 32) | version_low, 0, 0, nullptr);
    return S_OK;
}

HRESULT InitializeApiImplEx(uint64_t version, uint64_t os_version, uint8_t flags) {
    report_init("InitializeApiImplEx", version, os_version, flags, nullptr);
    return S_OK;
}

HRESULT InitializeApiImplEx2(uint64_t version, uint64_t os_version, uint8_t flags,
                             const void* options) {
    report_init("InitializeApiImplEx2", version, os_version, flags, options);
    return S_OK;
}

HRESULT UninitializeApiImpl() {
    return S_OK;
}

bool XGameRuntimeIsFeatureAvailable(uint32_t feature) {
    const uint64_t bit = feature < 64 ? uint64_t{1} << feature : 0;
    if (bit == 0 || (g_reported_features.fetch_or(bit) & bit) == 0) {
        LOG_INFO(gdk, "XGameRuntimeIsFeatureAvailable({}): available", feature);
    }
    return true;
}

} // namespace g6::gdk
