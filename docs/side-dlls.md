# Side DLLs under the loader (Phase 3)

**Status: implemented.** The plan was approved on 2026-11-21 and is in place:
`runtime/imports` (the policy, xmem, and COM and WinRT activation), the replaced-DLL rule in the
loader, and the tests. [As built](#as-built) lists where the code goes beyond the plan and what
the tests found on 1.0.2.1.

Phase 3 of [execution-model.md](execution-model.md) runs the console's side DLLs natively and
settles the two "investigate" rows. Findings are marked **checked here** (measured on our
1.0.2.1 files) or **reported** (from [related-work.md](related-work.md), not yet checked).

## What the side DLLs import

* **Checked here.** Every import of the DLLs we map is one of these:
  * an API-set function the host provides (each one resolves on Windows build 26200);
  * an export of another console DLL we map (the CRT family imports itself);
  * `oleaut32` ordinals 4 and 6;
  * `XAudio2_9` ordinal 1;
  * `xmem.dll` (`XMemAlloc` and `XMemFree`, from Bink), the only DLL with no host provider.
* `d3d12_xs.dll` is imported only by the two DLLs this plan stops mapping.

So the work is in the policy, not in new HLE. A few host functions must not be forwarded:
those that reach COM or WinRT classes, file paths, the registry, or other processes.

## Which modules get mapped

| Module | Plan |
|---|---|
| vcruntime140, vcruntime140_1, msvcp140, msvcp140_1, msvcp140_2, msvcp140_atomic_wait, msvcp140_codecvt_ids, concrt140, vccorlib140, vcomp140 | Map. These are the console's MSVC 14.29 builds. The guest's CRT imports bind to them, never to the host's newer copies |
| Bink2Scarlett, amd_fidelityfx_loader_xbox | Map |
| amd_fidelityfx_upscaler_xbox | **Replaced**: never mapped (below) |
| XboxBvhBuilder | **Replaced**: never mapped (below) |
| XCurl, libHttpClient.GDK | Never mapped (rule 5); unchanged |

XCurl has a second way into the Game Runtime: it can load the PC's Gaming Services runtime,
`gameplatformservices.dll`, from System32 by name, after reading a `GamingServices` registry
value. Reported by Leonida; checked here: XCurl holds the DLL's name and the registry value's
name. On a PC that DLL reaches Microsoft's online services, so the import policy lists it with
the networking DLLs (rule 5): it is never forwarded or loaded for the guest, whichever module
asks.

## Default import policy

A new library, `runtime/imports`, provides `default_game_policy()`. It builds the
`ImportRegistry` that `g6recomp` and the `game` tests use. The policy is built from the
registry's existing rule kinds, plus one new one: replaced DLLs.

| Imports | Binding | Why |
|---|---|---|
| `api-ms-win-crt-*` | Forward to the host Universal CRT | Same contract as the console's; the console's own vcruntime and msvcp sit on top of it |
| `api-ms-win-core-*` for synchronization, interlocked, heap, handles, errors, debug, profile, `rtlsupport`, `util`, strings, localization, fibers, the thread pool, console, environment, and `sysinfo` | Forward | Same contract as the console's |
| `processthreads` | Forward | `CreateThread`, priorities, ideal processor, and affinity become HLE once `runtime/kernel` has the console core layout |
| Process creation (`CreateProcess*`) | Stub | Never launches host programs. The side DLLs don't import it, but the executable might |
| `libraryloader` | HLE (exists) | Resource lookups (`FindResourceExW`, `LoadResource`) forward: a guest handle is a real mapped image |
| File functions that take a path: `CreateFileW`, `CreateFile2`, `FindFirstFile[Ex]W`, `GetFileAttributesExW`, `SetFileAttributesW`, `CreateDirectoryW`, `GetDiskFreeSpaceExW`, `GetTempPathW`, `CopyFile2`, `CreateHardLinkW`, `CreateSymbolicLinkW`, `_wfsopen`, `_fsopen`, `_wremove`, `_wrename`, `_wrmdir`, `_wchdir`, `_wgetcwd` | Stub until `runtime/io`; since 2026-11-22 HLE on it ([io.md](io.md)), except the two link functions | Console paths must go through `runtime/io`. Forwarding would open host paths |
| File functions that take a handle (`ReadFile`, `WriteFile`, `SetFilePointerEx`, `FindNextFileW`, ...) | Forward | A guest can only get a handle from a function we allow |
| Registry (`RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey`) | Stub | Only `msvcp140_atomic_wait` imports them, presumably for the time zone database |
| `CoCreateInstance[Ex]`, `RoGetActivationFactory`, `RoActivateInstance` | HLE with a class allowlist, empty for now | Rule 5: forwarding could reach any registered class, networking ones included. A refused class is logged by CLSID or class name |
| The rest of COM, WinRT, WinRT strings and errors | Forward | Apartments, marshaling, and task memory stay in-process |
| `oleaut32` #4 and #6 | Forward by name to `SysAllocStringLen` and `SysFreeString` | Bound by name, so they don't depend on the host's ordinals |
| `XAudio2_9` #1 | Forward by name to `XAudio2Create` | **Checked here:** host ordinal 1 is `XAudio2Create`. Bink's use of it is unverified until a video plays with sound |
| `xmem.dll` | HLE (below) | Console-only |
| `d3d12_xs.dll` | Stub, returning `E_NOTIMPL`, until `runtime/gpu` | |
| Networking DLLs and API sets | Offline | Unchanged |
| Anything else | Stub | Deny by default; unchanged |

Some stubs need their own return value, because the default (zero, with
`ERROR_CALL_NOT_IMPLEMENTED` set) means success for them:

| Functions | Stub returns |
|---|---|
| `CreateFileW`, `CreateFile2`, `FindFirstFileW`, `FindFirstFileExW` | `INVALID_HANDLE_VALUE` |
| `_wremove`, `_wrename`, `_wrmdir`, `_wchdir` | `-1` |
| `RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey` | `ERROR_CALL_NOT_IMPLEMENTED`, as the status they return |
| `CopyFile2` | `E_NOTIMPL` |

`test_imports` (nogame, CI) checks `default_game_policy()`:

* those return values;
* no networking forward;
* COM and WinRT activation never forwarded;
* both replaced DLLs marked replaced.

It also tests xmem: both observed attribute words, alignment and zeroing, and frees of
pointers that aren't live allocations.

## xmem

The console's general allocator. Only Bink imports it among the side DLLs; the executable
probably uses more of it.

* **Signatures** follow SlimEra's public reference:
  * `void* XMemAlloc(SIZE_T size, ULONGLONG attributes)`
  * `void XMemFree(void* address, ULONGLONG attributes)`
* **Attribute word** (SlimEra's Xbox One layout, consistent with both words observed on
  Scarlett):

  | Bits | Field |
  |---|---|
  | 0–13 | Object type |
  | 14–15 | Page size |
  | 16–23 | Allocator ID |
  | 24–28 | log2 of the alignment |
  | 29–32 | Memory type: 0 is the heap, then graphics and physical types |

* **Heap memory type.** Allocate from a dedicated host heap, aligned to the larger of 16 and
  the requested alignment, and zeroed. It is unknown whether the console zeroes; zeroing means
  no caller depends on leftovers. Free looks the allocation up by address.
  * **Checked here:** Bink's word is `0x05e50000`: heap type, allocator `0xe5`, 32-byte
    alignment.
  * **Reported:** Bink asks for 64 bytes more than its caller wants and aligns the pointer it
    hands out itself. It frees with the base `XMemAlloc` returned and the same word, so
    `XMemFree` only sees addresses `XMemAlloc` handed out. When a title installs Bink allocation
    callbacks (`BinkSetMemory`) and the callback returns null, Bink falls back to `XMemAlloc`.
* **Graphics and physical memory types.** Not before `memory_tracker` exists. Log once and
  return null, an honest failure rather than CPU memory the GPU can't see.
* **`XMemSetAllocationHooks`.** In SlimEra, `XMemAlloc` and `XMemFree` go through hooks a
  title can install. If the executable installs its own allocator, Bink's allocations have to
  reach it. Implemented when the executable imports it, stub first.
* **Other xmem functions.** `XMemGetWorkingSetStatistics`, `XMemVirtualQuery`, and the rest
  stay stubs until something we map calls them. **Reported:** the upscaler treats a failed
  statistics call as zero, so a stub is safe there.

## Replaced DLLs

A replaced DLL is one the loader must never map from the search path, even though the file is
there. Its job goes to our own code instead.

**Registry.** A new rule kind, `ImportRegistry::replace_dll(name)`, with `is_replaced(dll)`
to query it. The policy lives in one object, like the networking list, but per policy rather
than fixed.

**Loader.**

* `find_guest_file` skips replaced DLLs, as it skips networking DLLs.
* `Loader::load` refuses them by name.
* Imports from a replaced DLL, and `LoadLibrary`, `GetModuleHandle`, and `GetProcAddress` on
  it, go through the registry, as they do for any non-guest DLL. With no rules for it:
  * `LoadLibrary` fails with `ERROR_MOD_NOT_FOUND`;
  * imports are stubbed and logged.
* Once an implementation exists, its HLE rules give it a pseudo handle like any other
  registry DLL.

**Fixture test (CI).** A fixture DLL in the search path, listed as replaced. The test checks
that:

* it isn't mapped when imported, or through `LoadLibrary`;
* the importing module still loads with stubs;
* an HLE rule for it gives `LoadLibrary` and `GetProcAddress` a pseudo handle and our function.

### The two "investigate" rows, settled

* **XboxBvhBuilder: replace with desktop DXR in `runtime/gpu`.**
  * Its 52 compute programs build the console GPU's own BVH format. They use console-only DXIL
    intrinsics and root signature flags ([shaders.md](shaders.md)).
  * Desktop acceleration structures are opaque, so no translation of its output could feed DXR.
  * The replacement implements its exported C++ class (constructor taking an `ID3D12Device11*`
    and a flag; build, prebuild, copy, and rehydrate methods) on DXR builds.
  * Until then the executable's imports from it are stubs, which the stub-first loop will
    surface.
  * **Risk, reported** (Leonida): `core_bvh4.rpf` holds about 272,000 prebuilt acceleration
    structures in the console's format, with no source geometry. With the rehydrate method
    above, that suggests the game loads them rather than building them, which a DXR build can't
    reproduce from nothing ([shaders.md](shaders.md)).
* **amd_fidelityfx_upscaler_xbox: replace with the public PC FidelityFX SDK (MIT).**
  * Its 29 shader containers carry no DXIL, only a console-specific part ([shaders.md](shaders.md)).
  * The FidelityFX loader DLL (mapped) finds the upscaler with `LoadLibraryA` and
    `GetProcAddress`. So the replacement is a set of HLE rules for that DLL name, exporting the
    five `ffx` entry points. **Reported** (Xbox2Windows): it asks for the upscaler with
    descriptor type `0x10000` and for frame generation with `0x20000` and `0x30000`, and tries
    `GetModuleHandle` before `LoadLibraryA`.
  * Those translate the console backend descriptor to the desktop DX12 one, with the host
    device behind the guest's. **Reported:** type `0x01000002`, device pointer at `+16`.
  * On PC, the SDK's FSR 4 path needs an RDNA 4 GPU; others get its FSR 3 upscaler.
  * Until then `LoadLibraryA` fails and the loader reports the effect as unavailable. The
    frame-generation DLL it can also ask for isn't in the package. **Checked here** and reported.

## Tests

A new `test_side_dlls`, labelled `game`. It runs locally on the user's files, never in CI.

1. **Each mapped DLL in turn.** Load it with `default_game_policy()`, check that it was
   initialized, and log its binding counts. Its stubs must be exactly the ones the policy says.
   For the CRT family that means only the file-path and registry stubs. Then unload it.
2. **Replaced and networking DLLs.** Present in the folder, never mapped.
3. **`crt_guest`.** A new original fixture DLL, built `/MD`, whose `vcruntime140`, `msvcp140`,
   `concrt140`, and `vcomp140` imports must all bind to the console's copies, not to the host.
   The test checks that first and names any import the console runtime doesn't export.
   * Our toolset is newer than the console's 14.29 runtime, so the fixture sticks to what 14.29
     exports, and defines `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`.
   * It exercises:
     * C++ exceptions thrown in the fixture and caught in the fixture and in the test;
     * containers and strings;
     * threads, mutexes, and condition variables;
     * string streams with a locale;
     * `std::atomic::wait` (`msvcp140_atomic_wait`);
     * an OpenMP parallel loop (`vcomp140`);
     * a PPL task and `parallel_for` (`concrt140`).

`docs/hle-status.md` gets a `runtime/imports` table for the policy above. `execution-model.md`
gets the settled rows and a Phase 3 status.

## As built

### Beyond the plan

* **Forwarding goes by API-set family**, never `api-ms-win-core-*` as a whole, so a family
  nobody reviewed stays stubbed. The forwarded families are the ones in the table above.
* **Deny lists cover what the host exports, not only what the side DLLs import.** Within the
  forwarded families, every function that reaches host paths, other programs, or host DLLs is
  stubbed by name:
  * the CRT's path functions (the `fopen`, `_open`, `_stat`, `_findfirst`, and `_utime`
    families, directory and current-directory functions, temporary names);
  * the whole CRT `process` set, `system`, `_loaddll`, and `_getdllprocaddr`;
  * the OneCore `_o_` copies in `api-ms-win-crt-private`;
  * `CreateProcess*`, `OpenProcess`, the current-directory functions, and `SearchPath`.
* **Return values for those stubs.**
  * CRT functions that return a pointer give null.
  * Those that return an error number give `ENOSYS`.
  * `GetFileAttributes*` and `GetCompressedFileSize*` give their `INVALID_*` values.
  * A stub can't set `errno`, so CRT callers see only the return value.
* **The file API sets have an explicit stub rule**: zero with `ERROR_CALL_NOT_IMPLEMENTED`
  for everything that isn't forwarded or given its own value above. The same function rules
  apply to imports from `kernel32` and `kernelbase`, but those two get no DLL rule. Anything
  else imported from them stays unreviewed and is logged as such.
* **COM and WinRT.**
  * `CoCreateInstanceFromApp` and `CoGetClassObject` are HLE too.
  * `CoGetInstanceFromFile` and `CoGetInstanceFromIStorage` are stubbed.
  * A runtime class the guest registered itself with `RoRegisterActivationFactories` may be
    activated: it is guest code, not a host class.
* **xmem.** A zero-byte request still gets an address of its own.
* **Known gaps.**
  * Resource lookups with a null module mean the host executable. They need HLE once the
    executable makes them.
  * A forwarded family's function the host lacks becomes a Win32-style stub, even in COM sets.
    `test_side_dlls` reports any such stub; there are none today.

### What the tests found (checked here)

* **Each mapped DLL loads and initializes on its own**, and every stub is one the policy chose:
  * `msvcp140`: 17, the CRT and file API path functions listed in the table;
  * `msvcp140_atomic_wait`: the three registry functions;
  * `vcomp140`: `FindFirstFileW`;
  * `Bink2Scarlett`: `CreateFile2`;
  * the others: none.
* **Since `runtime/io` (2026-11-22)** the path functions above are HLE, and the stubs left are:
  * `msvcp140`: `CreateHardLinkW` and `CreateSymbolicLinkW`, which stay stubs so that no link
    can lead out of a mount;
  * `msvcp140_atomic_wait`: the three registry functions;
  * the others, `vcomp140` and `Bink2Scarlett` included: none.
* **Replaced and networking DLLs** are refused by name and by path.
* **`crt_guest`.** All 143 imports from the console runtime bind to the console's copies, and
  14.29 exports every one of them. Every check passes, and the OpenMP team spans all logical
  processors. How the fixture is built:
  * It links `onecore_apiset` instead of `kernel32`, so its system imports are API sets, as
    in console code.
  * It starts its OpenMP region through `vcomp140`'s `_vcomp_fork`, as MSVC-compiled code
    does, because clang-cl's OpenMP support targets LLVM's runtime instead.
  * Its test never destroys its loader. The OpenMP and ConcRT worker threads stay in console
    code after the calls return, so those modules must stay mapped while the process lives.
    For the game this matters only at exit.

## Decisions (approved 2026-11-21)

1. The default policy above, in a new `runtime/imports` library.
2. COM and WinRT activation as HLE with an empty allowlist.
3. Path-based file functions and the registry stubbed, with the return values above, until
   `runtime/io` exists.
4. The xmem HLE as described. Graphics memory types fail until `memory_tracker` exists.
5. `XAudio2_9` #1 and `oleaut32` #4 and #6 forwarded by name.
6. Replaced DLLs as a registry rule that the loader honours.
7. The investigate rows: XboxBvhBuilder replaced with desktop DXR, the upscaler with the PC
   FidelityFX SDK. Both are implemented at M3, not in this phase.
8. The tests above.
