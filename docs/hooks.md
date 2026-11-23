# Hook engine

Status: **implemented (Phase 2 of `docs/execution-model.md`)** and tested in CI on an original
fixture. This note covers the engine's design and its patching rules. CLAUDE.md asks for these
rules to be agreed before they change, so a change to them starts with a change to this note.

## Scope

The engine applies four kinds of patch:

* **Replacement**: a guest function's entry jumps to a C++ function with the same signature,
  which can call the original through a trampoline.
* **Mid-function hook**: a jump to a generated stub that hands the guest's registers to C++ in a
  context, then resumes.
* **Data patch**: bytes written at an address.
* **Trap patching**: every `cpuid` and `xgetbv` site answers with the Zen 2 view from
  `runtime/cpu/cpuid`.

Not done yet:

* Reading hooks from `game/config` TOML, and the registry of hook functions in `game/hooks`. Both
  come with the first real hook (Phase 5), along with toml++ as a new submodule.
* Patching code that is already running (for example, toggling an enhancement in game).
* Using `x64recomp scan` ([scan.md](scan.md)) in the loader, for the cached site list for the
  main executable.

## Where it lives

* The engine is `runtime/loader/src/patcher.cpp`, with its public types in
  `g6/loader/patches.h`. `LoaderOptions::patches` maps a module's file name (matched like a DLL
  name) to its `ModulePatches`. The loader applies a module's patches after binding its imports
  and before making its code executable, so before any TLS callback or `DllMain` runs. The
  image is still writable and no thread runs its code yet, so nothing has to be suspended.
* The analysis code (code walker, function discovery, jump tables) is the `x64analysis` library.
  The loader and `x64recomp_core` both link it. The code generator didn't change, and neither
  did its output.
* Zydis is only used to decode. The engine writes its few encodings itself: near jumps, near
  conditional jumps, an absolute jump through memory, the stub's moves, and `ud2`.

## API

```cpp
struct HookContext {             // alignas(32)
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, ..., r15;  // encoding order
    uint64_t rflags;
    uint64_t rip;                // the hook site; change it to resume somewhere else
    std::array<Ymm, 16> ymm;     // full 256-bit registers
};
using MidHook = void (*)(HookContext& context);

struct ModulePatches {
    Replacement& replace(uint32_t rva, const void* function, void** original = nullptr);
    MidFunctionHook& mid(uint32_t rva, MidHook hook);
    DataPatch& write(uint32_t rva, std::vector<uint8_t> bytes);
    bool trap_cpuid = false;
};
```

Every patch can carry `ExpectedBytes{size, hash}`: a 64-bit FNV-1a hash (`hash_bytes`) of `size`
bytes of the module's file at the patch address, before relocation. It never carries the bytes
themselves (rule 3). A mismatch fails the load. That catches a wrong address, or a config meant
for another build.

The loader sets `*original` only after every patch of the module is in. If the load fails, or
when the module is unloaded, it resets `*original` to null.

## Patching rules

1. **Jumps through a nearby arena.** A patch site gets a 5-byte near jump to code in a patch
   arena: memory the loader reserves within 2 GiB of the whole module. It looks after the
   module first, then before it. The arena holds the stubs, the trampolines, the moved windows,
   and absolute jumps to host C++, which can be anywhere in the address space. It is filled
   while the module is patched, then sealed: made execute-read, and its function table
   registered with `RtlAddFunctionTable`. It is released with the module.
2. **The patch window** is the whole set of instructions the jump overwrites, at least 5 bytes.
   The bytes after the jump become `int3`. The loader checks these rules in this order:
   * The site is inside a function that function discovery found. A replacement must be at the
     function's entry.
   * The site is the start of an instruction that the function's code reaches.
   * The site isn't a call's return address. Unwinding out of the call, the unwinder reads the
     code there to tell whether the caller is in an epilogue, and it would take the patch jump
     for an epilogue's jump out of the function. This holds for every kind of patch and any
     handler, and `handler_checked` doesn't change it
     ([Windows at return addresses](#windows-at-return-addresses)).
   * A mid-function hook in a function whose exception handler the engine can't check is refused
     unless its registration sets `handler_checked` (see rule 4).
   * The window lies inside the function (its `.pdata` range or fragment, or its walked extent
     for a leaf). The one exception is a replacement whose original is never called, whose
     window may run on into `int3` padding.
   * No catch block resumes in the window except at its first byte. This holds for every kind of
     patch, and `handler_checked` doesn't change it: after the catch, the guest would run the
     jump's tail and the `int3` bytes behind it. The continuations come from the C++ exception
     map ([below](#c-exception-functions)), and the code walk starts from them too, so code that
     only a catch reaches counts as reached.
   * No jump inside the function lands in the window except at its first byte. Jumps from the
     code walk count here, and so do switch-table targets and the `__except` targets in C SEH
     scope tables.
   * If the window's instructions still run (call-original, or any mid-function hook), they must
     be movable:
     * no `call` (the copy would return into the arena), no `loop` or `jrcxz`;
     * no other relative operand and no 32-bit RIP-relative address;
     * in a function with unwind info, no epilogue code: `ret`, `pop`, an `add`, `lea` or `mov`
       that writes `rsp`, an indirect `jmp`, or a `jmp` that leaves the function. The unwinder
       recognizes an epilogue at its original address by its code, but the moved copy is
       described as prolog, which can't say that the frame is already gone.

     Short jumps are rewritten as near jumps. RIP-relative operands are adjusted, and the patch
     is refused if the arena is too far from their target. A window whose instructions run may
     also not overlap a C `__try` range.
   * A window whose instructions run may not hold a `cpuid` or `xgetbv` site the loader traps. A
     replacement window that never runs simply covers such a site, which is then not trapped.
   * The window doesn't overlap another patch.

   A patch that breaks a rule fails the load with `"<module>: <kind> at <rva> refused: <reason>"`,
   and the module is unmapped, so nothing is left half patched.
3. **Replacement.** The entry jumps to an absolute jump in the arena, which goes on to the C++
   function and leaves the stack as the call made it. `original` points at a trampoline: the
   moved window, then a jump back to the rest of the function. The trampoline gets the
   function's own unwind codes, rewritten for the copy. The copy counts as all prolog: codes
   for instructions before the site always apply, codes for moved instructions apply once
   their copy has run, and codes for later instructions never apply. Chained unwind info is
   folded in, and the handler is left out. A stack walk from inside the trampoline therefore
   works. Throwing through a replaced function needs nothing more, because the C++ frames and
   the original's frames each have their own unwind info.
4. **Mid-function hook.** The site jumps to the stub's entry, which pushes the moved window's
   address as a fake return address. To the unwinder, the stub then looks like a function
   called from the moved window. That address can't be the patched site itself: the unwinder
   would read the site's code, the new jump, as an epilogue. The stub body:
   * sets up an `rbp` frame, saves the flags, aligns `rsp` to 32, and saves the 16 general
     registers, the guest's `rsp` and `rbp`, the site as `rip`, and YMM0 to YMM15 at full width
     (C++ may change their upper halves) into a `HookContext`. It then clears the direction
     flag and calls the hook through a host wrapper;
   * restores everything from the context. If the hook left `rip` at the site, the stub resumes
     at the moved window, which jumps back after the original window. Otherwise it resumes at
     `rip`, with `rsp` taken from the context, so a hook can skip code or return early. The
     stub stashes the resume address and flags just below the new `rsp` and leaves with
     `lea rsp, [rsp+8]` and `jmp [rsp-8]`, not `ret`, so it never upsets a CET shadow stack.
   * A hook may raise `rsp` but never lower it, because the memory below the guest's `rsp`
     holds the context while the hook runs. The wrapper terminates the process with an error
     log if a hook lowers it.

   The stub body has unwind info (`push rbp; mov rbp, rsp`, frame register `rbp`). The moved
   window gets the function's own unwind codes the same way a trampoline does. An exception
   thrown out of a hook therefore unwinds through the stub into the guest function, with the
   guest's registers as they were at the site. A fault in the moved window reaches the guest's
   callers as it would from the original address. Windows has no red zone in its x64 calling
   convention, so the stub may use the stack below the guest's `rsp`.

   The function's own handler never sees an exception raised in the stub or the moved window,
   because their unwind info carries no handler. So a mid-function hook is refused where that
   handler has work to do:
   * a window that overlaps a C `__try` range;
   * under a C++ handler the engine reads ([below](#c-exception-functions)), a window any of
     whose bytes is in an exception state other than -1. The message names the state, and the
     try block when the state is in one ("inside the try block of states [1, 1], whose catch
     blocks wouldn't see an exception..."); otherwise it says objects are live whose destructors
     wouldn't run. A function's entry is normally in state -1, so a hook there is accepted;
   * every site in a C++ catch or cleanup funclet, in a function with a catch whose continuation
     isn't known, in a function whose C++ tables don't read, and in a function with any other
     handler. "Other" means a handler that is not a C scope table, not a C++ handler the engine
     reads, and not the C runtime's stack cookie check. A handler that only checks the cookie has
     no work to do, so its functions take hooks as if they had no handler
     ([below](#functions-under-a-cookie-check-alone)).

   `handler_checked` overrides these refusals, for a site checked by hand: the hook can't throw
   and the moved instructions can't fault, or the handler has nothing to do there. It never
   overrides rule 2. Replacements aren't affected: nothing of theirs runs in the guest
   function's frame except the moved prologue, and no try block covers a prologue.
5. **Data patch.** Writes the bytes after checking that they lie inside the image and don't
   overlap another patch. The window rules don't apply, so a data patch over code is the
   author's responsibility.
6. **`cpuid` and `xgetbv` traps.** Function discovery finds the sites. Only decoded code counts,
   never a byte search, so data that happens to look like `cpuid` is never touched. Each site's
   first two bytes become `ud2`, after the code patches are in, and a site under another patch
   fails the load. The loader installs one vectored exception handler, first in the chain,
   once any module has traps. On an illegal-instruction fault at a known site, it answers
   through `guest_cpuid` or `guest_xgetbv`, sets the result registers, and skips the
   instruction. Every other fault passes on untouched. If the host rejects an `xgetbv` index,
   the guest sees the illegal-instruction fault at the site, as it would from a real `xgetbv`.
   Discovery runs at load for now. For the main executable it will be cached per build ID in
   the user's data directory, like the rest of the pre-flight data.

## Tests (CI, `nogame` label)

`test_hooks` runs on `hook_guest`, an original fixture DLL in `tests/fixtures/hook_guest/`. The
DLL is written in C, C++ (built with `/EHc-`, so a `try` around an `extern "C"` call is kept),
and top-level assembly with SEH directives that gives the exact prologues the tests need. The
tests cover:

* replacements that call the original: a prologue with a RIP-relative load, a short conditional
  jump at the entry, a near-entry call, a framed function, and a three-byte function whose
  no-original window runs into `int3` padding. The originals reset to null on unload;
* each refused window: a replacement away from the entry, a site inside an instruction, a loop
  back into the window, a `call` in a running window, a window past the function's end,
  overlapping mid-function hooks, a patch overlapping a data patch, a wrong hash, epilogue code,
  a data patch outside the image, and a running window over a trapped `cpuid`. A refused load
  never hands out the original;
* a mid-function hook at a call's return address, refused even with `handler_checked`, and one
  an instruction later, which loads and runs;
* a mid-function hook in `g6h_handled`, whose handler is one the engine doesn't read: refused
  without `handler_checked`, and loaded and run with it;
* a matching hash, a no-original replacement whose window holds a `call`, and mid-function
  hooks at the entry of a function with `__try` and at the entry of a C++ function. The
  fixture links the C runtime statically, so its C++ functions use an in-module
  `__CxxFrameHandler3` that the engine finds by the FH3 magic number;
* mid-function hooks that read and change general registers and flags, see the direction flag
  set and run with it clear, redirect `rip`, and return early by raising `rsp`;
* YMM0 to YMM15 surviving a hook at full width, including when the host code clobbers them, and
  a hook that changes them;
* exceptions:
  * C++ and SEH exceptions thrown through a replaced framed function to a guest catch;
  * a C++ exception and an SEH exception raised by a mid-function hook body, caught by the guest
    function's caller, with the guest's destructors and `__finally` blocks run on the way;
  * an access violation in a moved window instruction reaching the guest caller's handler;
* trapped `cpuid` and `xgetbv` returning what `guest_cpuid` and `guest_xgetbv` return, an
  unsupported `xgetbv` index faulting as an illegal instruction, and bytes after the fixture's
  code that read as `cpuid` and `xgetbv` staying untouched;
* a data patch.

## C++ exception functions

**Status: implemented.** Proposed and approved on 2026-11-21. It changed rules 2 and 4 above.

### Why

Without reading the C++ exception tables, the engine had to refuse every mid-function hook in a
function with a C++ handler unless a hand check set `handler_checked`. A hand check has to rule
out two things:

1. **A catch block resumes inside the window.** After the catch, the guest would run the jump's
   tail and the `int3` bytes behind it.
2. **The function's C++ handler has work to do at the window.** That means either a try block
   that should catch an exception the hook throws, or live objects whose destructors should run
   as an exception passes. The handler never sees the stub or the moved window.

The side DLLs show what to expect from the main executable. They are console builds from the
same toolchain, and in the C++ ones a large share of functions have a C++ handler (522 of
msvcp140's 1676). A hand check for each hook wouldn't scale, and a wrong one fails silently.

### What the engine reads

**The tables** (`common/pe/cxx_eh.h`). The reader is written from the formats as they are
publicly described and checked against compiler output and the CFG EH continuation table. No
CRT source was copied. It decodes only what the rules need:

* **FH3 `FuncInfo`**, found by its magic number (`0x19930520` to `0x19930522` in the low 29
  bits): the state count, the IP-to-state map, the try blocks with each catch's funclet, and the
  cleanup funclets the unwind map calls.
* **FH4 `FuncInfo4`**, which is compressed: the same maps, the continuation addresses FH4 may
  record for each catch (as offsets from the function or as RVAs), and the per-segment maps of
  separated functions. The IP-to-state offsets count from the start of the function or segment,
  and code before the first entry is in state -1.

Malformed tables don't read: states or try blocks out of range, funclets or map entries outside
code, implausible counts, or FH4's reserved continuation count.

**The map** (`tools/x64recomp/src/cxx_exceptions.cpp`, in `x64analysis`, shared with the scan).
For each function with a language handler, the map first decides the handler's kind, after
following up to four direct jumps, such as incremental-link thunks:

* `__CxxFrameHandler3` or `__CxxFrameHandler4`, reached through an import thunk (`jmp [rip+x]`
  to the import slot of that name) or as an export of that name;
* a GS wrapper (`__GSHandlerCheck_EH`, `__GSHandlerCheck_EH4`): an in-module function that,
  walked, calls or jumps to exactly one of the two handlers. Its handler data starts with the
  FuncInfo's RVA, like the handlers' own;
* otherwise FH3, if the handler data's first field points at FH3's magic number. That is how a
  statically linked C runtime's handler shows up: XCurl and the `hook_guest` fixture have one;
* otherwise a stack cookie check, if the handler only checks the cookie
  ([below](#functions-under-a-cookie-check-alone));
* otherwise another handler.

It then finds where each catch resumes, using every source present:

* the CFG EH continuation table (`/guard:ehcont`), which lists every continuation in the module;
* the continuations FH4 records;
* when a catch has no recorded continuation, the addresses its funclet takes with a RIP-relative
  `lea` inside the function. The funclet hands that address back to the C++ runtime. The
  funclet is decoded front to back, not just the code its entry reaches. This is because
  msvcp140 has a funclet with a block nothing branches to, and the guard table still lists that
  block's continuation.

A recorded or returned continuation only counts if it lies in a function that shares the
catch's tables. That is the function itself, a separated part of it, or, for a catch inside a
catch, the outer catch funclet. A function is a *funclet* when its FH4 tables say so or another
function's tables name it as a catch or cleanup funclet. A function is *verifiable* when it isn't
a funclet and every one of its catches resumes somewhere a source names. With a guard table,
every catch counts as named, since the table lists them all.

Rule 2 then refuses any window over a continuation, and rule 4 checks the states of a
verifiable function's windows.

### What `x64recomp scan` reports

The scan reports ([scan.md](scan.md)):

* the handler mix, including the functions under a cookie check alone;
* the funclets;
* the functions with catches, and how many are unverifiable;
* the continuations, by source;
* review notes for tables that don't read, for unverifiable functions, and for continuations
  missing from the guard table.

On the main executable, that will say how many hooks need a hand check before any is written.

### Tests

* **Unit tests (CI).**
  * `test_pe` (`tests/unit/test_cxx_eh.cpp`) covers the readers on synthetic tables:
    * FH4's compressed integers, in each length and at each length boundary;
    * an FH3 table, and each magic number;
    * the innermost of nested try blocks;
    * an FH4 table with every optional catch field and both kinds of continuation;
    * a separated FH4 catch funclet;
    * malformed tables of both kinds.
  * `test_recomp` (`tests/unit/test_cxx_map.cpp`) covers the map on a synthetic image:
    * a handler through an import thunk, through a GS wrapper, and by magic number;
    * tables that don't read, and another handler;
    * a handler that only checks the stack cookie, and two near misses: one that returns 0
      instead of `ExceptionContinueSearch`, and one that makes a second call;
    * recorded and returned continuations, including from a block nothing reaches;
    * an unverifiable function;
    * the same image with a guard table, which accounts for every catch and shows a stray
      continuation.
* **`test_cxx_hooks` (CI)** on `eh_guest`, an original fixture in `tests/fixtures/eh_guest/`. It
  is built four ways, all optimized `/MD` with `/GS` like the side DLLs: by clang-cl, which emits
  FH3, and by `cl.exe`, which emits FH4, each with and without `/guard:ehcont`. Without `cl.exe`
  on the path, the FH4 builds are skipped. It is on the path in a developer prompt and in CI's
  MSVC environment. The test checks:
  * what the map reads from each build: the handler kind, the catches, and where they resume;
  * that each build runs unpatched;
  * that a hook at the entry of a function with a try block, and of one with a live object, is
    accepted, and that exceptions thrown later still reach the catch and run the destructors;
  * that every window in a try block, or where an object is live, is refused. With
    `handler_checked`, such a window loads where no other rule stops it, and exceptions thrown
    through the function are still caught. The fixture reads a volatile global between and
    after the calls in its try block, so both compilers emit such windows;
  * that a window over a catch continuation is refused, even with `handler_checked`;
  * that a window at a call's return address is refused, even with `handler_checked`;
  * that a function whose only handler is the C runtime's `__GSHandlerCheck` takes hooks
    without a hand check: every window either loads or is refused by a rule unrelated to the
    handler. Exceptions thrown through the function still reach its caller's catch. `cl.exe`
    gives that handler to a function with a `/GS` buffer and no exception handling. clang-cl
    checks the cookie in the function itself and gives it no handler, so in the FH3 builds the
    same function shows the case with no handler;
  * that a catch with no known continuation, and a funclet, keep the hand check.

  The test also maps the guest's C++ runtime as a guest. It copies the host's
  `vcruntime140.dll` and `vcruntime140_1.dll` into a folder of their own and puts that folder on
  the search path:
  * If the runtime were forwarded to the host instead, its frame handlers would call the host's
    `RtlPcToFileHeader`. That can't see images the loader mapped, so no catch would match. The
    loader's own `RtlPcToFileHeader` (`hle_loader.cpp`) does see them.
  * The folder can't be System32 itself, because the loader treats every DLL in a search path as
    a guest file.

  The game ships its own copies of both DLLs, which are guests anyway.
* **Locally (`game` label)**, `test_game_files` maps every side DLL. It checks that every C++
  handler's tables read, and that no recorded or returned continuation is missing from a guard
  table. It also checks that, in each function with catches, every guard entry is one the map
  found from FH4's records or a funclet.

### Results on the 1.0.2.1 side DLLs

Every table reads and no continuation is stray.

| DLL | FH3 | FH4 | GS-wrapped | Funclets | With catches | Unverifiable | Guard / recorded / returned |
|---|---|---|---|---|---|---|---|
| concrt140 | 0 | 263 | 16 | 1 | 12 | 0 | 9 / 6 / 0 |
| libHttpClient.GDK | 1117 | 2 | 97 | 336 | 124 | 1 | no table / 1 / 230 |
| msvcp140 | 0 | 522 | 84 | 2 | 115 | 0 | 129 / 121 / 5 |
| msvcp140_2 | 0 | 110 | 12 | 2 | 47 | 0 | 49 / 47 / 0 |
| msvcp140_atomic_wait | 0 | 12 | 0 | 2 | 3 | 0 | 5 / 3 / 0 |
| vccorlib140 | 0 | 298 | 13 | 2 | 54 | 0 | 57 / 54 / 0 |
| XCurl | 41, all by magic | 0 | 0 | 0 | 0 | 0 | no table |
| XboxBvhBuilder | 0 | 43 | 28 | 0 | 0 | 0 | no table |
| amd_fidelityfx_upscaler_xbox | 0 | 24 | 16 | 0 | 0 | 0 | no table |

The other seven DLLs have no C++ handler. Guard entries outside the functions with catches
aren't checked. The table also lists where C `__except` blocks resume, which is why
vcruntime140, with no C++ handler, has 17 entries.

### Limits

* **FH4's IP-to-state maps are coarse under `/EHs`.** `cl.exe` records a state change only where
  a call could throw. After a try block's first call, the map shows the try state until the
  next call, and it never returns to -1 at the end of the try block. The engine refuses those
  windows. That is safe, but it refuses some windows that are really outside every try block. A
  finer check that uses the `/EHs` flags was decided against
  ([below](#a-finer-state-check-under-ehs)).
* **A function's entry isn't always in state -1.** A compiler can start a try state within the
  entry's first five bytes, and an entry hook then needs a hand check. The fixture reads a
  global before each try block, so that its entries stay in state -1.

### What we don't know yet

Three things depend on the main executable, and its scan report answers them: which handlers
it uses, whether it has an EH continuation table, and how many of its functions are
unverifiable.

## Windows at return addresses

**Status: implemented.** Proposed and approved on 2026-11-21. It added a bullet to rule 2.

A window that starts at a call's return address puts the patch jump exactly where the callee
returns. That breaks unwinding out of the callee:

1. When an exception unwinds out of the callee, the unwinder reads the code at that return
   address to decide whether the caller is in an epilogue.
2. The jump there looks like an epilogue's tail jump out of the function.
3. The unwinder therefore unwinds the caller's frame as if the frame were already gone.

The exception is mis-unwound. `test_cxx_hooks` found this before the rule existed, with
`handler_checked` at a site right after a call: the exception went unhandled, and the process
exited with the C++ exception code. Stack walks through that frame use the same unwinder.

So rule 2 refuses any code patch whose window starts at a call's return address. That holds for
every kind, under any handler or none, and regardless of `handler_checked`. The code walk finds
every call, so the return addresses are known. The refusal names the call and suggests hooking
the next instruction instead.

A return address strictly inside a window can't happen: that call would be in the window, and
rule 2 refuses moving a call. Replacements sit at function entries, which aren't return
addresses, so in practice the rule only refuses mid-function hooks.

Tests:

* `test_hooks` refuses a hook at the return address in `hook_guest`'s `g6h_after_call`, and
  loads and runs one an instruction later.
* `test_cxx_hooks` refuses a hand-checked window at every return address in `eh_guest`'s
  functions, in all four builds. It also throws through every hand-checked window in a try
  block that loads.

## Functions under a cookie check alone

**Status: implemented.** Decided on 2026-11-21. It changed rule 4.

### Why

`cl.exe` gives a language handler to a function that has a `/GS` buffer but no exception
handling of its own: the C runtime's `__GSHandlerCheck`. When an exception passes through the
frame, that handler checks the frame's stack cookie and fails fast if the cookie was
overwritten. Otherwise it lets the exception go on. It never catches anything and never runs a
cleanup.

Before this change, rule 4 refused every window in such a function unless a hand check set
`handler_checked`, because the engine didn't read the handler. The side DLLs have 358 of these
functions, more than all their functions with catches. The main executable comes from the same
toolchain, so the same holds there.

A hook skips the handler only for an exception raised in the stub or the moved window. What it
skips is the cookie check for that one exception. The function still checks its cookie when it
returns normally, in its own epilogue code. So a function under this handler is checked as if it
had no handler: rules 1 to 3 still apply, including the refusal at a call's return address, but
it needs no hand check.

### How the engine recognizes it

The engine recognizes the handler by what it does, because the module's own copy has no name.
After following jumps, the handler must be a discovered function that:

* runs straight through in at most 16 instructions, with no branch, interrupt, system call,
  system instruction, or `ud2`;
* makes exactly one direct call, and the call goes into the image's code;
* sets `eax` or `rax` to 1 (`ExceptionContinueSearch`) after that call, as its last write to
  `rax` before it returns;
* reaches code that reads the module's security cookie RIP-relatively, through that call and at
  most two more levels of direct calls or tail jumps. The cookie's address comes from the load
  config directory.

In the `cl.exe` fixture, the handler calls a shared routine, which tail-jumps to the C runtime's
cookie check. The side DLLs' handlers pass the same checks. A module links one C runtime, so it
has at most one such handler, and `test_game_files` checks that.

A handler that returns anything other than `ExceptionContinueSearch`, or does anything more,
stays "another handler" and keeps the hand check.

### Results on the 1.0.2.1 side DLLs

The engine recognizes 358 functions as being under a cookie check alone. That leaves 97 under
other handlers, C scope tables included.

| DLL | Cookie check only | Other handlers |
|---|---|---|
| libHttpClient.GDK | 106 | 4 |
| msvcp140 | 64 | 6 |
| amd_fidelityfx_upscaler_xbox | 48 | 4 |
| XboxBvhBuilder | 46 | 7 |
| Bink2Scarlett | 39 | 4 |
| XCurl | 22 | 11 |
| vcruntime140 | 11 | 18 |
| vcomp140 | 7 | 1 |
| concrt140 | 5 | 9 |
| msvcp140_2 | 5 | 4 |
| vcruntime140_1 | 3 | 6 |
| vccorlib140 | 2 | 7 |

The other four DLLs have no such function. Each has 4 other handlers.

## A finer state check under /EHs

**Status: decided against on 2026-11-21.**

Under `/EHs`, the compiler assumes that only a call can throw. So `cl.exe` keeps FH4's
IP-to-state map accurate only at calls ([Limits](#limits)). Between two calls inside a try
block, the map can't tell whether a window is still in the try block or already past its end.
The FH4 header and FH3's `FuncInfo` carry flags for `/EHs`, and FH4 also has one for
`noexcept`.

The idea was to use those flags to accept such windows. A window holds no call, because rule 2
never moves one, so under `/EHs` nothing in the window could throw.

That reasoning leaves out the hook itself. A hook body can throw, and `test_hooks` tests that it
can. If the window really is inside the try block, the guest's catch should see that exception.
It can't, because the stub has no handler, and the map can't tell which case applies. So the
finer check would be sound only for a hook that can't throw. Saying that a hook can't throw is
exactly what `handler_checked` already does, by hand. The flags would add an automatic version
of the same promise, which the engine has no way to check. So the windows stay refused, and a
hook that can't throw sets `handler_checked`.
