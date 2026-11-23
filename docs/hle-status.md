# HLE status

One table per folder under `runtime/imports/<dll>` and `runtime/gdk`, listing every function the
game imports from it and how we provide it. The loader binds each import by this status (see
"Import policy" in `docs/execution-model.md`); anything without a row is stubbed.

* **forwarded**: bound straight to the identical host function.
* **implemented**: our own implementation, because console behavior differs (say how).
* **partial**: implemented for the cases the game uses; note what is missing.
* **offline**: reports no connectivity. Networking is never forwarded (hard rule 5).
* **stubbed**: logs once with DLL, symbol or ordinal, and caller address, then returns a
  documented error (`E_NOTIMPL` for HRESULT APIs, zero plus `ERROR_CALL_NOT_IMPLEMENTED` for
  Win32 APIs, unless a row says otherwise). Never pretends to succeed.

Console-only imports are named after the game's import table, or `<dll>_ord<N>` when imported
by ordinal only.

Notes for `runtime/gdk`, reported by other projects ([related-work.md](related-work.md)) and to
be checked when it is built:

* Some GDK libraries reach the Game Runtime without importing it: they load `xgameruntime.dll`
  by name, look up `QueryApiImpl`, and call services by interface ID and vtable slot.
  **Checked here:** `XCurl.dll` and libHttpClient contain those names. Our loader fails
  `LoadLibrary` for an unknown DLL, so `runtime/gdk` provides that DLL as a guest-visible
  module.
* 33 functions in HRESULT-returning families return something else, such as
  `XTaskQueueDispatch`, `XUserCloseHandle`, and `XStoreIsLicenseValid`. Their stubs return
  that type's failure value, not `E_NOTIMPL`.
* `XUserFindUserByLocalId` returns `E_GAMEUSER_USER_NOT_FOUND` on the console, where the
  documentation says `ERROR_NOT_FOUND`. Follow the console.
* **Assumption** (Leonida): the main executable reaches the Game Runtime only through the
  `xgameruntime.lib` thunks, `QueryApiImpl` and then a vtable slot, not through named imports.
  So `runtime/gdk` is organised by interface ID and vtable, with each method at its slot.
  * Leonida's static scan finds 16 thunks in `XCurl.dll`, all of them called, and 404 in
    libHttpClient, of which it calls 15, all in `XThreadingImpl` and `XErrorImpl`.
  * The public layouts lack two interface IDs: one for `XGameSaveImpl` slots 33 to 49 and one
    for `XGameStreamingImpl` slots 3 to 6.
  * The two public layout sources, xgameruntime-docs and the Wine IDL in
    xodus-gaming/xgameruntime, disagree where the documentation has an "unknown" slot: the IDL
    leaves it out, so every later method sits one slot early there.
    `XUserImpl` slot 15 and `XDisplayImpl` slot 3 are such slots.
* More functions whose real return type isn't HRESULT (Leonida): `XErrorSetOptions` and
  `XSystemAllowFullDownloadBandwidth` return nothing, `XSystemIsHandleValid` returns `bool`,
  and `XErrorReport` does return HRESULT. `XThreadingImpl` slot 8 fails rather than returning
  `S_OK`.
* `XGameSave` calls on a closed handle return `E_GS_HANDLE_EXPIRED` (0x8083000D)
  (xbox-emulator).
* `XStore` sizes, unverified (Leonida): `STORE_SKU_ID_SIZE` 18 and `SKU_ID_SIZE` 5, which make
  `XStoreGameLicense` 104 bytes. Check them against the executable's use before relying on
  them.
* `XUserRegisterForDeviceAssociationChanged` (xcl, on Minecraft): the title treats a gamepad as
  connected only after the callback reports it moving from no user to the local user, and the
  callback runs on the title's task queue. Our single offline user gets the same transition.
* A title's window: Fortnite creates it with `CreateWindowExW` from
  `ext-ms-win-rtcore-ntuser-window-l1-1-0` (xcl), so the window HLE covers both the A and W
  forms.

Notes for `runtime/kernel`:

* `VirtualAlloc` on the console takes memory-type bits that desktop Windows refuses.
  **Checked here** (Windows 11 26200): `MEM_GRAPHICS` (0x10000000) and `MEM_TITLE`
  (0x40000000) each make the call fail with `ERROR_INVALID_PARAMETER`, with or without
  `MEM_RESERVE`. Leonida reported the bit values and drops both. Here `MEM_GRAPHICS` memory is
  GPU-visible, so it goes through `memory_tracker`, and `MEM_TITLE` is dropped. No side DLL
  imports `api-ms-win-core-memory-*`, so the family is unbound until the executable needs it.

So far there is the loader's own HLE, and `runtime/imports` with the default import policy
(`default_game_policy()`), xmem, COM and WinRT activation, and the path functions on
`runtime/io`. `runtime/gdk` doesn't exist yet.
The rows below cover what the side DLLs import ([side-dlls.md](side-dlls.md)). The networking
libraries are never mapped: the game's imports from them bind to offline implementations. The
full list comes from the main executable: `x64recomp imports <exe>` gives the starting list.
Read it locally and don't paste it here wholesale; add rows as each import gets a shim.

## runtime/loader

The loader functions, which `register_loader_hle` binds for imports from `kernel32.dll`,
`kernelbase.dll`, and `api-ms-win-core-libraryloader-*` (and `RtlPcToFileHeader` also from
`ntdll.dll` and `api-ms-win-core-rtlsupport-*`, and `FreeLibraryWhenCallbackReturns` from
`api-ms-win-core-threadpool-*`). They see only guest modules and the import
policy, never host DLLs outside it. A guest module's handle is its base; a DLL the policy
provides without a guest module gets a pseudo handle (see "The loader" in
`docs/execution-model.md`). On success each keeps the caller's last error.

| Function | Status | Notes |
|---|---|---|
| `GetModuleHandleA`, `GetModuleHandleW` | implemented | Null gives the guest executable (the host's while none is loaded). A name finds a guest module, or a pseudo handle for a DLL the policy sends to a host DLL that exists; anything else fails with `ERROR_MOD_NOT_FOUND`. |
| `GetModuleHandleExA`, `GetModuleHandleExW` | implemented | As above, plus `FROM_ADDRESS`, which finds the guest module containing the address and asks the host otherwise. Guest modules stay mapped until the loader goes, so the reference-count flags change nothing. |
| `GetProcAddress` | implemented | Null or a guest module resolves through that module's exports, forwarders included. A pseudo handle, or a host module the guest got from host code, resolves through the policy under its DLL name; a function the policy stubs gives null with `ERROR_PROC_NOT_FOUND`. |
| `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExA`, `LoadLibraryExW` | implemented | Maps a guest DLL found in the search paths, with its dependencies, or returns a pseudo handle as above. Networking and unknown DLLs fail with `ERROR_MOD_NOT_FOUND`; a guest DLL that fails to load gives `ERROR_DLL_INIT_FAILED`. Loading a guest DLL as a data file or image resource isn't supported (`ERROR_NOT_SUPPORTED`). |
| `FreeLibrary` | implemented | Succeeds and does nothing for guest modules and pseudo handles: guest modules stay mapped until the loader goes. |
| `FreeLibraryAndExitThread` | implemented | `ExitThread` for guest modules and pseudo handles. |
| `FreeLibraryWhenCallbackReturns` | implemented | Does nothing for guest modules and pseudo handles; anything else goes to the host's pool. The console's `msvcp140` pins itself this way in its task scheduler's threadpool callbacks. Windows 11's pool ignores the failed unload of a handle it doesn't know, but nothing documents that, so a guest handle never reaches it. xcl hit this on Minecraft ([related-work.md](related-work.md)). |
| `DisableThreadLibraryCalls` | implemented | Stops `DllMain` and TLS callback thread notifications for a guest module without static TLS. A module with static TLS keeps them, as on Windows, which accepts the call and ignores it. |
| `GetModuleFileNameA`, `GetModuleFileNameW` | implemented | The path a guest module was mapped from, or the DLL name for a pseudo handle; null gives the guest executable. Truncates and sets `ERROR_INSUFFICIENT_BUFFER` like Windows. |
| `RtlPcToFileHeader` | implemented | Guest modules first, then the host's answer. The C++ runtime's throw uses it to find the image base of its type information. |

The crash report's own function, which `register_crash_report` binds for imports from
`api-ms-win-core-errorhandling-*`, `kernel32.dll`, and `kernelbase.dll`
([execution-model.md](execution-model.md), "Crash reports").

| Function | Status | Notes |
|---|---|---|
| `SetUnhandledExceptionFilter` | implemented | Keeps the guest's filter instead of installing it and returns the one it kept before. The first call installs the crash report, whose filter calls the guest's after logging; when the guest's returns `EXCEPTION_CONTINUE_SEARCH`, the host's filter from before the report gets the exception. An executable's C runtime startup installs a filter before `main`. |

Other ways to look up a module or function exist: `GetProcAddressForCaller`,
`LoadPackagedLibrary`, and ntdll's `LdrGetProcedureAddress`, `LdrGetProcedureAddressEx`,
`LdrLoadDll`, and `LdrGetDllHandle`. xbox-emulator had to route each of them
([related-work.md](related-work.md)). Here none of them can reach the host: an import without a
rule becomes a logged stub. No side DLL imports them. If the main executable does, each gets
an HLE entry that calls the functions above.

## runtime/imports (the default policy)

`policy.cpp` binds each family of imports; the tables after it cover the HLE folders. The
console's own runtime DLLs (`vcruntime140`, `msvcp140`, and the rest) are guest modules, not
rows here. Why each row is what it is: [side-dlls.md](side-dlls.md), "Default import policy".

| Imports | Status | Notes |
|---|---|---|
| `api-ms-win-crt-*` | forwarded | The host Universal CRT. The common path functions and the exit functions are HLE (`runtime/imports/ucrtbase`), and so are the argument functions (`runtime/imports`, "the command line", below); the remaining path, process, and DLL-loading functions are stubbed by name: `-1`, null, or `ENOSYS`, by what each returns on failure. The whole `process` and `private` sets are stubbed. |
| `api-ms-win-core-*` families: console, debug, errorhandling, fibers, handle, heap, interlocked, localization, processenvironment, processthreads, processtopology, profile, rtlsupport, string, synch, sysinfo, threadpool, util | forwarded | The current-directory functions are HLE (`runtime/imports/kernelbase`), and so are `GetCommandLineA` and `GetCommandLineW` (below) and `SetUnhandledExceptionFilter` (`runtime/loader`, above). `CreateProcess*`, `OpenProcess`, `SearchPath`, and `NeedCurrentDirectoryForExePath` are stubbed. Thread creation and affinity become HLE with the console core layout in `runtime/kernel`. |
| `api-ms-win-core-libraryloader-*` | implemented | The loader's HLE (above). The resource functions are forwarded: a guest handle is a mapped image. |
| `api-ms-win-core-file-*` (and the same names from `kernel32`, `kernelbase`) | partial | Functions that take a handle are forwarded. The common path functions are HLE on `runtime/io` (`runtime/imports/kernelbase`). The other path functions stay stubbed: `INVALID_HANDLE_VALUE` for `CreateFile3` and the other `FindFirst` families, `INVALID_FILE_SIZE` for `GetCompressedFileSize`, zero otherwise. |
| `api-ms-win-core-registry-*` (and the same four names from `advapi32`) | partial | Reading the console-mode key is HLE (`runtime/imports/kernelbase`, below). Everything else returns `ERROR_CALL_NOT_IMPLEMENTED` as the status. Of the mapped side DLLs only `msvcp140_atomic_wait` imports it, for the leap-second key; the GDK thunk in `XCurl` and `libHttpClient.GDK` (never mapped) imports the four HLE functions. |
| `api-ms-win-core-com-*`, `api-ms-win-core-winrt-*` | forwarded | Except class activation (`runtime/imports/combase`). `CoGetInstanceFromFile` and `CoGetInstanceFromIStorage` are stubbed. |
| `oleaut32` | forwarded | Ordinals 4 and 6 bound by name to `SysAllocStringLen` and `SysFreeString`. |
| `xaudio2_9` | forwarded | Ordinal 1 bound by name to `XAudio2Create`. Bink's use is unverified until a video plays with sound. |
| `xmem.dll` | partial | `runtime/imports/xmem` |
| `d3d12_xs.dll` | stubbed | `E_NOTIMPL` until `runtime/gpu`. Only the two replaced DLLs import it. |
| `amd_fidelityfx_upscaler_xbox.dll`, `XboxBvhBuilder.dll` | replaced | Never mapped. Our replacements come at M3; until then imports from them are stubbed and `LoadLibrary` fails. |
| Networking DLLs and API sets | offline | Never forwarded or mapped (hard rule 5). |
| Anything else | stubbed | Deny by default. |

## runtime/imports/xmem

Console-only, so the names come from the import table. Signatures and the attribute word
follow SlimEra's public reference ([related-work.md](related-work.md)).

| Function | Status | Notes |
|---|---|---|
| `XMemAlloc` | partial | Heap memory type: a dedicated host heap, aligned to the larger of 16 and the requested alignment, and zeroed. Graphics and physical types log once and return null until `memory_tracker` exists. |
| `XMemFree` | implemented | Looks the address up. Null is ignored; a pointer that isn't a live allocation is logged once and ignored, not freed. |
| `XMemVirtualQuery` | partial | `(process, address, info)`, as the BVH builder calls it. Fills the host's `MEMORY_BASIC_INFORMATION` and, at +48, GPU access bits: bit 0 for committed, readable memory without a guard, since `memory_tracker` will be able to sync any of it. The caller accepts either of bits 0–1 as GPU memory; what each means is unknown. Bytes past +52 are never written. |
| `XMemGetWorkingSetStatistics` | partial | `(attributes, stats)`, as the FSR upscaler calls it with a 128-byte buffer, reading +8. Writes only +0, the title's memory limit (13.5 GiB, a Series X), and +8, the process's private bytes capped at that limit; the allocator in `attributes` is ignored. The structure's size differs between GDK versions, so writing more could overrun an older caller's (xcl found this). |

Only the two replaced DLLs import the last two; they are here for the main executable, which
links the same GDK.

Reported by xbox-emulator, for when the main executable imports it: Trackmania calls
`XMemGetWin32HeapStatistics` on the process heap, subtracts the sum of the first two fields
from its memory budget, and stops loading maps when less than 256 MB remain. xbox-emulator first
returned committed then allocated bytes, which counted the heap twice. It now returns allocated
bytes, then committed minus allocated. The console fills the structure through a heap query
that desktop `ntdll` doesn't support, so the function needs HLE rather than forwarding.

## runtime/imports/combase

Class activation, for imports from `api-ms-win-core-com-*`, `api-ms-win-core-winrt-*`,
`combase`, and `ole32`. Forwarding it could reach any registered class, networking ones
included (hard rule 5), so it goes through an allowlist, empty for now. A refused class is
logged once by CLSID or class name.

| Function | Status | Notes |
|---|---|---|
| `CoCreateInstance`, `CoCreateInstanceEx`, `CoCreateInstanceFromApp`, `CoGetClassObject` | implemented | Allowed classes are forwarded. Others give `REGDB_E_CLASSNOTREG` with the out pointers cleared. A remote server is always refused. |
| `RoGetActivationFactory`, `RoActivateInstance` | implemented | Allowed classes, and classes the guest registered itself, are forwarded. Others give `REGDB_E_CLASSNOTREG`. |
| `RoRegisterActivationFactories`, `RoRevokeActivationFactories` | implemented | Forwarded, recording which classes the guest registered. |

## runtime/imports/kernelbase

Win32 path functions on the console's drives ([io.md](io.md)), for imports from
`api-ms-win-core-file-*` (the current-directory functions from
`api-ms-win-core-processenvironment-*`, and four move and copy functions from
`api-ms-win-core-kernel32-legacy-*`), `kernel32`, and `kernelbase`. Each resolves its path
through `runtime/io`, checks the mount's access, and calls the host function on the host path.
A path that leaves the mounts gives the error Win32 gives for it; a write to the read-only
install gives `ERROR_ACCESS_DENIED`. The handles are host handles, so the handle functions stay
forwarded.

| Function | Status | Notes |
|---|---|---|
| `CreateFileW`, `CreateFileA`, `CreateFile2` | implemented | On a read-only mount, write access, `DELETE_ON_CLOSE`, and the creating dispositions are refused; `OPEN_ALWAYS` on an existing file opens it and sets `ERROR_ALREADY_EXISTS`. The guest's security descriptor is dropped; inheritance is kept. |
| `FindFirstFileW`, `FindFirstFileA`, `FindFirstFileExW`, `FindFirstFileExA` | implemented | Wildcards only in the last component. A mount's root lists `.` and `..`, unlike a drive's root. |
| `GetFileAttributesW`, `GetFileAttributesA`, `GetFileAttributesExW`, `GetFileAttributesExA`, `SetFileAttributesW` | implemented | |
| `CreateDirectoryW`, `CreateDirectoryA`, `RemoveDirectoryW`, `RemoveDirectoryA`, `DeleteFileW`, `DeleteFileA` | implemented | A mount's root can't be removed. |
| `MoveFileW`, `MoveFileA`, `MoveFileExW`, `MoveFileExA`, `MoveFileWithProgressW`, `MoveFileWithProgressA` | implemented | Both ends mounted and writable, and neither a mount's root. Flags other than replace, copy, write-through, and fail-if-not-trackable are refused with `ERROR_INVALID_PARAMETER`, so moves at reboot (a null destination included) and hard links are. `MoveFile` is `MoveFileEx` with `MOVEFILE_COPY_ALLOWED`, which xcl measured on Windows 11. The progress routine is guest code, called directly. The legacy kernel32 API set carries `MoveFileW`, `MoveFileA`, and `MoveFileExA`. |
| `CopyFileW`, `CopyFileA`, `CopyFileExW`, `CopyFileExA`, `CopyFile2` | implemented | The source mounted (writable only with `COPY_FILE_OPEN_SOURCE_FOR_WRITE`), the destination mounted and writable; copying a symlink as a link is refused. `CopyFile` is `CopyFileEx` with `COPY_FILE_FAIL_IF_EXISTS` for its flag. The host's `CopyFile2` is looked up at run time, not imported: Wine 9, under Proton, has none and binds an import of it to a stub that ends the process (reported by Leonida). Without it, `CopyFileExW` does the copy with the flags it knows and the cancel flag, and the progress routine isn't called. `CopyFileW` comes from `api-ms-win-core-file-l2-1-2`, `CopyFileA` from the legacy kernel32 API set. |
| `ReplaceFileW`, `ReplaceFileA` | implemented | The usual save step. The replaced file, the replacement, and the backup when one is named must all be mounted and writable, and none a mount's root. The two reserved arguments must be null, or the call fails with `ERROR_INVALID_PARAMETER`. xcl and xbox-emulator both pointed out that the move, copy, and replace functions need the same redirection as `CreateFileW`. |
| `GetDiskFreeSpaceExW` | implemented | The host volume behind the mount. |
| `GetTempPathW`, `GetTempPath2W` | implemented | `T:\`. |
| `GetFullPathNameW`, `GetFullPathNameA` | implemented | Console paths only. |
| `GetCurrentDirectoryW`, `GetCurrentDirectoryA`, `SetCurrentDirectoryW`, `SetCurrentDirectoryA` | implemented | The guest's own current directory, starting at `G:\`; the host process's is never changed. |

The registry, for imports from `api-ms-win-core-registry-*` and `advapi32`. The GDK thunk
library that titles link works out `XSystemGetDeviceType` itself, from one registry value
rather than a call into the Game Runtime: `ConsoleMode`, a `REG_DWORD` under
`HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM`. It maps 0x60 to Series S (6), 0x64
to Series X (7), 0x68 to a Series devkit (8), lower values to the Xbox One family, and a
missing key or value to PC (1). Checked on the thunk in `XCurl.dll`; xcl
([related-work.md](related-work.md)) reported it first. We answer that key as a Series X, so
the title takes its console paths, and no other key: the host registry is never read.

| Function | Status | Notes |
|---|---|---|
| `RegOpenKeyExW`, `RegOpenKeyW` | partial | The OEM key under `HKEY_LOCAL_MACHINE`, in any case and with stray separators, opens as a handle of our own. Any other key is logged once and gets `ERROR_CALL_NOT_IMPLEMENTED`. |
| `RegQueryValueExW` | partial | On the OEM key: `ConsoleMode` is `REG_DWORD` 0x64, with the Win32 size rules (`ERROR_MORE_DATA` and the needed size for a short buffer); other values, the default value included, are `ERROR_FILE_NOT_FOUND`. On any other key: logged once, `ERROR_CALL_NOT_IMPLEMENTED`. |
| `RegCloseKey` | implemented | Only the OEM key is ever open; anything else is `ERROR_INVALID_HANDLE`. |

## runtime/imports/ucrtbase

C runtime path functions, for imports from `api-ms-win-crt-*`. Each resolves its path through
`runtime/io` and calls the wide function of `ucrtbase.dll`, the CRT the guest's other CRT
imports resolve to, so `FILE*` objects, `errno`, and returned buffers belong to the guest's CRT
even in a debug build of the runtime. Errors set `errno` and `_doserrno` as the CRT does.

| Function | Status | Notes |
|---|---|---|
| `fopen`, `_wfopen`, `fopen_s`, `_wfopen_s`, `_fsopen`, `_wfsopen` | implemented | A writing mode on the read-only mount gives `EACCES`. |
| `_stat64`, `_wstat64`, `_access`, `_waccess` | implemented | `st_dev` and `st_rdev` are the console drive's number. |
| `_mkdir`, `_wmkdir`, `_rmdir`, `_wrmdir`, `remove`, `_wremove`, `_unlink`, `_wunlink`, `rename`, `_wrename` | implemented | A mount's root can't be removed or renamed. |
| `_getcwd`, `_wgetcwd`, `_chdir`, `_wchdir` | implemented | The same current directory as the Win32 functions. A null buffer is allocated with the guest CRT's `malloc`. |

The executable's exit handlers, from `api-ms-win-crt-runtime-*`. The host CRT's tables are
process-wide, so these work on tables of the guest's own, with the UCRT's rules as measured on
the host ([execution-model.md](execution-model.md), phase 5, "Exit"). The ending functions then
call the runner's end function, `ExitProcess` by default.

| Function | Status | Notes |
|---|---|---|
| `_crt_atexit`, `_crt_at_quick_exit` | implemented | What `atexit`, `at_quick_exit`, and an executable's static destructors reach. A null function is refused with -1; the UCRT faults on one. DLLs keep their own tables through `_register_onexit_function`, which stays forwarded. |
| `_register_thread_local_exe_atexit_callback` | implemented | Kept for the guest alone, so a host executable that registered with the same CRT doesn't clash. A second call ends the process with a fast fail, as the UCRT's does. |
| `exit`, `_cexit` | implemented | The thread_local callback for the calling thread, then the atexit table newest first; a handler registered meanwhile runs next. `exit` then ends the guest process; `_cexit` returns, and doesn't flush the streams, as the UCRT's doesn't. |
| `quick_exit` | implemented | The at-quick-exit table, then the end. |
| `_exit`, `_Exit`, `_c_exit` | implemented | No handlers; the first two end the guest process. |

## runtime/imports (the command line)

The guest's own program path and command line (`command_line.cpp`), which the runner sets
before it loads anything. `GetCommandLineA` and `GetCommandLineW` come from
`api-ms-win-core-processenvironment-*`, `kernel32`, and `kernelbase`; the CRT functions from
`api-ms-win-crt-*`. The parsing follows the host UCRT as measured on it
([execution-model.md](execution-model.md), phase 5, "Command line"). The host process's own
command line is never read or changed.

| Function | Status | Notes |
|---|---|---|
| `GetCommandLineA`, `GetCommandLineW` | implemented | The guest's line; the narrow one in the ANSI code page. A line left empty is the quoted program path, as `CreateProcess` makes it. |
| `_configure_narrow_argv`, `_configure_wide_argv` | implemented | Parse `_acmdln` or `_wcmdln` as it is when called, into a new argv each call, and set the program name of their width. An empty or null line parses the program path instead. Mode 0 changes nothing; a mode out of range is `EINVAL` through the invalid parameter handler. Expanded mode leaves wildcards as written and logs that once. |
| `__p___argc`, `__p___argv`, `__p___wargv`, `__p__acmdln`, `__p__wcmdln`, `__p__pgmptr`, `__p__wpgmptr` | implemented | Stable pointers to the guest's own globals. |
| `_get_pgmptr`, `_get_wpgmptr` | implemented | `EINVAL` through the handler before the configure of their width, or for a null out pointer. |
| `_get_narrow_winmain_command_line`, `_get_wide_winmain_command_line` | implemented | The rest of `_acmdln` or `_wcmdln` after the program name and the blanks after it; an empty string for a null line. |

<!-- Template for a folder:

## runtime/imports/<dll>

| Function | Status | Notes |
|---|---|---|
| `ExampleFunction` | forwarded | |

-->
