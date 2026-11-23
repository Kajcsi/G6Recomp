// The Win32 file functions that take a path (docs/io.md, "Functions"). Each resolves its path
// through the active g6::io::FileSystem and calls the host function on the host path, so the
// guest reaches its mounted drives and nothing else.

#include "g6/imports/files.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "g6/io/file_system.h"
#include "g6/io/text.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"

namespace g6::imports::kernelbase {
namespace {

using io::Resolved;
using io::Wildcards;

// Access rights that change a file or its metadata. MAXIMUM_ALLOWED is handled on its own.
constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
                               FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC |
                               WRITE_OWNER;

std::wstring_view view(LPCWSTR text) {
    return text == nullptr ? std::wstring_view{} : std::wstring_view{text};
}

std::wstring widen(LPCSTR text) {
    return text == nullptr ? std::wstring{} : io::widen(text);
}

// A null name stays null, so the wide function fails it the same way.
LPCWSTR wide_or_null(LPCSTR text, const std::wstring& wide) {
    return text == nullptr ? nullptr : wide.c_str();
}

BOOL fail(DWORD error) {
    SetLastError(error);
    return FALSE;
}

HANDLE fail_handle(DWORD error) {
    SetLastError(error);
    return INVALID_HANDLE_VALUE;
}

using CopyFile2Function = HRESULT(WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);

// The host's CopyFile2, or null. It is looked up rather than imported because Wine 9, which
// Proton builds on, has none, and binds an import of it to a stub that ends the process.
CopyFile2Function host_copy_file2() {
    static CopyFile2Function function = [] {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel == nullptr
                   ? nullptr
                   : reinterpret_cast<CopyFile2Function>(GetProcAddress(kernel, "CopyFile2"));
    }();
    return function;
}

// Resolves a path for reading. On failure the last error is set.
Resolved for_reading(LPCWSTR name, Wildcards wildcards = Wildcards::Refuse) {
    Resolved resolved = io::active_file_system().resolve(view(name), wildcards);
    if (!resolved.ok()) {
        SetLastError(resolved.error);
    }
    return resolved;
}

// Resolves a path for writing: a read-only mount fails with ERROR_ACCESS_DENIED.
Resolved for_writing(LPCWSTR name) {
    Resolved resolved = for_reading(name);
    if (resolved.ok() && !resolved.writable()) {
        resolved.error = ERROR_ACCESS_DENIED;
        SetLastError(ERROR_ACCESS_DENIED);
    }
    return resolved;
}

// Resolves a path whose entry is moved, replaced, or removed: as for writing, and a mount's root
// stays, whatever the guest asks, so it fails with ERROR_ACCESS_DENIED.
Resolved for_changing(LPCWSTR name) {
    Resolved resolved = for_writing(name);
    if (resolved.ok() && resolved.console.components.empty()) {
        resolved.error = ERROR_ACCESS_DENIED;
        SetLastError(ERROR_ACCESS_DENIED);
    }
    return resolved;
}

// Both ends of a copy: the source for reading, or for writing when the copy opens it for
// writing, and the destination for writing. Copying a link as a link could leave a mount, so
// COPY_FILE_COPY_SYMLINK is refused. On failure `error` and the last error are set.
struct CopyEnds {
    Resolved from;
    Resolved to;
    DWORD error = 0;
};

CopyEnds copy_ends(LPCWSTR existing, LPCWSTR destination, DWORD flags) {
    CopyEnds ends;
    if ((flags & COPY_FILE_COPY_SYMLINK) != 0) {
        ends.error = ERROR_INVALID_PARAMETER;
        SetLastError(ERROR_INVALID_PARAMETER);
        return ends;
    }
    ends.from = (flags & COPY_FILE_OPEN_SOURCE_FOR_WRITE) != 0 ? for_writing(existing)
                                                               : for_reading(existing);
    if (!ends.from.ok()) {
        ends.error = ends.from.error;
        return ends;
    }
    ends.to = for_writing(destination);
    ends.error = ends.to.error;
    return ends;
}

bool exists(const Resolved& resolved) {
    return ::GetFileAttributesW(resolved.host.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Copies `text` into a caller's buffer with Win32's rules: the length without the terminator
// when it fits, the size it needs with the terminator when it doesn't.
template <typename Char>
DWORD copy_out(std::basic_string_view<Char> text, DWORD length, Char* buffer) {
    if (buffer == nullptr || length <= text.size()) {
        return static_cast<DWORD>(text.size() + 1);
    }
    std::copy(text.begin(), text.end(), buffer);
    buffer[text.size()] = Char{};
    return static_cast<DWORD>(text.size());
}

// What CreateFile gets for an open on a mount: the access, or an error when the open would write
// to a read-only mount.
struct OpenRequest {
    DWORD access = 0;
    DWORD disposition = 0;
    DWORD error = 0;
    bool existed = false; // OPEN_ALWAYS turned into OPEN_EXISTING on a read-only mount
};

OpenRequest plan_open(const Resolved& resolved, DWORD access, DWORD disposition, DWORD flags) {
    OpenRequest request{.access = access, .disposition = disposition};
    if (resolved.writable()) {
        return request;
    }
    // A read-only mount grants reading at most.
    if ((request.access & MAXIMUM_ALLOWED) != 0) {
        request.access = (request.access & ~static_cast<DWORD>(MAXIMUM_ALLOWED)) | GENERIC_READ;
    }
    const bool writes = (request.access & kWriteAccess) != 0 ||
                        (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0 || disposition == CREATE_NEW ||
                        disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING;
    if (writes) {
        request.error = ERROR_ACCESS_DENIED;
        return request;
    }
    if (disposition == OPEN_ALWAYS) {
        // Opens an existing file as it would, and refuses to create a missing one.
        if (!exists(resolved)) {
            request.error = ERROR_ACCESS_DENIED;
            return request;
        }
        request.disposition = OPEN_EXISTING;
        request.existed = true;
    }
    return request;
}

// The guest's security attributes without their descriptor: handle inheritance carries over,
// but a guest ACL is never put on a host file.
struct Inheritance {
    SECURITY_ATTRIBUTES attributes{};
    LPSECURITY_ATTRIBUTES pointer = nullptr;

    explicit Inheritance(const SECURITY_ATTRIBUTES* security) {
        if (security != nullptr) {
            attributes = {.nLength = sizeof(SECURITY_ATTRIBUTES),
                          .lpSecurityDescriptor = nullptr,
                          .bInheritHandle = security->bInheritHandle};
            pointer = &attributes;
        }
    }
    Inheritance(const Inheritance&) = delete;
    Inheritance& operator=(const Inheritance&) = delete;
};

// OPEN_ALWAYS reports an existing file through the last error; keep that after the rewrite.
HANDLE finish_open(HANDLE handle, const OpenRequest& request) {
    if (handle != INVALID_HANDLE_VALUE && request.existed) {
        SetLastError(ERROR_ALREADY_EXISTS);
    }
    return handle;
}

void to_ansi(const WIN32_FIND_DATAW& wide, WIN32_FIND_DATAA& ansi) {
    ansi.dwFileAttributes = wide.dwFileAttributes;
    ansi.ftCreationTime = wide.ftCreationTime;
    ansi.ftLastAccessTime = wide.ftLastAccessTime;
    ansi.ftLastWriteTime = wide.ftLastWriteTime;
    ansi.nFileSizeHigh = wide.nFileSizeHigh;
    ansi.nFileSizeLow = wide.nFileSizeLow;
    ansi.dwReserved0 = wide.dwReserved0;
    ansi.dwReserved1 = wide.dwReserved1;
    const std::string name = io::narrow(wide.cFileName);
    const std::string alternate = io::narrow(wide.cAlternateFileName);
    const size_t name_length = std::min(name.size(), std::size(ansi.cFileName) - 1);
    const size_t alternate_length =
        std::min(alternate.size(), std::size(ansi.cAlternateFileName) - 1);
    std::memcpy(ansi.cFileName, name.data(), name_length);
    ansi.cFileName[name_length] = '\0';
    std::memcpy(ansi.cAlternateFileName, alternate.data(), alternate_length);
    ansi.cAlternateFileName[alternate_length] = '\0';
}

} // namespace

// ---- Opening files ----

HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                          DWORD disposition, DWORD flags, HANDLE template_file) {
    const Resolved resolved = for_reading(name);
    if (!resolved.ok()) {
        return INVALID_HANDLE_VALUE;
    }
    const OpenRequest request = plan_open(resolved, access, disposition, flags);
    if (request.error != 0) {
        return fail_handle(request.error);
    }
    const Inheritance inheritance(security);
    return finish_open(::CreateFileW(resolved.host.c_str(), request.access, share,
                                     inheritance.pointer, request.disposition, flags,
                                     template_file),
                       request);
}

HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                          DWORD disposition, DWORD flags, HANDLE template_file) {
    const std::wstring wide = widen(name);
    return kernelbase::CreateFileW(wide_or_null(name, wide), access, share, security, disposition,
                                   flags, template_file);
}

HANDLE WINAPI CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                          LPCREATEFILE2_EXTENDED_PARAMETERS parameters) {
    const Resolved resolved = for_reading(name);
    if (!resolved.ok()) {
        return INVALID_HANDLE_VALUE;
    }
    const DWORD flags = parameters != nullptr ? parameters->dwFileFlags : 0;
    const OpenRequest request = plan_open(resolved, access, disposition, flags);
    if (request.error != 0) {
        return fail_handle(request.error);
    }
    CREATEFILE2_EXTENDED_PARAMETERS host{};
    host.dwSize = sizeof(host);
    if (parameters != nullptr) {
        host.dwFileAttributes = parameters->dwFileAttributes;
        host.dwFileFlags = parameters->dwFileFlags;
        host.dwSecurityQosFlags = parameters->dwSecurityQosFlags;
        host.hTemplateFile = parameters->hTemplateFile;
    }
    const Inheritance inheritance(parameters != nullptr ? parameters->lpSecurityAttributes
                                                        : nullptr);
    host.lpSecurityAttributes = inheritance.pointer;
    return finish_open(
        ::CreateFile2(resolved.host.c_str(), request.access, share, request.disposition, &host),
        request);
}

// ---- Directory searches ----

HANDLE WINAPI FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                               FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    const Resolved resolved = for_reading(name, Wildcards::LastComponent);
    if (!resolved.ok()) {
        return INVALID_HANDLE_VALUE;
    }
    // A drive's root has no name to find, as in Win32.
    if (resolved.console.components.empty()) {
        return fail_handle(ERROR_FILE_NOT_FOUND);
    }
    return ::FindFirstFileExW(resolved.host.c_str(), level, data, search, filter, flags);
}

HANDLE WINAPI FindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                               FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    if (data == nullptr || (level != FindExInfoStandard && level != FindExInfoBasic)) {
        return fail_handle(ERROR_INVALID_PARAMETER);
    }
    const std::wstring wide = widen(name);
    WIN32_FIND_DATAW wide_data{};
    HANDLE handle = kernelbase::FindFirstFileExW(wide_or_null(name, wide), level, &wide_data,
                                                 search, filter, flags);
    if (handle != INVALID_HANDLE_VALUE) {
        to_ansi(wide_data, *static_cast<WIN32_FIND_DATAA*>(data));
    }
    return handle;
}

HANDLE WINAPI FindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
    return kernelbase::FindFirstFileExW(name, FindExInfoStandard, data, FindExSearchNameMatch,
                                        nullptr, 0);
}

HANDLE WINAPI FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data) {
    return kernelbase::FindFirstFileExA(name, FindExInfoStandard, data, FindExSearchNameMatch,
                                        nullptr, 0);
}

// ---- Attributes ----

DWORD WINAPI GetFileAttributesW(LPCWSTR name) {
    const Resolved resolved = for_reading(name);
    if (!resolved.ok()) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return ::GetFileAttributesW(resolved.host.c_str());
}

DWORD WINAPI GetFileAttributesA(LPCSTR name) {
    const std::wstring wide = widen(name);
    return kernelbase::GetFileAttributesW(wide_or_null(name, wide));
}

BOOL WINAPI GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    const Resolved resolved = for_reading(name);
    if (!resolved.ok()) {
        return FALSE;
    }
    return ::GetFileAttributesExW(resolved.host.c_str(), level, info);
}

BOOL WINAPI GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    // WIN32_FILE_ATTRIBUTE_DATA has no strings, so both versions fill the same structure.
    const std::wstring wide = widen(name);
    return kernelbase::GetFileAttributesExW(wide_or_null(name, wide), level, info);
}

BOOL WINAPI SetFileAttributesW(LPCWSTR name, DWORD attributes) {
    const Resolved resolved = for_writing(name);
    if (!resolved.ok()) {
        return FALSE;
    }
    return ::SetFileAttributesW(resolved.host.c_str(), attributes);
}

// ---- Creating, removing, and moving ----

BOOL WINAPI CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES /*security*/) {
    const Resolved resolved = for_reading(name);
    if (!resolved.ok()) {
        return FALSE;
    }
    if (!resolved.writable()) {
        // An existing directory is reported as such, so code that creates its directories
        // unconditionally carries on.
        return fail(exists(resolved) ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED);
    }
    // The guest's security descriptor is never applied to a host directory.
    return ::CreateDirectoryW(resolved.host.c_str(), nullptr);
}

BOOL WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES security) {
    const std::wstring wide = widen(name);
    return kernelbase::CreateDirectoryW(wide_or_null(name, wide), security);
}

BOOL WINAPI RemoveDirectoryW(LPCWSTR name) {
    const Resolved resolved = for_changing(name);
    if (!resolved.ok()) {
        return FALSE;
    }
    return ::RemoveDirectoryW(resolved.host.c_str());
}

BOOL WINAPI RemoveDirectoryA(LPCSTR name) {
    const std::wstring wide = widen(name);
    return kernelbase::RemoveDirectoryW(wide_or_null(name, wide));
}

BOOL WINAPI DeleteFileW(LPCWSTR name) {
    const Resolved resolved = for_writing(name);
    if (!resolved.ok()) {
        return FALSE;
    }
    return ::DeleteFileW(resolved.host.c_str());
}

BOOL WINAPI DeleteFileA(LPCSTR name) {
    const std::wstring wide = widen(name);
    return kernelbase::DeleteFileW(wide_or_null(name, wide));
}

// ---- Moving, copying, and replacing ----
//
// The progress routines these take are guest code, called directly like any other guest
// callback.

BOOL WINAPI MoveFileWithProgressW(LPCWSTR existing, LPCWSTR destination,
                                  LPPROGRESS_ROUTINE progress, LPVOID data, DWORD flags) {
    // Moves at reboot go through the host's registry, and hard links can leave a mount. A null
    // destination means a delete at reboot, so it goes too.
    constexpr DWORD kAllowed = MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED |
                               MOVEFILE_WRITE_THROUGH | MOVEFILE_FAIL_IF_NOT_TRACKABLE;
    if ((flags & ~kAllowed) != 0 || destination == nullptr) {
        return fail(ERROR_INVALID_PARAMETER);
    }
    const Resolved from = for_changing(existing);
    if (!from.ok()) {
        return FALSE;
    }
    const Resolved to = for_changing(destination);
    if (!to.ok()) {
        return FALSE;
    }
    return ::MoveFileWithProgressW(from.host.c_str(), to.host.c_str(), progress, data, flags);
}

BOOL WINAPI MoveFileWithProgressA(LPCSTR existing, LPCSTR destination, LPPROGRESS_ROUTINE progress,
                                  LPVOID data, DWORD flags) {
    const std::wstring from = widen(existing);
    const std::wstring to = widen(destination);
    return kernelbase::MoveFileWithProgressW(wide_or_null(existing, from),
                                             wide_or_null(destination, to), progress, data, flags);
}

BOOL WINAPI MoveFileExW(LPCWSTR existing, LPCWSTR destination, DWORD flags) {
    return kernelbase::MoveFileWithProgressW(existing, destination, nullptr, nullptr, flags);
}

BOOL WINAPI MoveFileExA(LPCSTR existing, LPCSTR destination, DWORD flags) {
    return kernelbase::MoveFileWithProgressA(existing, destination, nullptr, nullptr, flags);
}

// MoveFile moves a file to another volume too, as MoveFileEx does with MOVEFILE_COPY_ALLOWED.
BOOL WINAPI MoveFileW(LPCWSTR existing, LPCWSTR destination) {
    return kernelbase::MoveFileWithProgressW(existing, destination, nullptr, nullptr,
                                             MOVEFILE_COPY_ALLOWED);
}

BOOL WINAPI MoveFileA(LPCSTR existing, LPCSTR destination) {
    return kernelbase::MoveFileWithProgressA(existing, destination, nullptr, nullptr,
                                             MOVEFILE_COPY_ALLOWED);
}

BOOL WINAPI CopyFileExW(LPCWSTR existing, LPCWSTR destination, LPPROGRESS_ROUTINE progress,
                        LPVOID data, LPBOOL cancel, DWORD flags) {
    const CopyEnds ends = copy_ends(existing, destination, flags);
    if (ends.error != 0) {
        return FALSE;
    }
    return ::CopyFileExW(ends.from.host.c_str(), ends.to.host.c_str(), progress, data, cancel,
                         flags);
}

BOOL WINAPI CopyFileExA(LPCSTR existing, LPCSTR destination, LPPROGRESS_ROUTINE progress,
                        LPVOID data, LPBOOL cancel, DWORD flags) {
    const std::wstring from = widen(existing);
    const std::wstring to = widen(destination);
    return kernelbase::CopyFileExW(wide_or_null(existing, from), wide_or_null(destination, to),
                                   progress, data, cancel, flags);
}

BOOL WINAPI CopyFileW(LPCWSTR existing, LPCWSTR destination, BOOL fail_if_exists) {
    return kernelbase::CopyFileExW(existing, destination, nullptr, nullptr, nullptr,
                                   fail_if_exists != FALSE ? COPY_FILE_FAIL_IF_EXISTS : 0);
}

BOOL WINAPI CopyFileA(LPCSTR existing, LPCSTR destination, BOOL fail_if_exists) {
    return kernelbase::CopyFileExA(existing, destination, nullptr, nullptr, nullptr,
                                   fail_if_exists != FALSE ? COPY_FILE_FAIL_IF_EXISTS : 0);
}

HRESULT WINAPI CopyFile2(PCWSTR existing, PCWSTR destination,
                         COPYFILE2_EXTENDED_PARAMETERS* parameters) {
    const CopyEnds ends =
        copy_ends(existing, destination, parameters != nullptr ? parameters->dwCopyFlags : 0);
    if (ends.error != 0) {
        return HRESULT_FROM_WIN32(ends.error);
    }
    if (CopyFile2Function host = host_copy_file2()) {
        return host(ends.from.host.c_str(), ends.to.host.c_str(), parameters);
    }
    return detail::copy_file2_fallback(ends.from.host.c_str(), ends.to.host.c_str(), parameters);
}

BOOL WINAPI ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags,
                         LPVOID exclude, LPVOID reserved) {
    // The last two are reserved and must be null.
    if (exclude != nullptr || reserved != nullptr) {
        return fail(ERROR_INVALID_PARAMETER);
    }
    // All three entries change: the replaced file's name moves to the backup, and the
    // replacement's name goes.
    const Resolved target = for_changing(replaced);
    if (!target.ok()) {
        return FALSE;
    }
    const Resolved source = for_changing(replacement);
    if (!source.ok()) {
        return FALSE;
    }
    Resolved saved;
    if (backup != nullptr) {
        saved = for_changing(backup);
        if (!saved.ok()) {
            return FALSE;
        }
    }
    return ::ReplaceFileW(target.host.c_str(), source.host.c_str(),
                          backup != nullptr ? saved.host.c_str() : nullptr, flags, nullptr,
                          nullptr);
}

BOOL WINAPI ReplaceFileA(LPCSTR replaced, LPCSTR replacement, LPCSTR backup, DWORD flags,
                         LPVOID exclude, LPVOID reserved) {
    const std::wstring target = widen(replaced);
    const std::wstring source = widen(replacement);
    const std::wstring saved = widen(backup);
    return kernelbase::ReplaceFileW(wide_or_null(replaced, target),
                                    wide_or_null(replacement, source), wide_or_null(backup, saved),
                                    flags, exclude, reserved);
}

namespace detail {

HRESULT copy_file2_fallback(PCWSTR existing, PCWSTR destination,
                            const COPYFILE2_EXTENDED_PARAMETERS* parameters) {
    // The flags CopyFileExW knows; the rest are hints or privilege requests CopyFile2 alone has.
    constexpr DWORD kCopyFileExFlags =
        COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_RESTARTABLE | COPY_FILE_OPEN_SOURCE_FOR_WRITE |
        COPY_FILE_ALLOW_DECRYPTED_DESTINATION | COPY_FILE_COPY_SYMLINK | COPY_FILE_NO_BUFFERING;
    DWORD flags = 0;
    BOOL* cancel = nullptr;
    if (parameters != nullptr) {
        flags = parameters->dwCopyFlags & kCopyFileExFlags;
        cancel = parameters->pfCancel;
        if (parameters->pProgressRoutine != nullptr) {
            static std::once_flag reported;
            std::call_once(reported, [] {
                LOG_WARN(imports, "CopyFile2: the host has none, so the copy goes through "
                                  "CopyFileExW without calling the progress routine (reported "
                                  "once)");
            });
        }
    }
    if (::CopyFileExW(existing, destination, nullptr, nullptr, cancel, flags) == FALSE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

} // namespace detail

// ---- Volumes and well-known directories ----

BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total,
                                PULARGE_INTEGER free) {
    // No directory means the current directory's drive.
    const std::wstring current =
        directory == nullptr ? io::active_file_system().current_directory() : std::wstring{};
    const Resolved resolved = for_reading(directory == nullptr ? current.c_str() : directory);
    if (!resolved.ok()) {
        return FALSE;
    }
    return ::GetDiskFreeSpaceExW(resolved.host.c_str(), available, total, free);
}

DWORD WINAPI GetTempPathW(DWORD length, LPWSTR buffer) {
    return copy_out(std::wstring_view{L"T:\\"}, length, buffer);
}

DWORD WINAPI GetTempPath2W(DWORD length, LPWSTR buffer) {
    return kernelbase::GetTempPathW(length, buffer);
}

// ---- Console paths ----

DWORD WINAPI GetFullPathNameW(LPCWSTR name, DWORD length, LPWSTR buffer, LPWSTR* file_part) {
    const io::ParsedPath parsed =
        io::active_file_system().absolute(view(name), Wildcards::LastComponent);
    if (parsed.error != 0) {
        SetLastError(parsed.error);
        return 0;
    }
    std::wstring full = parsed.path.str();
    const bool names_a_file = !parsed.path.components.empty() && !parsed.trailing_separator;
    if (!parsed.path.components.empty() && parsed.trailing_separator) {
        full += L'\\';
    }
    const DWORD result = copy_out(std::wstring_view{full}, length, buffer);
    if (file_part != nullptr && result == full.size()) {
        *file_part =
            names_a_file ? buffer + (full.size() - parsed.path.components.back().size()) : nullptr;
    }
    return result;
}

DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD length, LPSTR buffer, LPSTR* file_part) {
    const std::wstring wide = widen(name);
    const io::ParsedPath parsed =
        io::active_file_system().absolute(view(wide_or_null(name, wide)), Wildcards::LastComponent);
    if (parsed.error != 0) {
        SetLastError(parsed.error);
        return 0;
    }
    std::wstring full = parsed.path.str();
    const bool names_a_file = !parsed.path.components.empty() && !parsed.trailing_separator;
    if (!parsed.path.components.empty() && parsed.trailing_separator) {
        full += L'\\';
    }
    const std::string ansi = io::narrow(full);
    const DWORD result = copy_out(std::string_view{ansi}, length, buffer);
    if (file_part != nullptr && result == ansi.size()) {
        *file_part = names_a_file
                         ? buffer + (ansi.size() - io::narrow(parsed.path.components.back()).size())
                         : nullptr;
    }
    return result;
}

DWORD WINAPI GetCurrentDirectoryW(DWORD length, LPWSTR buffer) {
    const std::wstring current = io::active_file_system().current_directory();
    if (current.empty()) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    return copy_out(std::wstring_view{current}, length, buffer);
}

DWORD WINAPI GetCurrentDirectoryA(DWORD length, LPSTR buffer) {
    const std::wstring current = io::active_file_system().current_directory();
    if (current.empty()) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    const std::string ansi = io::narrow(current);
    return copy_out(std::string_view{ansi}, length, buffer);
}

BOOL WINAPI SetCurrentDirectoryW(LPCWSTR name) {
    const uint32_t error = io::active_file_system().set_current_directory(view(name));
    return error == 0 ? TRUE : fail(error);
}

BOOL WINAPI SetCurrentDirectoryA(LPCSTR name) {
    const std::wstring wide = widen(name);
    return kernelbase::SetCurrentDirectoryW(wide_or_null(name, wide));
}

// ---- Registration ----

namespace {

struct Entry {
    const char* name;
    const void* function;
};

template <typename Function>
Entry entry(const char* name, Function* function) {
    return {name, reinterpret_cast<const void*>(function)};
}

} // namespace

void register_files(loader::ImportRegistry& imports) {
    const Entry files[] = {
        entry("CreateFileW", &CreateFileW),
        entry("CreateFileA", &CreateFileA),
        entry("CreateFile2", &CreateFile2),
        entry("FindFirstFileExW", &FindFirstFileExW),
        entry("FindFirstFileExA", &FindFirstFileExA),
        entry("FindFirstFileW", &FindFirstFileW),
        entry("FindFirstFileA", &FindFirstFileA),
        entry("GetFileAttributesW", &GetFileAttributesW),
        entry("GetFileAttributesA", &GetFileAttributesA),
        entry("GetFileAttributesExW", &GetFileAttributesExW),
        entry("GetFileAttributesExA", &GetFileAttributesExA),
        entry("SetFileAttributesW", &SetFileAttributesW),
        entry("CreateDirectoryW", &CreateDirectoryW),
        entry("CreateDirectoryA", &CreateDirectoryA),
        entry("RemoveDirectoryW", &RemoveDirectoryW),
        entry("RemoveDirectoryA", &RemoveDirectoryA),
        entry("DeleteFileW", &DeleteFileW),
        entry("DeleteFileA", &DeleteFileA),
        entry("MoveFileWithProgressW", &MoveFileWithProgressW),
        entry("MoveFileWithProgressA", &MoveFileWithProgressA),
        entry("MoveFileExW", &MoveFileExW),
        entry("MoveFileExA", &MoveFileExA),
        entry("MoveFileW", &MoveFileW),
        entry("MoveFileA", &MoveFileA),
        entry("CopyFileExW", &CopyFileExW),
        entry("CopyFileExA", &CopyFileExA),
        entry("CopyFileW", &CopyFileW),
        entry("CopyFileA", &CopyFileA),
        entry("CopyFile2", &CopyFile2),
        entry("ReplaceFileW", &ReplaceFileW),
        entry("ReplaceFileA", &ReplaceFileA),
        entry("GetDiskFreeSpaceExW", &GetDiskFreeSpaceExW),
        entry("GetTempPathW", &GetTempPathW),
        entry("GetTempPath2W", &GetTempPath2W),
        entry("GetFullPathNameW", &GetFullPathNameW),
        entry("GetFullPathNameA", &GetFullPathNameA),
    };
    const Entry current_directory[] = {
        entry("GetCurrentDirectoryW", &GetCurrentDirectoryW),
        entry("GetCurrentDirectoryA", &GetCurrentDirectoryA),
        entry("SetCurrentDirectoryW", &SetCurrentDirectoryW),
        entry("SetCurrentDirectoryA", &SetCurrentDirectoryA),
    };
    // The ANSI file API sets (api-ms-win-core-file-ansi-*) are file API sets too.
    for (const char* dll : {"api-ms-win-core-file-*", "kernel32.dll", "kernelbase.dll"}) {
        for (const Entry& item : files) {
            imports.hle(dll, loader::Symbol::by_name(item.name), item.function);
        }
    }
    // The path functions the legacy kernel32 API set has.
    const Entry legacy[] = {
        entry("MoveFileExA", &MoveFileExA),
        entry("MoveFileW", &MoveFileW),
        entry("MoveFileA", &MoveFileA),
        entry("CopyFileA", &CopyFileA),
    };
    for (const Entry& item : legacy) {
        imports.hle("api-ms-win-core-kernel32-legacy-*", loader::Symbol::by_name(item.name),
                    item.function);
    }
    for (const char* dll :
         {"api-ms-win-core-processenvironment-*", "kernel32.dll", "kernelbase.dll"}) {
        for (const Entry& item : current_directory) {
            imports.hle(dll, loader::Symbol::by_name(item.name), item.function);
        }
    }
}

} // namespace g6::imports::kernelbase
