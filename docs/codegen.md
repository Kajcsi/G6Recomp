# Code generation

> **Frozen backend.** Since the move to native execution (`docs/execution-model.md`), the game
> doesn't run through generated code. The generator, `GuestContext`, the dispatch table, and the
> differential tests keep building and stay in CI, but get no new instruction work and no
> runtime integration. They are kept for a function that patching can't fix, which would first
> need a planned bridge between recompiled and native code, and for any future non-x86 host.
> The guest unwinder, `gs:` rewriting, and import placeholders described as later work below
> are dropped.

How `x64recomp` turns guest functions into C++, and what that C++ runs on. Approved before
implementation (see CLAUDE.md: changes to the output format or `GuestContext` need a plan first).

## Output

`x64recomp emit <pe>` writes, for one module:

* `functions.h`: a declaration for every guest function, plus the module's function table.
* `functions_<n>.cpp`: the functions, split into a fixed number of chunks so a large module
  compiles in parallel.
* `table.cpp`: the module's guest-address-to-host-function table.

Each guest function becomes `void sub_<address>(g6::cpu::GuestContext& ctx)`, named by its
address at the module's assigned load address. Its basic blocks become labels, and branches
become `goto`. Generated files go under `build/generated/<build-id>/` for game modules (or the
build tree for test programs) and are never committed. They carry a comment per instruction for
debugging; that is fine because they never leave the user's machine.

## GuestContext

`runtime/cpu/include/g6/cpu/context.h`. It holds only what must survive a call:

* the 16 general-purpose registers, in encoding order;
* the 16 vector registers at full 256-bit width (the target is Zen 2, so AVX2 and no AVX-512);
* the base of the guest thread's TEB, which `gs:`-relative accesses use.

Deliberately left out:

* **Status flags** are locals in each generated function. The Win64 ABI leaves them undefined
  across calls, so nothing legitimate reads them after a call or at function entry, and the
  compiler deletes every flag that is computed but never read. `pushfq`/`popfq` and
  `lahf`/`sahf` build and unpack the flags from those locals.
* **The direction flag** is a local too; the ABI requires it clear at every call boundary.
* **MXCSR** is the host's own register. Guest threads run on host threads and SSE/AVX
  instructions execute natively through intrinsics, so the guest's rounding and exception
  masks already apply. `ldmxcsr`/`stmxcsr` act on the real register.
* **x87** is not modeled. The only x87 instructions the side DLLs of 1.0.2.1 reach are
  control-word and exception-state operations in the C runtime (`fnstcw`, `fldcw`, `fnclex`).
  Like MXCSR, the control word and exception flags are the host thread's own, so those
  operations act on the real x87 unit. Any other x87 instruction stops with a logged error
  until a real need shows up.

## Registers inside a function

A generated function copies the registers it uses into locals on entry and works on the locals.
Before every call, return, indirect jump, and tail call it writes back the registers it writes
anywhere, and after every call it reloads every register it uses. No calling-convention
assumptions are made: a callee may change any register, so hand-written assembly and custom
conventions keep working. Narrowing this per function (for example, trusting the ABI's
nonvolatile registers) is a later optimization and needs approval first.

## Memory and the stack

Guest pointers are host pointers. Each module is mapped at its assigned base and its relocations
are applied, so addresses in guest memory and constants in generated code agree, and loads and
stores are plain memory accesses (through `memcpy`, so unaligned access is defined).

The guest stack is ordinary memory. `call` pushes the real guest return address and `ret` pops
it, so a guest frame looks exactly as it would on the console. Unwinding from `.pdata`, stack
arguments, stack probes, and code that reads its own return address all depend on that. A
host-side shadow return stack would make returns cheaper but would break all of those, so the
real stack stays.

## Load addresses

All sixteen side DLLs of 1.0.2.1 prefer `0x180000000`, so they cannot all sit at their preferred
base. Each module gets a fixed, non-overlapping base when it is recompiled (`--base`), and every
address in its generated code is computed at that base. The loader maps the module there and
applies its base relocations; immediates covered by a relocation are rebased in the generated
code too. The main executable keeps its preferred base when it is free. The host executable is
built with high-entropy ASLR so it stays out of the low guest ranges, and the guest ranges are
reserved at startup; if one is taken, startup stops with a clear error.

## Calls and jumps

* A direct call to a known function is a direct C++ call.
* Indirect calls and jumps look the target up in the dispatch table
  (`runtime/cpu/include/g6/cpu/dispatch.h`): a dense array per module for 16-byte-aligned
  addresses (CFG requires that alignment for indirect-call targets) and a hash map for the
  rest. An unknown target logs the address and stops. Nothing interprets guest code, and the
  original code is mapped read-only and non-executable, so it never runs.
* A jump into another function is a guaranteed tail call (`[[clang::musttail]]`), so chains of
  tail calls don't grow the host stack.
* A switch dispatch becomes a `switch` on the computed target, with one case per table entry
  (see `docs/function-discovery.md`).
* A call that should not return (the next address is outside the function) stops with an error
  if it ever does return, instead of falling into whatever follows.
* Imports were to get placeholder addresses in the dispatch table, and host APIs that receive
  guest callbacks an adapter that sets up a context. Both are dropped: under native execution,
  imports are bound directly and callbacks are plain calls.

## Instructions

Instructions are implemented in order of how often the binaries use them: the side DLLs use 456
distinct mnemonics, and the most common 100 cover 98.6% of their instructions. Integer
instructions compute their documented results and flags in inline helpers
(`runtime/cpu/include/g6/cpu/guest.h`), including BMI1/BMI2, `shld`/`shrd`, `adc`/`sbb`, and
the string instructions with and without `rep`. SSE through SSE4.2, AVX, AVX2, FMA, and F16C
map to the same host instructions through intrinsics. `lock`-prefixed instructions become
atomic operations, and `cmpxchg16b` is always atomic, with or without `lock`, since the
difference can't be observed by a single thread.

A few details:

* The SSE4.2 string comparisons take explicit lengths from `rax` and `rdx`. With REX.W the
  instruction reads 64-bit lengths, which it treats as absolute values capped at 16; the
  helper saturates them to the range the 32-bit intrinsic accepts, which gives the same result.
* `rdsspd`/`rdsspq` (read the shadow-stack pointer) do nothing. Zen 2 has no shadow stacks, and
  without an active one the instruction leaves its register unchanged, which is what console
  code expects.
* `clflush` is forwarded. It has no effect a guest can observe apart from timing.

`cpuid` and `xgetbv` report the console's CPU rather than the host's
(`runtime/cpu/include/g6/cpu/cpuid.h`). Guest code picks code paths from them, and only the
paths a Zen 2 can take are known to translate. The runtime asks the host and keeps a feature
only if Zen 2 has it too, so nothing the host lacks is ever reported either. Concretely: the
highest basic leaf is capped at Zen 2's 0x10, the feature leaves drop AVX-512, ERMS, GFNI,
VAES, Bulldozer's XOP/FMA4/TBM, and everything newer, `xgetbv` reports only x87, SSE, and AVX
state, and the hypervisor bit and leaves are hidden. Vendor, family, model, cache, and
topology leaves pass through. The masks follow AMD's public description of Zen 2; the
console's exact values aren't known, so a feature the console lacks could still slip through,
but nothing past Zen 2 can.

An instruction that isn't implemented yet becomes a call that logs its address and mnemonic and
stops, so a module always compiles and the gaps are measurable. `x64recomp emit` prints the
counts, `--missing <n>` lists the most common untranslated mnemonics, and `--strict` makes any
untranslated instruction or decode error fail the run (the differential tests build with it).

Where the architecture leaves a flag undefined, the helpers pick a value (noted in `guest.h`)
and the differential tests don't compare it.

### Floating point and NaNs

Floating-point results match the console bit for bit, with one known exception. When both
inputs of an addition, multiplication, dot product, or fused multiply-add are NaN, x86 returns
the first one. LLVM treats those operations as commutative and may swap their operands, so the
recompiled code can return the other input's NaN. Only the NaN payload differs: the result is a
NaN either way. Forcing the original order would take inline assembly for every such
instruction and stop the compiler from optimizing around them, and no ordinary code depends on
which of two NaN payloads survives, so this stays a documented divergence. The differential
tests accept it for exactly those operations and nowhere else.

### Coverage on 1.0.2.1

All sixteen side DLLs translate with no decode errors: 11,187 functions and 488,460
instructions. Only 24 instructions (0.005%), in 5 functions of the HTTP client, are not
translated: AVX-512 64-bit minimum and maximum (`vpminuq`, `vpminsq`, `vpmaxuq`, `vpmaxsq`).
They sit behind a feature check. Zen 2 has no AVX-512, so the console never runs them, and
since `cpuid` reports Zen 2's features, recompiled code doesn't either, even on a host with
AVX-512. They stay untranslated on purpose: reaching one means the feature masking is wrong.

## Hooks

Hooks are patched into natively running code instead (`docs/execution-model.md`), so the
generator has no hook support and none is planned.

## Differential tests

`tests/diff/` holds an original test program, `programs/diff_program.c`, built as a plain x64
DLL with no CRT and no imports. It is built twice: once for the baseline target (SSE2) with
the assembly probes, and once with `/arch:AVX2`, which turns the same C into VEX, FMA, and BMI
code. At build time `x64recomp emit --strict` translates both DLLs, and the generated code is
linked into `test_diff`. The test loads each DLL natively, maps a second copy at a different
base the way the runtime does, runs the recompiled version, and compares results over boundary
values and pseudo-random inputs.

* **Assembly probes** (`programs/probe_list.h`) each wrap one instruction: integer arithmetic,
  shifts and rotates at every count, multiply and divide, memory operands and atomics, all 16
  conditions for `setcc`, `cmovcc`, and `jcc`, string operations in both directions,
  packed integer and floating-point instructions in their SSE, VEX.128, and VEX.256 forms,
  SSE4.2 string comparisons over short random strings with embedded terminators, masked
  loads and stores, half-precision conversions in every rounding mode, and the x87 control
  word.
  They return the flags through `pushfq`, and the test compares the flags each instruction
  defines (which can depend on the shift count).
* **Compiled C** covers what an optimizing compiler emits for ordinary code: loops, jump tables,
  recursion, calls through function-pointer tables, tail calls, stack arguments, structure
  returns, 128-bit multiplies, floating-point conversions, and autovectorized loops.
* Every recompiled call must also preserve the nonvolatile registers, `rsp`, and the low halves
  of `xmm6`–`xmm15` (the harness fills them with sentinels).
* `cpuid` and `xgetbv` are the one place where recompiled and native results differ on
  purpose. Their test checks the recompiled answer against the native one through the rule
  above (vendor and model unchanged, Zen 2 features kept when the host has them, AVX-512 and
  newer features absent), with the bit positions written out in the test rather than taken
  from the runtime. The same test checks that recompiled `rdsspq` reports no shadow stack,
  whatever the host has.
* Setting `G6_DIFF_TRACE` in the environment names each call on stderr before it runs, to find
  one that hangs.

Not covered: guest exceptions and TLS. The generator never got exception or TLS models, and
under native execution Windows handles both, so their tests belong to the loader
(`tests/fixtures/`).
