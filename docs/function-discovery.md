# Function discovery

`x64recomp` emits one host function per guest function, so it first has to know where every
guest function starts and which bytes belong to it. This page describes how
`tools/x64recomp/src/function_discovery.cpp` decides that, and what it does not handle yet.

## Sources of truth, strongest first

1. **`.pdata`.** Every x64 function that touches the stack, saves a nonvolatile register, or
   calls anything must have a `RUNTIME_FUNCTION` entry, so the exception directory lists the
   start and end of nearly all real code. Entries whose unwind info is *chained* (or whose
   entry is *indirect*) describe a fragment of another function: cold code split away from its
   hot path, or a shrink-wrapped region. These are grouped under the primary entry they chain
   to, so a function is a primary range plus zero or more fragments.
2. **Image metadata seeds.** The entry point, exported functions (data exports are skipped),
   TLS callbacks, and the Control Flow Guard function table. The CFG table lists every function
   whose address is taken, which is exactly the set indirect calls can reach. EH-continuation
   targets are *not* function starts and are not used as seeds.
3. **Relocated code pointers.** Every `DIR64` relocation whose target lands in an executable
   section: vtables, callback tables, `mov reg, imm64` of a function address.
4. **Control flow.** Direct `call` targets and tail jumps (unconditional or conditional jumps
   that leave a function) found by walking every known function.
5. **Addresses taken with `lea`.** A RIP-relative `lea` in a walked function whose target is
   16-byte aligned and has an `int3` padding byte just before it. Labels inside a function have
   no padding in front of them, so this only picks up function starts. It matters for leaves
   in an image without a CFG table: a leaf the compiler placed right after another leaf that
   jumps to it is walked as part of that leaf, and the `lea` that passes its address as a
   callback is the only thing that marks where it begins. `--no-follow` turns this source off
   along with calls and tail jumps.

Anything that is not already a `.pdata` function becomes a *leaf candidate*. A leaf is a
function with no unwind data; by the ABI it can't call anything or touch the stack, so in
compiler output leaves are small. Hand-written assembly can break those rules.

## Verifying a leaf candidate

The code walker (`code_walker.cpp`, Zydis-based recursive descent) follows control flow from
the candidate. The candidate may extend up to the next known function or fragment, or the end
of its section. It is accepted when every path decodes and ends in one of:

* a return or a trap (`int3`, `ud2`, `hlt`, `int 29h` for fast-fail, `int 2Ch` for asserts),
* a tail jump to another address (recorded and classified in turn),
* a jump through a RIP-relative slot (an import thunk),
* an indirect jump: either a switch dispatch whose table resolves (its targets are walked like
  any other branch, see below) or one that stays unresolved (counted),
* sequential flow into another known function's entry (a *fall-through*).

It is rejected when a path fails to decode, runs past its limit into something that isn't a
function, branches outside the image, or when the candidate starts with a trap (a pointer to
padding).

Candidates are processed lowest address first. That makes a real function accepted before any
bogus pointer into its middle. Hand-written code sometimes falls through from one entry point
into another (two exported names sharing a tail). The lower entry is accepted first and takes
in both bodies. When the higher entry comes up inside it, the first leaf is re-walked as if the
second entry were already a function. The split stands only if the first leaf then reaches the
second entry solely by falling or jumping to it exactly, and the second never jumps back into
the first. Otherwise the second address is treated as a bogus mid-function seed and dropped.

## Jump tables

Code emission needs the targets of every switch dispatch, and discovery needs them too, since
case blocks are only reachable through the table. `jump_table.cpp` resolves them while the
walker runs. The walker keeps the path it took to each instruction: the last 64 steps, with
which way each conditional branch went. When it reaches an indirect `jmp`, the analyzer works
backwards along that path to find out where the jump target came from.

**Shapes it recognizes.** All three are table lookups indexed by the switch value:

* *Image-relative entries* (MSVC's usual form). The image base is loaded into a register, a
  32-bit entry is read from the table, which sits at a known offset from that base, and the base
  is added back before the jump.
* *Table-relative entries* (clang's form). The table address is loaded, a signed 32-bit entry is
  read from it, and the table address is added to the entry.
* *Absolute entries.* A 64-bit code pointer is read from the table, either into a register or by
  the `jmp` itself. Entries go through the relocation-aware VA-to-RVA mapping.

In the first two forms, the index may first go through a *byte table*: the switch value picks a
one-byte case number, which then indexes the main table. This is how MSVC compresses sparse
switches. The byte table is bounded by the switch value and the main table by the largest byte
in the byte table.

**Bounds.** A table is only trusted once the analyzer knows how many entries it has. It gets the
bound from the guard on the path: a compare of the index against a constant, followed by the
branch that skips the dispatch when the index is too large. That is an unsigned "above" or
"above or equal" check, or, for a single-case guard, an equality check. A signed branch, or an
equality test that went the unequal way, may sit between the compare and that branch; it is
passed over as long as nothing changes the flags. The index may be derived from the compared
value by a copy, sign extension, adding a non-negative constant, masking with `and`, or shifting
right. The bound is carried through each of these in program order. A mask with a small
constant bounds the index on its own.

The compare may also be on the other side of a register copy. `mov r15d, edx` followed by
`cmp edx, 8` bounds r15, and so does `mov edx, r15d` followed by `cmp edx, 8`, as long as
neither register is written between the copy and the compare. The copy must be at least 32
bits wide, so the compare sees the whole value.

A register written before the analyzer's window, or by a call (for a volatile register), is
unknown. One case is accepted anyway: in the image-relative form, the image base is often
loaded long before the dispatch and kept in a nonvolatile register. When the same unknown
register is both the base of the table read and the value added to the entry, it is assumed to
hold the image base. That assumption is accepted only when the table is bounded and every entry
lands in the function.

**Validation.** Every entry must land inside the function being walked, meaning its primary
range and fragments, or, for a leaf candidate, the range it may extend into. It must also not
be another function's entry. A bounded table with even one bad entry is rejected outright and counted, not
truncated. An unbounded table, one whose guard isn't on the path, is read entry by entry until
the first invalid one, up to 4096 entries. That is a fallback, so the count is reported
separately. A table whose base is only an assumption is never read unbounded.

An unbounded read also stops earlier in two cases. If the index was zero-extended from a byte
on the way, it can't pass 255, adjusted by any later steps. And it stops where another known
table begins, since compilers place a function's tables back to back, and the entries of the
next table often land inside the same function. When the walk of a function finds an unbounded
table that ran into a table it found later, the function is walked again with every table
known. The visitor sees only the walk that stands, so code reached through the bad entries
never adds `cpuid` sites. Discovery hands the table addresses it found to later walks (the
pre-flight scan and the C++ exception map), which then stop at once.

**What isn't a table.** An indirect jump through a register loaded from a non-indexed memory
slot, such as a field or a vtable slot, is an *indirect tail call*. Code emission treats it like
an indirect call followed by a return. Anything else, such as a jump to a function argument, a
CFG dispatch stub, or the delay-load helper's final jump, stays *unresolved* and is counted.
Those need runtime dispatch through the address table.

`x64recomp functions <pe> --tables` lists every resolved table: the function, the jump, the
table, the entry size and count, and the byte table if there is one.

**Cross-check.** For two `XboxBvhBuilder.dll` functions with four dispatches between them, the
results were compared with Ghidra's (through REA). The two decompilations agreed on every jump
site, table address, entry count, and byte table. They also agreed on the set of destinations,
counting the entries Ghidra files as the default case.

## Limits and next steps

* **One path per dispatch.** The bound comes from the first path the walker takes to a dispatch.
  If that path skips the guard (for example, a loop back-edge that re-enters after the check),
  the table is read unbounded. None of the 1.0.2.1 DLLs hit this.
* **No-return calls.** A `.pdata` function that ends in a call to a function that never returns
  can appear to fall through into the next function. This is harmless for discovery, since the
  next function is already known, but code emission must not treat it as a real edge.
* **Code reached only through computed addresses** (no relocation, no CFG entry, no direct
  reference) can't be found statically. Such addresses go into `game/config` as seeds.
* `.pdata` is trusted. If a fragment's unwind chain is malformed, the entry is kept as a
  function of its own and counted in `unwind_errors`.

## Results so far (build 1.0.2.1)

All sixteen top-level DLLs parse and run discovery with no unwind or directory errors. Across
them, 75 jump tables resolve, all bounded, with none rejected, and every target falls inside its
own function. Fifteen indirect jumps are tail calls through memory. Twenty-eight stay unresolved,
and each is a genuinely dynamic jump. Every DLL has one in its CFG dispatch stub. The rest are
helpers that jump to an address passed in an argument register, such as the exception-handling
continuation helpers, plus the delay-load helper's final jump. The same checks run on the main
executable through `ctest --preset windows-debug -L "^game$"` and `x64recomp functions <exe>`.

The main executable has 438,049 functions: 298,742 from `.pdata` (with 515 chained fragments)
and 139,307 leaves, 232 of them import thunks. 3,403 jump tables resolve with 39,849 entries;
234 of them are unbounded, and 14 more are rejected. 8,159 indirect jumps are tail calls through
memory, and 184 stay unresolved. The walk has no decode, unwind, or directory errors. The
executable has no CFG table, and code takes function addresses with `lea`, which needs no
relocation: 91,285 functions have their address taken that way, 56,009 of them `.pdata`
functions. 33,758 leaves are found by nothing else, and before the `lea` seed discovery found
403,884 functions. 3,170 leaves are cut where they run into another function's entry. Before
unbounded reads stopped at a byte index's range and at the next table, the scan found 23 decode
errors, all in code reached through entries read past the end of a table: one from a bytecode
interpreter's 256-entry opcode table, the rest from tables read into the next table of the same
function.
