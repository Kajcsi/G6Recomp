#pragma once

// The Win32 file functions that take a path (docs/io.md, "Functions"), for imports from
// api-ms-win-core-file-*, api-ms-win-core-processenvironment-*, kernel32, and kernelbase. Each
// resolves its path through the active g6::io::FileSystem, refuses writes to a read-only mount
// with ERROR_ACCESS_DENIED, and calls the host function on the host path. Handles they return
// are host handles, so the functions that take a handle stay forwarded.
//
// The console's current directory and temp path are console paths: GetCurrentDirectory gives
// "G:\" and GetTempPath gives "T:\".

#include <windows.h>

namespace g6::loader {
class ImportRegistry;
}

namespace g6::imports::kernelbase {

HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                          DWORD disposition, DWORD flags, HANDLE template_file);
HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                          DWORD disposition, DWORD flags, HANDLE template_file);
HANDLE WINAPI CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                          LPCREATEFILE2_EXTENDED_PARAMETERS parameters);

HANDLE WINAPI FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                               FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags);
HANDLE WINAPI FindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                               FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags);
HANDLE WINAPI FindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data);
HANDLE WINAPI FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data);

DWORD WINAPI GetFileAttributesW(LPCWSTR name);
DWORD WINAPI GetFileAttributesA(LPCSTR name);
BOOL WINAPI GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL WINAPI GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL WINAPI SetFileAttributesW(LPCWSTR name, DWORD attributes);

BOOL WINAPI CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security);
BOOL WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES security);
BOOL WINAPI RemoveDirectoryW(LPCWSTR name);
BOOL WINAPI RemoveDirectoryA(LPCSTR name);
BOOL WINAPI DeleteFileW(LPCWSTR name);
BOOL WINAPI DeleteFileA(LPCSTR name);

// Moves keep to the mounts: a move at reboot (a null destination included) or into a hard link
// fails with ERROR_INVALID_PARAMETER, and a mount's root can't move or be moved onto. MoveFile is
// MoveFileEx with MOVEFILE_COPY_ALLOWED.
BOOL WINAPI MoveFileWithProgressW(LPCWSTR existing, LPCWSTR destination,
                                  LPPROGRESS_ROUTINE progress, LPVOID data, DWORD flags);
BOOL WINAPI MoveFileWithProgressA(LPCSTR existing, LPCSTR destination, LPPROGRESS_ROUTINE progress,
                                  LPVOID data, DWORD flags);
BOOL WINAPI MoveFileExW(LPCWSTR existing, LPCWSTR destination, DWORD flags);
BOOL WINAPI MoveFileExA(LPCSTR existing, LPCSTR destination, DWORD flags);
BOOL WINAPI MoveFileW(LPCWSTR existing, LPCWSTR destination);
BOOL WINAPI MoveFileA(LPCSTR existing, LPCSTR destination);

// Copies read the source and write the destination; COPY_FILE_OPEN_SOURCE_FOR_WRITE needs a
// writable source, and COPY_FILE_COPY_SYMLINK fails with ERROR_INVALID_PARAMETER.
BOOL WINAPI CopyFileExW(LPCWSTR existing, LPCWSTR destination, LPPROGRESS_ROUTINE progress,
                        LPVOID data, LPBOOL cancel, DWORD flags);
BOOL WINAPI CopyFileExA(LPCSTR existing, LPCSTR destination, LPPROGRESS_ROUTINE progress,
                        LPVOID data, LPBOOL cancel, DWORD flags);
BOOL WINAPI CopyFileW(LPCWSTR existing, LPCWSTR destination, BOOL fail_if_exists);
BOOL WINAPI CopyFileA(LPCSTR existing, LPCSTR destination, BOOL fail_if_exists);
HRESULT WINAPI CopyFile2(PCWSTR existing, PCWSTR destination,
                         COPYFILE2_EXTENDED_PARAMETERS* parameters);

// The usual save step: the replacement takes the replaced file's name, and the replaced file
// moves to the backup when one is named. All three must be writable and none a mount's root. The
// two reserved arguments must be null, or the call fails with ERROR_INVALID_PARAMETER.
BOOL WINAPI ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags,
                         LPVOID exclude, LPVOID reserved);
BOOL WINAPI ReplaceFileA(LPCSTR replaced, LPCSTR replacement, LPCSTR backup, DWORD flags,
                         LPVOID exclude, LPVOID reserved);

namespace detail {
// What CopyFile2 does on a host without one (Wine 9): CopyFileExW on the host paths, with the
// flags CopyFileExW knows and the cancel flag. The progress routine isn't called.
HRESULT copy_file2_fallback(PCWSTR existing, PCWSTR destination,
                            const COPYFILE2_EXTENDED_PARAMETERS* parameters);
} // namespace detail

BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total,
                                PULARGE_INTEGER free);
DWORD WINAPI GetTempPathW(DWORD length, LPWSTR buffer);
DWORD WINAPI GetTempPath2W(DWORD length, LPWSTR buffer);

DWORD WINAPI GetFullPathNameW(LPCWSTR name, DWORD length, LPWSTR buffer, LPWSTR* file_part);
DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD length, LPSTR buffer, LPSTR* file_part);
DWORD WINAPI GetCurrentDirectoryW(DWORD length, LPWSTR buffer);
DWORD WINAPI GetCurrentDirectoryA(DWORD length, LPSTR buffer);
BOOL WINAPI SetCurrentDirectoryW(LPCWSTR name);
BOOL WINAPI SetCurrentDirectoryA(LPCSTR name);

// Binds the functions above for the file and process-environment API sets, kernel32, and
// kernelbase, and those the legacy kernel32 API set has for it.
void register_files(loader::ImportRegistry& imports);

} // namespace g6::imports::kernelbase
