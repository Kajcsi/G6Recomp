# File I/O

How guest code reaches files: `runtime/io` maps console paths to the user's install and to a
per-user data directory, and the HLE file functions in `runtime/imports` go through it.

**Status: implemented and tested (2026-11-22).** `runtime/io` holds the mounts and the path
rules; the default policy binds the functions below as HLE. Before this, every import that took a
path was a stub ([side-dlls.md](side-dlls.md), "Default import policy"). The runner,
`g6recomp.exe`, mounts the drives before it loads the main executable
([execution-model.md](execution-model.md), phase 5, "The runner"); `test_runner` checks them
from an original guest executable.

## Why

Forwarding a path function would open a host path. The guest would then see the host's file
system, and a stray absolute path could write anywhere. The console has a fixed set of drives, so
`runtime/io` gives the guest those drives and nothing else.

## The model

**Mounts.** Each mount maps one console drive letter to a host directory, either read-only or
read-write:

| Drive | Host directory | Access | Why |
|---|---|---|---|
| `G:` | The user's install | read-only | The package drive. The console mounts it read-only too |
| `T:` | `<data>\temp` | read-write | Scratch space |

* Only mounted drives exist; any other drive gives `ERROR_PATH_NOT_FOUND`.
* The guest's current directory starts at `G:\`, so relative paths open files in the install.
* The guest sees its own modules at their console paths: `GetModuleFileName` and the program
  name in its command line give `G:\<file>`, which its file functions open. The runner gives
  the loader a function that maps a module's host file to its console path
  (`FileSystem::console_path_of`), and refuses an executable outside the install.
* The drive letters are an assumption until the pre-flight checks
  ([execution-model.md](execution-model.md)) confirm them: one check is which drive-letter
  paths the main executable's strings contain.
* Persistent local storage (the package config asks for 2048 MB,
  [game-package.md](game-package.md)) gets its own mount, under `<data>\pls`, when
  `runtime/gdk` implements `XPersistentLocalStorageGetPath`. We choose what that function
  returns, so the mount follows from it.
  * Make it a drive root, such as `S:\`. **Reported** by xcl: Minecraft keeps only the drive
    letter of the returned path, so a path below a root breaks it.
  * **Reported** by xcl: a title reads a failed or tiny free-space answer as a full disk, so
    `GetDiskFreeSpaceExW` on that mount reports the host volume's space, capped at the quota.
* **Reported** by xcl: XDefiant uses `D:\` for its package drive, so the pre-flight check of
  drive-letter strings looks for `D:` as well as `G:`.

**The data directory.** `<data>` is `%LOCALAPPDATA%\G6Recomp\<build ID>`, one per supported
build. XGameSave keeps its containers in `<data>\saves` (`docs/gdk.md`, "XGameSave"), and
settings will go here too, which is how `CLAUDE.md`'s "redirects writes" is met: the game writes
to the drives and services the console gives it for writing, and those live here.

**Writes to a read-only mount** fail with `ERROR_ACCESS_DENIED`, as on the console. There is no
overlay that copies files out of the install: the console's package drive can't be written
either, so the game doesn't try. An error Win32 checks for first still comes first: `CreateFile`
with `TRUNCATE_EXISTING` but without `GENERIC_WRITE` is `ERROR_INVALID_PARAMETER`, and an open
that needs a missing directory or file gets the not-found error, as on any read-only volume.

## Path rules

A console path resolves the way a Win32 path does, minus everything that could reach outside
the mounts:

* **Separators.** `\` and `/` both separate. Runs of separators count as one.
* **Forms.** An absolute path (`G:\a`), a drive-relative path (`G:a`, against the current
  directory on its drive, or the drive's root otherwise), a rooted path (`\a`, on the current
  drive), or a relative path (`a\b`).
* **`.` and `..`** are resolved before any host call. `..` stops at the drive's root, as it
  does in Win32, so it can never climb out of a mount.
* **Trailing dots and spaces** are dropped from every component, as Win32 does when it opens a
  file. A component that is nothing but dots and spaces (other than `.` and `..`) disappears
  when it is the last one and is refused with `ERROR_PATH_NOT_FOUND` anywhere else.
* **Refused with `ERROR_INVALID_NAME`:**
  * UNC paths and every `\\?\`, `\\.\`, and `\??\` form;
  * a `:` anywhere except right after the drive letter, which covers alternate data streams and
    a second drive letter in the middle of a path;
  * control characters and `<`, `>`, `"`, and `|`;
  * `?` and `*`, except in the last component of a directory search;
  * device names as a component (`CON`, `PRN`, `AUX`, `NUL`, `CONIN$`, `CONOUT$`, `COM0`–`COM9`,
    `LPT0`–`LPT9`, and the superscript-digit forms, with or without an extension), which Win32
    would open as host devices.
* **Too long:** a component over 255 characters or a path over 32,767 gives
  `ERROR_FILENAME_EXCED_RANGE`.
* **Host paths.** The resolved host path is the mount's directory plus the components. It gets
  a `\\?\` prefix when it is long, so the host's `MAX_PATH` limit doesn't apply.

## Functions

The HLE functions keep the real signatures and set the last error or `errno` as the originals
do. Each one resolves its path, checks the mount's access, and calls the host function on the
host path. The handles they return are host handles, so the functions that take a handle stay
forwarded.

**Win32** (`runtime/imports/kernelbase`), for imports from `api-ms-win-core-file-*` (the ANSI
`api-ms-win-core-file-ansi-*` sets included), `api-ms-win-core-processenvironment-*`, `kernel32`,
and `kernelbase`, and the four move and copy functions `api-ms-win-core-kernel32-legacy-*` has:

* `CreateFileW`, `CreateFileA`, and `CreateFile2`;
* `FindFirstFileW`, `FindFirstFileA`, `FindFirstFileExW`, and `FindFirstFileExA`;
* `GetFileAttributesW`, `GetFileAttributesA`, `GetFileAttributesExW`, `GetFileAttributesExA`,
  and `SetFileAttributesW`;
* `CreateDirectoryW`, `CreateDirectoryA`, `RemoveDirectoryW`, `RemoveDirectoryA`, `DeleteFileW`,
  and `DeleteFileA`;
* every way to move, copy, or replace a file: `MoveFileW`, `MoveFileA`, `MoveFileExW`,
  `MoveFileExA`, `MoveFileWithProgressW`, `MoveFileWithProgressA`, `CopyFileW`, `CopyFileA`,
  `CopyFileExW`, `CopyFileExA`, `CopyFile2`, `ReplaceFileW`, and `ReplaceFileA`. Every end must
  be mounted and writable, except the source of a copy, and none may be a mount's root;
* `GetDiskFreeSpaceExW`, reporting the host volume behind the mount;
* `GetTempPathW` and `GetTempPath2W`, which give `T:\`;
* `GetFullPathNameW`, `GetFullPathNameA`, `GetCurrentDirectoryW`, `GetCurrentDirectoryA`,
  `SetCurrentDirectoryW`, and `SetCurrentDirectoryA`, which work on console paths only.

**C runtime** (`runtime/imports/ucrtbase`), for imports from `api-ms-win-crt-*`. These are the
side DLLs' imports plus the common ones. Each calls the wide function of `ucrtbase.dll`, the CRT
the guest's forwarded `api-ms-win-crt-*` imports resolve to, on the host path. The wrappers look
those functions up in `ucrtbase.dll` by name instead of calling the runtime's own CRT, because a
debug build of the runtime links `ucrtbased.dll`: a `FILE*`, `errno`, or a buffer from
`_getcwd(nullptr, 0)` has to belong to the guest's CRT, or the guest's `fclose` and `free`
would get objects from another heap:

* `fopen`, `_wfopen`, `fopen_s`, `_wfopen_s`, `_fsopen`, and `_wfsopen`;
* `_stat64`, `_wstat64`, `_access`, and `_waccess`;
* `_mkdir`, `_wmkdir`, `_rmdir`, `_wrmdir`, `remove`, `_wremove`, `_unlink`, `_wunlink`,
  `rename`, and `_wrename`;
* `_getcwd`, `_wgetcwd`, `_chdir`, and `_wchdir`.

Everything else that takes a path stays a stub until something imports it. That applies in
particular to the following:

* `CreateHardLinkW` and `CreateSymbolicLinkW`. A link would let a later open leave its mount.
* `GetFinalPathNameByHandleW`, which would hand out a host path. When something imports it, it
  becomes HLE that maps the path back to a console path.

**ANSI functions** convert with the active code page, as desktop Windows does. Which code page
the console uses is still unknown. A name cut to fit a fixed field, such as `cFileName` in
`WIN32_FIND_DATAA`, ends after a whole character, so a double-byte or UTF-8 sequence is never
split (`g6::narrow_prefix`, tested by `test_text`).

## Known differences

* A mount's root is a host directory, not a drive's root, so a search for `G:\*` lists `.` and
  `..`, which a real drive's root doesn't. No guest has been seen to care; filtering them would
  mean wrapping `FindNextFile` too.
* `GetDiskFreeSpaceExW` reports the host volume that holds the mount, not the console's
  partition sizes.
* The user data directory comes from `%LOCALAPPDATA%`, not from the shell's known-folder API,
  which would bring `shell32` and its dependencies into the process for one path. Linking
  `shell32` once crashed `test_side_dlls`. That was a loader bug, now fixed: host DLLs with
  static TLS of their own moved the TLS array while the loader started
  ([execution-model.md](execution-model.md), "TLS and the TEB").

## Under Proton

**Reported** by Leonida (`6d55b156`), from tests under Wine:

* **Cancelled copies.** When the cancel flag is already set, Windows' `CopyFile2` fails with
  `ERROR_REQUEST_ABORTED` and writes nothing. Wine 11.20's `CopyFile2` and Wine 9's
  `CopyFileExW` ignore the flag and copy anyway. Our `CopyFile2` checks the flag itself before
  it copies, so the guest gets Windows' answer under Proton too.
* **Renames that change only case.** Wine 11.20's `MoveFileW` to a name that differs only in
  case reports success without renaming. Our HLE moves files through the host's
  `MoveFileWithProgressW` with no special case for this, so expect the same under Proton (not
  tested here).

## Tests

* `test_io` (CI) covers the path rules: every form, `.` and `..` at the root, trailing dots,
  each refused form, device names, look-alike colons and slashes (ordinary name characters),
  wildcards, and length limits. On mounts in a temporary
  directory it covers mount validation, resolving to host paths, the long-path prefix, mapping
  host paths back, the current directory, and the standard drives.
* `test_imports` (CI) checks that the policy binds each function above as HLE, never as the
  host's. On the standard drives in a temporary directory it calls the Win32 and CRT wrappers:
  opening, creating, and deleting files; access denied on the read-only mount; directory
  searches; attributes; moves and copies; the temp path; full paths; and the current directory.
* `test_side_dlls` (local) still initializes all 12 mapped side DLLs under the new policy.
