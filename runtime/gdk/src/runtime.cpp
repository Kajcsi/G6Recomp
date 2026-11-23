#include "g6/gdk/runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <intrin.h>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "g6/gdk/async.h"
#include "g6/gdk/error.h"
#include "g6/gdk/game_save.h"
#include "g6/gdk/task_queue.h"
#include "g6/gdk/thread.h"
#include "g6/gdk/user.h"
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

// Whether `key` is reported for the first time.
struct Reported {
    std::mutex mutex;
    std::set<std::string> keys;
};

bool first_report(std::string key) {
    static Reported reported;
    const std::scoped_lock lock(reported.mutex);
    return reported.keys.insert(std::move(key)).second;
}

// ---- IUnknown ----

const GUID& interface_of(const Object* self);
const Object* find_interface(const Object* self, const GUID& interface_id);

HRESULT query_interface(const Object* self, const GUID* interface_id, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    const Object* const found =
        interface_id != nullptr ? find_interface(self, *interface_id) : nullptr;
    *object = const_cast<Object*>(found);
    return found != nullptr ? S_OK : E_NOINTERFACE;
}

ULONG add_ref(const Object* /*self*/) {
    return 1;
}

ULONG release(const Object* /*self*/) {
    return 1;
}

// ---- Slots nothing has explained yet ----

// A slot whose function is unknown or not provided: logged once per interface with the caller,
// then E_NOTIMPL. The arguments, if any, are left alone.
template <int Slot>
HRESULT unknown_slot(const Object* self) {
    const std::string name = guid_text(interface_of(self));
    if (first_report(fmt::format("{} slot {}", name, Slot))) {
        LOG_WARN(gdk,
                 "xgameruntime.dll: interface {} slot {} is unknown, called from {}; "
                 "returning E_NOTIMPL",
                 name, Slot, _ReturnAddress());
    }
    return E_NOTIMPL;
}

template <int Slot>
const void* unknown() noexcept {
    return reinterpret_cast<const void*>(&unknown_slot<Slot>);
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
    unknown<8>(),
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
    unknown<28>(),
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

// XUserImpl, the user functions. Slot 15 has no thunk. Slots 37 to 41 and 43 to 46 aren't
// provided: their thunks pass 2, 2, 4, 4, 2 and 2, 1, 3, 2 arguments (docs/gdk.md, "XUser").
const std::array<const void*, 47> kUserSlots = {
    reinterpret_cast<const void*>(&query_interface),
    reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),
    method<&XUserDuplicateHandle>(),
    method<&XUserCloseHandle>(),
    method<&XUserCompare>(),
    method<&XUserGetMaxUsers>(),
    method<&XUserAddAsync>(),
    method<&XUserAddResult>(),
    method<&XUserGetLocalId>(),
    method<&XUserFindUserByLocalId>(),
    method<&XUserGetId>(),
    method<&XUserFindUserById>(),
    method<&XUserGetIsGuest>(),
    method<&XUserGetState>(),
    unknown<15>(),
    method<&XUserGetGamerPictureAsync>(),
    method<&XUserGetGamerPictureResultSize>(),
    method<&XUserGetGamerPictureResult>(),
    method<&XUserGetAgeGroup>(),
    method<&XUserCheckPrivilege>(),
    method<&XUserResolvePrivilegeWithUiAsync>(),
    method<&XUserResolvePrivilegeWithUiResult>(),
    method<&XUserGetTokenAndSignatureAsync>(),
    method<&XUserGetTokenAndSignatureResultSize>(),
    method<&XUserGetTokenAndSignatureResult>(),
    method<&XUserGetTokenAndSignatureUtf16Async>(),
    method<&XUserGetTokenAndSignatureUtf16ResultSize>(),
    method<&XUserGetTokenAndSignatureUtf16Result>(),
    method<&XUserResolveIssueWithUiAsync>(),
    method<&XUserResolveIssueWithUiResult>(),
    method<&XUserResolveIssueWithUiUtf16Async>(),
    method<&XUserResolveIssueWithUiUtf16Result>(),
    method<&XUserRegisterForChangeEvent>(),
    method<&XUserUnregisterForChangeEvent>(),
    method<&XUserGetSignOutDeferral>(),
    method<&XUserCloseSignOutDeferralHandle>(),
    unknown<37>(),
    unknown<38>(),
    unknown<39>(),
    unknown<40>(),
    unknown<41>(),
    method<&XUserIsStoreUser>(),
    unknown<43>(),
    unknown<44>(),
    unknown<45>(),
    unknown<46>(),
};

// XUser's gamertag interface.
const std::array<const void*, 4> kUserGamertagSlots = {
    reinterpret_cast<const void*>(&query_interface),
    reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),
    method<&XUserGetGamertag>(),
};

// XUser's device functions.
const std::array<const void*, 11> kUserDeviceSlots = {
    reinterpret_cast<const void*>(&query_interface),
    reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),
    method<&XUserFindForDevice>(),
    method<&XUserRegisterForDeviceAssociationChanged>(),
    method<&XUserUnregisterForDeviceAssociationChanged>(),
    method<&XUserGetDefaultAudioEndpointUtf16>(),
    method<&XUserRegisterForDefaultAudioEndpointUtf16Changed>(),
    method<&XUserUnregisterForDefaultAudioEndpointUtf16Changed>(),
    method<&XUserFindControllerForUserWithUiAsync>(),
    method<&XUserFindControllerForUserWithUiResult>(),
};

// XGameSaveImpl. Slots 30 to 32 are XGameSaveFiles, which isn't provided. Slots 33 to 49,
// called through the second interface, have no known function: their thunks pass 2, 1, 2, 3, 2,
// 1, 4, 2, 2, 2, 2, 2, 3, 1, 3, 1, and 2 arguments (docs/gdk.md, "XGameSave").
const std::array<const void*, 50> kGameSaveSlots = {
    reinterpret_cast<const void*>(&query_interface),
    reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),
    method<&XGameSaveInitializeProvider>(),
    method<&XGameSaveInitializeProviderAsync>(),
    method<&XGameSaveInitializeProviderResult>(),
    method<&XGameSaveCloseProvider>(),
    method<&XGameSaveGetRemainingQuota>(),
    method<&XGameSaveGetRemainingQuotaAsync>(),
    method<&XGameSaveGetRemainingQuotaResult>(),
    method<&XGameSaveDeleteContainer>(),
    method<&XGameSaveDeleteContainerAsync>(),
    method<&XGameSaveDeleteContainerResult>(),
    method<&XGameSaveGetContainerInfo>(),
    method<&XGameSaveEnumerateContainerInfo>(),
    method<&XGameSaveEnumerateContainerInfoByName>(),
    method<&XGameSaveCreateContainer>(),
    method<&XGameSaveCloseContainer>(),
    method<&XGameSaveEnumerateBlobInfo>(),
    method<&XGameSaveEnumerateBlobInfoByName>(),
    method<&XGameSaveReadBlobData>(),
    method<&XGameSaveReadBlobDataAsync>(),
    method<&XGameSaveReadBlobDataResult>(),
    method<&XGameSaveCreateUpdate>(),
    method<&XGameSaveCloseUpdate>(),
    method<&XGameSaveSubmitBlobWrite>(),
    method<&XGameSaveSubmitBlobDelete>(),
    method<&XGameSaveSubmitUpdate>(),
    method<&XGameSaveSubmitUpdateAsync>(),
    method<&XGameSaveSubmitUpdateResult>(),
    method<&XGameSaveFilesGetFolderWithUiAsync>(),
    method<&XGameSaveFilesGetFolderWithUiResult>(),
    method<&XGameSaveFilesGetRemainingQuota>(),
    unknown<33>(),
    unknown<34>(),
    unknown<35>(),
    unknown<36>(),
    unknown<37>(),
    unknown<38>(),
    unknown<39>(),
    unknown<40>(),
    unknown<41>(),
    unknown<42>(),
    unknown<43>(),
    unknown<44>(),
    unknown<45>(),
    unknown<46>(),
    unknown<47>(),
    unknown<48>(),
    unknown<49>(),
};

const Object kThreading{kThreadingSlots.data()};
const Object kError{kErrorSlots.data()};
const Object kFeature{kFeatureSlots.data()};
const Object kUser{kUserSlots.data()};
const Object kUserGamertag{kUserGamertagSlots.data()};
const Object kUserDevice{kUserDeviceSlots.data()};
// Two objects on one vtable, so that each interface logs as itself.
const Object kGameSave{kGameSaveSlots.data()};
const Object kGameSave2{kGameSaveSlots.data()};

// Each interface a service has, and the object that answers for it. The first row of a class
// also answers for IUnknown.
struct Interface {
    const GUID& runtime_class;
    const GUID& id;
    const Object& object;
};

const std::array<Interface, 8> kInterfaces = {{
    {kThreadingId, kThreadingId, kThreading},
    {kErrorId, kErrorId, kError},
    {kFeatureId, kFeatureId, kFeature},
    {kUserClassId, kUserInterfaceId, kUser},
    {kUserClassId, kUserGamertagInterfaceId, kUserGamertag},
    {kUserDeviceClassId, kUserDeviceInterfaceId, kUserDevice},
    {kGameSaveClassId, kGameSaveInterfaceId, kGameSave},
    {kGameSaveClassId, kGameSave2InterfaceId, kGameSave2},
}};

const Interface* row_of(const Object* self) {
    for (const Interface& row : kInterfaces) {
        if (&row.object == self) {
            return &row;
        }
    }
    return nullptr;
}

const GUID& interface_of(const Object* self) {
    const Interface* const row = row_of(self);
    return row != nullptr ? row->id : kUnknownId;
}

bool has_class(const GUID& runtime_class) {
    return std::ranges::any_of(
        kInterfaces, [&](const Interface& row) { return row.runtime_class == runtime_class; });
}

// The object for `interface_id` in a class, or null.
const Object* find_in_class(const GUID& runtime_class, const GUID& interface_id) {
    const Object* first = nullptr;
    for (const Interface& row : kInterfaces) {
        if (row.runtime_class != runtime_class) {
            continue;
        }
        if (row.id == interface_id) {
            return &row.object;
        }
        if (first == nullptr) {
            first = &row.object;
        }
    }
    return interface_id == kUnknownId ? first : nullptr;
}

const Object* find_interface(const Object* self, const GUID& interface_id) {
    const Interface* const row = row_of(self);
    return row != nullptr ? find_in_class(row->runtime_class, interface_id) : nullptr;
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
    if (!has_class(*runtime_class)) {
        if (first_report(guid_text(*runtime_class))) {
            LOG_WARN(gdk, "QueryApiImpl: no service {} yet (interface {}), called from {}",
                     guid_text(*runtime_class), guid_text(*interface_id), _ReturnAddress());
        }
        return REGDB_E_CLASSNOTREG;
    }
    const Object* const found = find_in_class(*runtime_class, *interface_id);
    if (found == nullptr) {
        if (first_report(guid_text(*runtime_class) + "/" + guid_text(*interface_id))) {
            LOG_WARN(gdk, "QueryApiImpl: service {} has no interface {}, called from {}",
                     guid_text(*runtime_class), guid_text(*interface_id), _ReturnAddress());
        }
        return E_NOINTERFACE;
    }
    *object = const_cast<Object*>(found);
    return S_OK;
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
