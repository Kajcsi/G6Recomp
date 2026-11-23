// XUser for one local offline user (docs/gdk.md, "XUser"): adding the user through the queue,
// its handle and identity, the gamertag, privileges, the calls that fail offline or without a
// dialog, registrations, devices, and the three interfaces called through their slots.

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include "g6/gdk/async.h"
#include "g6/gdk/runtime.h"
#include "g6/gdk/task_queue.h"
#include "g6/gdk/user.h"
#include "g6test.h"

using namespace g6::gdk;

namespace {

using Mode = XTaskQueueDispatchMode;
using Port = XTaskQueuePort;

// A manual queue for one test.
struct Queue {
    XTaskQueueHandle handle = nullptr;
    Queue() { REQUIRE(XTaskQueueCreate(Mode::Manual, Mode::Manual, &handle) == S_OK); }
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;
    ~Queue() { XTaskQueueCloseHandle(handle); }

    void finish() const {
        while (XTaskQueueDispatch(handle, Port::Work, 0) ||
               XTaskQueueDispatch(handle, Port::Completion, 0)) {
        }
    }
};

XAsyncBlock block_on(const Queue& queue) {
    XAsyncBlock async{};
    async.queue = queue.handle;
    return async;
}

XUserHandle add_user() {
    const Queue queue;
    XAsyncBlock async = block_on(queue);
    REQUIRE(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &async) == S_OK);
    queue.finish();
    XUserHandle user = nullptr;
    REQUIRE(XUserAddResult(&async, &user) == S_OK);
    REQUIRE(user != nullptr);
    return user;
}

void* service(const GUID& runtime_class, const GUID& interface_id) {
    void* object = nullptr;
    REQUIRE(QueryApiImpl(&runtime_class, &interface_id, &object) == S_OK);
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

void CALLBACK on_change(void* /*context*/, XUserLocalId /*user*/, XUserChangeEvent /*event*/) {}

void CALLBACK on_device(void* /*context*/, const XUserDeviceAssociationChange* /*change*/) {}

// A value outside an enum's list, as a caller might pass one.
template <typename Enum>
Enum out_of_range(uint32_t value) {
    return std::bit_cast<Enum>(value);
}

constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80070490);

} // namespace

TEST_CASE("adding a user gives the offline user through the queue") {
    const Queue queue;
    XAsyncBlock async = block_on(queue);
    REQUIRE(XUserAddAsync(XUserAddOptions::None, &async) == S_OK);
    CHECK(XAsyncGetStatus(&async, false) == E_PENDING);
    queue.finish();
    CHECK(XAsyncGetStatus(&async, false) == S_OK);
    XUserHandle user = nullptr;
    CHECK(XUserAddResult(&async, &user) == S_OK);
    CHECK(user != nullptr);
    // The result is offered once.
    XUserHandle again = nullptr;
    CHECK(XUserAddResult(&async, &again) == E_ILLEGAL_METHOD_CALL);

    XUser* const second = add_user();
    CHECK_EQ(XUserCompare(user, second), 0);
    CHECK(XUserCompare(user, nullptr) != 0);
    XUserCloseHandle(second);
    XUserCloseHandle(user);
}

TEST_CASE("the offline user's handle and identity") {
    XUser* const user = add_user();

    XUserHandle duplicate = nullptr;
    CHECK(XUserDuplicateHandle(user, &duplicate) == S_OK);
    CHECK_EQ(XUserCompare(user, duplicate), 0);
    XUserCloseHandle(duplicate);
    CHECK(XUserDuplicateHandle(user, nullptr) == E_INVALIDARG);

    uint32_t max_users = 0;
    CHECK(XUserGetMaxUsers(&max_users) == S_OK);
    CHECK_EQ(max_users, 1U);

    XUserLocalId local_id{};
    CHECK(XUserGetLocalId(user, &local_id) == S_OK);
    CHECK_EQ(local_id.value, kOfflineUserLocalId);
    XUserHandle found = nullptr;
    CHECK(XUserFindUserByLocalId(local_id, &found) == S_OK);
    CHECK_EQ(XUserCompare(user, found), 0);
    XUserCloseHandle(found);
    // The console's answer for another user, not the documented ERROR_NOT_FOUND.
    CHECK(XUserFindUserByLocalId(XUserLocalId{local_id.value + 1}, &found) == kGameUserNotFound);

    uint64_t id = 0;
    CHECK(XUserGetId(user, &id) == S_OK);
    CHECK_EQ(id, kOfflineUserId);
    CHECK(XUserFindUserById(id, &found) == S_OK);
    XUserCloseHandle(found);
    CHECK(XUserFindUserById(id + 1, &found) == kGameUserNotFound);

    bool guest = true;
    CHECK(XUserGetIsGuest(user, &guest) == S_OK);
    CHECK(!guest);
    auto state = XUserState::SignedOut;
    CHECK(XUserGetState(user, &state) == S_OK);
    CHECK(state == XUserState::SignedIn);
    auto age = XUserAgeGroup::Unknown;
    CHECK(XUserGetAgeGroup(user, &age) == S_OK);
    CHECK(age == XUserAgeGroup::Adult);
    CHECK(XUserIsStoreUser(user));

    // Any other handle is refused.
    auto* const other = reinterpret_cast<XUserHandle>(&state);
    CHECK(XUserGetId(other, &id) == E_INVALIDARG);
    CHECK(XUserGetState(other, &state) == E_INVALIDARG);
    CHECK(!XUserIsStoreUser(other));
    XUserCloseHandle(user);
}

TEST_CASE("the gamertag") {
    XUser* const user = add_user();
    char name[kGamertagUniqueModernMaxBytes] = {};
    size_t used = 0;
    CHECK(XUserGetGamertag(user, XUserGamertagComponent::Classic, kGamertagClassicMaxBytes, name,
                           &used) == S_OK);
    CHECK_EQ(std::string(name), std::string(kOfflineGamertag));
    CHECK_EQ(used, std::strlen(kOfflineGamertag) + 1);
    CHECK(XUserGetGamertag(user, XUserGamertagComponent::UniqueModern, sizeof(name), name, &used) ==
          S_OK);
    CHECK_EQ(std::string(name), std::string(kOfflineGamertag));
    // No suffix.
    CHECK(XUserGetGamertag(user, XUserGamertagComponent::ModernSuffix,
                           kGamertagModernSuffixMaxBytes, name, &used) == S_OK);
    CHECK(name[0] == '\0');
    CHECK_EQ(used, size_t{1});

    CHECK(XUserGetGamertag(user, XUserGamertagComponent::Modern, 3, name, &used) ==
          E_NOT_SUFFICIENT_BUFFER);
    CHECK(XUserGetGamertag(user, out_of_range<XUserGamertagComponent>(4), sizeof(name), name,
                           &used) == E_INVALIDARG);
    CHECK(XUserGetGamertag(user, XUserGamertagComponent::Modern, sizeof(name), nullptr, &used) ==
          E_INVALIDARG);
    XUserCloseHandle(user);
}

TEST_CASE("online privileges are denied, local ones granted") {
    XUser* const user = add_user();
    bool has = true;
    auto reason = XUserPrivilegeDenyReason::None;
    CHECK(XUserCheckPrivilege(user, XUserPrivilegeOptions::None, XUserPrivilege::Multiplayer, &has,
                              &reason) == S_OK);
    CHECK(!has);
    CHECK(reason == XUserPrivilegeDenyReason::Restricted);
    CHECK(XUserCheckPrivilege(user, XUserPrivilegeOptions::None, XUserPrivilege::CloudSavedGames,
                              &has, &reason) == S_OK);
    CHECK(has);
    CHECK(reason == XUserPrivilegeDenyReason::None);
    CHECK(XUserCheckPrivilege(user, XUserPrivilegeOptions::AllUsers, XUserPrivilege::GameDvr, &has,
                              nullptr) == S_OK);
    CHECK(has);
    // An unknown privilege is denied.
    CHECK(XUserCheckPrivilege(user, XUserPrivilegeOptions::None, out_of_range<XUserPrivilege>(1000),
                              &has, &reason) == S_OK);
    CHECK(!has);

    // Resolving without a dialog: granted is S_OK, denied is a dismissed dialog.
    const Queue queue;
    XAsyncBlock granted = block_on(queue);
    XAsyncBlock denied = block_on(queue);
    CHECK(XUserResolvePrivilegeWithUiAsync(user, XUserPrivilegeOptions::None,
                                           XUserPrivilege::UserGeneratedContent, &granted) == S_OK);
    CHECK(XUserResolvePrivilegeWithUiAsync(user, XUserPrivilegeOptions::None,
                                           XUserPrivilege::Communications, &denied) == S_OK);
    CHECK(XUserResolvePrivilegeWithUiResult(&granted) == E_PENDING);
    queue.finish();
    CHECK(XUserResolvePrivilegeWithUiResult(&granted) == S_OK);
    CHECK(XUserResolvePrivilegeWithUiResult(&denied) == E_ABORT);
    XUserCloseHandle(user);
}

TEST_CASE("calls that need the network or a dialog fail through the queue") {
    XUser* const user = add_user();
    const Queue queue;

    XAsyncBlock token = block_on(queue);
    CHECK(XUserGetTokenAndSignatureAsync(user, XUserGetTokenAndSignatureOptions::None, "GET",
                                         "https://example.invalid/", 0, nullptr, 0, nullptr,
                                         &token) == S_OK);
    XAsyncBlock token16 = block_on(queue);
    CHECK(XUserGetTokenAndSignatureUtf16Async(user, XUserGetTokenAndSignatureOptions::None, L"GET",
                                              L"https://example.invalid/", 0, nullptr, 0, nullptr,
                                              &token16) == S_OK);
    XAsyncBlock picture = block_on(queue);
    CHECK(XUserGetGamerPictureAsync(user, XUserGamerPictureSize::Small, &picture) == S_OK);
    XAsyncBlock issue = block_on(queue);
    CHECK(XUserResolveIssueWithUiAsync(user, "https://example.invalid/", &issue) == S_OK);
    XAsyncBlock issue16 = block_on(queue);
    CHECK(XUserResolveIssueWithUiUtf16Async(user, L"https://example.invalid/", &issue16) == S_OK);
    XAsyncBlock controller = block_on(queue);
    CHECK(XUserFindControllerForUserWithUiAsync(user, &controller) == S_OK);
    queue.finish();

    size_t size = 1;
    CHECK(XUserGetTokenAndSignatureResultSize(&token, &size) == kCannotConnect);
    char buffer[64] = {};
    auto* data = reinterpret_cast<XUserGetTokenAndSignatureData*>(1);
    CHECK(XUserGetTokenAndSignatureResult(&token, sizeof(buffer), buffer, &data, nullptr) ==
          kCannotConnect);
    CHECK(data == nullptr);
    XUserGetTokenAndSignatureUtf16Data* data16 = nullptr;
    CHECK(XUserGetTokenAndSignatureUtf16Result(&token16, sizeof(buffer), buffer, &data16,
                                               nullptr) == kCannotConnect);
    CHECK(XUserGetGamerPictureResultSize(&picture, &size) == kCannotConnect);
    CHECK(XUserGetGamerPictureResult(&picture, sizeof(buffer), buffer, nullptr) == kCannotConnect);
    CHECK(XUserResolveIssueWithUiResult(&issue) == E_ABORT);
    CHECK(XUserResolveIssueWithUiUtf16Result(&issue16) == E_ABORT);
    AppLocalDeviceId device{};
    CHECK(XUserFindControllerForUserWithUiResult(&controller, &device) == E_ABORT);
    XUserCloseHandle(user);
}

TEST_CASE("registrations are kept and never called") {
    XTaskQueueRegistrationToken first{};
    XTaskQueueRegistrationToken second{};
    CHECK(XUserRegisterForChangeEvent(nullptr, nullptr, &on_change, &first) == S_OK);
    CHECK(XUserRegisterForDeviceAssociationChanged(nullptr, nullptr, &on_device, &second) == S_OK);
    CHECK(first.token != second.token);
    CHECK(XUserUnregisterForChangeEvent(first, true));
    CHECK(XUserUnregisterForDeviceAssociationChanged(second, false));
    CHECK(XUserRegisterForChangeEvent(nullptr, nullptr, nullptr, &first) == E_INVALIDARG);
    CHECK(XUserRegisterForChangeEvent(nullptr, nullptr, &on_change, nullptr) == E_INVALIDARG);

    // No sign-out ever comes, so there is never a deferral.
    auto* deferral = reinterpret_cast<XUserSignOutDeferralHandle>(1);
    CHECK(XUserGetSignOutDeferral(&deferral) == kGameUserDeferralNotAvailable);
    CHECK(deferral == nullptr);
    XUserCloseSignOutDeferralHandle(deferral);
}

TEST_CASE("every device belongs to the offline user") {
    XUser* const user = add_user();
    AppLocalDeviceId device{};
    device.value[0] = 7;
    XUserHandle owner = nullptr;
    CHECK(XUserFindForDevice(&device, &owner) == S_OK);
    CHECK_EQ(XUserCompare(user, owner), 0);
    XUserCloseHandle(owner);
    CHECK(XUserFindForDevice(nullptr, &owner) == E_INVALIDARG);

    wchar_t endpoint[64] = {};
    size_t used = 0;
    CHECK(XUserGetDefaultAudioEndpointUtf16(user,
                                            XUserDefaultAudioEndpointKind::CommunicationRender, 64,
                                            endpoint, &used) == kNotFound);
    XUserCloseHandle(user);
}

TEST_CASE("XUser through its interfaces' slots") {
    void* const users = service(kUserClassId, kUserInterfaceId);
    void* const gamertags = service(kUserClassId, kUserGamertagInterfaceId);
    void* const devices = service(kUserDeviceClassId, kUserDeviceInterfaceId);
    CHECK(users != gamertags);

    // Within the class, each object answers for the other interface and for IUnknown.
    void* object = nullptr;
    CHECK(call<HRESULT>(users, 0, &kUserGamertagInterfaceId, &object) == S_OK);
    CHECK(object == gamertags);
    CHECK(call<HRESULT>(gamertags, 0, &kUserInterfaceId, &object) == S_OK);
    CHECK(object == users);
    constexpr GUID kUnknownId = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    CHECK(QueryApiImpl(&kUserClassId, &kUnknownId, &object) == S_OK);
    CHECK(object == users);
    CHECK(call<HRESULT>(users, 0, &kUserDeviceInterfaceId, &object) == E_NOINTERFACE);
    CHECK(QueryApiImpl(&kUserClassId, &kThreadingId, &object) == E_NOINTERFACE);
    CHECK(QueryApiImpl(&kUserDeviceClassId, &kUserInterfaceId, &object) == E_NOINTERFACE);

    // Add the user (slots 7 and 8), then read it back (11, 14) and its gamertag.
    const Queue queue;
    XAsyncBlock async = block_on(queue);
    CHECK(call<HRESULT>(users, 7, XUserAddOptions::AddDefaultUserSilently, &async) == S_OK);
    queue.finish();
    XUserHandle user = nullptr;
    REQUIRE(call<HRESULT>(users, 8, &async, &user) == S_OK);
    uint64_t id = 0;
    CHECK(call<HRESULT>(users, 11, user, &id) == S_OK);
    CHECK_EQ(id, kOfflineUserId);
    auto state = XUserState::SignedOut;
    CHECK(call<HRESULT>(users, 14, user, &state) == S_OK);
    CHECK(state == XUserState::SignedIn);
    const bool store_user = call<bool>(users, 42, user);
    CHECK(store_user);
    char name[kGamertagModernMaxBytes] = {};
    size_t used = 0;
    CHECK(call<HRESULT>(gamertags, 3, user, XUserGamertagComponent::Modern, sizeof(name), &name[0],
                        &used) == S_OK);
    CHECK_EQ(std::string(name), std::string(kOfflineGamertag));

    // The slots without a function fail.
    CHECK(call<HRESULT>(users, 15) == E_NOTIMPL);
    CHECK(call<HRESULT>(users, 37) == E_NOTIMPL);
    CHECK(call<HRESULT>(users, 46) == E_NOTIMPL);

    // The device interface: slot 3 finds the device's user.
    AppLocalDeviceId device{};
    XUserHandle owner = nullptr;
    CHECK(call<HRESULT>(devices, 3, &device, &owner) == S_OK);
    CHECK_EQ(call<int32_t>(users, 5, user, owner), 0);
    call<void>(users, 4, owner);
    call<void>(users, 4, user);
}
