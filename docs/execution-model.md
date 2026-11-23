# Execution model: native execution with HLE

**Status: approved on 2026-11-21.** Phases 0 (documentation), 1 (loader), 2 (hook engine), 3
(side DLLs), and 4 (`x64recomp scan`) are done. `CLAUDE.md` summarizes this model, and `docs/codegen.md` describes the frozen
recompiler backend.

## Decision

Run the game's code directly on the host CPU, and keep high-level emulation (HLE) of the
console's OS services and APIs underneath it, the way Wine and Proton run Windows programs.
Keep the recompiler, but only for the jobs it is better at: analysing the binaries, and as a
frozen, tested fallback.

"Emulation" here means only that HLE layer. The CPU is not emulated, and the console hardware
is not emulated. A hardware-level emulator would need the console's system software, which we
don't have and won't use, and a model of its custom GPU. That is far harder than either
option and is ruled out.

## Why

N64Recomp and UnleashedRecomp recompile because MIPS and PowerPC code can't run on a PC. This
game is x86-64 code for a Windows-derived OS, so the host CPU runs it as it is. The recompiler
already keeps everything native on purpose (real guest addresses, the real stack, the host's
MXCSR and x87 state), so translation adds cost and risk without enabling anything that native
execution can't do.

Most of the remaining work doesn't depend on this choice. GPU translation, the GDK, kernel
services, I/O, audio, input, and `memory_tracker` are the same either way. Only the CPU layer
changes:

| | Recompiled | Native |
|---|---|---|
| Exceptions | Needs a guest unwinder (open) | The game's own `.pdata`, registered with Windows |
| TLS | Rewrite every `gs:` access to a guest TEB (open) | The host's real TEB (see below) |
| Fibers | Stack switching inside `GuestContext` (open) | Work as they do on Windows |
| Callbacks from host APIs | An adapter per callback type | A plain function pointer call |
| HLE functions | Read arguments out of `GuestContext` | Ordinary C++ with the API's real signature |
| Coverage | Every function found, every instruction translated | Nothing to find or translate |
| Main executable | Generated C++ in the millions of lines, long rebuilds | No build step |
| Speed | Same at best, probably slower | Native |
| Rules 2 and 3 | Generated code derived from the game, kept local | Nothing generated |

What the side DLLs of 1.0.2.1 show so far:

* No `syscall` instructions. `int 0x29` is Windows' standard fail-fast and `int3` is padding.
* Every `gs:` access uses one of four TEB fields: `0x30` (the TEB's own address), `0x58` (the
  TLS array), `0x08` (stack base), and `0x10` (stack limit). They sit at the same offsets on
  desktop Windows. The stack fields are read through the TEB's own address, which is how
  `x64recomp scan` found the stack base ([scan.md](scan.md)).
* They import mostly Windows API sets (`api-ms-win-core-*`, `api-ms-win-crt-*`), plus a few
  console-only DLLs (memory, D3D12.X, XAudio2) and, in the two networking libraries, WinHTTP
  and the crypto API.
* Three of them have a TLS directory, so a loader must support static TLS from the start.

The main executable gets the same checks (see
[Pre-flight checks](#pre-flight-checks-for-the-main-executable)).

## What runs natively

### Loader

`runtime/loader` maps the game's modules itself, building on the existing PE readers
(`common/`) and on `runtime/kernel/image_loader`, which already maps an image at a chosen base
and applies relocations. It doesn't hand the modules to the Windows loader.

The Windows loader would set up TLS and exception tables for free, but it binds imports by
DLL name, so it would decide which host functions the game reaches. Several things depend on
controlling that:

* Rule 5: networking APIs must never reach the host, even through an API set.
* Console paths in CRT calls (`fopen` and friends) must go through `runtime/io`, not straight
  to the host's CRT.
* The console's CRT DLLs share names with the host's, and some API set names resolve to the
  host's own DLLs whatever is on the search path.
* Stand-in DLLs for the console's system DLLs would have to export the game's full import
  lists, which are dumped tables under rule 2 unless they are generated on the user's machine.

The fallback, if the TLS approach below fails in Phase 1, is the Windows loader with stand-in
DLLs generated locally at configure time.

The loader (`g6::loader::Loader`):

1. **Maps** each module and the guest modules it needs: imported DLLs, delay-loaded ones, and
   DLLs its exports forward to, when they are found in the search paths and aren't networking
   DLLs. Each goes at its preferred base, or wherever there is room with relocations applied,
   or at an explicit base. Code sections become executable once the module is bound.
2. **Binds imports** through guest modules' exports and a registry of implementations, using
   the policy below. Delay-load imports are bound at load time like the others, so the guest's
   delay-load helper never runs; the module handle slot gets the guest module's base or a pseudo
   handle (step 7). Export forwarders are followed, up to 16 deep.
3. **Registers exception tables** with `RtlAddFunctionTable`, so Windows unwinds guest frames
   the same way it unwinds the host's. SEH and C++ exceptions then work across guest and host
   frames, because the calling convention and unwind format are identical.
4. **Sets up static TLS** (see below).
5. **Sets the security cookie** in the load config to a random value with the top 16 bits
   clear, under the Windows loader's conditions as Xbox2Windows measured them: the load-config
   directory and its `Size` both reach 0x70, `GuardFlags` lacks
   `IMAGE_GUARD_SECURITY_COOKIE_UNUSED` (0x800), and the slot still holds the linker's default. A
   value the image seeded itself is kept. It **leaves CFG alone**. The guest's check and dispatch
   pointers keep their default no-op routines. Still open: neither the tests nor `g6recomp.exe`
   are built with CFG enforced; if the runner ever is, host code calling guest callbacks may need
   the guest's code marked as valid call targets.
6. **Initializes the new modules in dependency order**: TLS callbacks, then `DllMain`, for each
   DLL; the executable gets its TLS callbacks only. If a `DllMain` fails, that DLL and the ones
   this load already initialized get `PROCESS_DETACH` in reverse, and the whole load is unmapped.
   Each detach is a full one, TLS callbacks first and then `DllMain`, as on an unload. That is
   what Windows 11 does with a DLL whose `DllMain` fails. No Microsoft documentation states it,
   so `test_loader` measures it with `LoadLibrary` on an original fixture and compares.
   `run_entry_point` starts the executable's entry point on a new thread with the stack reserve
   from its PE header, passing the PEB as Windows does.
7. **Replaces the guest-visible loader functions** (`GetModuleHandle[Ex]`, `GetProcAddress`,
   `LoadLibrary[Ex]`, `FreeLibrary`, `DisableThreadLibraryCalls`, `GetModuleFileName`, and
   `RtlPcToFileHeader`) with HLE versions that see only guest modules and the registry. A guest
   module's handle is its base. Any other DLL the registry provides gets a **pseudo handle**, an
   address in a reserved, inaccessible range that stands for the DLL name; `GetProcAddress` on
   it resolves through the registry under that name. A guest can't use them to load a host DLL
   or fetch a host function outside the policy. `RtlPcToFileHeader` is there because the C++
   runtime's throw uses it to find the image base of its type information, and Windows doesn't
   know modules it didn't load.

### Import policy

Every imported function lands in exactly one category. Anything not listed is a stub.

| Category | What it does | Example |
|---|---|---|
| Forward | Bound straight to the host function of the same name | Synchronization, interlocked, most of the CRT |
| HLE | Our implementation in `runtime/imports` or `runtime/gdk` | Memory, D3D12.X, XUser, XGameSave, file paths |
| Offline | Reports no connectivity; never forwarded (rule 5) | WinHTTP, sockets, XCurl, libHttpClient |
| Stub | Logs the DLL, symbol or ordinal, and caller once, then returns an error | Everything else |

* HLE functions become plain C++ with the public signature, named after the public Win32 or GDK
  name, as the conventions already say. `GuestContext` isn't involved.
* A stub is a small thunk generated at load time. It passes its slot number to a common
  handler, which takes the caller from the return address. Its return value comes from a
  per-DLL default with per-function overrides, so a stub never looks like success:
  `E_NOTIMPL` for HRESULT APIs, and zero plus `ERROR_CALL_NOT_IMPLEMENTED` for Win32 APIs.
* A test checks the policy table: no networking DLL may be forwarded, whatever the table says
  elsewhere.
* CRT API sets mostly forward to the host's Universal CRT. The functions that take paths are
  HLE wrappers that translate console paths through `runtime/io`.

### TLS and the TEB

The guest uses the host thread's real TEB. Stack limits, the TLS array, fiber data, and last
error are all the host's own values, and they are correct.

Static TLS for modules we map ourselves uses only documented behaviour:

* The runtime ships four tiny **donor DLLs** (`g6tls0.dll` to `g6tls3.dll`), each holding one
  TLS block, and loads them normally. Windows then gives every thread a block for each donor,
  including threads started later by host libraries. A donor must import at least one
  function: Windows sets up the static TLS of a DLL loaded at run time while binding its
  imports, and a DLL with none keeps index 0 and gets no blocks. Xbox2Windows measured the same
  on Windows 10 19045, and that an import table holding only its null entry is enough. It
  also found that a TLS template ending past the image fails the load with error 998, and one
  ending before it starts with error 193.
* Each guest module with a TLS directory gets one donor. The loader writes that donor's TLS
  index into the guest module's own index variable.
* A **thread notifier** passes `DLL_THREAD_ATTACH` and `DLL_THREAD_DETACH` on to the loader. It
  is a TLS callback in the host module (`runtime/loader/support/thread_notifier.c`, part of the
  loader library), so it sits in the host executable's TLS callback array next to the C
  runtime's. On attach the loader copies each guest module's TLS template into its block, then
  runs the guest's TLS callbacks and `DllMain` in initialization order; on detach it runs them in
  reverse. A guest executable's TLS callbacks take part too, from the end of its process attach,
  as under a Windows launch; it has no `DllMain` to call. The template is copied from the mapped
  image, so pointers in it are relocated. The
  loader refuses to start if the notifier never ran, which means the host's TLS directory doesn't
  list it.
* The notifier also keeps a list of the threads it has seen. When a module loads, the loader
  fills its block on every thread in the list. Windows calls an executable's TLS callbacks from
  the start of the process, so the list holds every thread that takes part in DLL notifications,
  however early it started. Threads of host libraries and thread-pool workers started before the
  first `Loader` get the guest's TLS too. `test_tls_order` (CI) checks a thread started before
  anything else, and the check fails when the notifier ignores threads from before the loader.
  Threads created to skip DLL notifications never appear in the list; ntdll's parallel-loader
  workers are such threads, and they run no DLL code. **Measured** on Windows 11 (build 26200):
  * The executable's TLS callbacks run after every DLL's `DllMain`, both on attach and on
    detach. Within the array they run in array order both times. The notifier's entry sorts
    first (`.CRT$XLB`), so on an exiting thread guest code runs before the destructors of the
    host's own `thread_local` variables.
  * A thread's detach notifications all run before Windows frees its TLS blocks. A walk of the
    list holds the list's lock, and an exiting thread takes the same lock to leave the list, so
    the loader never fills a block that is being freed.
* **Why the notifier isn't a DLL, and why we don't walk the process's threads.** The notifier
  used to be a DLL, `g6threads.dll`, which only saw threads started after it loaded. A thread
  started earlier kept the donor's contents, so the runtime had to create the `Loader` before
  any other thread. Leonida gives such threads their TLS by listing the process's threads and
  writing through each one's TEB ([related-work.md](related-work.md)). We didn't, because of what
  we measured on the same build. An exiting thread clears its TEB's pointer to the TLS array
  before it ends, but not under the loader lock: in 2 of 3,000 exits the pointer changed while
  another thread held that lock. So taking the lock doesn't keep the blocks alive. And threads
  that skip DLL notifications never say they are leaving. A write through a thread the notifier
  hasn't seen can therefore land in freed memory.
* A donor holds 64 KiB (`G6_TLS_DONOR_CAPACITY` in CMake), and the loader rejects a module that
  needs more or aligns its TLS to more than 16 bytes. Sizing the donors from the pinned build's
  TLS directories at configure time is later work.
* **A guest executable gets TLS index 0.** Windows always gives an executable's TLS block index
  0. Code in an executable built with `/GA` relies on that: it takes its block from slot 0 of
  the TLS array and never reads its index variable (`x64recomp scan` lists such sites as *fixed
  slot*). With a donor's index, that code would read the host executable's block instead.
  * So the host executable gives up the start of its own block. It links
    `runtime/loader/support/exe_tls.c` (the CMake object library `g6exetls`) in place of the C
    runtime's TLS support. That file writes out the host's TLS directory itself, so the block
    starts with an area of `G6_TLS_DONOR_CAPACITY` bytes that nothing of the host's uses. The
    host's own `thread_local` data follows, and the C runtime's TLS callbacks still run. Two
    exported functions mark the offer.
  * When the loader starts, it checks the offer: the area must start the block, Windows must
    have given the host index 0, and the calling thread's block must hold the area's template.
    A misbuilt host is an error rather than a silent fallback.
  * A guest executable then gets the area as its block, just like a donor's, and the loader
    writes 0 into its index variable. Guest DLLs always get donors. Under a host that doesn't
    link `g6exetls`, a guest executable gets a donor too, and the log says so.
  * Each thread's block grows by the area (64 KiB). The runtime's own executable must link
    `g6exetls`.
  * `test_exe_tls` (CI) checks all of this. It uses an original fixture built with `/GA` and
    covers threads started before and after the load, the entry point's thread, and a second
    load after an unload. It also checks the host's own thread-locals, including one the C
    runtime initializes through a TLS callback. With the lending turned off, the test fails.
  * xcl met the same problem and solves it differently ([related-work.md](related-work.md)).
    This design is our own; none of xcl's code was taken.
* **A pointer to a thread's TLS array is only good until the next DLL with static TLS loads.**
  The array has room for a fixed number of indices. When a DLL's index doesn't fit, Windows
  gives every thread a longer array and moves the TEB's pointer. The blocks stay where they are,
  and only the array holding their addresses moves. **Measured** on Windows 11 (build 26200):
  the array starts with room for 9 indices and grows 8 at a time. So the loader reads the array
  from the TEB each time it needs it, after any load, and never keeps a copy. Other threads'
  arrays are read without the Windows loader lock. So a host DLL with static TLS loading on
  another thread at that moment could replace an array mid-read. Whether that matters depends on
  whether Windows frees the old array at once, which hasn't been checked.

  * **What went wrong:** the loader once read the array before loading the donors and used that
    copy to check each donor's block. That worked while few host DLLs had static TLS. Linking
    `shell32` (with `ole32`) into `test_side_dlls` loads five system DLLs that have their own:
    KernelBase, gdi32full, shell32, combase, and ole32. Together with the test executable they
    take indices 0 to 5, so the donors got 6 to 9. Loading the fourth donor moved the array, and
    the check read past the end of the old one. That crashed the first `Loader` before it
    mapped any guest, which looked like the side DLLs failing to initialize.
  * The real runtime will load D3D12, SDL, and other host libraries first, so the loader must
    not depend on how many TLS indices they take. `test_tls_order` (CI) checks that.
    * Before the loader starts, it loads 0 to 24 copies of an original DLL with static TLS
      (`tests/fixtures/tls_order`), one count per child process.
    * In each child it then checks a guest module's TLS on two threads.
    * It also checks that the blocks Windows made earlier are still in place once the array
      moves.
    * In 12 of the 25 children the array moves while the loader starts. Before the fix, every
      one of those children crashed.

If the main executable reads TEB fields that desktop Windows doesn't have, the few
instructions involved are patched to call a stub (see Hooks). Recompilation isn't needed.

### Threads, fibers, and callbacks

Guest threads are host threads created through the HLE `CreateThread`, which applies the
console core layout (affinity, priorities) as HLE. Fibers forward to the host. A guest callback
handed to a host API, such as a thread start routine, a wait callback, or an XTaskQueue
callback, is called directly.

### CPU features

Native code executes the real `cpuid`, so by default the game would see the host CPU. The
loader patches every `cpuid` and `xgetbv` site in guest modules with a one-byte trapping
instruction. The exception handler answers through the existing masking in
`runtime/cpu/cpuid.cpp`, so the game keeps seeing the intersection of Zen 2 and the host and
takes only the code paths the console ran. The sites are few and run rarely, so the cost
doesn't matter.

The opposite risk is an instruction Zen 2 has but the host lacks, used without a feature
check. Examples include SHA, ADX, CLWB, RDPID, and AMD's SSE4a on some AVX2 hosts. These mostly
come from intrinsics in crypto and memory code. The pre-flight scan lists every such site
for the host. The fixes, cheapest first:

1. Emulate the instruction in a vectored exception handler (fine for rare sites).
2. Patch the site to call a C++ stub (for hot sites).
3. Recompile the function (last resort; see below).

Some instructions both CPUs have still give vendor-defined results. **Reported** by
Xbox2Windows (`e2526e1`): when several inputs of `DPPS` or `DPPD` are NaN, which one the result
carries differs between Intel and AMD. So on an Intel host such a result can differ from the
console's in its NaN bits. Nothing here corrects that; it matters only to code that looks at NaN
payloads.

### Side DLLs

| Module | Plan |
|---|---|
| vcruntime140, vcruntime140_1, msvcp140*, concrt140, vccorlib140, vcomp140 | Run natively: the console builds the game shipped with |
| XCurl, libHttpClient.GDK | Never mapped. The executable's imports from them bind to offline HLE |
| Bink2Scarlett | Run natively. Its audio, memory, and file calls go through HLE |
| amd_fidelityfx_loader_xbox | Run natively. Its `LoadLibrary` goes through the HLE loader |
| amd_fidelityfx_upscaler_xbox | **Replaced**, never mapped. Its shaders carry no DXIL, so the public PC FidelityFX SDK (MIT) was to take its place behind the same five entry points, at M3. xcl reports that its native programs translate ([shaders.md](shaders.md)), which reopens the choice; M3 decides |
| XboxBvhBuilder | **Replaced**, never mapped. It builds the console GPU's own BVH format, which no translation could turn into desktop acceleration structures, so DXR builds in `runtime/gpu` take its place, at M3 |

The two former "investigate" rows were settled in [side-dlls.md](side-dlls.md), with the shader
findings in [shaders.md](shaders.md). A replaced DLL is a registry rule the loader honours:
it never maps the file, and imports from it go through the policy.

### Hooks and patches

Hooks keep the same registration: config by build ID and address, with a prose comment
describing what the original does. Their bodies are still written from behaviour (rule 4).
The mechanism becomes runtime patching of the mapped image:

* **Replacement**: overwrite the function's entry with a jump to a C++ function that has the
  same signature. Calling the original goes through a trampoline that holds the relocated
  first instructions.
* **Mid-function hook**: a jump to a generated stub, which saves the registers and flags into
  a context structure and calls C++ with it. The C++ can change registers or redirect control.
  The stub then restores everything, runs the relocated instructions, and jumps back. Stubs get
  unwind info, so exceptions pass through them.
* **Data patch**: write bytes at an address after load (constants, limits, tables).

The engine is ours, built on Zydis (already a dependency). Function discovery already knows
the function boundaries and the branch targets inside them, so it can reject a patch window
that something else jumps into. Microsoft Detours (MIT) is the alternative, but it doesn't do
mid-function register contexts. Relocated instructions exist only in memory at run time, never
in the repo.

### Crash reports

When a guest crashes, the report has to name guest code the way the analysis tools do, and the
stack has to get through guest frames. Leonida reported that `RtlCaptureStackBackTrace` stops
at the first frame in a manually mapped image, and that walking with `RtlLookupFunctionEntry`
and `RtlVirtualUnwind` over the registered tables gets through.

**Checked here** (Windows 11 26200, an original guest DLL that calls back into the host two
frames deep): `RtlCaptureStackBackTrace` returns the host callback and the first guest return
address, then stops, so the guest's caller and everything below it are missing.
`RtlLookupFunctionEntry` finds the tables the loader registers with `RtlAddFunctionTable`, and a
walk with it gets through the guest frames to the host code that called them and on to the
thread's start.

**Ours** (`g6/loader/crash_report.h`): `walk_stack` unwinds from a context with those two
functions. A function without unwind data is a leaf, whose return address is at the stack
pointer; that also covers a call through a bad pointer. The walk stops at a null return
address, at a stack pointer that doesn't grow or leaves the thread's stack, and at unwind data
that faults when followed. Each frame is named by module and offset; a guest frame also gets
its address at the module's preferred base, which is what `x64recomp` and REA show. A guest
module is found through the active `Loader`'s module list, which includes modules whose
`DllMain` is still running.

`install_crash_report` installs a top-level exception filter. For an exception no frame
handled, it logs one report at error level (category `crash`): the exception's code and name,
where it happened, what an access violation touched, the type of a C++ exception (read from the
throw information the compiler emits), the registers, and the stack. It then hands the
exception to the filter that was there before, so the process ends, or a debugger gets it, as
it would have. A stack overflow is reported from a thread of its own, since little of the
faulting thread's stack is left; the faulting thread waits for it, at most ten seconds.

A guest that installed a top-level filter of its own would replace ours, and every MSVC
executable does: **checked here** on original `/MD` programs built by clang-cl and `cl.exe`,
the filter in place when `main` starts is in the executable, installed by its C runtime
startup. So the guest's `SetUnhandledExceptionFilter` is HLE (`register_crash_report`, in the
default policy): it keeps the guest's filter instead of installing it, returns the one the
guest kept before, and installs the report's filter on the first call. After its report, our
filter calls the guest's; when that returns `EXCEPTION_CONTINUE_SEARCH`, the filter from before
ours gets the exception. A guest's call to `UnhandledExceptionFilter` itself stays forwarded and
reaches the same filters.

Tested by `test_crash_report` on an original optimized DLL: a fault in a leaf without unwind
data two guest frames down, a walk from a host callback the guest called, a C++ exception's
type, the installed filter handing the exception on, and a filter the guest installs running
after the report.

## What stays recompiled

### x64recomp as the analysis tool

`info`, `imports`, `exports`, `functions`, and jump-table resolution stay, and a new `scan`
command ([scan.md](scan.md)) adds:

* instructions the host CPU lacks, by site;
* every `fs:` and `gs:` offset, any PEB access, and how TLS accesses pick their slot;
* `syscall` and other system instructions;
* `cpuid` and `xgetbv` sites;
* code that reads, writes, or takes the address of other code;
* TLS, relocation, CFG, and import facts.

The loader uses the same library for patch-window checks, and will use it for its pre-flight
report, cached per build ID in the user's data directory.

### Code generation, frozen

The code generator, `GuestContext`, the dispatch table, and the differential tests stay in the
tree. They keep building and stay in CI, but get no new instruction work and no runtime
integration. They are there for two cases:

* **A function patching can't fix.** Mixing recompiled and native code on one thread needs a
  bridge first. Recompiled code keeps the guest stack in `ctx.rsp`, while its own C++ frames
  use the real stack, so the two need separate stacks and hand-written unwind info at the
  switch. That bridge gets its own plan if a real case shows up.
* **A non-x86 host.** If an ARM64 port is ever wanted, translation is the only route, and this
  is where it starts.

### Dropped from the roadmap

* The guest unwinder, whole-program `gs:` rewriting, and the guest TEB object.
* Import placeholder addresses in the dispatch table, and host-to-guest callback adapters.
* Whole-program regeneration of the game in the default build (the `regen` target).
* `cpuid` masking inside generated code. Masking lives on through site patching.

## Hard rules under this model

* **Rules 1–3** get easier: the default path generates nothing from game files. The
  exceptions are the TLS size header and the pre-flight cache, which stay local.
* **Rule 4** is unchanged for hook bodies.
* **Rule 5** depends on the import policy, which native execution makes even more important:
  native code can call anything it can reach. Everything the guest reaches goes through
  bound imports or the HLE loader functions, networking is never forwarded, and a test enforces
  that. The pre-flight scan flags PEB walking, which could let code find host modules by
  itself.
* **Rule 6** is unchanged.

## Pre-flight checks for the main executable

Run `x64recomp scan` and the loader's dry run on the main executable before building anything
on it:

1. A relocation table, or a preferred base that is free in the host process. Section raw data
   aligned to 0x200-byte sectors: Windows reads whole sectors, and our reader copies exactly
   `SizeOfRawData`, which agree only for aligned sections ([related-work.md](related-work.md)).
   * **Measured here** (Windows 11 26200, `LoadLibraryExW` on an original fixture with its
     section headers edited), agreeing with Xbox2Windows' 11 cases on Windows 10: Windows
     refuses an image (error 193) only when a section's raw data ends at or past 4 GiB, or when
     the last section's raw data runs past the end of the file, even by one byte. A section
     with `SizeOfRawData` 0 isn't checked. Any other section may run past the end of the file
     or start after it, and the bytes the file lacks read as zero. Our PE reader applies the
     same checks (`test_pe`).
   * Not modelled: the last section also failed whenever its raw data started at an offset that
     isn't a multiple of `FileAlignment` (193, or 998 at one offset), while a middle section at
     such an offset loaded. Every side DLL has aligned offsets and its last section inside the
     file.
2. TEB offsets beyond the four above, and any PEB access.
3. `syscall` or other system instructions.
4. Instructions the target hosts lack, and the `cpuid`/`xgetbv` sites.
5. Whether TLS accesses read the module's TLS index or assume slot 0.
6. The imported DLLs by category, and what the stub list looks like on a first run.
7. Whether the code verifies its own bytes. If it does, stop and decide together how to
   proceed before any hooking. This plan doesn't cover working around it.
8. Which drive letters its path strings use, so `runtime/io` mounts the right ones
   ([io.md](io.md), "The model"). `G:` for the package and `T:` for scratch space are
   assumptions until then. Look for `D:` and the `GAME:` prefix too: XDefiant uses `D:` for its
   package (xcl), and Leonida serves `GAME:`, `G:` and `T:` because it doesn't know which one
   GTA VI uses.

## Phases

Phases 1, 2, and 4 don't need the main executable.

0. **Adopt.** Approve this plan. Rewrite the affected parts of `CLAUDE.md`: what the project
   is, why native execution, the architecture, the hard parts, commands, and milestones. Then
   the README, `docs/codegen.md` (marked as the frozen backend), and `docs/hle-status.md`.
1. **Loader on original fixtures (CI).** Fixture DLLs built from our own C test:
   * static TLS from threads started before and after load;
   * SEH and C++ exceptions crossing guest, host, and guest frames;
   * fibers;
   * ordinal and delay-load imports from a made-up console DLL name;
   * the stub's log line and error value;
   * loading at a non-preferred base;
   * the policy test.

   Exit: all of these pass in CI. If the donor-DLL TLS fails here, switch to the Windows-loader
   fallback before going further.

   Status: implemented in `runtime/loader`, with `test_import_policy` and `test_loader` on the
   fixtures in `tests/fixtures/loader_guest` (a DLL with the static C runtime and CFG, and an
   executable). Beyond the list above, the tests cover the HLE loader functions, an explicit
   base, the security cookie, an executable's entry point, and failed loads. They also cover the
   order of process notifications on a load, an unload, and a load whose `DllMain` fails,
   compared with Windows' own loader on `init_guest`, a fixture with no C runtime. The donor-DLL
   TLS works, so the fallback isn't needed. `test_tls_order` starts the loader after other DLLs
   with static TLS have taken the low indices (see "TLS and the TEB").
2. **Hook engine (CI).** Replacement with call-original, mid-function hooks with a register
   context, data patches, and trap patching (`cpuid`), all on fixtures, including an exception
   thrown through a hooked function.

   Status: implemented in `runtime/loader` (`patcher.cpp`, public types in
   `g6/loader/patches.h`), with `test_hooks` on `tests/fixtures/hook_guest`. `docs/hooks.md`
   holds the design and the patching rules. The analysis code moved into the `x64analysis`
   library, which the loader links.
3. **Side DLLs natively (local, `game` label).** The console CRT DLLs initialize and run under
   the loader. Settle the two "investigate" rows.

   Status: implemented as planned in [side-dlls.md](side-dlls.md):
   * `runtime/imports` holds `default_game_policy()`, the xmem HLE, and COM and WinRT
     activation behind an allowlist;
   * the loader honours replaced DLLs.

   Tests:
   * `test_imports` (CI) checks the policy and xmem.
   * `test_side_dlls` (local) checks that all 12 mapped side DLLs initialize, each with only
     the stubs the policy chose.
   * The same test runs `tests/fixtures/crt_guest`, an original `/MD` DLL, on the console's
     C++ runtime: exceptions, threads, locales, `atomic::wait`, OpenMP, and PPL.

   Both investigate rows end in replacement, implemented at M3.
4. **`x64recomp scan`.** Built and tested on fixtures and the side DLLs.

   Status: implemented in `tools/x64recomp/src/scan.cpp`, described in [scan.md](scan.md).
   Tests:
   * `test_scan` (CI) runs on `tests/fixtures/scan_guest`, an original DLL with one function per
     finding.
   * `test_recomp` (CI) covers the header facts on synthetic images.
   * `test_game_files` (local) checks the side DLLs.

   The side DLLs pass every check:
   * no system instructions;
   * only the four expected TEB fields;
   * TLS accessed only through each module's own index;
   * no code reads or writes;
   * the only instructions Zen 2 lacks are libHttpClient's 24 AVX-512 ones, behind a feature
     check.

   The scan and the hook engine share a map of the C++ exception tables (FH3 and FH4), so the
   engine checks mid-function hooks in C++ functions by itself
   ([hooks.md](hooks.md#c-exception-functions)). Every side DLL's tables read. The scan reports
   how many functions still need a hand check: catch and cleanup funclets, functions whose catch
   continuations no source names, and functions under any other handler. A handler that only
   checks the stack cookie needs no hand check
   ([hooks.md](hooks.md#functions-under-a-cookie-check-alone)). A window may not
   start at a call's return address, where the unwinder would mistake the patch jump for an
   epilogue ([hooks.md](hooks.md#windows-at-return-addresses)).
5. **Main executable.** Pre-flight, then map it and run it to its entry point. Then the
   stub-first loop from `CLAUDE.md`: add a logging stub, confirm the game gets further, then
   implement. [boot.md](boot.md) keeps the stops in order, starting with the ones Leonida
   measured on the same executable.

   Rules other projects found running an executable this way (reported by Leonida, measured
   against native runs there):
   * **Exit.** The mapped executable's CRT startup registers its `atexit` handlers and static
     destructors in the host CRT's process-wide table, since the CRT API sets forward to the
     host's UCRT. A runner that ends with `ExitProcess` never runs them, and a guest `exit`
     would run the host's handlers too. Leonida gives the guest a table of its own and follows
     the native rules for `exit`, `_cexit`, `_exit` and `quick_exit`. `at_quick_exit` and
     `_register_thread_local_exe_atexit_callback` are open there. Check what the UCRT does on a
     second registration of the latter, since our host executable may register first.

     **Checked here** (Windows 11 26200, a scratch `/MD` program on the host UCRT): a second
     `_register_thread_local_exe_atexit_callback` ends the process with a fast fail
     (0xC0000409). `exit` calls that callback with `DLL_PROCESS_DETACH` for the calling thread,
     then runs the atexit table newest first; a handler a handler registers runs next. `_cexit`
     does the same and returns, and an `exit` after it runs only what was registered since.
     `quick_exit` runs the at-quick-exit table alone. None of them flushes the streams: the
     UCRT does that when the process ends, also after `_exit` or a plain `ExitProcess`, but not
     after `TerminateProcess`.

     **Ours** (`g6/imports/crt_exit.h`): the guest gets its own two tables and callback, and
     `exit`, `_exit`, `_Exit`, `quick_exit`, `_cexit`, and `_c_exit` follow those rules on them,
     under an exit lock that the leaving thread keeps. The ending forms then call the runner's
     end function, `ExitProcess` by default, which ends the host process with the guest's code
     and flushes the guest's streams on the way. `abort`, `terminate`, and the guest's own
     `ExitProcess` stay forwarded: they end the process as on the console. Guest DLLs get no
     `DLL_PROCESS_DETACH` when the process ends this way, since Windows only notifies the DLLs it
     loaded; their static destructors don't run. Tested by `test_crt_exit` on an original `/MD`
     executable.
   * **Command line.** The game's command line has to be in place before the process-attach
     callbacks run, and the CRT's argument functions read the environment when they are called,
     not from a pointer kept at startup.

     **Checked here** (Windows 11 26200, ANSI code page 1252, scratch `/MD` programs on the host
     UCRT): `_acmdln` is the pointer `GetCommandLineA` returns. `_configure_narrow_argv` reads
     `_acmdln` when it is called and builds a new argv each time; the pointer `__p___argv`
     returns stays the same. Mode 0 (no arguments) returns 0 and changes nothing; a mode out of
     range sets `errno` to `EINVAL`, calls the invalid parameter handler, and returns 22. In the
     program name, quotes only toggle, backslashes stay as written, and only a space or tab
     outside quotes ends it, so leading spaces give an empty `argv[0]`. The arguments follow the
     documented backslash and quote rules, with a doubled quote inside quotes giving one literal
     quote. An empty `_acmdln` makes the CRT parse the program's path in its place, so a path
     with a space splits; a null one gives an empty WinMain line. The WinMain line skips the
     program name, which ends at any byte up to a space outside quotes, then the blanks after it.
     `_pgmptr` is set only by the narrow configure and `_wpgmptr` only by the wide one; before
     that, `_get_pgmptr` gives `EINVAL` through the handler. `CreateProcess` with no command
     line gives the child the quoted application path.

     **Ours** (`g6/imports/command_line.h`): the runner sets the guest's program path and
     command line before it loads anything, and `GetCommandLineA`, `GetCommandLineW`, and the
     CRT's argument functions (`_configure_narrow_argv`, `_configure_wide_argv`, `__p___argc`,
     `__p___argv`, `__p___wargv`, `__p__acmdln`, `__p__wcmdln`, `__p__pgmptr`, `__p__wpgmptr`,
     `_get_pgmptr`, `_get_wpgmptr`, and the two WinMain functions) answer from it with the rules
     above. A line left empty becomes the quoted program path, as `CreateProcess` would make
     it. Expanded-argument mode leaves wildcards as written; a wildcard in an argument is logged
     once. The host process's own command line is never read or changed. Tested by
     `test_command_line`, which compares the parser with the host CRT line by line, and
     `test_crt_args`, where an original `/MD` executable's `main` gets the guest's arguments and
     a DLL it imports sees the line while the loader attaches it.
   * **The runner.** `g6recomp.exe` (`app/g6recomp`, `g6/app/runner.h`) starts the guest
     process. It checks the host CPU (below), installs the crash report, then:
     1. Finds the executable: `--exe`, a path or a file name in the install, or else the main
        executable of the known build the install holds. CMake writes each build's ID,
        executable name, and SHA-256 from `known_builds.toml` into a header. An executable named
        as a known build's must have that build's hash; a build with none recorded runs
        unverified, with a warning that gives the hash. Any other executable runs as no build
        and needs `--data-dir`.
     2. Mounts the drives ([io.md](io.md)): the install read-only at `G:`, and `<data>\temp` at
        `T:`, where `<data>` is `--data-dir` or `%LOCALAPPDATA%\G6Recomp\<build ID>`.
     3. Sets the guest's command line: the executable's console path as its program name, then
        the arguments after `--`. The guest sees each of its modules at its console path, so
        `GetModuleFileName` gives `G:\<file>`, which the guest's file functions open: the
        loader reports what the runner's `guest_path` function in `LoaderOptions` gives. An
        executable outside the install has no console path, so the runner refuses it.
     4. Starts the guest's exit tables, loads the executable under `default_game_policy()` with
        the install as the search path, and runs its entry point.

     The host process ends with the guest's exit code: through the guest's `exit`, or with the
     entry point's return value, as Windows ends a process whose main thread returns from its
     entry point. The loader is never destroyed, since other guest threads may still run; the
     guest's modules stay mapped while the process ends, as Windows leaves a process's images.
     The runner links `g6exetls`, so the executable's static TLS gets index 0. It exits with 2
     for a command line it can't use and 1 when the guest can't be started, and logs why.

     **The host CPU.** The executable uses AVX, AVX2, BMI1, BMI2, FMA, F16C, MOVBE, LZCNT, AES
     and PCLMULQDQ without checking for them (the scan's baseline, [scan.md](scan.md)). On a host
     without one of them it would crash at the first such instruction, deep in guest code. So
     before it loads anything, the runner reads `cpuid` and `xgetbv` and refuses a host that
     lacks any of 22 features, or whose Windows doesn't save the AVX registers, and names each
     missing feature. The features Zen 2 has and some AVX2 hosts don't (SSE4a, CLZERO, MONITORX,
     ADX, CLWB) aren't required: the scan lists their sites, and "CPU features" above gives the
     fixes.

     **The stack.** The entry point runs on a thread the loader creates with the larger of the
     executable's stack reserve (1 MiB) and the runner's own (8 MiB, set when it is linked); a
     thread the guest starts with no stack size gets the runner's reserve from Windows. Host
     drivers compile shaders on the thread that creates a pipeline, and their compilers recurse
     with the shader's nesting, which 1 MiB doesn't hold. A thread the guest starts with a size
     of its own keeps that size.

     **Soft asserts.** A console build's assert is an `int3`, where a devkit's debugger stops;
     without one, Windows ends the process. Many are soft: the code after them carries on.
     `--skip-int3` adds a last vectored handler that steps over an `int3` in guest code when no
     debugger is attached, and logs it once per address. It shows how far the guest gets past
     them; each address it logs is a finding to fix, not a fix. Host code's `int3` and any other
     exception are left alone.

     Tested by `test_runner`: the argument parsing, the hash, and the build identification in
     process, and `g6recomp.exe` in a child process on an original executable in a folder that
     stands in for an install. Its `main` gets its arguments, finds its own path as
     `G:\runner_guest.exe` in `argv[0]` and from `GetModuleFileName`, and opens itself by it. It
     starts in `G:\`, reads a file there by a relative path, can't write to `G:`, and writes to
     `T:`, which lands in the data directory's temp folder. `test_loader` checks the
     `guest_path` option on its own. The fixture's C++ runtime is linked in: the host's
     `vcruntime140.dll` imports `kernel32.dll`, which the game's policy stubs (its `DllMain`
     fails on `InitializeCriticalSectionEx`), while the console's imports API sets. The fixture
     also checks the stack reserve of its entry thread and of a thread it starts with no size,
     and catches its own `int3` unless `--skip-int3` steps over it. The CPU check is tested on
     made-up `cpuid` answers: a host without AVX2 and LZCNT, one whose Windows doesn't save the
     AVX registers, one whose highest leaf is 6, and one with nothing.

## Milestones

* **M0**: PE readers, function discovery, import table. Unchanged and done for the side DLLs.
* **M1** (rewritten): the native loader runs guest code, with imports bound by policy and
  exceptions, TLS, and fibers working. The side DLLs initialize, and the main executable
  reaches its entry point and initializes its heaps.
* **M2–M5**: unchanged.

## Decisions (2026-11-21)

1. Native execution is the default model.
2. The code generator stays in the tree, frozen but tested: it's finished, and it's the only
   route to a non-x86 host.
3. The hook engine is our own, on Zydis, rather than Microsoft Detours, for mid-function
   contexts and the patch-window checks.
4. The game's view of the CPU stays at Zen 2, through patched `cpuid` and `xgetbv` sites.
5. The project keeps the name G6Recomp. It comes from the original recompilation plan, and
   nothing depends on it.
