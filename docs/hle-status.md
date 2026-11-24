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
be checked as each service is built:

* Some GDK libraries reach the Game Runtime without importing it: they load `xgameruntime.dll`
  by name, look up `QueryApiImpl`, and call services by interface ID and vtable slot.
  **Checked here** on the console's libHttpClient and XCurl, slot by slot. **Taken:**
  `runtime/gdk` provides that DLL, and the loader gives the guest a pseudo handle for it
  ([gdk.md](gdk.md), "The Game Runtime DLL").
* 33 functions in HRESULT-returning families return something else, such as
  `XTaskQueueDispatch`, `XUserCloseHandle`, and `XStoreIsLicenseValid`. Their stubs return
  that type's failure value, not `E_NOTIMPL`.
* `XUserFindUserByLocalId` returns `E_GAMEUSER_USER_NOT_FOUND` on the console, where the
  documentation says `ERROR_NOT_FOUND`. Follow the console. **Taken**, for `XUserFindUserById`
  too.
* **Assumption** (Leonida): the main executable reaches the Game Runtime only through the
  `xgameruntime.lib` thunks, `QueryApiImpl` and then a vtable slot, not through named imports.
  So `runtime/gdk` is organised by interface ID and vtable, with each method at its slot.
  * Leonida's static scan finds 16 thunks in `XCurl.dll`, all of them called, and 404 in
    libHttpClient, of which it calls 15, all in `XThreadingImpl` and `XErrorImpl`.
  * The public layouts lack two interface IDs: one for `XGameSaveImpl` slots 33 to 49 and one
    for `XGameStreamingImpl` slots 3 to 6. The first is ab4ae4fb-6508-4950-a032-45fd4bf8c43b,
    from the console's libHttpClient thunks ([gdk.md](gdk.md), "XGameSave").
  * The two public layout sources, xgameruntime-docs and the Wine IDL in
    xodus-gaming/xgameruntime, disagree where the documentation has an "unknown" slot: the IDL
    leaves it out, so every later method sits one slot early there.
    `XUserImpl` slot 15 and `XDisplayImpl` slot 3 are such slots.
* More functions whose real return type isn't HRESULT (Leonida): `XErrorSetOptions` and
  `XSystemAllowFullDownloadBandwidth` return nothing, `XSystemIsHandleValid` returns `bool`,
  and `XErrorReport` does return HRESULT. `XThreadingImpl` slot 8 fails rather than returning
  `S_OK`. **Taken** for the XError functions and slot 8, which fails with `E_NOTIMPL` here.
* `XGameSave` calls on a closed handle return `E_GS_HANDLE_EXPIRED` (0x8083000D)
  (xbox-emulator). **Taken**, also for the containers and updates of a closed provider.
* `XStore` sizes, unverified (Leonida): `STORE_SKU_ID_SIZE` 18 and `SKU_ID_SIZE` 5, which make
  `XStoreGameLicense` 104 bytes. Check them against the executable's use before relying on
  them.
* `XUserRegisterForDeviceAssociationChanged` (xcl, on Minecraft): the title treats a gamepad as
  connected only after the callback reports it moving from no user to the local user, and the
  callback runs on the title's task queue. Our single offline user gets the same transition.
  xbox-emulator adds the reverse on unplug, `E_GAMEUSER_USER_NOT_FOUND` from
  `XUserFindForDevice` for any ID but the pad's, and `E_ABORT` from
  `XUserFindControllerForUserWithUiAsync` with no pad, as a dismissed pairing dialog gives.
  **Taken** for `E_ABORT`. The callbacks are kept but not called, and every device belongs to
  the user, until input exists (M4).
* `XGameSave` container names are 1 to 256 characters, otherwise
  `E_GS_INVALID_CONTAINER_NAME`, in the sync and async forms (xcl: an empty name deleted the
  whole save folder). **Taken**, with the public character rules as well.
* `XPackageGetIdentifier` takes three arguments: path or name, buffer size, buffer (xcl).
* `XSystemGetConsoleId` gives the documented PC id, `00000000.00000000.00000000.00000000.00`,
  38 characters; titles size the buffer for exactly that (xcl, and xbox-emulator's 39 bytes).
* `XNetworking`'s two change registrations send one initial callback on the completion port
  and none after (Leonida, from Microsoft's documentation).
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
`runtime/io`. `runtime/gdk` has XTaskQueue, XAsync, XError, and XThread, which the guest
reaches through our `xgameruntime.dll`.
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
| `GetModuleHandleA`, `GetModuleHandleW` | implemented | Null gives the guest executable (the host's while none is loaded). A name finds a guest module, or a pseudo handle for a DLL the policy provides once it counts as loaded: a guest module imports it or has loaded it with `LoadLibrary`, or it is an API set whose host DLL is loaded. Anything else fails with `ERROR_MOD_NOT_FOUND`, as on Windows, so a title probing for an optional DLL doesn't find one nothing loaded (xcl found this). **Checked here** (Windows 11 26200): the host's `GetModuleHandle` answers an API set name, in any case and without `.dll`, with its host DLL, and a set that doesn't exist with error 126. |
| `GetModuleHandleExA`, `GetModuleHandleExW` | implemented | As above, plus `FROM_ADDRESS`, which finds the guest module containing the address and asks the host otherwise. Guest modules stay mapped until the loader goes, so the reference-count flags change nothing. |
| `GetProcAddress` | implemented | Null or a guest module resolves through that module's exports, forwarders included. A pseudo handle, or a host module the guest got from host code, resolves through the policy under its DLL name; a function the policy stubs gives null with `ERROR_PROC_NOT_FOUND`. |
| `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExA`, `LoadLibraryExW` | implemented | Maps a guest DLL found in the search paths, with its dependencies, or returns a pseudo handle as above. Networking and unknown DLLs fail with `ERROR_MOD_NOT_FOUND`; a guest DLL that fails to load gives `ERROR_DLL_INIT_FAILED`. Loading a guest DLL as a data file or image resource isn't supported (`ERROR_NOT_SUPPORTED`). |
| `FreeLibrary` | implemented | Succeeds and does nothing for guest modules and pseudo handles: guest modules stay mapped until the loader goes. |
| `FreeLibraryAndExitThread` | implemented | `ExitThread` for guest modules and pseudo handles. |
| `FreeLibraryWhenCallbackReturns` | implemented | Does nothing for guest modules and pseudo handles; anything else goes to the host's pool. The console's `msvcp140` pins itself this way in its task scheduler's threadpool callbacks. Windows 11's pool ignores the failed unload of a handle it doesn't know, but nothing documents that, so a guest handle never reaches it. xcl hit this on Minecraft ([related-work.md](related-work.md)). |
| `DisableThreadLibraryCalls` | implemented | Stops `DllMain` and TLS callback thread notifications for a guest module without static TLS. A module with static TLS keeps them, as on Windows, which accepts the call and ignores it. |
| `GetModuleFileNameA`, `GetModuleFileNameW` | implemented | The path a guest module was mapped from, as the `guest_path` loader option gives it (the runner gives console paths, such as `G:\game.exe`), or the DLL name for a pseudo handle; null gives the guest executable. Truncates and sets `ERROR_INSUFFICIENT_BUFFER` like Windows; the ANSI form cuts after a whole character, never inside a double-byte one. |
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
| `api-ms-win-core-*` families: console, debug, errorhandling, fibers, handle, heap, interlocked, localization, processenvironment, processthreads, processtopology, profile, psm-appnotify, rtlsupport, string, synch, sysinfo, threadpool, util | forwarded | The current-directory functions are HLE (`runtime/imports/kernelbase`), and so are `GetCommandLineA` and `GetCommandLineW` (below) and `SetUnhandledExceptionFilter` (`runtime/loader`, above). `CreateProcess*`, `OpenProcess`, `SearchPath`, and `NeedCurrentDirectoryForExePath` are stubbed. Thread creation and affinity become HLE with the console core layout in `runtime/kernel`. |
| `api-ms-win-core-libraryloader-*` | implemented | The loader's HLE (above). The resource functions are forwarded: a guest handle is a mapped image. |
| `api-ms-win-core-file-*` (and the same names from `kernel32`, `kernelbase`) | partial | Functions that take a handle are forwarded. The common path functions are HLE on `runtime/io` (`runtime/imports/kernelbase`). The other path functions stay stubbed: `INVALID_HANDLE_VALUE` for `CreateFile3` and the other `FindFirst` families, `INVALID_FILE_SIZE` for `GetCompressedFileSize`, zero otherwise. |
| `api-ms-win-core-registry-*` (and the same four names from `advapi32`) | partial | Reading the console-mode key is HLE (`runtime/imports/kernelbase`, below). Everything else returns `ERROR_CALL_NOT_IMPLEMENTED` as the status. Of the mapped side DLLs only `msvcp140_atomic_wait` imports it, for the leap-second key; the GDK thunk in `XCurl` and `libHttpClient.GDK` (never mapped) imports the four HLE functions. |
| `api-ms-win-core-com-*`, `api-ms-win-core-winrt-*` | forwarded | Except class activation (`runtime/imports/combase`). `CoGetInstanceFromFile` and `CoGetInstanceFromIStorage` are stubbed. |
| `oleaut32` | forwarded | Ordinals 4 and 6 bound by name to `SysAllocStringLen` and `SysFreeString`. |
| `xaudio2_9` | forwarded | Ordinal 1 bound by name to `XAudio2Create`. Bink's use is unverified until a video plays with sound. |
| `xmem.dll` | partial | `runtime/imports/xmem` |
| `ext-ms-win-rtcore-ntuser-window-*`, `ext-ms-win-rtcore-ntuser-message-*` (and their `-ansi` sets) | forwarded | The seven functions the executable imports, bound from `user32` by name, since a host need not have the extension sets: `RegisterClassExA`, `CreateWindowExA`, `DefWindowProcA`, `ShowWindow`, `PeekMessageA`, `DispatchMessageA`, and `TranslateMessage`. The window procedure is guest code, which `user32` calls directly. Anything else from these sets is unreviewed and stubbed. |
| `api-ms-win-core-memory-*` | stubbed | `VirtualAlloc`, `VirtualFree`, and `VirtualQueryEx`, which the executable imports. The game uses them on its physical-page windows and placeholders (xcl, Leonida; [boot.md](boot.md)), so they become HLE through `memory_tracker`, not forwards. |
| `d3d12_xs.dll` | partial | HLE in `runtime/imports/d3d12_xs`: `D3D12ConfigureVirtualMemoryX` logs the description and succeeds, since the host GPU has its own address space, and `D3D12XboxCreateDevice` hands out `runtime/gpu`'s device proxy over a PC device on the default adapter ([d3d12x.md](d3d12x.md), "Our device"). The adapter and the parameter block are logged, not used. Everything else is `E_NOTIMPL`. |
| `gameinput.dll` | partial | Ordinal 1, `GameInputCreate`, the only import, gives a stand-in `IGameInput` with no devices (`runtime/input`): the timestamp works, and every other slot is logged once and fails with `E_NOTIMPL`. Keyboard, mouse, and gamepads come at M4. |
| `xcrdapi.dll` | partial | DirectStorage. `DStorageGetFactory`, the only import, gives `runtime/io`'s factory ([io.md](io.md), "DirectStorage"). |
| `AcpHal.dll`, `xapu.dll`, `chad.dll` (the audio processor), `xg_xs.dll` (texture layouts) | stubbed | `E_NOTIMPL`, since these are HRESULT APIs. `ApuMapApuAddress`, `ApuIsVirtualAddressValid`, and `XApuGetResultCount` return an address, a BOOL, and a count, so their stubs return zero with `ERROR_CALL_NOT_IMPLEMENTED`. xcl listed the DLLs a retail title loads ([related-work.md](related-work.md)). Without these rules their stubs returned zero, which an HRESULT caller reads as `S_OK`. |
| `amd_fidelityfx_upscaler_xbox.dll`, `XboxBvhBuilder.dll` | replaced | Never mapped. Our replacements come at M3; until then imports from them are stubbed and `LoadLibrary` fails. |
| Networking DLLs and API sets | offline | Never forwarded or mapped (hard rule 5). |
| Anything else | stubbed | Deny by default. |

## runtime/imports/xmem

Console-only, so the names come from the import table. Signatures and the attribute word
follow SlimEra's public reference ([related-work.md](related-work.md)).

| Function | Status | Notes |
|---|---|---|
| `XMemAlloc` | partial | Heap memory type: a dedicated host heap, aligned to the larger of 16 and the requested alignment, and zeroed. Graphics and physical types log once and return null until `memory_tracker` serves them. |
| `XMemFree` | implemented | Looks the address up. Null is ignored; a pointer that isn't a live allocation is logged once and ignored, not freed. |
| `XMemVirtualQuery` | partial | `(process, address, info)`, as the BVH builder calls it. Fills the host's `MEMORY_BASIC_INFORMATION` and, at +48, GPU access bits: bit 0 for committed, readable memory without a guard, since `memory_tracker` will be able to sync any of it. The caller accepts either of bits 0–1 as GPU memory; what each means is unknown. Bytes past +52 are never written. |
| `XMemGetWorkingSetStatistics` | partial | `(attributes, stats)`, as the FSR upscaler calls it with a 128-byte buffer, reading +8. Writes only +0, the title's memory limit (13.5 GiB, a Series X), and +8, the process's private bytes capped at that limit; the allocator in `attributes` is ignored. The structure's size differs between GDK versions, so writing more could overrun an older caller's (xcl found this). **Reported** by Xbox2Windows: +8 is the GPU memory in use, which it reads from DXGI's `QueryVideoMemoryInfo`, counting each adapter LUID once. Not taken yet: until `memory_tracker` owns GPU memory, private bytes are the closest count. |
| `XMemTransferMemory` | implemented | `(direction, size)`. The executable calls it once at start with 1 and 16 MiB (Leonida found the call). It succeeds and moves nothing, since a PC process has no title and system partitions to move memory between. |
| `XMemVirtualAlloc` | partial | `(address, size, type, attributes, protection)`, through `memory_tracker`, in the forms the executable calls first ([boot.md](boot.md)). The page-size bits of `type` give the alignment. A physical reserve without commit stays a placeholder for page maps, `MEM_RESERVE_PLACEHOLDER` reserves one, and `MEM_REPLACE_PLACEHOLDER` carves the range out of an earlier one. The `PAGE_GRAPHICS_*` bits of `protection` are recorded for the GPU side and left off the host page. The attribute word (3, 7, and 0x21 seen) is logged and ignored. |
| `XMemAllocDefault`, `XMemFreeDefault` | implemented | The same as `XMemAlloc` and `XMemFree`: the default allocator behind the allocation hooks, and we install none. |
| `XMemIsToolsMemAvailable` | implemented | FALSE, as on a retail console. |
| `XMemAllocatePhysicalPages`, `XMemFreePhysicalPages`, `XMemMapPhysicalPages`, `XMemMapPhysicalPagesScatter` | implemented | Through `memory_tracker`, CPU side only: one pagefile-backed section per allocation call, one 64 KiB view per mapped page, numbers in 4 KiB frames ([boot.md](boot.md), "Physical pages"). Each new attribute word (`0x280` and `0x380` seen) is logged once and otherwise ignored. Map returns the address and scatter TRUE; a failure is logged with the host error. |

Only the BVH builder, which is replaced, imports `XMemVirtualQuery`. **Checked here:** the
executable imports 12 xmem functions: `XMemAlloc`, `XMemFree`, `XMemGetWorkingSetStatistics`
and `XMemTransferMemory`, and the eight added for the first boot runs: `XMemVirtualAlloc`,
the four physical-page functions, `XMemAllocDefault`, `XMemFreeDefault`, and
`XMemIsToolsMemAvailable`.

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
install gives `ERROR_ACCESS_DENIED`, unless Win32 would fail it for something it checks first.
The handles are host handles, so the handle functions stay forwarded.

| Function | Status | Notes |
|---|---|---|
| `CreateFileW`, `CreateFileA`, `CreateFile2` | implemented | On a read-only mount, write access, `DELETE_ON_CLOSE`, and the creating dispositions are refused, with the error Win32's order of checks gives (measured on Windows 11): `TRUNCATE_EXISTING` without the `GENERIC_WRITE` bit is `ERROR_INVALID_PARAMETER` even for a missing file (xcl noted the check), then a missing directory on the way is `ERROR_PATH_NOT_FOUND`, a missing file the disposition needs `ERROR_FILE_NOT_FOUND`, and the rest `ERROR_ACCESS_DENIED`. `OPEN_ALWAYS` on an existing file opens it and sets `ERROR_ALREADY_EXISTS`. The guest's security descriptor is dropped; inheritance is kept. |
| `FindFirstFileW`, `FindFirstFileA`, `FindFirstFileExW`, `FindFirstFileExA` | implemented | Wildcards only in the last component. A mount's root lists `.` and `..`, unlike a drive's root. |
| `GetFileAttributesW`, `GetFileAttributesA`, `GetFileAttributesExW`, `GetFileAttributesExA`, `SetFileAttributesW` | implemented | |
| `CreateDirectoryW`, `CreateDirectoryA`, `RemoveDirectoryW`, `RemoveDirectoryA`, `DeleteFileW`, `DeleteFileA` | implemented | A mount's root can't be removed. |
| `MoveFileW`, `MoveFileA`, `MoveFileExW`, `MoveFileExA`, `MoveFileWithProgressW`, `MoveFileWithProgressA` | implemented | Both ends mounted and writable, and neither a mount's root. Flags other than replace, copy, write-through, and fail-if-not-trackable are refused with `ERROR_INVALID_PARAMETER`, so moves at reboot (a null destination included) and hard links are. `MoveFile` is `MoveFileEx` with `MOVEFILE_COPY_ALLOWED`, which xcl measured on Windows 11. The progress routine is guest code, called directly. The legacy kernel32 API set carries `MoveFileW`, `MoveFileA`, and `MoveFileExA`. |
| `CopyFileW`, `CopyFileA`, `CopyFileExW`, `CopyFileExA`, `CopyFile2` | implemented | The source mounted (writable only with `COPY_FILE_OPEN_SOURCE_FOR_WRITE`), the destination mounted and writable; copying a symlink as a link is refused. `CopyFile` is `CopyFileEx` with `COPY_FILE_FAIL_IF_EXISTS` for its flag. The host's `CopyFile2` is looked up at run time, not imported: Wine 9, under Proton, has none and binds an import of it to a stub that ends the process (reported by Leonida). `CopyFile2` checks its extended parameters before either path, as Windows 11 does (measured): the exact size of the first or second version; no `dwCopyFlags` bit `0x40000000` (every other bit, defined or not, is taken); and in the second version only the defined `dwCopyFlagsV2` bits, an I/O size of zero or 4 KiB to 1 GiB, an I/O rate of zero or at least 512 KB/s, and zeroed reserved fields. Anything else is `E_INVALIDARG`. Without a host `CopyFile2`, `CopyFileExW` does the copy with the flags it knows, the cancel flag, and the second version's old-style progress routine, which Windows also calls with the callback context. A `CopyFile2` progress routine isn't called (logged once), and copying a directory or a reparse point as such (`COPY_FILE_DIRECTORY`, `COPY_FILE_OPEN_AND_COPY_REPARSE_POINT`) fails with `ERROR_NOT_SUPPORTED` (logged once). On an ordinary file those flags change nothing, so they are dropped there; Leonida's fallback refuses them whatever the source is. `COPY_FILE_SKIP_ALTERNATE_STREAMS` is dropped, since the guest can't name a stream to tell the difference. `CopyFileW` comes from `api-ms-win-core-file-l2-1-2`, `CopyFileA` from the legacy kernel32 API set. |
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

## runtime/gdk

The Game Runtime ([gdk.md](gdk.md)). Signatures and layouts are the public GDK's; XTaskQueue
and XAsync follow libHttpClient's behavior (MIT), with no code taken. A title reaches these
functions through `xgameruntime.dll`'s `QueryApiImpl` and the service vtables, not through named
imports, so the rows give the public names and gdk.md ("Slots", "XUser", and "XGameSave") gives
each slot. Tested by `test_gdk`, and the policy's binding by `test_imports`.

**xgameruntime.dll**, bound by `default_game_policy()`. Every other export is `E_NOTIMPL`, and
`GetProcAddress` finds nothing for it.

| Function | Status | Notes |
|---|---|---|
| `QueryApiImpl` | implemented | Static objects for the threading, error, feature, XUser, XUser device, and XGameSave services. `REGDB_E_CLASSNOTREG` for any other service, logged once per class; `E_NOINTERFACE` for an interface the class doesn't have, logged once per class and interface. |
| `InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2` | implemented | Log their versions and flags once and succeed. |
| `UninitializeApiImpl` | implemented | Succeeds. |
| `XErrorReport` | implemented | Also exported by name. See the XError rows. |

**Services.**

| Function | Status | Notes |
|---|---|---|
| `XGameRuntimeIsFeatureAvailable` | implemented | True for every feature, as on the console; each feature number is logged once. |
| `XErrorReport`, `XErrorSetCallback`, `XErrorSetOptions` | implemented | Every report reaches our log, then the callback, then the options. `FailFastOnError` is logged, not acted on. Error slot 3 is unknown: logged once and ignored. |
| `XThreadSetTimeSensitive`, `XThreadIsTimeSensitive`, `XThreadAssertNotTimeSensitive` | implemented | A per-thread mark; the assert logs once per marked thread and never stops the call. |
| Threading slots 8 and 28 | stubbed | Unknown functions: logged once with the caller, `E_NOTIMPL`. |
| `XTaskQueueCreate`, `XTaskQueueCreateComposite` | implemented | Manual, ThreadPool, SerializedThreadPool, and Immediate ports on the host thread pool. A composite queue shares other queues' ports and terminates only its own callbacks. |
| `XTaskQueueGetPort`, `XTaskQueueDuplicateHandle`, `XTaskQueueCloseHandle` | implemented | Counted handles; a handle that isn't a live queue gives `E_GAMERUNTIME_INVALID_HANDLE` (0x8924010C). |
| `XTaskQueueDispatch` | implemented | Any port, not only Manual ones. Returns `bool`, not HRESULT. |
| `XTaskQueueIsEmpty` | implemented | Private to the Game Runtime, and no slot reaches it (slot 28 is something else). Covers the whole port, which other queues may share. |
| `XTaskQueueTerminate` | implemented | Waiting callbacks, delayed ones, and waiters run once more as canceled; work port, then completion port, then the callback. |
| `XTaskQueueSubmitCallback`, `XTaskQueueSubmitDelayedCallback` | implemented | `E_ABORT` once the queue is terminating. Delayed callbacks run in due order. |
| `XTaskQueueRegisterWaiter`, `XTaskQueueUnregisterWaiter` | implemented | Through a duplicate of the caller's handle, re-armed after each callback. |
| `XTaskQueueRegisterMonitor`, `XTaskQueueUnregisterMonitor` | implemented | Unregister waits out calls under way on other threads. |
| `XTaskQueueSuspendTermination`, `XTaskQueueResumeTermination` | implemented | |
| `XTaskQueueGetCurrentProcessTaskQueue`, `XTaskQueueSetCurrentProcessTaskQueue` | implemented | Two ThreadPool ports by default, made on first use; null leaves no process queue. |
| `XAsyncBegin`, `XAsyncBeginAlloc` | implemented | `E_NO_TASK_QUEUE` (0x800701AB) without a queue, `E_ABORT` on a terminating one. A failing begin step ends the call while `XAsyncBegin` returns `S_OK`. `XAsyncBeginAlloc` is private to the Game Runtime, and no known slot reaches it. |
| `XAsyncSchedule`, `XAsyncComplete` | implemented | A second schedule before the first has run, and a work step that succeeds without completing, give `E_UNEXPECTED`. |
| `XAsyncRun` | implemented | |
| `XAsyncGetStatus`, `XAsyncGetResultSize`, `XAsyncGetResult` | implemented | Checks in libHttpClient's order ([gdk.md](gdk.md), "Status and results"). |
| `XAsyncCancel` | implemented | Asks the provider; a provider that can't cancel runs to its end. |
| `XUserAddAsync`, `XUserAddResult` | implemented | Any options give the one offline user through the queue, without UI. |
| `XUserDuplicateHandle`, `XUserCloseHandle`, `XUserCompare`, `XUserGetMaxUsers` | implemented | One handle with a reference count; any other handle gives `E_INVALIDARG`. `XUserCloseHandle` returns nothing. One user at most. |
| `XUserGetLocalId`, `XUserGetId`, `XUserGetIsGuest`, `XUserGetState`, `XUserGetAgeGroup` | implemented | Local ID 1, user ID 0x0009000000000001, not a guest, signed in, adult. |
| `XUserFindUserByLocalId`, `XUserFindUserById` | implemented | The user for its own IDs, `E_GAMEUSER_USER_NOT_FOUND` (0x89245104) for any other, as on the console. |
| `XUserGetGamertag` | implemented | "Player", with an empty suffix. `E_NOT_SUFFICIENT_BUFFER` for a buffer too small for it. |
| `XUserIsStoreUser` | implemented | True. |
| `XUserCheckPrivilege` | implemented | Game DVR, cloud saved games, and user-generated content granted; online privileges denied as `Restricted`; an unknown one denied and logged once. |
| `XUserResolvePrivilegeWithUiAsync`, `XUserResolvePrivilegeWithUiResult` | implemented | No UI: `S_OK` for a granted privilege, `E_ABORT` for a denied one, through the queue. |
| `XUserGetTokenAndSignatureAsync` and its results, in both forms | offline | End with 0x80072EFD (`ERROR_INTERNET_CANNOT_CONNECT`) through the queue, as on a console that can't reach the Xbox network; logged once. |
| `XUserGetGamerPictureAsync` and its results | offline | The same failure: there is no picture without the network. |
| `XUserResolveIssueWithUiAsync`, `XUserResolveIssueWithUiResult`, in both forms | implemented | No UI: `E_ABORT` through the queue, as a dismissed dialog gives. |
| `XUserRegisterForChangeEvent`, `XUserUnregisterForChangeEvent` | implemented | Kept and never called: the user never changes. Unregister returns `bool` and always succeeds. |
| `XUserGetSignOutDeferral`, `XUserCloseSignOutDeferralHandle` | implemented | `E_GAMEUSER_DEFERRAL_NOT_AVAILABLE` (0x89245103): no sign-out ever comes. |
| `XUserFindForDevice` | partial | Every device belongs to the one user. Once input exists, only connected devices should (M4). |
| `XUserRegisterForDeviceAssociationChanged`, `XUserUnregisterForDeviceAssociationChanged` | partial | Kept and never called yet; a gamepad's arrival has to be reported through it once input exists (M4). |
| `XUserGetDefaultAudioEndpointUtf16` and its change registration | implemented | No chat endpoint: `ERROR_NOT_FOUND`, logged once. The registration is kept and never called. |
| `XUserFindControllerForUserWithUiAsync`, `XUserFindControllerForUserWithUiResult` | implemented | No pairing dialog: `E_ABORT` through the queue. |
| XUser slots 15, 37 to 41, and 43 to 46 | stubbed | No thunk (15), or functions an offline user doesn't need: logged once with the caller, `E_NOTIMPL` ([gdk.md](gdk.md), "XUser"). |
| `XGameSaveInitializeProvider`, its async form and result, `XGameSaveCloseProvider` | implemented | The offline user only, otherwise `E_INVALIDARG`; a folder per configuration ID under `<data>\saves`, none syncing. A null or empty ID gives `E_GS_NO_SERVICE_CONFIGURATION`; no save directory gives `E_GS_NO_ACCESS`, logged. Closing expires the provider's containers and updates. |
| `XGameSaveGetRemainingQuota`, its async form and result | implemented | 256 MB per configuration, less every blob's size. |
| `XGameSaveCreateContainer`, `XGameSaveCloseContainer` | implemented | Opens a container whether or not it exists; it exists once an update is submitted. Names follow the public rules, 1 to 256 characters, and match without case. |
| `XGameSaveDeleteContainer`, its async form and result | implemented | One rename moves the folder aside before it is removed; a missing container succeeds. Invalid names give `E_GS_INVALID_CONTAINER_NAME`. |
| `XGameSaveGetContainerInfo`, `XGameSaveEnumerateContainerInfo`, `XGameSaveEnumerateContainerInfoByName` | implemented | Name order without case, a prefix for `ByName`, `needs_sync` always false. A container that doesn't exist gives `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` without a callback (our choice). |
| `XGameSaveEnumerateBlobInfo`, `XGameSaveEnumerateBlobInfoByName` | implemented | Name order without case. |
| `XGameSaveReadBlobData`, its async form and result | implemented | 24-byte records, then the data, in one buffer. `E_GS_BLOB_NOT_FOUND` and `E_GS_PROVIDED_BUFFER_TOO_SMALL` as documented. |
| `XGameSaveCreateUpdate`, `XGameSaveCloseUpdate`, `XGameSaveSubmitBlobWrite`, `XGameSaveSubmitBlobDelete` | implemented | Data copied when submitted to the update; 16 MB per update, otherwise `E_GS_UPDATE_TOO_BIG`. |
| `XGameSaveSubmitUpdate`, its async form and result | implemented | All changes or none: new files, then one manifest rename. Once per update, then `E_GS_HANDLE_EXPIRED`; `E_GS_QUOTA_EXCEEDED` past the quota. |
| The synchronous disk calls on a time-sensitive thread | implemented | `E_GS_ASYNC_FUNCTION_REQUIRED`, logged once per function, leaving an update as it was. |
| `XGameSaveFilesGetFolderWithUiAsync`, its result, `XGameSaveFilesGetRemainingQuota` | stubbed | XGameSaveFiles isn't provided: `E_NOTIMPL`, logged once. |
| XGameSave slots 33 to 49 (interface ab4ae4fb) | stubbed | Unknown: logged once with the caller, `E_NOTIMPL` ([gdk.md](gdk.md), "XGameSave"). |

<!-- Template for a folder:

## runtime/imports/<dll>

| Function | Status | Notes |
|---|---|---|
| `ExampleFunction` | forwarded | |

-->
