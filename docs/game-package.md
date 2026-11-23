# The game package

What a decrypted 1.0.2.1 package contains and what the project uses from it, described in
prose. File contents never enter the repo; see `CLAUDE.md`.

## Identity

The package's `MicrosoftGame.Config` names the main executable `game_scarlett_master.exe`, sets
title ID `7952987A`, reserves 2048 MB of persistent local storage, and enables SMT. The package
version is 1.0.2.1, which is the build ID used by `game/config`.

## Pieces

* **Main executable.** An x86-64 PE32+ image, which the loader maps and runs natively.
* **Side DLLs.** Sixteen plain PE32+ DLLs next to the executable: the MSVC runtime family
  (vcruntime, msvcp, concrt, vcomp, vccorlib), middleware (Bink 2, XCurl, libHttpClient for
  GDK, a BVH builder), and AMD FidelityFX loader and upscaler builds for the console. They
  import Windows API sets (`api-ms-win-*`) the same way PC binaries do. Their SHA-256 hashes are
  pinned in `game/config/known_builds.toml`. **Checked here:** `XCurl.dll` has file version
  2604.5.0.0, so the package was built with a 2026 GDK.
* **Archives.** Most game data lives in RPF archives (magic `9FPR`, the RPF9 format) at the top
  level and under `pack/`. Some loose files use a separate container with magic `RDS5`.
  `runtime/io` will read these in place; nothing is ever extracted to disk.
  * **Checked here:** there are 22 archives, all RPF9, 150.23 GiB in all. `rpfscan` carves
    their data areas into 2,127,667 zlib files, which inflate from 98.92 GiB to 339.95 GiB.
    The other 51.29 GiB is stored data and padding. See [rpf.md](rpf.md).
  * **Checked here:** the world textures are 57 loose `RDS5` texture pools (`.ttpf`), 46 under
    `data` and 11 under `pack`, 57.9 GiB in all.
  * **Reported** (Leonida): a pool's codec field is the public BCPack mode. BCPack covers 11.6
    GiB of the pools, almost all of it BC1, and what it decodes to is still tiled. Tile mode 0
    is a 64 KiB tile. The pools store each mip chain smallest mip first, which contradicts the
    "largest first" reported earlier ([related-work.md](related-work.md)).
  * **Reported** (Xbox2Windows): BCPack's BC1 blocks decode; BC7 still needs its tables.
  * **Checked here:** `shaders.rpf` keeps its shaders in a stored (uncompressed) entry, as
    records of raw DEFLATE; see [shaders.md](shaders.md).
  * **Reported** (Leonida, [related-work.md](related-work.md)):
    * Five archives have tables of contents that look packed or encrypted: `levels`, `movies`,
      `core_bvh4`, `navmeshes`, and `data`. The game's own code reads them when it runs, so
      the runtime needs no key.
    * Most archive data is 93–99% plain zlib. The exceptions are `audio.rpf`, which is raw, and
      about half of `story000` and `shared_minigame000`. **Checked here:** zlib covers 92–98%
      of the data areas of `anim`, `levels`, `navmeshes`, `core_bvh4`, `packs`, and `textures`,
      and 88% of `data`. It covers 0.1% of `audio`, none of `movies`, 34.5% of
      `story000`, and 54.8% of `shared_minigame000` ([rpf.md](rpf.md)).
    * The archives hold 797 Bink 2 videos. **Reported** (Leonida), by archive: 515 in `movies`,
      264 in `story000`, 7 in `story000_restofworld`, and 11 in `core_restofworld`. Its notes
      on `movies` alone give 515, which is where the earlier mismatch came from.
    * A container with the parts `PSIN`, `LSCH`, `LENM`, `LSNS`, `LGID`, `LRSC`, and `CHKS` is
      RAGE's reflection-schema format. It was first described as PSO metadata.
* **Audio.** **Reported:** `audio.rpf` holds AWC sound banks. Their streams are:
  * 1,601,167 Opus
  * 1,818 XMA2
  * 227 float PCM
  * 19 PCM16

  Opus packets are framed in 2048-byte blocks, each packet preceded by a 16-bit length. Desktop
  XAudio2 plays neither Opus nor XMA2, so `runtime/audio` will need software decoders for both.
  * **Reported** (Leonida): dialogue is 48 kHz mono Opus, and AWC tag 0x5c holds lip-sync data.
  * **Reported** (Xbox2Windows): all 1,342 format tags it read say 48 kHz and codec 11.
  * **Reported** (RageForge): a stream's own sample rate is the 16-bit word at +8 of its format
    chunk (tag 0x76). In a multichannel bank, the stream-format chunk (tag 0x48) holds a 16-byte
    record per channel from +12, with the channel's rate at +10 of the record. RageForge falls
    back to 48 kHz when a declared rate lies outside 8 to 96 kHz.
* **`gameos.xvd`.** The console OS image the title runs on. The project doesn't use it: the
  HLE runtime replaces the console OS.
* **Store and shell assets.** Logos, splash screen, `appxmanifest.xml`, `resources.pri`, and
  package layout and catalog files. Not needed at runtime.

## Open questions

* Which compression formats the RPF9 archives use, and which of them console hardware
  decompresses. This decides what software decoders `runtime/io` needs. The zlib share above
  is now checked. What the remaining 51 GiB of gaps hold isn't fully explained, though, and
  BCPack textures are only partly decoded elsewhere (BC1, not BC7).
* ~~What the shader blobs in `shaders.rpf` contain.~~ Answered and checked here: native
  machine code only, with no LLVM bitcode ([shaders.md](shaders.md)).
