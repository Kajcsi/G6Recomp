# Pre-flight scan (`x64recomp scan`)

**Status: implemented (Phase 4 of [execution-model.md](execution-model.md)).** The scan is
`tools/x64recomp/src/scan.cpp` in the `x64analysis` library, with the command in
`tools/x64recomp/src/main.cpp`. It is tested in CI on an original fixture and locally on the
1.0.2.1 side DLLs.

## Why

Native execution runs the guest's bytes as they are. Before a module is mapped, we need to know
what in it doesn't fit a desktop host. The scan answers the
[pre-flight checks](execution-model.md#pre-flight-checks-for-the-main-executable):

| Check | Where the scan answers it |
|---|---|
| 1. Relocations, or a free preferred base | `module` (relocation counts, base, size) |
| 2. TEB offsets and PEB access | `segment accesses` |
| 3. System instructions | `system instructions` |
| 4. Instructions the hosts lack, `cpuid` and `xgetbv` sites | `features`, `cpuid and xgetbv` |
| 5. TLS slot assumptions | `tls slots` |
| 6. Imports by category, first-run stubs | `module` lists the imported DLLs; categories and stubs come from the loader's dry run |
| 7. Code that verifies its own bytes | `code reads`, `code writes`, `addresses inside functions` |

The report is derived from the scanned file, so for game files it stays on the user's machine
(rule 2). Counts and prose findings, like the ones below, can go in the repo.

## Usage

```
x64recomp scan <file> [--sites] [--no-sweep]
```

* `--sites` lists every site of every finding instead of the first four.
* `--no-sweep` skips the sweep (below), so only code the walk reached is reported.

The output has these sections:

1. **module**: kind, base, size, DLL characteristics, relocations, TLS, CFG, and imports. Then
   the exception handlers, from the C++ exception map ([hooks.md](hooks.md#c-exception-functions)):
   how many functions have a handler and of which kind (FH3, by magic number or not; FH4; through
   a GS wrapper; a stack cookie check alone, and by how many handlers; other), the C++ catch and
   cleanup funclets, the functions with catches and how many of those are unverifiable, tables
   that don't read, and the catch continuations by source (the guard table, FH4's records,
   funclet returns) with any strays.
2. **coverage**: functions, instructions, `.pdata` bytes, and unreached bytes.
3. **features**: each instruction set extension outside the baseline, with its class, count,
   sites, and whether this host has it. The baseline extensions follow on one line.
4. **segment accesses**: each `fs:` and `gs:` access, its TEB field name, and the TLS slot
   summary.
5. **system instructions**, **traps**, and **timing**.
6. **cpuid and xgetbv**.
7. **code reads**, **code writes**, and **addresses inside functions**.
8. **review**: one line per finding that needs a decision before the module runs natively. An
   empty list means the scan found nothing that stops the module from being mapped as it is.

Sites are RVAs. A site marked `(swept)` was found only by the sweep.

## How it works

### Walk

The scan starts from function discovery ([function-discovery.md](function-discovery.md)). It
walks each function from its entry and from any CFG EH continuation targets inside it, and
decodes and classifies each instruction once.

### Taken addresses

Code sometimes takes the address of other code that isn't a function start. This happens either
as a RIP-relative `lea`, or as a 64-bit relocated pointer in the image. These addresses are
reported as **addresses inside functions**. The scan then walks from each one as if it were an
entry, stopping at code already covered, for up to eight rounds, since a newly walked piece can
take further addresses.

On the side DLLs these are the continuations of C++ catch blocks. A catch funclet returns the
address where execution resumes after the catch, and nothing else jumps there. Without this
step, the code after every catch block counted as unreached.

### Sweep

Bytes that `.pdata` marks as code but the walk didn't decode are decoded in a straight line.
`int3` padding, NOP runs, and resolved jump tables are skipped. A linear sweep can decode data
as code, so a review note whose sites were all found by the sweep says so ("only the sweep
found these, so they may be data"). The original fixture shows why. Its statically linked CRT
has a few data bytes that decode as `insd`, which only the sweep finds.

### What each instruction is checked for

* **Instruction set extensions**, by Zydis's extension name, in three classes:
  * *baseline*: every supported host has it. That covers x86-64 up to AVX2, plus FMA, BMI1 and
    BMI2, AES, PCLMULQDQ, F16C, LZCNT, MOVBE, XSAVE, RDRAND, and RDTSCP.
  * *host-check*: Zen 2 has it, but a supported host might not. That covers ADX, RDSEED,
    CLFLUSHOPT, CLWB, SHA, RDPID, XSAVEC, SSE4a, MONITORX, CLZERO, RDPRU, and MCOMMIT. Each one
    carries its CPUID bit, so the report can say whether this host has it.
  * *not Zen 2*: anything else, such as AVX-512. The console couldn't run these, so the code
    must sit behind a feature check, and the loader's `cpuid` trap answers that check the way
    Zen 2 would.

  CET instructions (`endbr64` and the like) run as no-ops on hosts without CET, so they aren't
  counted.
* **Segment accesses**: every `fs:` or `gs:` memory operand, grouped by offset. Accesses through
  the pointer loaded from `gs:[0x30]` (the TEB's own address) count too, if they come within the
  next 16 instructions. Accesses where a register picks the offset are grouped separately,
  because the field they reach depends on what that register can hold. Four offsets are
  expected, since they sit at the same place on desktop Windows: `0x08` StackBase, `0x10`
  StackLimit, `0x30` Self, and `0x58` the TLS array. Any other offset, any `fs:` access, and any
  read of the PEB (`gs:[0x60]`, which would let code find host modules by itself, rule 5) gets a
  review note.
* **TLS slots**: after a load of the TLS array from `gs:[0x58]`, the scan follows the register
  for up to 16 instructions to the load that picks the slot. The slot is either:
  * *indexed*: a register indexes it, in a function that reads the module's TLS index. This is
    how compilers do it, and it works with the loader's donor-DLL TLS.
  * *other index*: a register indexes it, but the function never reads the TLS index.
  * *fixed slot*: the code assumes its module's TLS index is a constant. `/GA` produces this
    in an executable, which always has index 0. Under the loader, guest DLLs get a donor's
    slot, so this would break there. A guest executable keeps index 0 when the host lends it
    (`docs/execution-model.md`, "TLS and the TEB").
  * *not followed*: the array pointer isn't used within the window.

  Everything except *indexed* gets a review note.
* **System instructions**: privileged, OS-level, and I/O instructions (`syscall`, `int 0x2e`,
  `hlt`, port I/O, and so on). Any of these gets a review note.
* **Traps**: `int3`, `ud2`, and the fail-fast (`int 0x29`) and assertion (`int 0x2c`)
  interrupts. These are listed, not flagged: they behave the same on desktop Windows.
* **Timing**: `rdtsc`, `rdtscp`, and `rdpru`. These are listed so that the frame-timing and
  thread work in M2 knows where the game reads the clock directly.
* **`cpuid` and `xgetbv`**: these are the sites the loader traps ([hooks.md](hooks.md)).
* **Code as data**:
  * *code reads*: a load from a code address, such as a checksum over a function;
  * *code writes*: a store to a code address;
  * *addresses inside functions*: the taken addresses above.

  Self-verification would show up as code reads, and self-modification as code writes. Both get
  a review note (pre-flight check 7). Taken addresses don't, because catch continuations
  account for them.

### Review notes

| Note | What to decide |
|---|---|
| no base relocations, or relocations that aren't 64-bit | Whether the preferred base is free in the host process |
| `fs:` accesses, unexpected TEB fields, computed TEB accesses | What the field holds on the console, and whether a patch or HLE is needed |
| reads the PEB | Where the code goes next (rule 5) |
| TLS: fixed slot, other index, not followed | Whether the site really picks a TLS slot, and if so how to patch it |
| system instructions | Each one needs a patch |
| host-check features (and whether this host has them) | Whether the code checks for the feature first; if not, the host requirement goes up |
| not-Zen 2 features | Confirm a feature check guards them |
| code reads or writes | Stop before hooking (pre-flight check 7) |
| unreached `.pdata` bytes | Usually nothing; look if the sweep found anything there |
| decode or directory errors | The file is malformed, or a reader needs fixing |
| C++ handler whose tables don't read | Whether the reader needs fixing; until then the hook engine treats the function's handler as unknown |
| functions with a catch whose continuation no source names | Expect a hand check (`handler_checked`) for mid-function hooks in them |
| catch continuations missing from the EH continuation table | One of the sources is being misread; fix it before trusting the map |

## Tests

* **`test_scan`** (CI, `nogame`) runs on `tests/fixtures/scan_guest`, an original DLL. It has a
  little C (a thread-local counter, accessed through the TLS index) and top-level assembly with
  one small function per finding:
  * each feature class, and CET;
  * `syscall`, `int 0x2e`, the traps, `rdtsc`, `cpuid`, and `xgetbv`;
  * `fs:`, the PEB, an unexpected TEB field (directly and through Self), and a computed TLS slot
    access;
  * a fixed TLS slot, another module's index, and a lost array pointer;
  * a checksum over code, and a byte patch of code;
  * a taken `lea` address, a relocated pointer into a function, and bytes only the sweep
    reaches.

  The test checks that each finding is reported at the function that has it and nowhere else.
  It checks the re-walk, the sweep, and the sweep turned off, and each review note, including
  the notes that must not appear.
* **test_recomp's scan cases** (`tests/unit/test_scan.cpp`, CI) use synthetic images from
  `PeBuilder`. They cover a module without relocations, a PEB read in an executable,
  relocations that aren't 64-bit, an array pointer that isn't followed past a `ret`, and the
  site and access formatting.
* **`test_game_files`** (local, `game` label) scans each side DLL and checks the facts below. It
  also scans the main executable when present, and prints its review notes.

## Results on the 1.0.2.1 side DLLs

These are **checked here**, on all 16 side DLLs:

* **No system instructions** anywhere, by walk or sweep.
* **TEB**: `gs:` accesses only at the four expected offsets, and no `fs:` accesses. Every DLL
  that touches the TEB reads StackBase (and some read StackLimit) through the Self pointer. In
  the CRT, the startup lock uses the stack base as the owning thread's identity.
* **TLS**: every access indexes the TLS array with the module's own TLS index. No fixed slots,
  no foreign indices, and none unfollowed.
* **Code as data**: no code reads and no code writes. The addresses inside functions are all
  catch continuations. libHttpClient has 333 of them because it has no EH continuation table, so
  the scan finds its continuations only through the funclets.
* **Features**: no host-check extensions anywhere. The only not-Zen 2 code is libHttpClient's 24
  AVX-512 (EVEX) instructions, which already sit behind a feature check. The `cpuid` trap answers
  that check as Zen 2, so they never run, and libHttpClient isn't mapped anyway (rule 5).
* **Timing**: vcomp140 (the OpenMP runtime) reads the time-stamp counter in 4 places and has no
  `cpuid` sites.
* **C++ exception handlers**: every FH3 and FH4 table reads, no continuation from FH4's records
  or a funclet is missing from a guard table, and in each function with catches every guard
  entry comes from one of those two. 358 functions are under a stack cookie check alone, and
  97 under other handlers. The counts are in [hooks.md](hooks.md#results-on-the-1021-side-dlls)
  and [below it](hooks.md#functions-under-a-cookie-check-alone).
* **Coverage**: the walk reaches nearly all `.pdata` code. The rest is between 2 and 167 bytes
  per DLL: XCurl 167, XboxBvhBuilder 39, the two AMD FidelityFX DLLs 19 each, Bink 10,
  libHttpClient 10, vccorlib140 5, and concrt140 2. The others have none. The sweep finds nothing
  to flag in those bytes, and we haven't looked into why the walk misses them.

Three false positives came up while calibrating the scan on these DLLs, and the design above
answers each one:

* StackBase read through Self was flagged everywhere until it joined the expected offsets.
* Catch continuations were reported as unreached code until the re-walk from taken addresses.
* One TLS array load was followed past the old 8-instruction window. The window is now 16.

## What this means for the hook engine

The patch-window rule in [hooks.md](hooks.md) counts jumps from the code walk, switch-table
targets, C `__except` targets, and catch continuations as places where execution can enter a
function. The continuations come from the C++ exception map, which the scan and the hook engine
share (`tools/x64recomp/src/cxx_exceptions.cpp`, in `x64analysis`). The map's counts in the
scan report tell how many mid-function hooks on a module the engine can check by itself:
everywhere except catch and cleanup funclets, unverifiable functions, and functions under any
other handler. A handler that only checks the stack cookie doesn't count as another handler
([hooks.md](hooks.md#functions-under-a-cookie-check-alone)).

The loader doesn't read the scan yet. Caching the report per build ID and using it in the
loader's pre-flight report comes with Phase 5.
