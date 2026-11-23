#pragma once
// XUser for one local offline user (docs/gdk.md, "XUser"). The user is always signed in, every
// request to add a user returns it, and nothing that needs the Xbox network succeeds (hard rule 5).
// Types, values, and signatures are the public GDK's.

#include <cstddef>
#include <cstdint>
#include <windows.h>

#include "g6/gdk/async.h"
#include "g6/gdk/task_queue.h"

namespace g6::gdk {

struct XUser;
using XUserHandle = XUser*;
struct XUserSignOutDeferral;
using XUserSignOutDeferralHandle = XUserSignOutDeferral*;

struct XUserLocalId {
    uint64_t value;
};

// GameInput's device ID.
struct AppLocalDeviceId {
    uint8_t value[32];
};

enum class XUserAddOptions : uint32_t {
    None = 0x00,
    AddDefaultUserSilently = 0x01,
    AllowGuests = 0x02,
    AddDefaultUserAllowingUI = 0x04,
};

enum class XUserState : uint32_t {
    SignedIn,
    SigningOut,
    SignedOut,
};

enum class XUserAgeGroup : uint32_t {
    Unknown,
    Child,
    Teen,
    Adult,
};

enum class XUserGamertagComponent : uint32_t {
    Classic,
    Modern,
    ModernSuffix,
    UniqueModern,
};

// The buffer each gamertag component needs at most, with its terminating null.
inline constexpr size_t kGamertagClassicMaxBytes = 16;
inline constexpr size_t kGamertagModernMaxBytes = 97;
inline constexpr size_t kGamertagModernSuffixMaxBytes = 15;
inline constexpr size_t kGamertagUniqueModernMaxBytes = 101;

enum class XUserGamerPictureSize : uint32_t {
    Small,
    Medium,
    Large,
    ExtraLarge,
};

enum class XUserPrivilegeOptions : uint32_t {
    None = 0x00,
    AllUsers = 0x01,
};

enum class XUserPrivilege : uint32_t {
    CrossPlay = 185,
    Clubs = 188,
    Sessions = 189,
    Broadcast = 190,
    ManageProfilePrivacy = 196,
    GameDvr = 198,
    MultiplayerParties = 203,
    CloudManageSession = 207,
    CloudJoinSession = 208,
    CloudSavedGames = 209,
    SocialNetworkSharing = 220,
    UserGeneratedContent = 247,
    Communications = 252,
    Multiplayer = 254,
    AddFriends = 255,
};

enum class XUserPrivilegeDenyReason : uint32_t {
    None = 0,
    PurchaseRequired = 1,
    Restricted = 2,
    Banned = 3,
    Unknown = 0xFFFFFFFF,
};

enum class XUserChangeEvent : uint32_t {
    SignedInAgain,
    SigningOut,
    SignedOut,
    Gamertag,
    GamerPicture,
    Privileges,
};

enum class XUserGetTokenAndSignatureOptions : uint32_t {
    None = 0x00,
    ForceRefresh = 0x01,
    AllUsers = 0x02,
};

enum class XUserDefaultAudioEndpointKind : uint32_t {
    CommunicationRender,
    CommunicationCapture,
};

struct XUserGetTokenAndSignatureHttpHeader {
    const char* name;
    const char* value;
};

struct XUserGetTokenAndSignatureUtf16HttpHeader {
    const wchar_t* name;
    const wchar_t* value;
};

struct XUserGetTokenAndSignatureData {
    size_t token_size;
    size_t signature_size;
    const char* token;
    const char* signature;
};

struct XUserGetTokenAndSignatureUtf16Data {
    size_t token_count;
    size_t signature_count;
    const wchar_t* token;
    const wchar_t* signature;
};

struct XUserDeviceAssociationChange;
using XUserChangeEventCallback = void CALLBACK(void* context, XUserLocalId user,
                                               XUserChangeEvent event);
using XUserDeviceAssociationChangedCallback = void
    CALLBACK(void* context, const XUserDeviceAssociationChange* change);
using XUserDefaultAudioEndpointUtf16ChangedCallback = void
    CALLBACK(void* context, XUserLocalId user, XUserDefaultAudioEndpointKind kind,
             const wchar_t* endpoint_id);

// XUser's errors (the public GDK's XGameErr.h).
inline constexpr HRESULT kGameUserDeferralNotAvailable = static_cast<HRESULT>(0x89245103);
inline constexpr HRESULT kGameUserNotFound = static_cast<HRESULT>(0x89245104);
// What a call that needs the Xbox network fails with, as on a console that can't reach it:
// HRESULT_FROM_WIN32(ERROR_INTERNET_CANNOT_CONNECT). Titles take their offline paths on it.
inline constexpr HRESULT kCannotConnect = static_cast<HRESULT>(0x80072EFD);

// The one user. The handle is the same for every reference; the count is kept for the log.
inline constexpr uint64_t kOfflineUserId = 0x0009000000000001; // in the range Xbox user IDs use
inline constexpr uint64_t kOfflineUserLocalId = 1;
inline constexpr const char* kOfflineGamertag = "Player";

// Whether `user` is the offline user's handle, for the other services. Logs nothing.
bool is_offline_user(XUserHandle user);

// Handles. Every function that takes a user fails with E_INVALIDARG for any other handle.
HRESULT XUserDuplicateHandle(XUserHandle handle, XUserHandle* duplicated);
void XUserCloseHandle(XUserHandle handle);
int32_t XUserCompare(XUserHandle first, XUserHandle second);
HRESULT XUserGetMaxUsers(uint32_t* max_users);

// Adding the user: any options give the one user, without UI, through the queue.
HRESULT XUserAddAsync(XUserAddOptions options, XAsyncBlock* async);
HRESULT XUserAddResult(XAsyncBlock* async, XUserHandle* user);

HRESULT XUserGetLocalId(XUserHandle user, XUserLocalId* local_id);
HRESULT XUserFindUserByLocalId(XUserLocalId local_id, XUserHandle* user);
HRESULT XUserGetId(XUserHandle user, uint64_t* user_id);
HRESULT XUserFindUserById(uint64_t user_id, XUserHandle* user);
HRESULT XUserGetIsGuest(XUserHandle user, bool* is_guest);
HRESULT XUserGetState(XUserHandle user, XUserState* state);
// UTF-8, with the terminating null counted in `used`. E_NOT_SUFFICIENT_BUFFER when the buffer is
// too small for this name, whatever the component's maximum.
HRESULT XUserGetGamertag(XUserHandle user, XUserGamertagComponent component, size_t size,
                         char* gamertag, size_t* used);

// No picture: the call fails with kCannotConnect through the queue.
HRESULT XUserGetGamerPictureAsync(XUserHandle user, XUserGamerPictureSize size, XAsyncBlock* async);
HRESULT XUserGetGamerPictureResultSize(XAsyncBlock* async, size_t* size);
HRESULT XUserGetGamerPictureResult(XAsyncBlock* async, size_t size, void* buffer, size_t* used);

HRESULT XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* age_group);
// The online privileges are denied as Restricted; game DVR, saved games, and user-generated
// content are granted (docs/gdk.md, "XUser"). An unknown privilege is denied and logged once.
HRESULT XUserCheckPrivilege(XUserHandle user, XUserPrivilegeOptions options,
                            XUserPrivilege privilege, bool* has_privilege,
                            XUserPrivilegeDenyReason* reason);
// No UI: a granted privilege resolves with S_OK, a denied one with E_ABORT, as a dismissed
// dialog gives.
HRESULT XUserResolvePrivilegeWithUiAsync(XUserHandle user, XUserPrivilegeOptions options,
                                         XUserPrivilege privilege, XAsyncBlock* async);
HRESULT XUserResolvePrivilegeWithUiResult(XAsyncBlock* async);

// Xbox network tokens: every request fails with kCannotConnect through the queue.
HRESULT XUserGetTokenAndSignatureAsync(XUserHandle user, XUserGetTokenAndSignatureOptions options,
                                       const char* method, const char* url, size_t header_count,
                                       const XUserGetTokenAndSignatureHttpHeader* headers,
                                       size_t body_size, const void* body, XAsyncBlock* async);
HRESULT XUserGetTokenAndSignatureResultSize(XAsyncBlock* async, size_t* size);
HRESULT XUserGetTokenAndSignatureResult(XAsyncBlock* async, size_t size, void* buffer,
                                        XUserGetTokenAndSignatureData** data, size_t* used);
HRESULT XUserGetTokenAndSignatureUtf16Async(XUserHandle user,
                                            XUserGetTokenAndSignatureOptions options,
                                            const wchar_t* method, const wchar_t* url,
                                            size_t header_count,
                                            const XUserGetTokenAndSignatureUtf16HttpHeader* headers,
                                            size_t body_size, const void* body, XAsyncBlock* async);
HRESULT XUserGetTokenAndSignatureUtf16ResultSize(XAsyncBlock* async, size_t* size);
HRESULT XUserGetTokenAndSignatureUtf16Result(XAsyncBlock* async, size_t size, void* buffer,
                                             XUserGetTokenAndSignatureUtf16Data** data,
                                             size_t* used);

// No UI: E_ABORT through the queue, as a dismissed dialog gives.
HRESULT XUserResolveIssueWithUiAsync(XUserHandle user, const char* url, XAsyncBlock* async);
HRESULT XUserResolveIssueWithUiResult(XAsyncBlock* async);
HRESULT XUserResolveIssueWithUiUtf16Async(XUserHandle user, const wchar_t* url, XAsyncBlock* async);
HRESULT XUserResolveIssueWithUiUtf16Result(XAsyncBlock* async);

// The user never changes, so a registered callback is kept until it is unregistered and never
// called. Unregistering always succeeds.
HRESULT XUserRegisterForChangeEvent(XTaskQueueHandle queue, void* context,
                                    XUserChangeEventCallback* callback,
                                    XTaskQueueRegistrationToken* token);
bool XUserUnregisterForChangeEvent(XTaskQueueRegistrationToken token, bool wait);
// Only inside a sign-out callback, which never comes: kGameUserDeferralNotAvailable.
HRESULT XUserGetSignOutDeferral(XUserSignOutDeferralHandle* deferral);
void XUserCloseSignOutDeferralHandle(XUserSignOutDeferralHandle deferral);
// The one user owns the game's licence.
bool XUserIsStoreUser(XUserHandle user);

// Devices. Every device belongs to the one user. No device association or audio endpoint
// change is reported yet, so those callbacks are kept and never called.
HRESULT XUserFindForDevice(const AppLocalDeviceId* device, XUserHandle* user);
HRESULT XUserRegisterForDeviceAssociationChanged(XTaskQueueHandle queue, void* context,
                                                 XUserDeviceAssociationChangedCallback* callback,
                                                 XTaskQueueRegistrationToken* token);
bool XUserUnregisterForDeviceAssociationChanged(XTaskQueueRegistrationToken token, bool wait);
// No chat endpoint: HRESULT_FROM_WIN32(ERROR_NOT_FOUND).
HRESULT XUserGetDefaultAudioEndpointUtf16(XUserHandle user, XUserDefaultAudioEndpointKind kind,
                                          size_t count, wchar_t* endpoint_id, size_t* used);
HRESULT XUserRegisterForDefaultAudioEndpointUtf16Changed(
    XTaskQueueHandle queue, void* context, XUserDefaultAudioEndpointUtf16ChangedCallback* callback,
    XTaskQueueRegistrationToken* token);
bool XUserUnregisterForDefaultAudioEndpointUtf16Changed(XTaskQueueRegistrationToken token,
                                                        bool wait);
// No pairing dialog: E_ABORT through the queue, as a dismissed one gives.
HRESULT XUserFindControllerForUserWithUiAsync(XUserHandle user, XAsyncBlock* async);
HRESULT XUserFindControllerForUserWithUiResult(XAsyncBlock* async, AppLocalDeviceId* device);

} // namespace g6::gdk
