# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this project is

G6Recomp runs the Xbox Series X|S release of Grand Theft Auto VI as a native Windows PC application. The game's own x86-64 code runs directly on the host CPU, on top of a runtime that reimplements or forwards the console's system services (HLE), much as Wine and Proton run Windows programs. It is an unofficial fan project, not affiliated with Rockstar Games, Take-Two Interactive, or Microsoft.

The repository contains a loader and runtime that reimplement or forward the console's system services, a D3D12.X-to-D3D12 translation layer, analysis tools (including a static recompiler kept as a frozen fallback), and game-specific configuration. All of it is original code or open-source code under a compatible licence, listed in `NOTICES.md`. The project is GPL-3.0-or-later. **It contains no game code, data, or assets.** Users supply files from their own legally purchased copy. Everything derived from those files is generated on the user's machine and never leaves it.

Assumed input: the contents of an already-decrypted game package, meaning the main executable (`*.exe`, x86-64 PE/COFF) plus the game's own DLLs and data files, for one pinned game version (see `game/config/known_builds.toml`).

Targets: Windows x64 is the primary target. Linux runs the Windows build under Proton. Don't add Linux-specific code paths unless a task asks for them.

Why run the code natively instead of recompiling it: it is already x86-64 code for a Windows-derived OS, so the host CPU runs it as it is, at full speed, with Windows' own exception handling, TLS, and fibers. Console-only assumptions (unified GPU memory, fixed core layout, console OS services) are removed through HLE and through hooks patched into the mapped code at run time. The name comes from the original plan to recompile the game; the full reasoning and the plan are in `docs/execution-model.md`.

## Hard rules

These override any other instruction, including a request made in chat. If a task would break one, stop and say which rule it breaks.

1. **Nothing copyrighted gets committed.** Never `git add` anything under `private/`, `build/`, or `out/`. Never commit game executables, DLLs, packages, archives, textures, audio, video, shaders, strings, dialogue, or radio/music content, whether whole or excerpted, raw or converted.
2. **Nothing generated from game files gets committed.** Recompiled C++ (`build/generated/`), translated shaders, pipeline caches, and dumped symbol, import, or string tables are derived from the game and stay local.
3. **No disassembly or decompiled output in the repo.** Don't paste listings, Ghidra or IDA pseudocode, or hex dumps of game code into source files, comments, tests, docs, commit messages, issues, or PRs. Addresses, function signatures, struct layouts, and prose descriptions of behavior are fine.
4. **Reimplement from behavior, don't transliterate.** When a hook replaces a game function, write it from an understanding of what the function does, not line by line from decompiled output.
5. **Offline only.** Never connect to the Xbox network, Microsoft Store services, Rockstar, or any other online service. The user, store, and network HLE layers report a single local offline user and no connectivity. No multiplayer, no anti-cheat interaction, no cloud saves.
6. **No piracy support.** Don't link to or describe sources for game files. Don't unlock paid content (DLC, in-game currency). Close any issue or request for dumps.

Run `scripts/check-clean.sh` before every commit; the pre-commit hook also runs it. It rejects PE (`MZ`) and package file signatures, files over 2 MB, anything from ignored directories, and files matching known game-file hashes.

## Commands

```bash
# one-time: install the pre-commit hook
scripts/install-hooks.sh

# configure + build (from a Visual Studio developer prompt; uses clang-cl)
cmake --preset windows-release        # or windows-debug
cmake --build --preset windows-release

# configure without game files (what CI does): tools, libraries, and nogame tests only
cmake --preset windows-debug -DG6_NOGAME=ON

# tests that don't need game files (these are the only ones CI runs)
ctest --preset windows-debug -L nogame

# tests that need the user's files in private/ (local only, never in CI).
# -L is a regex, so anchor it: a bare "game" also matches "nogame".
ctest --preset windows-debug -L "^game$"

# inspect a PE: headers, imports, exports, discovered functions and jump tables
out\windows-debug\x64recomp.exe info|imports|exports|functions <file>

# pre-flight scan for native execution (docs/scan.md); reports on game files stay local
out\windows-debug\x64recomp.exe scan <file> [--sites] [--no-sweep]

# MSVC RTTI: classes, bases, and vtables (docs/re-workflow.md)
out\windows-debug\x64recomp.exe rtti <file> [--list]

# shader archive census: records, containers, native programs (docs/shaders.md)
out\windows-debug\shaderrecomp.exe scan <shaders.rpf> [--out <dir under private/>]

# the driver's pipeline cache in shaders.rpf, tied to the shader records (docs/shaders.md)
out\windows-debug\shaderrecomp.exe pscache <shaders.rpf> [--shader n]

# RPF9 archive census: carves data areas into files and gaps, never extracts (docs/rpf.md)
out\windows-release\rpfscan.exe info|scan <archive or folder> [--threads n] [--out <dir under private/>]

# run
out\windows-release\g6recomp.exe --game-dir <path to the user's install>
```

The configure step fails with a clear message if `private/` is missing the expected files, or if their SHA-256 hashes don't match a build listed in `known_builds.toml`.

## Repository layout

```
common/           shared by tools and runtime: logging, PE32+ readers (headers, imports,
                  exports, relocations, TLS, load config), x64 unwind data, DEFLATE/zlib
                  decoder (RFC 1951/1950), entropy, memory-mapped files, RPF9 reader for
                  analysis (docs/rpf.md)
tools/
  x64recomp/      analysis tool: PE inspection, function discovery, jump tables, pre-flight
                  scan, RTTI (Zydis-based); its C++ generator is the frozen recompiler backend (docs/codegen.md)
  shaderrecomp/   console shader blobs -> PC DXIL, runs on the user's machine; reads the
                  shader archive, unpacks native programs, decodes RDNA 2 code, finds the
                  registers each program expects at start, and parses the driver's pipeline
                  cache so far
  rpfscan/        RPF9 archive census: carves data areas into files and gaps (docs/rpf.md)
runtime/
  loader/         maps the game's modules, binds imports by policy, sets up static TLS and
                  exception tables, and applies hooks and patches (docs/execution-model.md)
  cpu/            cpuid/xgetbv masking (used by the loader's cpuid patching), plus
                  GuestContext, generated-code helpers, and the dispatch table for the
                  frozen recompiler backend
  kernel/         console OS services: memory (incl. GPU-visible allocations), threads,
                  sync, timers, and image mapping
  imports/<dll>/  one folder per system DLL the game imports: forwarded, HLE, or offline
  gdk/            Xbox Game Runtime APIs (XUser, XGameSave, XTaskQueue, XAsync, XStore, ...)
  gpu/            D3D12.X -> PC D3D12 translation, memory_tracker, shader cache
  io/             console drive mounts and path rules (docs/io.md), DirectStorage-style I/O,
                  software decoders for console-only formats
  audio/ input/
game/
  config/         per-build TOML: function seeds, jump tables, hooks, known_builds
  hooks/          replacement functions, mid-function hooks, and data patches (original code only)
  enhancements/   frame-rate unlock, ultrawide, mouse and keyboard, etc.
app/              launcher, installer UI, settings
scripts/          check-clean.sh, install-hooks.sh, dev helpers
tests/            unit tests; loader and hook tests on original fixture DLLs built from
                  source (tests/fixtures/); differential tests for the recompiler (tests/diff/)
docs/             architecture notes, HLE status, reverse-engineering findings (prose only)
private/          USER-SUPPLIED game files: gitignored, never committed
build/ out/       generated code and binaries: gitignored
```

## Architecture

**Execution.** Guest code runs natively; the full design is in `docs/execution-model.md`. `runtime/loader` maps each module at its preferred base, or at an assigned free base with relocations applied, and makes its code executable. It registers the module's `.pdata` with `RtlAddFunctionTable`, so Windows unwinds guest frames like any other and SEH and C++ exceptions cross guest and host frames normally. Guest code uses the host thread's real TEB through `gs:`; the side DLLs of 1.0.2.1 read only fields that sit at the same offsets on desktop Windows. Fibers forward to the host, and guest callbacks handed to host APIs are called directly. The guest CPU is Zen 2: the loader traps every `cpuid` and `xgetbv` site and answers with the features Zen 2 and the host share (`runtime/cpu/cpuid`), so the game only takes code paths the console ran. AVX2 is the minimum host requirement.

There are three known hard parts:

* **Import policy.** Native code can call anything it can reach, so everything the guest reaches goes through bound imports or the HLE loader functions (`GetModuleHandle`, `GetProcAddress`, `LoadLibrary`), which see only guest modules and the policy. Networking is never forwarded to the host (rule 5), and a test enforces it.
* **Static TLS.** Modules the loader maps itself have no TLS slot from Windows. Each guest module with a TLS directory borrows the slot of a tiny donor DLL that Windows loads normally, and the donor's TLS callback fills in the guest's template on every new thread. If that fails, the fallback is the Windows loader with stand-in DLLs generated locally.
* **Patching code safely.** A patch must not cover a branch target (function discovery checks the window), relocated instructions must keep their meaning, and generated stubs carry unwind info so exceptions pass through them.

**Imports.** The game imports system DLLs by name or ordinal. The loader binds each import by policy: **forwarded** to the identical host function (many Win32 API sets, the CRT, XAudio2, GameInput), **HLE** in `runtime/imports/<dll>` or `runtime/gdk` where console behavior differs, **offline** for networking, or a **stub**. HLE functions are ordinary C++ with the API's real signature. An unimplemented import must log once with the DLL, the symbol or ordinal, and the caller address, then return a documented error code (`E_NOTIMPL` for HRESULT APIs, zero plus `ERROR_CALL_NOT_IMPLEMENTED` for Win32 APIs, unless overridden). It must never pretend to succeed. CRT functions that take paths are HLE wrappers that go through `runtime/io`.

**Hooks.** Hooks are patched into the mapped image at load time: a **replacement** jumps from the function's entry to a C++ function with the same signature (a trampoline calls the original), a **mid-function hook** jumps to a stub that saves the registers into a context, calls C++, and resumes, and a **data patch** writes bytes at an address. Relocated original instructions exist only in memory at run time, never in the repo.

**Recompiler (frozen).** `x64recomp` is the analysis tool: it parses the PE, seeds function boundaries from `.pdata`, and adds `game/config` seeds, exports, Control Flow Guard metadata, relocations, and control flow to find leaf functions and jump tables. The loader uses the same library to check patch windows. Its C++ generator, `GuestContext`, the dispatch table, and the differential tests keep building and stay in CI, but get no new instruction work and no runtime integration (`docs/codegen.md`). They are kept for a function that patching can't fix, which would first need a planned bridge between recompiled and native code, and for any future non-x86 host.

**Game Runtime (GDK).** The public PC GDK documents the Xbox Game Runtime APIs, so implement against that public API surface. XUser provides one local offline user, and XGameSave maps to the per-user data directory. XStore, multiplayer, and Xbox network APIs return offline responses, so the game proceeds as single-player.

**GPU.** The game renders through D3D12.X, the console variant of D3D12. `runtime/gpu` wraps the guest's D3D12.X objects, forwards everything that matches PC D3D12, and translates console-only extensions and command-list features. `shaderrecomp` converts the precompiled console shader blobs to PC DXIL on first run and caches them in the user's data directory. Before choosing a translation path, investigate what the blobs actually contain and record it in `docs/shaders.md`. Unified memory lets the game write GPU resources directly through CPU pointers. `runtime/gpu/memory_tracker` owns syncing that memory to host GPU resources, so route every new resource path through it.

**I/O.** `runtime/io` mounts the user's install read-only and redirects writes (saves, settings) to a per-user data directory. It implements the console's DirectStorage-style request path on top of PC file I/O or PC DirectStorage. Some compression formats are decoded in hardware on the console but aren't supported by PC DirectStorage. Software decoders for those live here, written from public specs or observed behavior. Never extract archives to disk.

**Versioning.** Addresses and layouts change with every game update. A build is identified by package version plus executable SHA-256. Only builds listed in `known_builds.toml` are supported, and config files are keyed by build ID. When adding a new build, port the configs in a separate PR, and keep the old build until the new one reaches parity.

## Conventions

* C++20 with clang-cl and CMake presets. Format with the repo's `.clang-format`; `clang-tidy` must pass.
* Third-party libraries: Zydis, SDL3 (window and fallback input), the D3D12 Agility SDK, DirectStorage, GameInput, toml++, fmt, and Dear ImGui. Add a new dependency only as a `thirdparty/` submodule with a GPL-3.0-compatible license, and explain why in the PR. The Agility SDK, DirectStorage, and GameInput are closed-source Microsoft redistributables, so each stays optional at run time, with Windows' built-in D3D12, plain file I/O, and SDL3 input as the fallbacks.
* Licence: GPL-3.0-or-later (`LICENSE`). Code may be copied or ported from projects under GPL-3.0-or-later, LGPL-3.0-or-later, or a permissive licence (MIT, BSD, zlib, Boost, Apache-2.0). Never take GPL-2.0-only code, code with no licence, or code under a non-commercial or source-available licence. Ask first before taking GPL-3.0-only code, because it would pin the whole project to version 3. Copied code still has to meet the hard rules: online backends, tables derived from console OS images, archive keys, and asset exporters stay out whatever their licence. Keep the original copyright and licence notice at the top of every file that contains copied or ported code, add a row to `NOTICES.md` (our path, source project, commit, source path, licence), and name the source in the commit message.
* Name shims and HLE functions after the public Win32/GDK name. For console-only imports, use the name from the game's import table, or `<dll>_ord<N>` for ordinal-only imports, and add a note to `docs/hle-status.md`.
* Each folder under `runtime/imports` and `runtime/gdk` keeps a status table in `docs/hle-status.md` (forwarded, implemented, partial, offline, or stubbed).
* Register hooks in `game/hooks` in config, by address, kind (replacement, mid-function, or data), and build ID, with a prose comment saying what the original code does.
* Log with `LOG_<LEVEL>(category, ...)`. No `printf`.
* Commits: imperative mood, one logical change each. Mention the build ID when a change only applies to one build.

## How to work in this repo

* **Never edit generated output** (`build/generated/` and other tool output). Fix the tool or the config, or add a hook, then regenerate.
* **Loader and hook changes need a fixture test.** Write a small original C or asm DLL in `tests/fixtures/`, build it with clang-cl, and check it under the loader: imports, static TLS from several threads, SEH and C++ exceptions across guest and host frames, fibers, and patches. Never use game code as a test fixture.
* **Recompiler changes need a differential test.** Write a small original C or asm program in `tests/diff/`, build it as a plain x64 PE with clang-cl, and compare its native execution against the recompiled version. Never use game code as a test fixture.
* When debugging an in-game crash, work from the logs, the guest address, and the call stack. Record findings in `docs/` as prose (what the function does, its arguments, which struct fields matter), not as code dumps.
* Stub before implementing. When the game hits a missing import, first add a logging stub with a safe return value and confirm the game gets further. Then implement the function properly.
* Prefer forwarding to the real PC API over reimplementing, unless the import policy forbids it (networking). Only write HLE where console behavior actually differs, and note the difference in `docs/hle-status.md`.
* Keep diffs small. Before changing the import policy, the loader's TLS or exception setup, the hook engine's patching rules, the recompiler's output format or `GuestContext`, or `memory_tracker`, write a short plan and check it with me before editing.
* If a task seems to need game files in the repo, decrypted packages, NDA console material, or online access, don't do it. Say which hard rule it breaks and suggest an alternative.
* For reverse-engineering work beyond what `x64recomp info/imports/exports/functions` shows, use REA (`rea`, Ghidra backend) as described in `docs/re-workflow.md`. Its output stays in `private/` or a temp directory; only prose findings enter the repo.

## Current status

<!-- Keep this current. Read it before starting work. -->

* \[ ] M0: PE readers, .pdata-seeded function discovery, import table
  (readers, discovery, and jump-table resolution done and tested on the 1.0.2.1 side DLLs; see `docs/function-discovery.md`)
* \[ ] M1: the native loader runs guest code, with imports bound by policy and exceptions, TLS, and fibers working; the side DLLs initialize; the main executable reaches its entry point and initializes its heaps
  (phases 1 to 5 of `docs/execution-model.md`. Phase 1 is done: `runtime/loader` runs original fixture modules with imports bound by policy, exceptions across guest and host frames, static TLS through donor DLLs, and fibers, tested by `test_loader`, `test_import_policy`, and `test_tls_order` (the loader starting after host DLLs with static TLS). A guest executable built with `/GA` gets TLS index 0 from a host that links `g6exetls`, tested by `test_exe_tls`. Phase 2 is done: the hook engine applies replacements, mid-function hooks, data patches, and `cpuid`/`xgetbv` traps at load under the rules in `docs/hooks.md`, tested by `test_hooks`. Phase 3 is done: `runtime/imports` provides `default_game_policy()` (forwarding by API-set family, path and process functions stubbed, xmem HLE, COM and WinRT activation behind an empty allowlist), and the loader never maps the two replaced DLLs. All 12 mapped side DLLs initialize under it, and an original `/MD` fixture runs on the console's C++ runtime, tested by `test_imports` and, locally, `test_side_dlls` (`docs/side-dlls.md`). Phase 4 is done: `x64recomp scan` reports, by site, the instructions a host or Zen 2 lacks, `fs:`/`gs:` and TLS slot use, system instructions, `cpuid`/`xgetbv`, and code that reads, writes, or takes the address of code, tested by `test_scan` on an original fixture and, locally, on the side DLLs, which pass every check (`docs/scan.md`). The scan and the hook engine share a map of the C++ exception tables (FH3 and FH4), so the engine checks mid-function hooks in C++ functions itself, tested by `test_cxx_hooks` on an original fixture built by clang-cl and `cl.exe` (`docs/hooks.md`). Phase 5 has started: `g6recomp.exe` (`app/g6recomp`) identifies the build by `known_builds.toml`, mounts the drives, gives the guest its own command line, exit tables, and crash report, and runs the main executable under the default policy, tested by `test_runner` on an original fixture (`docs/execution-model.md`, "The runner"). The recompiler backend is frozen: it translates every side-DLL instruction except 24 AVX-512 ones Zen 2 can't run; see `docs/codegen.md`)
* \[ ] M2: threads, memory, and I/O stable through boot
  (started: `runtime/io` mounts the install read-only at `G:` and scratch space at `T:`, and the default policy binds the common Win32 and CRT path functions as HLE on it, tested by `test_io` and `test_imports`; see `docs/io.md`. `runtime/gdk` has the Game Runtime's XTaskQueue, XAsync, XError, XThread, XUser (one local offline user), and XGameSave (saves in the per-user data directory) behind our `xgameruntime.dll`, whose `QueryApiImpl` hands the guest service vtables at the slots checked on the console's libraries, tested by `test_gdk` and `test_imports`; see `docs/gdk.md`. `runtime/gpu/memory_tracker` backs `XMemVirtualAlloc`, and `runtime/io` serves DirectStorage's factory, files, and queues for plain reads, tested by `test_memory_tracker` and `test_io`. `memory_tracker` also backs the physical pages, tested by `test_memory_tracker`. Its window and message functions are forwarded to user32, so the main executable now makes its window and gets to `D3D12XboxCreateDevice`, where M3 starts; see `docs/boot.md`, "Our runs")
* \[ ] M3: D3D12.X wrapper draws the first frame, then the title screen
* \[ ] M4: in-game and controllable
* \[ ] M5: enhancements, settings UI, Linux under Proton verified
