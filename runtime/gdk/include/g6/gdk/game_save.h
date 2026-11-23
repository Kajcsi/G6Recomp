#pragma once
// XGameSave on the per-user data directory (docs/gdk.md, "XGameSave"). Containers and blobs are
// kept on the user's machine only: nothing syncs anywhere (hard rule 5), so no container ever
// needs a sync. Types, values, and signatures are the public GDK's.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <windows.h>

#include "g6/gdk/async.h"
#include "g6/gdk/user.h"

namespace g6::gdk {

struct XGameSaveProvider;
using XGameSaveProviderHandle = XGameSaveProvider*;
struct XGameSaveContainer;
using XGameSaveContainerHandle = XGameSaveContainer*;
struct XGameSaveUpdate;
using XGameSaveUpdateHandle = XGameSaveUpdate*;

struct XGameSaveContainerInfo {
    const char* name;
    const char* display_name;
    uint32_t blob_count;
    uint64_t total_size;
    int64_t last_modified_time; // time_t: seconds since 1970, UTC
    bool needs_sync;
};
static_assert(sizeof(XGameSaveContainerInfo) == 48);

struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};
static_assert(sizeof(XGameSaveBlobInfo) == 16);

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};
static_assert(sizeof(XGameSaveBlob) == 24);

// Return false to stop the enumeration.
using XGameSaveContainerInfoCallback = bool CALLBACK(const XGameSaveContainerInfo* info,
                                                     void* context);
using XGameSaveBlobInfoCallback = bool CALLBACK(const XGameSaveBlobInfo* info, void* context);

// GS_MAX_BLOB_SIZE: the most one blob, and one whole update, may write.
inline constexpr size_t kGameSaveMaxUpdateSize = size_t{16} * 1024 * 1024;
// The longest container name.
inline constexpr size_t kGameSaveMaxContainerName = 256;
// The default per-user quota of a title.
inline constexpr int64_t kGameSaveQuota = int64_t{256} * 1024 * 1024;

// XGameSave's errors (the public GDK's XGameErr.h).
inline constexpr HRESULT kGameSaveInvalidContainerName = static_cast<HRESULT>(0x80830001);
inline constexpr HRESULT kGameSaveNoAccess = static_cast<HRESULT>(0x80830002);
inline constexpr HRESULT kGameSaveOutOfLocalStorage = static_cast<HRESULT>(0x80830003);
inline constexpr HRESULT kGameSaveUserCanceled = static_cast<HRESULT>(0x80830004);
inline constexpr HRESULT kGameSaveUpdateTooBig = static_cast<HRESULT>(0x80830005);
inline constexpr HRESULT kGameSaveQuotaExceeded = static_cast<HRESULT>(0x80830006);
inline constexpr HRESULT kGameSaveProvidedBufferTooSmall = static_cast<HRESULT>(0x80830007);
inline constexpr HRESULT kGameSaveBlobNotFound = static_cast<HRESULT>(0x80830008);
inline constexpr HRESULT kGameSaveNoServiceConfiguration = static_cast<HRESULT>(0x80830009);
inline constexpr HRESULT kGameSaveContainerNotInSync = static_cast<HRESULT>(0x8083000A);
inline constexpr HRESULT kGameSaveContainerSyncFailed = static_cast<HRESULT>(0x8083000B);
inline constexpr HRESULT kGameSaveUserNotRegisteredInService = static_cast<HRESULT>(0x8083000C);
inline constexpr HRESULT kGameSaveHandleExpired = static_cast<HRESULT>(0x8083000D);
inline constexpr HRESULT kGameSaveAsyncFunctionRequired = static_cast<HRESULT>(0x8083000E);
inline constexpr HRESULT kGameSaveProviderMismatch = static_cast<HRESULT>(0x8083000F);

// Where saves live: one folder per configuration ID below `root`. The runner sets
// <data>\saves before the guest starts. Until it is set, providers fail with kGameSaveNoAccess.
void set_game_save_directory(const std::filesystem::path& root);

// Providers, for the offline user only (E_INVALIDARG for any other handle). A missing or empty
// configuration ID gives kGameSaveNoServiceConfiguration. The synchronous forms that touch the
// disk fail with kGameSaveAsyncFunctionRequired on a time-sensitive thread, as the console's do.
HRESULT XGameSaveInitializeProvider(XUserHandle requesting_user, const char* configuration_id,
                                    bool sync_on_demand, XGameSaveProviderHandle* provider);
HRESULT XGameSaveInitializeProviderAsync(XUserHandle requesting_user, const char* configuration_id,
                                         bool sync_on_demand, XAsyncBlock* async);
// A null `provider` gives E_INVALIDARG and leaves the result to fetch.
HRESULT XGameSaveInitializeProviderResult(XAsyncBlock* async, XGameSaveProviderHandle* provider);
// Its containers and updates expire with it: later calls on them give kGameSaveHandleExpired.
void XGameSaveCloseProvider(XGameSaveProviderHandle provider);

// kGameSaveQuota less the size of every blob in the provider's containers, never below 0.
HRESULT XGameSaveGetRemainingQuota(XGameSaveProviderHandle provider, int64_t* remaining_quota);
HRESULT XGameSaveGetRemainingQuotaAsync(XGameSaveProviderHandle provider, XAsyncBlock* async);
HRESULT XGameSaveGetRemainingQuotaResult(XAsyncBlock* async, int64_t* remaining_quota);

// A container that doesn't exist is deleted already: S_OK.
HRESULT XGameSaveDeleteContainer(XGameSaveProviderHandle provider, const char* container_name);
HRESULT XGameSaveDeleteContainerAsync(XGameSaveProviderHandle provider, const char* container_name,
                                      XAsyncBlock* async);
HRESULT XGameSaveDeleteContainerResult(XAsyncBlock* async);

// Calls `callback` once for the container, or fails with HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
// when there is none.
HRESULT XGameSaveGetContainerInfo(XGameSaveProviderHandle provider, const char* container_name,
                                  void* context, XGameSaveContainerInfoCallback* callback);
// Containers in name order, compared without case; the ByName form takes those whose name
// starts with the prefix. A container exists once an update to it has been submitted.
HRESULT XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle provider, void* context,
                                        XGameSaveContainerInfoCallback* callback);
HRESULT XGameSaveEnumerateContainerInfoByName(XGameSaveProviderHandle provider,
                                              const char* container_name_prefix, void* context,
                                              XGameSaveContainerInfoCallback* callback);

// Opens a container by name, whether or not it exists yet.
HRESULT XGameSaveCreateContainer(XGameSaveProviderHandle provider, const char* container_name,
                                 XGameSaveContainerHandle* container);
void XGameSaveCloseContainer(XGameSaveContainerHandle container);

// Blobs in name order, compared without case.
HRESULT XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle container, void* context,
                                   XGameSaveBlobInfoCallback* callback);
HRESULT XGameSaveEnumerateBlobInfoByName(XGameSaveContainerHandle container,
                                         const char* blob_name_prefix, void* context,
                                         XGameSaveBlobInfoCallback* callback);

// Reads the named blobs, or every blob for null `blob_names`. The buffer receives one
// XGameSaveBlob per blob, followed by their data; the names point to storage that lives as long
// as the process. `count_of_blobs` is the number of names on entry and the number of blobs read
// on return. kGameSaveBlobNotFound for a missing blob, kGameSaveProvidedBufferTooSmall for a
// buffer that can't hold them all.
HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle container, const char** blob_names,
                              uint32_t* count_of_blobs, size_t blobs_size,
                              XGameSaveBlob* blob_data);
HRESULT XGameSaveReadBlobDataAsync(XGameSaveContainerHandle container, const char** blob_names,
                                   uint32_t count_of_blobs, XAsyncBlock* async);
HRESULT XGameSaveReadBlobDataResult(XAsyncBlock* async, size_t blobs_size, XGameSaveBlob* blob_data,
                                    uint32_t* count_of_blobs);

// An update collects blob writes and deletes and applies them all at once: either every change
// is on disk, or none is. A null display name keeps the container's. One update writes at most
// kGameSaveMaxUpdateSize bytes (kGameSaveUpdateTooBig), and it can be submitted once
// (kGameSaveHandleExpired after).
HRESULT XGameSaveCreateUpdate(XGameSaveContainerHandle container,
                              const char* container_display_name, XGameSaveUpdateHandle* update);
void XGameSaveCloseUpdate(XGameSaveUpdateHandle update);
HRESULT XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle update, const char* blob_name,
                                 const uint8_t* data, size_t byte_count);
HRESULT XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle update, const char* blob_name);
// kGameSaveQuotaExceeded when the provider's blobs would pass kGameSaveQuota.
HRESULT XGameSaveSubmitUpdate(XGameSaveUpdateHandle update);
HRESULT XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle update, XAsyncBlock* async);
HRESULT XGameSaveSubmitUpdateResult(XAsyncBlock* async);

// XGameSaveFiles, the folder-based alternative, isn't provided: each logs once and gives
// E_NOTIMPL.
HRESULT XGameSaveFilesGetFolderWithUiAsync(XUserHandle requesting_user,
                                           const char* configuration_id, XAsyncBlock* async);
HRESULT XGameSaveFilesGetFolderWithUiResult(XAsyncBlock* async, size_t folder_size,
                                            char* folder_result);
HRESULT XGameSaveFilesGetRemainingQuota(XUserHandle requesting_user, const char* configuration_id,
                                        int64_t* remaining_quota);

} // namespace g6::gdk
