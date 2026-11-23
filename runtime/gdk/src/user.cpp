#include "g6/gdk/user.h"

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_set>

#include "g6/log.h"

// One local offline user, always signed in (docs/gdk.md, "XUser"). Every handle is the same
// object, so comparing handles compares users. Calls that would need the Xbox network or a
// system dialog fail the way an offline console or a dismissed dialog fails them, through the
// caller's queue like any other call.

namespace g6::gdk {

struct XUser {
    int unused;
};

namespace {

XUser g_user{};
std::atomic<int64_t> g_references{0};

// HRESULT_FROM_WIN32(ERROR_NOT_FOUND).
constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80070490);

XUserHandle add_reference() {
    g_references.fetch_add(1, std::memory_order_relaxed);
    return &g_user;
}

bool is_user(XUserHandle user) {
    if (user == &g_user) {
        return true;
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        LOG_WARN(gdk, "XUser: {} is not the offline user's handle; failing with E_INVALIDARG",
                 static_cast<const void*>(user));
    }
    return false;
}

// Logs `what` the first time `reported` is set.
void report_once(std::atomic<bool>& reported, std::string_view what) {
    if (!reported.exchange(true)) {
        LOG_INFO(gdk, "{}", what);
    }
}

// ---- Registrations ----

// Callbacks registered for events the offline user never has. They are kept so that
// unregistering finds them, and never called.
struct Registrations {
    std::mutex mutex;
    uint64_t next = 1;
    std::unordered_set<uint64_t> live;
};

Registrations& registrations() {
    static Registrations instance;
    return instance;
}

HRESULT add_registration(const void* callback, XTaskQueueRegistrationToken* token) {
    if (callback == nullptr || token == nullptr) {
        return E_INVALIDARG;
    }
    Registrations& all = registrations();
    const std::scoped_lock lock(all.mutex);
    token->token = all.next++;
    all.live.insert(token->token);
    return S_OK;
}

bool remove_registration(XTaskQueueRegistrationToken token) {
    Registrations& all = registrations();
    const std::scoped_lock lock(all.mutex);
    all.live.erase(token.token);
    return true;
}

// ---- Asynchronous calls ----

// A call with no result that ends with `Result` on the queue's work port.
template <HRESULT Result>
HRESULT CALLBACK ending_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork: XAsyncComplete(data->async, Result, 0); return S_OK;
    case XAsyncOp::GetResult:
    case XAsyncOp::Cancel:
    case XAsyncOp::Cleanup: return S_OK;
    }
    return S_OK;
}

// XUserAddAsync: the result is a new reference to the user, taken when it is fetched.
HRESULT CALLBACK add_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork: XAsyncComplete(data->async, S_OK, sizeof(XUserHandle)); return S_OK;
    case XAsyncOp::GetResult:
        *static_cast<XUserHandle*>(data->buffer) = add_reference();
        return S_OK;
    case XAsyncOp::Cancel:
    case XAsyncOp::Cleanup: return S_OK;
    }
    return S_OK;
}

template <auto Function>
const void* identity() {
    return reinterpret_cast<const void*>(Function);
}

// The status of a call with no result, which ends it.
template <auto Function>
HRESULT no_result(XAsyncBlock* async) {
    return XAsyncGetResult(async, identity<Function>(), 0, nullptr, nullptr);
}

// ---- Privileges ----

// Online privileges are denied; the rest a single-player game may check is granted.
bool grants(XUserPrivilege privilege) {
    switch (privilege) {
    case XUserPrivilege::GameDvr:
    case XUserPrivilege::CloudSavedGames:
    case XUserPrivilege::UserGeneratedContent: return true;
    case XUserPrivilege::CrossPlay:
    case XUserPrivilege::Clubs:
    case XUserPrivilege::Sessions:
    case XUserPrivilege::Broadcast:
    case XUserPrivilege::ManageProfilePrivacy:
    case XUserPrivilege::MultiplayerParties:
    case XUserPrivilege::CloudManageSession:
    case XUserPrivilege::CloudJoinSession:
    case XUserPrivilege::SocialNetworkSharing:
    case XUserPrivilege::Communications:
    case XUserPrivilege::Multiplayer:
    case XUserPrivilege::AddFriends: return false;
    }
    static std::array<std::atomic<uint64_t>, 4> reported{};
    const auto number = static_cast<uint32_t>(privilege);
    const uint64_t bit = uint64_t{1} << (number % 64);
    if (number >= 256 || (reported[number / 64].fetch_or(bit) & bit) == 0) {
        LOG_WARN(gdk, "XUserCheckPrivilege: unknown privilege {}; denied", number);
    }
    return false;
}

} // namespace

// ---- Handles ----

HRESULT XUserDuplicateHandle(XUserHandle handle, XUserHandle* duplicated) {
    if (duplicated == nullptr || !is_user(handle)) {
        return E_INVALIDARG;
    }
    *duplicated = add_reference();
    return S_OK;
}

void XUserCloseHandle(XUserHandle handle) {
    if (!is_user(handle)) {
        return;
    }
    if (g_references.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            LOG_WARN(gdk, "XUserCloseHandle: the offline user's handle was closed more often "
                          "than it was handed out");
        }
    }
}

int32_t XUserCompare(XUserHandle first, XUserHandle second) {
    if (first == second) {
        return 0;
    }
    return std::less<>{}(first, second) ? -1 : 1;
}

HRESULT XUserGetMaxUsers(uint32_t* max_users) {
    if (max_users == nullptr) {
        return E_INVALIDARG;
    }
    *max_users = 1;
    return S_OK;
}

// ---- Adding the user ----

HRESULT XUserAddAsync(XUserAddOptions options, XAsyncBlock* async) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        LOG_INFO(gdk, "XUserAddAsync (options {:#x}): the offline user",
                 static_cast<uint32_t>(options));
    }
    return XAsyncBegin(async, nullptr, identity<&XUserAddAsync>(), "XUserAddAsync", &add_provider);
}

HRESULT XUserAddResult(XAsyncBlock* async, XUserHandle* user) {
    if (user == nullptr) {
        return E_INVALIDARG;
    }
    return XAsyncGetResult(async, identity<&XUserAddAsync>(), sizeof(XUserHandle),
                           static_cast<void*>(user), nullptr);
}

// ---- Identity ----

HRESULT XUserGetLocalId(XUserHandle user, XUserLocalId* local_id) {
    if (local_id == nullptr || !is_user(user)) {
        return E_INVALIDARG;
    }
    local_id->value = kOfflineUserLocalId;
    return S_OK;
}

HRESULT XUserFindUserByLocalId(XUserLocalId local_id, XUserHandle* user) {
    if (user == nullptr) {
        return E_INVALIDARG;
    }
    if (local_id.value != kOfflineUserLocalId) {
        return kGameUserNotFound;
    }
    *user = add_reference();
    return S_OK;
}

HRESULT XUserGetId(XUserHandle user, uint64_t* user_id) {
    if (user_id == nullptr || !is_user(user)) {
        return E_INVALIDARG;
    }
    *user_id = kOfflineUserId;
    return S_OK;
}

HRESULT XUserFindUserById(uint64_t user_id, XUserHandle* user) {
    if (user == nullptr) {
        return E_INVALIDARG;
    }
    if (user_id != kOfflineUserId) {
        return kGameUserNotFound;
    }
    *user = add_reference();
    return S_OK;
}

HRESULT XUserGetIsGuest(XUserHandle user, bool* is_guest) {
    if (is_guest == nullptr || !is_user(user)) {
        return E_INVALIDARG;
    }
    *is_guest = false;
    return S_OK;
}

HRESULT XUserGetState(XUserHandle user, XUserState* state) {
    if (state == nullptr || !is_user(user)) {
        return E_INVALIDARG;
    }
    *state = XUserState::SignedIn;
    return S_OK;
}

HRESULT XUserGetGamertag(XUserHandle user, XUserGamertagComponent component, size_t size,
                         char* gamertag, size_t* used) {
    if (gamertag == nullptr || !is_user(user) ||
        static_cast<uint32_t>(component) >
            static_cast<uint32_t>(XUserGamertagComponent::UniqueModern)) {
        return E_INVALIDARG;
    }
    // A gamertag without a suffix: the modern and unique forms are the classic one.
    const std::string_view text =
        component == XUserGamertagComponent::ModernSuffix ? "" : kOfflineGamertag;
    if (size < text.size() + 1) {
        return E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(gamertag, text.data(), text.size());
    gamertag[text.size()] = '\0';
    if (used != nullptr) {
        *used = text.size() + 1;
    }
    return S_OK;
}

// ---- Gamer picture ----

HRESULT XUserGetGamerPictureAsync(XUserHandle user, XUserGamerPictureSize /*size*/,
                                  XAsyncBlock* async) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    static std::atomic<bool> reported{false};
    report_once(reported, "XUserGetGamerPictureAsync: the offline user has no picture");
    return XAsyncBegin(async, nullptr, identity<&XUserGetGamerPictureAsync>(),
                       "XUserGetGamerPictureAsync", &ending_provider<kCannotConnect>);
}

HRESULT XUserGetGamerPictureResultSize(XAsyncBlock* async, size_t* size) {
    return XAsyncGetResultSize(async, size);
}

HRESULT XUserGetGamerPictureResult(XAsyncBlock* async, size_t size, void* buffer, size_t* used) {
    return XAsyncGetResult(async, identity<&XUserGetGamerPictureAsync>(), size, buffer, used);
}

// ---- Age and privileges ----

HRESULT XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* age_group) {
    if (age_group == nullptr || !is_user(user)) {
        return E_INVALIDARG;
    }
    *age_group = XUserAgeGroup::Adult;
    return S_OK;
}

HRESULT XUserCheckPrivilege(XUserHandle user, XUserPrivilegeOptions /*options*/,
                            XUserPrivilege privilege, bool* has_privilege,
                            XUserPrivilegeDenyReason* reason) {
    if (has_privilege == nullptr || !is_user(user)) {
        return E_INVALIDARG;
    }
    const bool granted = grants(privilege);
    *has_privilege = granted;
    if (reason != nullptr) {
        *reason = granted ? XUserPrivilegeDenyReason::None : XUserPrivilegeDenyReason::Restricted;
    }
    return S_OK;
}

HRESULT XUserResolvePrivilegeWithUiAsync(XUserHandle user, XUserPrivilegeOptions /*options*/,
                                         XUserPrivilege privilege, XAsyncBlock* async) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    XAsyncProvider* provider =
        grants(privilege) ? &ending_provider<S_OK> : &ending_provider<E_ABORT>;
    return XAsyncBegin(async, nullptr, identity<&XUserResolvePrivilegeWithUiAsync>(),
                       "XUserResolvePrivilegeWithUiAsync", provider);
}

HRESULT XUserResolvePrivilegeWithUiResult(XAsyncBlock* async) {
    return no_result<&XUserResolvePrivilegeWithUiAsync>(async);
}

// ---- Tokens ----

HRESULT
XUserGetTokenAndSignatureAsync(XUserHandle user, XUserGetTokenAndSignatureOptions /*options*/,
                               const char* /*method*/, const char* url, size_t /*header_count*/,
                               const XUserGetTokenAndSignatureHttpHeader* /*headers*/,
                               size_t /*body_size*/, const void* /*body*/, XAsyncBlock* async) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        LOG_INFO(gdk, "XUserGetTokenAndSignatureAsync ({}): offline, no token",
                 url != nullptr ? url : "no URL");
    }
    return XAsyncBegin(async, nullptr, identity<&XUserGetTokenAndSignatureAsync>(),
                       "XUserGetTokenAndSignatureAsync", &ending_provider<kCannotConnect>);
}

HRESULT XUserGetTokenAndSignatureResultSize(XAsyncBlock* async, size_t* size) {
    return XAsyncGetResultSize(async, size);
}

HRESULT XUserGetTokenAndSignatureResult(XAsyncBlock* async, size_t size, void* buffer,
                                        XUserGetTokenAndSignatureData** data, size_t* used) {
    if (data != nullptr) {
        *data = nullptr;
    }
    return XAsyncGetResult(async, identity<&XUserGetTokenAndSignatureAsync>(), size, buffer, used);
}

HRESULT XUserGetTokenAndSignatureUtf16Async(
    XUserHandle user, XUserGetTokenAndSignatureOptions /*options*/, const wchar_t* /*method*/,
    const wchar_t* /*url*/, size_t /*header_count*/,
    const XUserGetTokenAndSignatureUtf16HttpHeader* /*headers*/, size_t /*body_size*/,
    const void* /*body*/, XAsyncBlock* async) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    static std::atomic<bool> reported{false};
    report_once(reported, "XUserGetTokenAndSignatureUtf16Async: offline, no token");
    return XAsyncBegin(async, nullptr, identity<&XUserGetTokenAndSignatureUtf16Async>(),
                       "XUserGetTokenAndSignatureUtf16Async", &ending_provider<kCannotConnect>);
}

HRESULT XUserGetTokenAndSignatureUtf16ResultSize(XAsyncBlock* async, size_t* size) {
    return XAsyncGetResultSize(async, size);
}

HRESULT XUserGetTokenAndSignatureUtf16Result(XAsyncBlock* async, size_t size, void* buffer,
                                             XUserGetTokenAndSignatureUtf16Data** data,
                                             size_t* used) {
    if (data != nullptr) {
        *data = nullptr;
    }
    return XAsyncGetResult(async, identity<&XUserGetTokenAndSignatureUtf16Async>(), size, buffer,
                           used);
}

// ---- Issues ----

HRESULT XUserResolveIssueWithUiAsync(XUserHandle user, const char* /*url*/, XAsyncBlock* async) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    return XAsyncBegin(async, nullptr, identity<&XUserResolveIssueWithUiAsync>(),
                       "XUserResolveIssueWithUiAsync", &ending_provider<E_ABORT>);
}

HRESULT XUserResolveIssueWithUiResult(XAsyncBlock* async) {
    return no_result<&XUserResolveIssueWithUiAsync>(async);
}

HRESULT XUserResolveIssueWithUiUtf16Async(XUserHandle user, const wchar_t* /*url*/,
                                          XAsyncBlock* async) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    return XAsyncBegin(async, nullptr, identity<&XUserResolveIssueWithUiUtf16Async>(),
                       "XUserResolveIssueWithUiUtf16Async", &ending_provider<E_ABORT>);
}

HRESULT XUserResolveIssueWithUiUtf16Result(XAsyncBlock* async) {
    return no_result<&XUserResolveIssueWithUiUtf16Async>(async);
}

// ---- Change events and sign-out ----

HRESULT XUserRegisterForChangeEvent(XTaskQueueHandle /*queue*/, void* /*context*/,
                                    XUserChangeEventCallback* callback,
                                    XTaskQueueRegistrationToken* token) {
    return add_registration(reinterpret_cast<const void*>(callback), token);
}

bool XUserUnregisterForChangeEvent(XTaskQueueRegistrationToken token, bool /*wait*/) {
    return remove_registration(token);
}

HRESULT XUserGetSignOutDeferral(XUserSignOutDeferralHandle* deferral) {
    if (deferral == nullptr) {
        return E_INVALIDARG;
    }
    *deferral = nullptr;
    return kGameUserDeferralNotAvailable;
}

void XUserCloseSignOutDeferralHandle(XUserSignOutDeferralHandle /*deferral*/) {}

bool XUserIsStoreUser(XUserHandle user) {
    return is_user(user);
}

// ---- Devices ----

HRESULT XUserFindForDevice(const AppLocalDeviceId* device, XUserHandle* user) {
    if (device == nullptr || user == nullptr) {
        return E_INVALIDARG;
    }
    *user = add_reference();
    return S_OK;
}

HRESULT XUserRegisterForDeviceAssociationChanged(XTaskQueueHandle /*queue*/, void* /*context*/,
                                                 XUserDeviceAssociationChangedCallback* callback,
                                                 XTaskQueueRegistrationToken* token) {
    return add_registration(reinterpret_cast<const void*>(callback), token);
}

bool XUserUnregisterForDeviceAssociationChanged(XTaskQueueRegistrationToken token, bool /*wait*/) {
    return remove_registration(token);
}

HRESULT XUserGetDefaultAudioEndpointUtf16(XUserHandle user, XUserDefaultAudioEndpointKind kind,
                                          size_t /*count*/, wchar_t* /*endpoint_id*/,
                                          size_t* /*used*/) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        LOG_INFO(gdk, "XUserGetDefaultAudioEndpointUtf16 (kind {}): no chat endpoint",
                 static_cast<uint32_t>(kind));
    }
    return kNotFound;
}

HRESULT XUserRegisterForDefaultAudioEndpointUtf16Changed(
    XTaskQueueHandle /*queue*/, void* /*context*/,
    XUserDefaultAudioEndpointUtf16ChangedCallback* callback, XTaskQueueRegistrationToken* token) {
    return add_registration(reinterpret_cast<const void*>(callback), token);
}

bool XUserUnregisterForDefaultAudioEndpointUtf16Changed(XTaskQueueRegistrationToken token,
                                                        bool /*wait*/) {
    return remove_registration(token);
}

HRESULT XUserFindControllerForUserWithUiAsync(XUserHandle user, XAsyncBlock* async) {
    if (!is_user(user)) {
        return E_INVALIDARG;
    }
    return XAsyncBegin(async, nullptr, identity<&XUserFindControllerForUserWithUiAsync>(),
                       "XUserFindControllerForUserWithUiAsync", &ending_provider<E_ABORT>);
}

HRESULT XUserFindControllerForUserWithUiResult(XAsyncBlock* async, AppLocalDeviceId* device) {
    if (device == nullptr) {
        return E_INVALIDARG;
    }
    return XAsyncGetResult(async, identity<&XUserFindControllerForUserWithUiAsync>(),
                           sizeof(AppLocalDeviceId), device, nullptr);
}

} // namespace g6::gdk
