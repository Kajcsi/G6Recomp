# Reverse-engineering workflow

How we look inside the game's binaries without putting any of it in the repository. Read the
hard rules in `CLAUDE.md` first; this page is about tools and habits, not exceptions to them.

## Tools

**x64recomp (ours).** The quickest way to answer structural questions about any PE:

```powershell
out\windows-debug\x64recomp.exe info      <pe>   # headers, sections with entropy, directories, TLS, load config
out\windows-debug\x64recomp.exe imports   <pe>   # regular and delay-load imports with IAT slots
out\windows-debug\x64recomp.exe exports   <pe>
out\windows-debug\x64recomp.exe functions <pe> [--list] [--tables] [--no-guard-cf] [--no-relocs] [--no-follow]
out\windows-debug\x64recomp.exe rtti      <pe> [--list]   # MSVC RTTI: classes, bases, vtables
```

Its output on game files is derived data: read it in the terminal, don't save it into the repo.

* **Section entropy** (`info`) is in bits per byte. The side DLLs' code sits between 5.3 and
  6.6. A code section at 7.5 or above is flagged: it's encrypted or compressed on disk, and
  nothing can read it until it's decrypted. Run `info` on the main executable before anything
  else.
* **RTTI** (`rtti`) reads the MSVC x64 type descriptors, complete object locators, class
  hierarchies, and base class descriptors, and finds the vtables that point at them.
  * It gives a binary without symbols a first map: class names, single, multiple, and virtual
    inheritance, and each vtable's address and slot count.
  * `--list` prints every class with its bases and vtables, undecorated by dbghelp.
  * Every locator in the 16 side DLLs reads cleanly. The local game suite holds that, and runs
    the same check on the main executable.
  * A vtable's slots are where hooks on virtual functions start.

**shaderrecomp (ours)** reads the console's shader archive:

```powershell
out\windows-debug\shaderrecomp.exe scan <install>\shaders.rpf [--out <dir>]
```

It finds every shader record, parses each container, unpacks its native programs, and prints a
census ([shaders.md](shaders.md)). `--out` writes each container, its `FSSM` native programs,
and an index. Those files come from the game, so give `--out` a folder under `private/` or the
temp directory.

**REA** ([morluto/rea](https://github.com/morluto/rea), npm package `rea-agents`, CLI `rea`)
for anything deeper: decompiling a function, cross-references, data types, or a second
opinion on what our own readers report. It drives Ghidra headlessly.

Setup on this machine:

* Ghidra 12.2 in `C:\tools\ghidra_12.2_PUBLIC`, Temurin JDK 25.
* Set `GHIDRA_INSTALL_DIR` and `JAVA_HOME` in the shell before calling `rea`; `rea doctor`
  should report the Ghidra backend as ready.
* `rea setup` registers REA as an MCP server by editing the configuration of every agent client
  it finds. Don't run it without the user's say-so.
* REA rejects inputs reached through junctions or other reparse points, so point it at the real
  install folder.

Useful commands (add `--json` for machine-readable output):

| Question | Command |
|---|---|
| Imports and delay-load imports | `rea inspect-pe-imports <pe>` |
| Exports | `rea inspect-native-exports <pe>` |
| TLS callbacks and load config | `rea inspect-pe-load-config <pe>` |
| What one function does | `rea function <pe> <address>` |
| Its API boundary (arguments, returns) | `rea inspect-native-api <pe> <address>` |
| Pseudocode for one function | `rea decompile <pe> <address>` |
| Callers and callees of a function | `rea procedure-references <pe> <address>` |
| What references a data address | `rea inspect-native-instruction <pe> <address>` |
| Strings or procedure names | `rea search <pe> <pattern>` |

## Where output goes

All REA output, Ghidra projects, notes with code in them, and anything else produced from game
files goes under `private/` (for example `private/rea/`), or the system temp directory. Both are
outside version control. Never write it into `docs/`, `game/`, tests, or commit messages.

## Turning findings into repo content

What may enter the repo is our *understanding*, written in our own words:

* **Prose** in `docs/`: what a function does, what its arguments mean, which struct fields
  matter, how a subsystem behaves. No pasted listings or pseudocode, not even a few lines.
* **Addresses, signatures, and struct layouts** in `game/config` and hook comments, keyed by
  build ID.
* **Hooks** in `game/hooks`, written from that prose understanding. Read the pseudocode, close
  it, then write the replacement from what you know the function must do, not line by line.

If a note can't be written without quoting code, it isn't ready for the repo yet.

## Cross-checks

Our PE readers were checked against REA on the 1.0.2.1 side DLLs (vcruntime140, msvcp140,
XCurl, libHttpClient.GDK): imports (module, delay-load flag, IAT slot, name or ordinal), exports
(ordinal, address, name, forwarder), load-config sizes, CFG and EH-continuation counts, and the
TLS directory all matched exactly. Repeat the comparison when the readers change or a new build
is added; keep the comparison output in `private/`.

Jump-table resolution was checked the same way. `rea function --json` reports Ghidra's switch
recovery under `normalized_result.native_api.jump_tables`: the dispatch address, the tables it
reads (with entry size and count), the case targets, and the default targets. Compare those
with `x64recomp functions <pe> --tables`. Ghidra lists entries that go to the default case
separately, so merge its case and default targets before comparing. See
`docs/function-discovery.md` for what matched.
