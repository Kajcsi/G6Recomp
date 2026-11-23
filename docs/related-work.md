# Related work

Other projects whose findings we follow. Before anything taken from one enters this repo, it
has to pass these checks:

* It meets the hard rules in `CLAUDE.md`.
* Findings are written in our own words.
* A design depends on it only after it has been checked against our own copy of the files.
  [shaders.md](shaders.md), [side-dlls.md](side-dlls.md), and
  [game-package.md](game-package.md) mark each finding as **checked here** or **reported**.
* Code is copied or ported only under a licence compatible with ours (GPL-3.0-or-later).
  Xbox2Windows is MIT, and Leonida and xbox-emulator are GPL-3.0-or-later, so their code
  qualifies. RageForge is LGPL-3.0-only, so we take only facts from it. xcl is GPL-2.0-only,
  which can't be combined with ours: we take facts and ideas from it and write our own code,
  never translating its. Copied code keeps its
  original notices and gets a row in [NOTICES.md](../NOTICES.md). The licence never overrides
  the hard rules, so the parts of these projects listed under "Not taken" stay out.

## Xbox2Windows

<https://github.com/R0Y00/Xbox2Windows>, MIT licence.

Research into running Xbox Series X|S software on x64 Windows, with the same GTA VI package as
its case study. Its tools are Python. Its tests are C++ and HLSL on source-built fixtures, and
it records measurements in `docs/experiments.md`.

It differs from us mainly in execution model. It statically translates selected guest
functions to C++, one at a time, and checks each against the original. We run guest code
natively. So its translators don't carry over, but its measurements of the package and of
desktop D3D12 do.

**Last reviewed: commit `8370c17` (2026-11-23).** No code has been copied. If code is ever
copied, its MIT notice goes with it and it gets a row in [NOTICES.md](../NOTICES.md).

### Taken

| Finding | Status here | Used in |
|---|---|---|
| The only side-DLL imports with no host provider are `xmem.dll` and `d3d12_xs.dll` | Checked: our own survey of the mapped side DLLs agrees (d3d12_xs is imported only by the two replaced DLLs) | [side-dlls.md](side-dlls.md) |
| Bink calls `XMemAlloc` and `XMemFree` with attribute word `0x05e50000` and aligns its own pointer | Checked: the word appears twice in Bink | [side-dlls.md](side-dlls.md) |
| The alignment exponent in that word is bits 24–28 (SlimEra's layout, below) | Reported; consistent with both observed words | [side-dlls.md](side-dlls.md) |
| The upscaler treats a failed `XMemGetWorkingSetStatistics` as zero | Reported | [side-dlls.md](side-dlls.md) |
| The BVH builder's `XMemVirtualQuery` callers check the state and protection fields where `MEMORY_BASIC_INFORMATION` has them, plus a console-only byte at `+0x30` | Reported | Not needed while the builder is replaced |
| Host `XAudio2_9.dll` ordinal 1 is `XAudio2Create` | Checked | [side-dlls.md](side-dlls.md) |
| Root signature flags `0x03000000` on all 52 BVH builder containers; none on the upscaler's 29 | Checked | [shaders.md](shaders.md) |
| Desktop D3D12 rejects those flags, and accepts the signatures once they are cleared | Reported | [shaders.md](shaders.md) |
| The upscaler has `ACCC` parts and no DXIL. It is FSR `4.1.1`, and the frame-generation DLL is missing from the package | No DXIL and the missing DLL checked; version reported | [shaders.md](shaders.md), [side-dlls.md](side-dlls.md) |
| The console FidelityFX backend descriptor has type `0x01000002`, with the device pointer at `+16`. The create descriptor's size fields sit where the public header puts them | Reported | [side-dlls.md](side-dlls.md) |
| The scene shader record format in `shaders.rpf` holds RDNA 2 (`gfx1013`) machine code | Format checked: `shaderrecomp scan` reads all 26,503 records and unpacks every native program. The ISA version is reported | [shaders.md](shaders.md) |
| An emulated 64-lane wave works on 32-lane hardware; native wave64 works on Radeon | Reported | [shaders.md](shaders.md) |
| Native 64-lane waves pass on five Radeon cards (RDNA 2 and RDNA 4). RDNA 2 defaults to 32 lanes and RDNA 4 to 64; NVIDIA reports a fixed 32-lane range | Reported | [shaders.md](shaders.md) |
| Desktop drivers differ on typed UAV stores to `R9G9B9E5_SHAREDEXP`: WARP accepts them, an NVIDIA driver (32.0.16.1656) doesn't | Reported | [shaders.md](shaders.md) |
| The upscaler provider creates 11 internal resources (12 with create flag `0x200`, a debug image) and keeps their indices in a 25-entry table | Reported | Not needed while the upscaler is replaced |
| BC7 textures are stored in GFX10 tiled layouts (`SW_4K_S`, a 4 KiB mip-tail tile). BCPack, the console's hardware texture compression, isn't decoded yet; current research points to an rANS entropy coder | Reported | Later: `runtime/io` and `runtime/gpu` (M2–M3) |
| When a DLL's entry point fails `DLL_PROCESS_ATTACH`, Windows 11 sends `DLL_PROCESS_DETACH` to its TLS callbacks and then its entry point, the same order as an unload | Checked: `test_loader` measures it with `LoadLibrary` on an original fixture. Our loader skipped the TLS callbacks there and now matches | [execution-model.md](execution-model.md) |
| Bink pads each allocation by 64 bytes, aligns its own pointer, frees with the original base, and falls back to `XMemAlloc` when an installed callback returns null | Reported | [side-dlls.md](side-dlls.md) |
| Where typed `R9G9B9E5_SHAREDEXP` UAV stores are missing, an `R32_UINT` view with packing in the shader matches the typed path bit for bit: clamp to [0, 65408], pick the shared exponent, round the 9-bit mantissas to nearest even | Reported | Later: `runtime/gpu` (M3) |
| DXIL module-only validation fails with DXC 1.7.2308 from Windows SDK 10.0.26100 (`0x80070057`) and passes with DXC 1.9.2609 | Reported | Later: `shaderrecomp` (M3) |
| FSSM word 0xC8 is the code's size in bytes: the code runs from word 0xC0 plus 0x28 to word 0xC0 plus word 0xC8, and ends in `s_code_end` padding. 6,189 pixel shaders have two to five `s_endpgm`, so the first one isn't the end | Checked here (structural scan): the range lies inside the image in all 26,698 programs, ends in `s_code_end` padding, and has `s_endpgm` right before it; the 6,189 count matches. No `s_endpgm` follows the range, so our earlier "last `s_endpgm`" rule found the same end | [shaders.md](shaders.md) |
| Where the stages find their resource tables: pixel shaders through s[8:9], mesh shaders through s17 and s[18:19], compute shaders through s[4:5] with entries 32 bytes apart. A stored vertex-shader group can hold a mesh shader (DXIL kind 13) | Reported | [shaders.md](shaders.md) |
| Typed `R9G9B9E5_SHAREDEXP` UAV stores work on an RDNA 2 integrated GPU, but round the mantissas toward zero: 317 of 1,024 test inputs give a different word from nearest even | Reported. With the two rows above, the fallback has to be chosen by measured result, not by reported support | Later: `runtime/gpu` (M3) |
| DXC 1.8.2502, shipped with a Windows SDK 10.0.26100 servicing update, is the first version whose module-only validation passes; `dxil.dll`'s file version tells which one is installed. It refuses a bare cast of infinity to an integer when the result reaches a UAV; mapping NaN to zero, truncating, and saturating at the endpoints passes | Reported. Narrows the DXC row above | Later: `shaderrecomp` (M3) |
| `v_fract_f32` clamps its result below 1.0. D3D's `frac` doesn't, and adapters differ: an NVIDIA driver returns 1.0 for -2^-30 where WARP returns the clamped value | Reported | [shaders.md](shaders.md) |
| Atomics on a UAV array indexed non-uniformly give four times the expected result on RDNA 3 and RDNA 4 and on Windows 10's WARP. Looping with `WaveReadLaneFirst` until each distinct index has been handled gives the right one | Reported | [shaders.md](shaders.md) |
| Zero-copy GPU access to title memory: `OpenExistingHeapFromAddress` needs the allocation's base, not a pointer into it, and the adapter has to report `D3D12_FEATURE_EXISTING_HEAPS` | Reported | Later: `memory_tracker` (M3) |
| `RtlCaptureStackBackTrace` stops at the first frame in a manually mapped image; walking with `RtlLookupFunctionEntry` and `RtlVirtualUnwind` over the registered tables gets through | Reported | Later: the crash log's stack walk |
| Windows 11's own loader reads section data in whole 0x200-byte file sectors, start rounded down and end rounded up, and leaves a section with `PointerToRawData` 0 zero-filled. It finds the TLS directory by address and ignores its size; with address 0 it runs no TLS callback, and a directory past the image fails with error 998. It calls neither the TLS callbacks nor the entry point of an image without `IMAGE_FILE_DLL`. An image with stripped relocations whose base is taken fails with error 487 | Reported. Checked here: every section of the 16 side DLLs is sector aligned, so our reader's exact copy of `SizeOfRawData` gives the same bytes. The main executable gets the same check at pre-flight | [execution-model.md](execution-model.md) |
| The Agility SDK can be deployed next to the executable on Windows 10 | Reported | Later: `app/` (M5) |
| The FidelityFX loader asks for the upscaler with descriptor type 0x10000 and for frame generation with 0x20000 and 0x30000, and calls `GetModuleHandle` before `LoadLibraryA` | Reported | [side-dlls.md](side-dlls.md) |
| vkd3d-proton accepts root signature flags `0x03000000`; Microsoft's D3D12 refuses each of the two bits on its own | Reported | [shaders.md](shaders.md) |
| All 1,342 AWC format tags they read say 48 kHz and codec 11 | Reported | [game-package.md](game-package.md) |
| BCPack: BC1 blocks decode; BC7 still needs its tables | Reported. Advances the BC7 row above | Later: `runtime/io` and `runtime/gpu` (M2–M3) |
| Pixel shaders' first unresolved read: a 16-byte descriptor load in 2,927 records, through s8, s9, or s10, most often at s9+0; `PSV0` doesn't give the original register or space | Reported | [shaders.md](shaders.md) |
| `v_cvt_u32_f32` truncates, maps NaN and negatives to 0, and saturates; `v_sin` and `v_cos` take turns; `v_cvt_f16_f32` rounds to nearest even and keeps the destination's high half | Reported, and for the conversions only an assumption: upstream now says that rule comes from an issue citing the ISA, not re-checked against it or measured on the console. It adds `v_cvt_i32_f32`: NaN gives 0, and values out of range saturate to 0x80000000 and 0x7FFFFFFF. Later still, upstream marks what any 16-bit result leaves in the high half as unmeasured, `v_cvt_f16_f32` included, and treats LLVM's rule as a hypothesis | [shaders.md](shaders.md) |
| The console's 32-bit denormal mode is unmeasured; their checks assume denormals are kept | Reported. Leonida reads them as flushed | [shaders.md](shaders.md) |
| A pixel shader exports binary16 rounded toward zero | Reported. Fits our scan: 4,776 pixel shaders switch f16 rounding to toward zero. The row below explains why | [shaders.md](shaders.md) |
| Round-toward-zero is there only for f16 interpolation: over 4,187 pixel records, the first MODE write that sets it comes before `v_interp_p1ll_f16` and `v_interp_p2_f16`, no other rounding binary16 instruction runs while it holds, and no other register or MODE field is written in any stage | Checked here with our decoder: the 4,776 programs are exactly those whose f16 interpolations read a high half and whose input table has a flag-bit-4 record; the rest agrees | [shaders.md](shaders.md) |
| 2,011 pixel records write `FLAT_SCR_LO` and `FLAT_SCR_HI` as their second and third instructions and contain no scratch or flat instruction | Checked here for every stage: none of the 12,741 programs that write the pair has such an instruction, so a translation drops the writes | [shaders.md](shaders.md) |
| Pixel shaders' descriptors traced over 4,411 records: how many each reads through `s_buffer_load`, how many entry registers each uses, and where the trace stops (1,553 at an entry SGPR nothing set, 582 at a 16-byte descriptor no load reads) | Reported | [shaders.md](shaders.md) |
| Interpolation on an RTX 3070 lands one or two f32 ulps low in 133 of 276 values, enough to change 17 pixels after rounding to f16 toward zero; WARP is exact | Reported | [shaders.md](shaders.md) |
| A comparison sampler's function is S# word 0 bits 14:12, in `D3D12_COMPARISON_FUNC` order minus one, and `TRUNC_COORD` is bit 27. A sampler used both for comparison and ordinary sampling needs two host samplers | Reported (RTX 3070, GTX 1060 and WARP). The split also runs at shader model 6.0, on the GTX 1060 and on in-box and app-local WARP | [shaders.md](shaders.md) |
| Host facts: Intel GPUs run 16-lane waves; WARP's width depends on its build, 4 lanes for Windows 10's in-box WARP and 8 for app-local WARP 1.0.22 (Agility SDK 1.620.2), so a test can't assume one WARP width; the `MinLod` weight is quantized to 1/256; WARP 10.0.26100.9818 removes the device for a mesh pipeline without a pixel shader; an RX 570 on vkd3d-proton has shader model 6.6; a GTX 1060 has DXR 1.0, and `DispatchRays` fixtures pass on it, and Windows 10 19041's WARP has none. DXC's GVN pass dominates compile time on sample-heavy shaders, and `-Od` cuts it by 84% or more | Reported | [shaders.md](shaders.md) |
| Windows refuses an image for raw data only when a section's raw data ends at or past 4 GiB or the last section's runs past the end of the file; other sections may lie past it and read as zero; a section with `SizeOfRawData` 0 isn't checked | Checked here on Windows 11 26200 with an original fixture, which also showed one rule they don't list (the last section at an unaligned offset fails). Our PE reader rejected a middle section past the end of the file and accepted a cut-off last one; it now follows Windows (`test_pe`) | [execution-model.md](execution-model.md), `common/src/pe/image.cpp` |
| A DLL with no import directory gets no TLS index, and an import table holding only its null entry is enough. A TLS template ending past the image fails with error 998, and one ending before it starts with 193 | Reported (Windows 10 19045). Our donors import a function, as the design requires | [execution-model.md](execution-model.md) |
| Their `D3D12XboxCreateDevice` shim refuses a parameter block and any interface but `ID3D12Device` | Reported as their assumption; Leonida reports a 24-word block | [d3d12x.md](d3d12x.md) |
| Some AWC banks are big-endian, and format tag 0xFA keeps its codec byte at +19 (from vgmstream) | Reported | [game-package.md](game-package.md) |
| Their census now matches ours: 21,522 cache shaders, 26,503 records, 25,889 digests | Reported; agrees with our counts | [shaders.md](shaders.md) |
| An export forwarder whose target DLL name is in upper case resolves to the same provider as in lower case | Checked here by reading: our loader lowercases the DLL part of a forwarder before the lookup. No fixture exercises a guest forwarder yet, and the 16 side DLLs export none | `runtime/loader` |
| NVIDIA drivers reject some shaders compiled with `-denorm preserve`: pipeline creation returns `E_OUTOFMEMORY` on an RTX 5080 (32.0.16.2101) and an RTX 5060 Ti (32.0.16.2124), where WARP accepts them | Reported. Matters when `shaderrecomp` picks a denormal mode | Later: `shaderrecomp` (M3) |
| DXIL has no binary16 denormal setting: DXC's `-denorm` sets only the 32-bit mode. Computing packed f16 in f32 and narrowing in integers gives the same results under either mode | Reported. The console's f16 denormal mode is still unmeasured | Later: `shaderrecomp` (M3) |
| An unguarded HLSL cast of NaN to an integer gives 0x80000000 on WARP and an RTX 3080; replacing NaN and clamping first passes on both | Reported. Narrows the DXC rows above | Later: `shaderrecomp` (M3) |
| SPIRV-Cross (at `57e4efc`) won't convert SPIR-V that uses physical addresses to HLSL, and that is the form Leonida's translator emits. Upstream rewrites such accesses as bounded storage-buffer accesses, sending bad addresses to offset 0xFFFFFFFC so that D3D12's bounds checks turn them into zero reads and dropped writes. The bounded window also runs at shader model 6.0 with `Int64ShaderOps`, on a GTX 1060 and on in-box and app-local WARP | Reported. Rules out reaching DXIL from Leonida's SPIR-V through SPIRV-Cross as it stands | Later: `shaderrecomp` (M3) |
| `v_madak_f32` and `v_madmk_f32` aren't fused: like `v_mad_f32`, they round the product to binary32 before the add and flush binary32 denormals. A draw tells the two apart on an RTX 3080 and on WARP | Reported. Whether the console flushes in these forms isn't measured | Later: `shaderrecomp` (M3) |
| Integer semantics checked by draws: the 24-bit multiplies use bits 23:0 (sign-extended in the `i24` forms), and their `hi` forms return bits 47:32 of the product; `v_subrev_nc_u32` subtracts the first source from the second and wraps; `s_lshl1_add_u32` to `s_lshl4_add_u32` set SCC on a carry out of bit 31, bits shifted out included; `s_bfm_b32` masks its count and offset to five bits and leaves SCC alone | Reported. Each agrees with AMD's RDNA 2 reference | Later: `shaderrecomp` (M3) |
| When an executable's entry point returns, Windows 11 doesn't end the process: a loader thread-pool worker can keep it alive for about 30 s, and the exit code then becomes 0. A startup wrapper that passes the entry's result to `ExitProcess` fixes it | Reported; upstream calls the cause a hypothesis. For phase 5: when the main executable's entry returns, our host has to end the process with that result itself | Later: `runtime/loader` (M1) |
| Without mesh shaders, compute dispatches can write each triangle into its own range of one buffer, and after one UAV-to-SRV transition vertex draws read the ranges in order into shared colour and depth targets. Depth order holds: a GTX 1060 and Windows 10's in-box WARP matched all 1,024 colour and 1,024 depth pixels with the order reversed and the depths swapped, and the stale-target controls caught their errors | Reported. A fallback for hosts without mesh shaders, for the 9,282 mesh programs | [shaders.md](shaders.md); later `runtime/gpu` (M3) |
| Refitting a DXR acceleration structure needs `ALLOW_UPDATE` on the build as well as `PERFORM_UPDATE` on the update: without it a GTX 1060 (32.0.15.6094) removed the device. With both, an in-place update matched a fresh full build there and on app-local WARP 1.0.22 | Reported | [shaders.md](shaders.md); later `runtime/gpu` (M3), the DXR stand-in for the BVH builder |
| DXR tier 1.0 runs `DispatchRays` (`lib_6_3`) but not inline `RayQuery`, which needs tier 1.1 and shader model 6.5. Tier 1.0 has no geometry index, so it goes in each geometry's hit-group record as a local root constant. Facing is decided in object space, so a mirrored instance doesn't flip it | Reported (GTX 1060). Translating the 64 ray-traced compute shaders to inline `RayQuery` would leave tier 1.0 hosts out | [shaders.md](shaders.md) |
| On an RX 9070 XT (driver 32.0.32015.7029), `CommittedRayT()` came back as the true distance plus TMin for 43 of 126 rays | Reported (an earlier upstream pull request, missed by the review before) | Later: `runtime/gpu` (M3) |
| Translation rules drawn bit-exact on an RTX 3060 or 4060 and WARP, from AMD's reference, LLVM, or Mesa: `v_bfe_u32`, `v_bfe_i32` and `v_bfm_b32` take the low five bits of offset and width, so a width of 32 is 0; `v_ffbh`/`v_ffbl` keep the all-ones result for no bit found; `v_cvt_pk_u16_u32` and `v_cvt_pk_i16_i32` put src0 in the low half and saturate (the saturation from Mesa, the ISA leaving it open); `v_sad_hi_u8`, `v_sad_u32` and `v_msad_u8` are unsigned and wrap at 2^32, and `v_msad_u8` skips byte lanes whose src1 byte is zero; `v_add_co_u32` and the subtracting forms take their carry rules from LLVM; `image_sample` with a partial `dmask` packs the enabled channels into consecutive VGPRs in RGBA order; `v_readfirstlane_b32` becomes `WaveReadLaneFirst` only for a source proven uniform | Reported, none measured on the console. Checked against our name tables: the packing conversions, the SADs, `v_bfe` and `v_add_co_u32` are VOP3-only opcodes there, as upstream says gfx10 has them | Later: `shaderrecomp` (M3) |
| Windows 10's in-box WARP (10.0.19041.5794) picks mip 1 for a per-sample `MinLod` clamp of 1.5625 under point mip filtering, where `SampleLevel` at the same LOD picks mip 2; linear mip filtering blends as expected | Reported, on that one adapter: a host difference, not a claim about the console | [shaders.md](shaders.md) |

### Not taken

* **The static translators and their generated C++,** including the new x87, SSE2 to SSE4,
  AES-NI, PCLMULQDQ and FMA3 lowering. We run guest code natively. Our frozen recompiler
  covers the cases patching can't.
* **Package verification and provenance material.** Hard rule 6.
* **Script archive and camera-state probes.** Game logic data. Not needed before M4.
* **The FSR4 render-graph traces** (passes 24 to 26 and RCAS). The upscaler is replaced by the
  public FidelityFX SDK at M3, so its internal passes never run here.
* **The standalone GPU check.** It's for collecting community hardware reports. Our tests run
  on fixtures in CI.
* **Its new RDNA 2 name-table entries.** The dot-product opcodes (VOP3P 0x13 to 0x19, VOP2
  `v_dot2c_f32_f16` and `v_dot4c_i32_i8`) appear in no scene program, and Leonida reports
  gfx1013 lacks `v_dot4`. We'd add them from AMD's reference if a program needs them. It
  also names VOP1 0x34
  `v_frexp_mant_f32`, where AMD's reference has `v_sqrt_f64` (our table agrees) and puts
  `v_frexp_mant_f32` at 0x40.
* **Code for `shaderrecomp` (M3)**, not copied yet: the quad DPP lowering through
  `QuadReadAcross*`, outside conditions; packed-f16 narrowing in integers, which can follow the
  round-toward-zero mode 4,776 pixel shaders set where `f32tof16` can't; and the NaN-safe,
  saturating float-to-int helpers.
* **Code for `runtime/gpu` (M3)**, not copied yet: `comparison_sampler_state()`, which maps the
  S# compare and `TRUNC_COORD` bits to a D3D12 sampler, and `title_va_lookup.hlsli`, a lookup of
  a guest address in a sorted range table, for `memory_tracker`. Its decoder fixes (64-bit
  VOP3 and DS operands, GFX10 `s_waitcnt` fields, `v_fma_mix` modifiers, DPP and SDWA second
  words) are worth comparing with ours.
* **The BCPack rANS decoder** (Python, vendored from bcpack-rs). Before taking it, find where
  its "MIT-licensed model tables" come from: tables derived from the console's software stay
  out (hard rule 2).
* **The TTPF batch DDS exporter,** and the Rust BCPack kernels added for it. An asset exporter,
  so it stays out whatever its licence.

### Next review

1. Run `git log 8370c17..origin/main` in a scratch clone outside the repo.
2. Read the diff of `docs/experiments.md` and `README.md`.
3. Add each new finding to the table above with its status here, check it when a design
   depends on it, and move the "last reviewed" commit.

## Leonida

<https://github.com/ItsNotRexed/Leonida>, GPL-3.0-or-later licence.

Groundwork for running the same 1.0.2.1 package natively on Windows. It has Python analysis
tools, a small C++ loader, and a translator from the console's RDNA 2 machine code to Vulkan
SPIR-V, tested on a GPU with hand-written test programs. It also exports the game's assets
(models, textures, audio) to files with tools built on rpf9-explorer.

Its execution model matches ours. It differs in two ways: it targets Vulkan where we target
D3D12, and it extracts assets where we read archives in place. Its licence matches ours, so its
code can be ported with its notices kept. Little of it fits as it is, though: its tools are
Python and its translator emits SPIR-V. We take its measurements as facts, check them on our
copy, and port code only where it fits our design.

**Last reviewed: commit `3faac557` (2026-11-23).** No code has been copied yet.

### Taken

| Finding | Status here | Used in |
|---|---|---|
| Its 108-file package manifest | Checked: same file names and sizes as our copy, and the side-DLL SHA-256 hashes match `known_builds.toml` | [game-package.md](game-package.md) |
| All 22 archives are RPF9 | Checked: 22 `.rpf` files, all with magic `9FPR` | [game-package.md](game-package.md) |
| Five archives have tables that look packed or encrypted; most archive data is 93–99% zlib, but `audio.rpf` and about half of two others aren't | Reported | [game-package.md](game-package.md) |
| World textures are 57 loose `RDS5` pools (`.ttpf`), not inside archives | Checked: 57 pools, 46 under `data` and 11 under `pack`, 57.9 GiB | [game-package.md](game-package.md) |
| 26,503 scene shaders: 2,236 vertex, 12,101 pixel, 2,689 compute, 9,282 mesh, 195 geometry, each with its native program unpacked from an `FSSM` image | Checked by our own reader, `shaderrecomp scan`, and the local `test_shader_census`. We also found that no container carries LLVM bitcode, and that 195 vertex shaders carry two native programs | [shaders.md](shaders.md) |
| Disassembly as `gfx1013`, and where the code starts in an `FSSM` image | Reported | [shaders.md](shaders.md) |
| Most shaders are wave64; mesh shaders are NGG wave64; geometry shaders are legacy wave32 | Reported, then corrected upstream: from FSSM word 0xD4 it now counts 23,841 wave64 and 2,662 wave32 records, all 195 geometry programs wave64. That agrees with our 24,036 and 2,662 programs (the 195 second programs make the difference) | [shaders.md](shaders.md) |
| Vertex fetch, pixel and compute system inputs, `COMPUTE_PGM_RSRC2` at FSSM word 0x138, and descriptors read through user SGPRs | Reported | [shaders.md](shaders.md) |
| POPS in 10 pixel shaders; hardware BVH intersection in 64 compute shaders, emulated the way RADV does in software | Reported | [shaders.md](shaders.md) |
| `audio.rpf` holds AWC banks: 1,601,167 Opus streams, 1,818 XMA2, 227 float PCM, 19 PCM16; Opus is framed as 2048-byte blocks of a 16-bit length and a packet | Reported | [game-package.md](game-package.md); later `runtime/audio` (M4) |
| Texture headers give a GPU pointer, size, DXGI format, and mip count; the data is `SW_4K_S` tiled, largest mip first | Reported; agrees with Xbox2Windows. Leonida's later notes say the pools store the smallest mip first (below), so the order is open | Later: `runtime/gpu` (M3) |
| 797 Bink 2 videos inside the archives; a new PSO metadata container (`PSIN`, `LSCH`, `LENM`, `LSNS`, `LGID`, `LRSC`, `CHKS`) | Reported. Its later count by archive reconciles the 797 with the 515 below | [game-package.md](game-package.md) |
| Section entropy flags an encrypted or compressed executable on the first day | Ported as our own code: `x64recomp info` prints each section's entropy | [re-workflow.md](re-workflow.md) |
| An RTTI scan names the classes, bases, and vtables of a binary without symbols | Ported as our own code: `x64recomp rtti`, tested on an original fixture and clean on all 16 side DLLs | [re-workflow.md](re-workflow.md) |
| Console command lists record their commands inline, into a buffer inside the list object, and their method slots are shifted from desktop D3D12: `SetDescriptorHeaps` is slot 32 (28 on desktop), `ExecuteIndirect` 61 (59), and slots 85, 141, and 146 exist only on the console | Inline records checked on the BVH builder: every one first checks for room and otherwise calls the method, so a list object with no room avoids the risk that game code writes a buffer we can't see. The main executable needs the same check. The builder's slots fit `SetDescriptorHeaps` at 32: desktop compute root slots 29 to 41 sit four higher. Slot 146 takes the enhanced `Barrier` arguments. The builder never calls `ExecuteIndirect`, but the upscaler settles it at 61 (below) | [d3d12x.md](d3d12x.md); later `runtime/gpu` (M3) |
| The device's descriptor-heap start-handle getters return the handle in RAX. Device slot 76 creates console textures; slot 67 creates a native compute pipeline and falls back to slot 62. `D3D12XboxCreateDevice` takes a 24-word parameter block | Reported | Later: `runtime/gpu` (M3) |
| `core_bvh4.rpf` holds about 272,000 prebuilt bottom-level acceleration structures (64-byte nodes, version 6) and no source geometry | Reported. A risk for replacing the BVH builder with desktop DXR builds, which need geometry. Later upstream: the structures are dehydrated, holding primitive references whose triangles come from `levels.rpf` drawables, and the rehydrate method runs on the CPU, which weakens the risk | [shaders.md](shaders.md) |
| Mesh shaders' v5 holds the group index times the thread count plus the lane | Reported. Leonida now reads v5 as xbox-emulator does | [shaders.md](shaders.md) |
| FSSM words 0x13C to 0x144 hold the thread-group size in 2,281 of 2,689 compute shaders | Checked here: in all 2,689 compute records the three words equal `PSV0`'s thread counts | [shaders.md](shaders.md) |
| 21,545 of the 26,503 shaders run 64 lanes, by broader markers than EXEC use | Reported, then retracted upstream for the 0xD4 count in the wave-size row above. Our EXEC word scan counts 20,634, a lower bound | [shaders.md](shaders.md) |
| The two-program vertex shaders are a merged export stage plus an ordinary vertex program | Reported. Supports our reading without proving it | [shaders.md](shaders.md) |
| Texture pools: the `RDS5` codec field is the public BCPack mode; BCPack covers 11.6 of 57.7 GiB, almost all BC1, and its output is still tiled; tile mode 0 is a 64 KiB tile; mips are stored smallest first | Reported | [game-package.md](game-package.md) |
| Translator facts: 1,473 shaders read constants from their own code; IEEE mode is off and denormals are flushed; some shaders truncate descriptor pointers to 32 bits, so shader-visible heaps must sit in the low 4 GiB of GPU addresses; large root signatures spill to memory; ray-tracing compute shaders compile slowly enough to need background compilation | Reported. The IEEE and denormal modes are an inference upstream, not a value read from the images: testing 88 FSSM words as `PGM_RSRC1` gave no result, and 25 positions survive its VGPR check. The words next to our known `PGM_RSRC2` words are the cheap next test | [shaders.md](shaders.md) |
| `SFI0` flags: 685 shaders use 64-bit atomics, 30 tiled resources, 10 rasterizer-ordered views | Checked here: the same three counts, the 64-bit atomics all on typed resources. 18,227 records also set descriptor-heap indexing | [shaders.md](shaders.md) |
| The BVH builder's batched build descriptor layout, and 53 shader containers | Layout reported. We count 52 containers. Settled upstream: the container for `GetBVHCompactedSize` is created twice, at the 47th and 53rd creations | [shaders.md](shaders.md) |
| More FSSM header words, and a table of 20-byte records | Reported | Later: `shaderrecomp` |
| GameInput's v0 interface differs from v1 and later, so a title's version has to be checked | Reported | Later: `runtime/input` (M4) |
| 33 GDK functions in HRESULT-returning families return something else (for example `XTaskQueueDispatch`, `XUserCloseHandle`, `XStoreIsLicenseValid`) | Reported. Our "unimplemented returns `E_NOTIMPL`" rule has to follow each function's real return type | [hle-status.md](hle-status.md) |
| Root signature version 1.2 isn't in the in-box D3D12 of Windows 10 or Windows 11 23H2 | Reported | Later: `runtime/gpu` (M3) |
| The console presents through a command-queue method, not DXGI | Reported; a lead | Later: `runtime/gpu` (M3) |
| `PSIN` is RAGE's reflection-schema format | Reported. Replaces our "PSO metadata" | [game-package.md](game-package.md) |
| 515 Bink 2 streams, whose decode calls return 0; dialogue is 48 kHz mono Opus; AWC tag 0x5c holds lip-sync data | Reported. The 515 are the streams in `movies`; the 797 count every archive (below) | [game-package.md](game-package.md) |
| A pixel shader's FSSM image has 8-byte input records from 0x138 while the first byte is 6; bits 0 and 1 of byte 7 give each position's read method (`v_interp_mov` or `p1`/`p2`), not its interpolation mode | Checked here, with a correction: the word at 0x238 counts the records (xcl), and 32 of them are of kind 7, where a first-byte rule stops. 55,937 records, 1,784 tables empty. Exactly one of the two bits is set in every record, bit 1 in 51,327 and bit 0 in 4,610. Which instruction reads each position is reported | [shaders.md](shaders.md) |
| Every interpolated pixel input is perspective-correct at the pixel center, by `PSV0`; the position's w holds 1/w | Reported | [shaders.md](shaders.md) |
| The only MODE field the programs write is the f16/f64 rounding field | Checked here (word scan): the only MODE field `s_setreg_imm32_b32` sets. Pixel shaders set it to toward zero (4,776) or nearest even; other stages only to nearest even. 19 words could be `s_setreg_b32` of the f32 field | [shaders.md](shaders.md) |
| 16-bit ALU results keep the high half on GFX10; narrowing to f16 is done in integers because AMD drivers lower `PackHalf2x16` to round toward zero; on a gfx1031 GPU the f16 transcendentals are correctly rounded and `v_sin`/`v_cos` take turns | Reported. The high-half rule is an assumption (Xbox2Windows marks it unmeasured); the one measured case is an SDWA `dst_sel:DWORD` result, which zeroes the high half. xbox-emulator's gfx1036 models refine the transcendentals: correctly rounded except about 20 sin and cos inputs | [shaders.md](shaders.md) |
| A NaN stored through a normalized buffer format becomes zero | Reported | [shaders.md](shaders.md) |
| `Barrier` (list slot 146) is called 29 times in 7 builder functions, 18 of them through a register | Checked here: 11 direct calls and 18 through a register, in 7 functions. Our earlier 11 counted only the direct ones | [d3d12x.md](d3d12x.md) |
| List slots 71 and 73 are probably a GPU label write and a masked-compare wait, behind a flag nothing sets, so the shipping builder never calls them | Checked here: all 7 calls of each sit behind two zero flag bytes that no builder code writes, takes the address of, or points at. What the slots do is reported | [d3d12x.md](d3d12x.md) |
| List slot 59 is a console-only GPU-event method whose first argument is a bool, so it isn't `ExecuteIndirect` | Checked here on the upscaler: slot 61 gets `ExecuteIndirect`'s arguments and slot 59 a flag, a pointer and a size | [d3d12x.md](d3d12x.md) |
| A byte at device +0x18 chooses an inline 32-byte descriptor copy over device slot 24 (`CopyDescriptorsSimple`) | Checked here on the upscaler, which also takes the handle increment from slot 15 as on desktop | [d3d12x.md](d3d12x.md); later `runtime/gpu` (M3) |
| List slot 148 is an upload allocator (40 bytes per element, 16-byte aligned), and its CPU pointer is passed unchanged as a root SRV's GPU address. Slot 80 is a dispatch with header word 0x8020 | Reported. The first means CPU and GPU addresses of such memory must match | Later: `runtime/gpu` and `memory_tracker` (M3) |
| In the upscaler, list slots 54 and 55 are `ClearUnorderedAccessViewFloat` and `DiscardResource`, and 85 has the shape of `CopyTextureRegion`. Device slot 67 is `DeserializeComputePipelineStateX` with slot 62 as a fallback, and pipeline-state slot 11 is a console-only scratch-size query | Reported | Later: `runtime/gpu` (M3) |
| An RX 7900 XTX faults on the builder's console DXIL too; only WARP accepts all 53 pipelines | Reported | Not needed while the builder is replaced |
| A user-SGPR census: root arguments start at s8 for pixel and vertex programs, s4 for compute, s16 for mesh, and s8 (64-bit) for geometry; pointers are 32-bit | Reported. **Conflicts** with Xbox2Windows's s17 for mesh shaders; open | [shaders.md](shaders.md) |
| No FSSM word holds `SPI_PS_INPUT_ENA` | Not taken: its search stopped at 0x138, and the word is at 0x248 (checked here) | [shaders.md](shaders.md) |
| Flags byte 7 of the pixel input records: bit 3 marks an f16-typed input (11,619 shaders), bit 4 use of `*_f16` opcodes (11,199) | Partly checked: we count records, not shaders. Bit 3 is set in 11,659 records and bit 4 in 11,239 | [shaders.md](shaders.md) |
| 207 pixel programs overwrite v0 or v1 before interpolating, so I and J have to be read from the registers the instruction names | Reported. Fits our check, which reads the register each interpolation names | [shaders.md](shaders.md) |
| The f16 high-half bit appears in 4,776 pixel programs, which pack two inputs per 32-bit channel; MODE writes number 9,417 in 6,333 shaders | Reported. The MODE counts don't line up with ours yet | [shaders.md](shaders.md) |
| Mesh programs: s3 holds the wave ID shifted left by 24. Cube coordinates: s and t in [1, 2], z = 8 × slice + face | Reported | [shaders.md](shaders.md) |
| On an RDNA 2 integrated GPU: partial-format stores fill missing channels with zero and unsigned stores saturate; loads repeat missing channels as c mod n; `v_pack_b32_f16` applies neg to each half. `ds_cmpstore_rtn_b32` takes its operands in a different order on GFX11 | Reported | Later: `shaderrecomp` (M3) |
| AMD's MIT-licensed ISA XML lists 1,153 RDNA 2 instructions and leaves out `v_mac_f32`, `v_mad_f32`, `v_mad_legacy_f32`, `v_madak_f32`, `v_madmk_f32`, and `s_get_waveid_in_workgroup` | Reported. Our decoder names the four gfx10.1 opcodes xcl reports | Later: `shaderrecomp` name tables |
| The upscaler's FSPX container has kinds 0, 1, 6 and 7; kind 6 uses `v_dot4`, so it targets gfx1030 | Reported | Not needed while the upscaler is replaced |
| BVH4 nodes: the four triangle slots per leaf and a remap byte at leaf +60 follow AMD's GPURT (MIT); bottom-level addresses are stored in 256-byte units. The XDXR layout comes from a public DagorEngine shader | Reported. The DagorEngine file may itself derive from console SDK material, so nothing is taken from it until that's settled | [shaders.md](shaders.md), "Ray tracing" |
| libHttpClient and XCurl load `xgameruntime.dll` at run time and look up five entry points (`InitializeApiImpl*`, `QueryApiImpl`). An interface-GUID census dates the GDK (`IXUserImpl5`, `IXPackageImpl3`) | Reported. The entry-point signatures come from a GPL-3.0 project that reverse engineered the PC runtime; its provenance has to be checked before we rely on it. Corrected upstream: libHttpClient references 23 of the 24 classes (not `XGameActivationImpl`), and the IDs come from thunks an incremental link kept, not from calls. The GDK dating stands; reading the list as the classes used doesn't | Later: `runtime/gdk` |
| A `GAME:\` path prefix (reported by xcl) | Reported; whether this build's own code uses it isn't known. We mount the install at `G:`. Leonida now serves `GAME:`, `G:` and `T:` and says only the executable can tell | [io.md](io.md), [execution-model.md](execution-model.md) pre-flight |
| Bink 2 streams by archive: 515 in `movies`, 264 in `story000`, 7 in `story000_restofworld`, 11 in `core_restofworld`, 797 in all | Reported. Settles the 797 against 515 question | [game-package.md](game-package.md) |
| Swizzles: `SW_64KB_D` at 4 bytes per pixel swaps address bits 6 and 7 compared with Navi 1x (167 of 167 validated); `SW_4KB_S` validated at 1, 2, 4 and 8 bytes per pixel | Reported | Later: `runtime/gpu` (M3) |
| AWC Opus: the sample count is in 48 kHz frames even at 32 kHz, +0x0C holds frames per channel, the 312-sample priming isn't stored, and 2 banks have no streams | Reported | Later: `runtime/audio` (M4) |
| Besides its bitcode, each of the BVH builder's 52 containers carries an `XDAT`/`XBCH` record that unpacks to an `FSSM` image, and that is what the console runs; 42 of the 52 programs translate so far. The builder makes 53 pipelines from the 52 containers | Reported. Settles the 52 against 53 row above: one container is used twice. 51 of the 52 now translate upstream. Whether our `FSSM` reader takes the 52 isn't checked yet | [shaders.md](shaders.md) |
| No GTA VI shader uses append or consume counters: no `PSV0` binding is a structured UAV with a counter, and no program has `ds_append`, `ds_consume`, or another GDS instruction. 5,359 shaders declare typed buffers | Checked for the code: no program reaches `ds_append`, `ds_consume`, `ds_ordered_count`, a GWS op, or a DS instruction with the gds bit. The driver's pipeline cache has 30 compute shaders with `ds_append` or `ds_consume` on LDS. The `PSV0` part is reported | [shaders.md](shaders.md) |
| RDNA 2 measurements: an output modifier on an f16 result acts only with IEEE mode off and f16 denormals flushed, and does nothing otherwise; `v_fma_mix*` rounds once; `v_cvt_flr_i32_f32` and `v_cvt_rpi_i32_f32` saturate a NaN by its sign instead of giving 0; the VOP3 clamp bit saturates the carry-out adds and subtracts too; a d16 load of a normalized format rounds the exact value once to f16 | Reported, from raw kernels on gfx1030 to gfx1036. Which f16 denormal mode GTA VI's programs run in decides the first, and it is still open | Later: `shaderrecomp` (M3) |
| In `ds_permute_b32` only active lanes send, the highest active source lane wins a collision, an active lane nobody targets reads 0, inactive lanes keep their value, and the two halves of a 64-lane wave stay apart | Reported, measured on gfx1030 | Later: `shaderrecomp` (M3) |
| A 32-lane pixel shader run 64 lanes wide reads the other half's lanes, which broke a pixel shader on RDNA 4, where pixel shaders default to 64 | Reported (Vulkan). D3D12 can't ask for a pixel shader's wave size, so a 32-lane pixel program has to be translated to work at either width | Later: `shaderrecomp` (M3) |
| Its shim serves `GetCommandLineA/W` and the UCRT's WinMain command-line helpers, so the game sees its own command line rather than the host's | Reported. No rule here names those functions yet; phase 5 has to decide what the main executable sees. Later upstream: the command line has to be set before the process-attach callbacks run, and the argument functions read the environment when called | [execution-model.md](execution-model.md) phase 5; later `runtime/imports` (M1) |
| A title reaches a Game Runtime service by `QueryApiImpl`, then calls one vtable slot with `this` and the function's own arguments, then `Release` (slot 2); `0x80040154` from `QueryApiImpl` is handled specially | Reported, with the provenance caveat of the `QueryApiImpl` row above | Later: `runtime/gdk` |
| How the interpolation unit lays out its parameters (48 bytes per attribute from M0), and what `v_interp_p1ll_f16`, `p1lv_f16` and `p2_f16` compute, `p2_f16` rounding once by MODE and keeping the high half | Reported, measured on gfx1035 and gfx1036 with parameters a compute wave wrote | [shaders.md](shaders.md) |
| The VOP3 f16 interpolations carry the attribute in bits 0–5 of the first source, the channel in 6–7 and `high` in bit 8; 4,776 programs use `high`, 4,731 of them reading both halves of a channel | Checked here: decoding bit 8 finds the same 4,776 programs, exactly those that set f16 rounding toward zero (the Xbox2Windows row above). The 4,731 is reported | [shaders.md](shaders.md) |
| Its input-signature join now matches by component-mask overlap and row span, since several elements can share a register row and one element can span rows | Reported. A lead for the records whose byte-1 register has fewer components than the record | Later: `shaderrecomp` |
| FSSM word 0xFC isn't a render-target count (5 or 6 in some programs, 16 or 18 in others); the count comes from `OSG1` | Reported | [shaders.md](shaders.md) |
| The upscaler's root signatures give its user SGPRs: 4 + one per table + two per root CBV, in signature order, with s0 to s3 a driver prefix and tables as 32-bit pointers from s4 | Reported, from the `RTS0` parts next to `COMPUTE_PGM_RSRC2` | [shaders.md](shaders.md) |
| 99.86% of 770,412 descriptor loads take their base from a wave-start SGPR used as a 32-bit pointer; resource and sampler heaps have separate bases; T# offsets are multiples of 32 and S# offsets of 16 | Reported. Consistent with our register model | [shaders.md](shaders.md) |
| Opcodes that exist only on gfx1013, against gfx103x (`v_mad`/`v_mac` and `_legacy`, `v_mul_lo_i32` as VOP3 0x16B, scalar stores, atomics and scratch, `s_dcache_wb`, the `ds_*_src2` forms), and VOP2 0x06 and VOP3 0x106 and 0x140 decoding differently on gfx103x | Reported (llvm-mc sweep). Our names agree with xbox-emulator's gfx1013 table (below) | [shaders.md](shaders.md) |
| `v_mad_f32`, `v_mac_f32` and `v_mad_legacy_f32` flush denormals whatever MODE says, before and after the output modifier, on gfx1010; 9,297 shaders use them | Reported | [shaders.md](shaders.md) |
| Buffer bounds follow Mesa's GFX11 column per `OOB_SELECT`, soffset included in raw mode (gfx1036 and gfx1011); the point mip filter rounds floor(λ+0.5) after `MIN_LOD` clamps (Xbox2Windows measured Windows 10's in-box WARP not doing so for a per-sample clamp, so that WARP is no reference there); helper lanes: EXEC holds covered pixels, quad lane order (x&1)+2(y&1), helpers hold their own I/J; `v_cvt_pknorm_*` rounds once; d16 integer loads saturate; `image_get_resinfo` gives 2D depth 0 | Reported | [shaders.md](shaders.md) |
| XCurl can load the PC's `gameplatformservices.dll` from System32, after reading a `GamingServices` registry value, and reach the Game Runtime through it | Checked here: `XCurl.dll` holds the names. The DLL is in the loader's networking deny list | [side-dlls.md](side-dlls.md), `runtime/loader` |
| Wine 9 has no `CopyFile2` and binds an import of it to a stub that ends the process | Reported. Our HLE looks it up at run time and falls back to `CopyFileExW` | [hle-status.md](hle-status.md) |
| `VirtualAlloc` with the console's `MEM_GRAPHICS` (0x10000000) or `MEM_TITLE` (0x40000000) fails on desktop with `ERROR_INVALID_PARAMETER` | Checked here on Windows 11 26200. The bit values are reported | [hle-status.md](hle-status.md) |
| The executable probably reaches the Game Runtime only through `xgameruntime.lib` thunks; XCurl has 16, all called, and libHttpClient 404, of which it calls 15. Two interface IDs are missing from the public layouts, and the Wine IDL drops the documentation's "unknown" slots | Reported; the first is an assumption | [hle-status.md](hle-status.md) |
| More real return types (`XErrorSetOptions`, `XSystemAllowFullDownloadBandwidth` void; `XSystemIsHandleValid` bool; `XErrorReport` HRESULT); `XThreadingImpl` slot 8 fails; `XStoreGameLicense` would be 104 bytes | Reported; the `XStore` sizes are unverified | [hle-status.md](hle-status.md) |
| The executable's `atexit` handlers land in the host CRT's process-wide table; Leonida gives the guest its own and follows the native `exit`, `_cexit`, `_exit` and `quick_exit` rules | Taken, our own code from the rules measured here: the guest has its own atexit and at-quick-exit tables and thread_local callback, which Leonida leaves to the host CRT. A second callback registration fast-fails on the host UCRT | [execution-model.md](execution-model.md) phase 5, `runtime/imports/ucrtbase` |
| Persistent local storage is 2048 MB in `MicrosoftGame.Config` | Checked here | [game-package.md](game-package.md), [io.md](io.md) |
| `Bink2Scarlett.dll` decodes all 515 movies' first frames and all 229 audio tracks under a native loader with only xmem shimmed; `BinkSetFileOffset` with open flag 0x20 plays from `movies.rpf` in place | Reported | [side-dlls.md](side-dlls.md) |
| `data.rpf` has 377 unread non-zero ranges (4.2%); streams aren't always 4 KiB-aligned; a resource's system part ends at a stream whose inflated size is a multiple of 64 KiB, plus at most one more; `core_bvh4.rpf` has 43 streams of exactly 8 MiB | Reported | [rpf.md](rpf.md) |
| Host `GameInput.dll` can be bound from mapped code (GameInput 0.2309); v0's `GetTemporalReading` returns `E_NOTIMPL` by design; a title's raw mouse registration replaces any of our own | Reported | Later: `runtime/input` (M4) |

### Not taken

* **The asset exporters, the viewer, and the rpf9-explorer dump tool,** and the new animation,
  vertex-stream, world-placement, and place-name decoders. They read or write the game's
  assets. Our runtime reads archives in place and never extracts them, and the game's own code
  decodes its assets when it runs.
* **Recovering the archive table-of-contents key from the executable.** We don't need it. The
  game's own code reads its archives when it runs natively, and a key is game data that can't
  enter the repo anyway (hard rules 1 and 2).
* **The hash dictionary, the generated SPIR-V, and its metadata index.** All three are derived
  from the game's files (hard rule 2).
* **The Vulkan translator and loader code.** We target DXIL on D3D12, and we have our own
  loader in `runtime/loader`. Its
  measurements of the console side are in the table above. Its Vulkan-only work, such as
  shared-exponent storage images and predicated buffer copies, doesn't apply to a D3D12 host.
* **Build.bat's install detection.** Later, in `app/` (M5).
* **Name recovery and save tools:** Bink, AWC and `PSIN` names, save keys, text tables,
  subtitles, and Opus tags. They are derived from the game's files (hard rules 1 and 2).
* **Its XCurl and libHttpClient networking classes.** Hard rule 5.
* **Code for later milestones**, not ported yet: the frame pacer (M5); the D3D12.X device
  shim's guard-page vtables and thunks (M3); the user-SGPR census, ISA coverage and wave64
  tools for `shaderrecomp`; the GPU instruction oracle; and the XDXR BVH builder, once its
  provenance is settled.
* **Its Game Runtime shims** (`runtime/shims/xgameruntime*.cpp`, about 3,700 lines: threading,
  errors, offline networking, system, package, storage, saves as files, one local user, a store
  with the game licence only, and offline stubs), for `runtime/gdk`. They fit when we build it,
  with three changes: saves go to our data directory, a save update is atomic for the whole
  container (theirs renames blob by blob), and **`LEONIDA_CONTENT_PACKAGES`, which mounts
  arbitrary content packages, stays out** (hard rule 6). Also for later: Microsoft's MIT
  XTaskQueue and XAsync, vendored unmodified from libHttpClient; its startup trace and "stop at
  the first unimplemented import" switch, as ideas for phase 5; and its static scan of Game
  Runtime thunks, whose class table comes from xgameruntime-docs, so that licence is checked
  first. Its offline WinHTTP layer loads the host's `winhttp.dll` for URL parsing, so it would
  need our own parser.
* **Shader tools for later:** the HIP launcher that runs raw gfx10 kernels as a hardware oracle,
  the interpolation probes' exact models and 30 f16 tie vectors (test vectors for our
  lowering), the heap-index and root-signature census scripts, and the D3D12 conformance
  renderer (M3).
* **Its notes on game content and mods:** the world-prop viewer, archetype swaps, first-person
  view, the music stem locations, and the models, animation, story and world notes (hard rule
  1). `d3d12_xs_surface.md` rests on another project's copy of a console vendor DLL, which is
  console OS material.
* **meetox80/6stars**, which its related-projects page lists, has no licence, so nothing is
  taken from it.

### Next review

1. Run `git log 3faac557..origin/main` in a scratch clone outside the repo.
2. Read the diff of `docs/` and `README.md`.
3. Add each new finding to the table above with its status here, check it when a design
   depends on it, and move the "last reviewed" commit.

## xbox-emulator

<https://github.com/omonkeyz/xbox-emulator>, GPL-3.0-or-later since commit `51f5319`
(2026-11-22), which a history rewrite the same day renamed `ceb2b87`; earlier releases stay
under MIT or Apache-2.0. It continues [TomBox](https://github.com/drycheetah84/TomBox).

A general runtime, written in Rust, for running dumped Xbox Series X|S and Xbox One titles as
Windows processes:
* It replaces the console's system DLLs with its own.
* It translates D3D12.X to desktop D3D12.
* It translates the precompiled RDNA 2 shaders to HLSL and compiles them with DXC.

Fortnite reaches its lobby. GTA VI is listed as not run, though its shaders were examined.

Its execution model is close to ours: guest code runs natively, and the console's libraries are
replaced. It differs in scope: it targets any title, online ones included. Its licence now
matches ours, so its code can be ported with its notices kept. Code from before `51f5319` can
also be taken under its earlier MIT licence. It is Rust where ours is C++, so taking code means
porting it. Its RDNA 2 decoder (`crates/tombox-xshader`) is now ported to `shaderrecomp`; most
of it is TomBox's code, which we use under MIT ("TomBox", below). The
remaining candidates, both for M3, are the HLSL side of that translator and parts of its D3D12.X
layer (`crates/compat/d3d12_xs`) for `runtime/gpu`. The method map and other tables it measured
on console OS binaries stay out (see "Not taken").

**Last reviewed: commit `de1e1da` (2026-11-23).** Upstream rewrote its history on 2026-11-22:
`51f5319` is `ceb2b87` on the new `main`, the two trees differing in one redaction line of a
publishing script.

### Taken

| Finding | Status here | Used in |
|---|---|---|
| Code: the RDNA 2 decoder in `crates/tombox-xshader` (`rdna2.rs` and its name tables, `rdna2/mnem.rs`) | Ported to C++ at `51f5319` and extended: VOP3P as its own encoding, VOP3B carry-outs, DPP and SDWA words, more opcode names, and a control-flow decode. Checked here: it decodes every reachable instruction of all 26,698 programs, and `test_rdna2` checks it on hand-built encodings | `tools/shaderrecomp/src/rdna2*`, [shaders.md](shaders.md), [NOTICES.md](../NOTICES.md) |
| No GTA VI shader container has a root signature part, so a shader's resource bindings are unknown until the game creates its root signature | Checked: none of the 26,503 containers has `RTS0` | [shaders.md](shaders.md) |
| 8,765 GTA VI mesh shaders | Checked: they are the 8,765 distinct containers among 9,282 mesh records, and they hold 8,291 distinct native programs. `shaderrecomp scan` now counts distinct containers and programs per stage, and the local `test_shader_census` holds the numbers | [shaders.md](shaders.md) |
| The console's mesh-shader conventions: what v5 and s3 hold, the `GS_ALLOC_REQ` message and its m0 layout, export targets 12 to 15 and 20, and how primitive indices are packed | Partly checked: every distinct mesh program sends `GS_ALLOC_REQ` and exports a position and a primitive. Register contents and bit layouts are reported. Leonida reads v5 differently | [shaders.md](shaders.md) |
| Root signature flag `0x01000000` forces memory-based root arguments, read through s[0:1]; the console requires it with amplification shaders, and WARP rejects it | Reported. The BVH builder's root signatures set it | [shaders.md](shaders.md) |
| Translating through HLSL and DXC. About a third of a 4,000-shader GTA VI vertex sample translates, and mesh translation stops at the first resource load when there's no root signature | Reported. Its later notes count verified stages at 0 of 8 and call the 105,506-shader figure a DXC compile sweep, not checked output. At `6e81ffc` it has an optional root-signature inference and a user-data constant-buffer fallback for shaders without one, not run on GTA VI | [shaders.md](shaders.md) |
| Every way of looking up a module or function has to be routed, including `GetProcAddressForCaller`, `LoadPackagedLibrary`, and ntdll's `Ldr*` lookups | Checked against our policy: an import without a rule becomes a logged stub, so none of them reaches the host. No side DLL imports them | [hle-status.md](hle-status.md) |
| The console's `XMemAllocatePhysicalPages` enforces pool limits, and `XMemAlloc` carves page-pool allocations out of 512 KiB or 2 MiB granules (4 MiB on an older console OS) | Reported | Later: `runtime/imports/xmem`, once the main executable imports more than `XMemAlloc` and `XMemFree` |
| A title reaches the Game Runtime by loading `xgameruntime.dll` by name, looking up `QueryApiImpl`, and calling services by interface ID and vtable slot | Partly checked: `XCurl.dll` and libHttpClient contain the strings `XGameRuntime`, `XGameRuntimeInitialize`, and `QueryApiImpl`. Our loader fails `LoadLibrary` for an unknown DLL, so `runtime/gdk` will need its own `xgameruntime.dll` behind that call | [hle-status.md](hle-status.md) |
| `XTaskQueue` behaves like libHttpClient's open-source (MIT) task queue | Reported | Later: `runtime/gdk` |
| Keeping title memory in sync: a write-back shadow heap for buffers costs 0.01–0.09 ms per execute; textures move to GPU-local memory with `UpdateTileMappings`; a heap opened over existing memory pins it, so `VirtualFree` fails while the heap is open; the console packs buffers tightly where desktop placement needs 64 KiB and 4 MiB alignment | Reported | Later: `memory_tracker` (M3) |
| Root signature flag names: `0x02000000` a spill area for compute, `0x03000000` internal to XDXR (the DXR layer), `0x80000000` discarded by the driver | Reported. Fits all 52 BVH builder root signatures setting `0x03000000` | [shaders.md](shaders.md) |
| RDNA 2 features with no D3D12 lowering: `ds_gws_*` and `ds_ordered_count`. GDS `ds_append` and `ds_consume` need their own lowering, and so do SDWA negate and absolute-value modifiers | Reported. For the GDS part, moot in 1.0.2.1: our decoder finds no GDS, GWS, `ds_ordered_count`, `ds_append`, or `ds_consume` in its programs | [shaders.md](shaders.md) |
| The console GPU's timestamp frequency is 99.75 MHz | Reported | Later: `runtime/gpu` (M3) |
| A title partition of 13.5 GiB | Reported. We forward the system-information functions, so a guest sees the host's totals | Later: `runtime/kernel` |
| The `XSystem` ID strings are 39, 16, and 45 bytes, and a smaller buffer gets nothing written | Reported | Later: `runtime/gdk` |
| `XUserFindUserByLocalId` returns `E_GAMEUSER_USER_NOT_FOUND` on the console, where the documentation says `ERROR_NOT_FOUND` | Reported | [hle-status.md](hle-status.md) |
| `d3d12_xs` device parameters change with the console OS: 0x40 bytes at version 0x20002, 0x48 bytes at version 0x20004 | Reported. Checked here: `XCurl.dll` is version 2604.5.0.0, so the package was built with a 2026 GDK. Our wrapper will read the block by its version field | Later: `runtime/gpu` (M3) |
| Path checks should be fuzzed with look-alike colons and slashes | Taken as a test: `test_io` checks that fullwidth, division, and fraction look-alikes stay ordinary name characters | [io.md](io.md) |
| `XMemGetWin32HeapStatistics` on the process heap returns allocated bytes, then committed minus allocated, and a title subtracts their sum from its memory budget | Reported. No side DLL imports it. At `6e81ffc` its figure counts only the title's own allocations | [hle-status.md](hle-status.md) |
| The console accepts constant buffer views at 16-byte aligned addresses, where desktop D3D12 needs 256; root constant buffers can be bound as raw buffer views instead | Reported | [shaders.md](shaders.md) |
| Console texture layouts include metadata planes that desktop sizes leave out: HTILE before depth and stencil for a single-mip depth texture, CMASK and FMASK for render targets. `GetResourceAllocationInfo` has to report the console's sizes, and `Map` works on a default-heap texture | Reported | Later: `runtime/gpu` and `memory_tracker` (M3) |
| Vertex format 189 (`R10G10B10_SNORM_A2_UNORM`) is console-only and can be fetched as `R10G10B10A2_UNORM` with the signs restored in the shader | Reported | [shaders.md](shaders.md) |
| Emulating f16 in f32 made one compute shader take 84 of 105 ms per frame; native 16-bit types fixed it | Reported | [shaders.md](shaders.md) |
| Translated pipelines take seconds to compile; a placeholder pipeline and worker threads hide it, and the driver's part recurs without a pipeline library | Reported | [shaders.md](shaders.md) |
| A typed buffer's element type has to come from the descriptor's numeric format, not from how the shader uses the values | Reported | [shaders.md](shaders.md) |
| BCPack BC1: the rANS entropy layer decodes exactly, checked against the silicon-rans tables and bcpack-rs's reference hashes. The block layer is fitted from samples, and chunks that use its unobserved or uncertain parts are refused, not guessed. No public mapping exists for BC2 to BC7 | Reported. Advances the BCPack rows above; GTA VI's BCPack pools are almost all BC1. We'd take bcpack-rs and silicon-rans (both MIT) from upstream | Later: `runtime/io` (M2–M3) |
| DXC packs signature elements into one register only when their interpolation modes match, so a pixel input's non-default modifier (centroid, `nointerpolation`) has to be repeated on the output that feeds it, or pipeline creation fails with a linkage error | Reported, tested on WARP. Applies to GTA VI's flat inputs | Later: `shaderrecomp` (M3) |
| The driver picks a shader's wave size, and AMD drivers can run pixel and compute shaders at 64 lanes, so two of the console's 32-lane waves can share one host wave. Lane-numbered operations (readlane, writelane, DPP row masks, EXECZ and VCCZ, GDS append counters) then have to address the guest wave's own half. Measured lane counts: RTX 5090 32, Intel UHD 770 16, WARP 4 | Reported; not yet run on 64-lane hardware. Matters for GTA VI's 32-lane programs | Later: `shaderrecomp` (M3) |
| NVIDIA (RTX 5090): `floor`, `ceil` and `trunc` return 0x7FFFFFFF for NaN and flush denormal inputs even with denormals preserved; `log2` of a power of two can be 1 ulp off; a 16-bit clamp followed by a 0xFFFF mask becomes a saturating convert without the mask; `ldexp` chains with denormal results are reassociated to 0; pipeline creation fails with `E_OUTOFMEMORY` on f16 unordered compares behind an integer NaN guard and on `InterlockedAdd` of a negated operand. On an RTX 3080, an f16 FMA's NaN result comes back as 0x7FFF | Reported | Later: `shaderrecomp` (M3) |
| The RTX 5090 quantises texture LOD to 1/256 steps, where WARP keeps float precision | Reported | Later: `shaderrecomp` (M3) |
| A 1- or 2-byte load that fits in one dword must not read the next dword, which can lie outside the mapped GPU window | Reported, tested | Later: `shaderrecomp` (M3) |
| A desktop texture can need more memory than the console's layout of it (a 2x MSAA depth texture, for one). Placed in a shared heap it then overlaps the title's next placement, so it gets its own heap covering only the title's range | Reported, tested on hardware. The console size in their test came from console SDK code, so the figure isn't taken. At `6e81ffc`, an MSAA texture at an offset that isn't a multiple of 4 MiB becomes a reserved resource tile-mapped onto the same heap range | Later: `memory_tracker` (M3) |
| Mesh shaders: the group dimensions must multiply to between 1 and 128, checked for overflow; amplification arguments need a strided zeroing loop when a group has fewer than 4 threads; a program that opens with a constant m0 and `GS_ALLOC_REQ`, with no branch after it, can keep its output counts per invocation | Reported, tested. How many GTA VI mesh programs open that way isn't counted yet. At `6e81ffc`, groups of 129 to 256 threads run as two 128-thread passes; GTA VI's mesh groups are 128 threads by `PSV0`, so that part doesn't apply | [shaders.md](shaders.md) |
| `XGameSaveReadBlobDataResult`'s buffer is the array of 24-byte blob records followed by the data; `XPersistentLocalStoragePromptUserForSpace` completes only with `S_OK` or `E_ABORT` | Reported; both follow from the public GDK | Later: `runtime/gdk` |
| A hardware vertex shader gets the vertex index in v0 and the instance index in v3, below the fetched attributes from v4 | Checked here: the FSSM word at 0x2BC loads v0 to v3 in exactly the 494 VS programs that need the instance ID, and every VS program reads only VGPRs it loads | [shaders.md](shaders.md) |
| Fetched attributes are packed by the elements the shader reads: each read element takes the next four VGPRs from v4, an unread one takes none, and missing components default to 0, 0, 0, 1 | Checked here for the packing: all 946 programs that call the fetch subroutine (788 VS, 158 ES) read exactly where it puts the elements their input signatures mark as read, from v4 in a VS and v9 in an ES. Our earlier 4 + 4*r* note fails in the 12 VS programs whose unread element precedes a read one, and is corrected. The default components aren't checked | [shaders.md](shaders.md) |
| d16 typed-buffer conversions measured on RDNA 2 (gfx1036): normalized and scaled loads round once to nearest-even f16 and saturate at ±65504, and f32 loads round toward zero; stores round unorm to nearest even and snorm ties away from zero, and write the whole element with missing components 0 | Reported. Agrees with Leonida's single rounding | Later: `shaderrecomp` (M3) |
| `ds_gws_init`, `ds_gws_sema_v` and `ds_gws_sema_br` lower to a counter in memory for straight-line 32-lane compute | Reported. **Contradicts** its earlier "no D3D12 counterpart" for GWS (above). Neither Leonida nor our decoder finds a GDS or GWS instruction in GTA VI's programs | [shaders.md](shaders.md) |
| The D3D12.X layer has to copy a pipeline description's semantic-name strings and hold a reference on its root signature, because a title can free both once the pipeline exists (a use-after-free in Fortnite's derived pipelines) | Reported | Later: `runtime/gpu` (M3) |
| Which interpolation attribute is which: attribute k is the k-th input register holding a value that isn't a system value, with layer and viewport registers after them; a channel with no element reads 0, w reads 1.0 | Reported, and a guess that still fails on one Trackmania shader. Byte 1 of our input records names the register, so we don't need it. Whether our kind-7 and kind-8 records hold the layer or viewport is unchecked | [shaders.md](shaders.md) |
| A vertex stage puts the layer in bits 10:0 of the z dword of position export 13, and the viewport index in bits 19:16 | Reported, from register documentation, tested only on a shader it wrote | [shaders.md](shaders.md) |
| The SMEM offset is a signed 21-bit value; its gfx1013 opcode table, built from LLVM's MC tests and AMD's MIT ISA XML | Checked here against our decoder, which read the offset unsigned, gave VOP3 names to 8 opcodes that exist only as VOP2 or VOP1, and named 8 MIMG slots as `gather4` `_d` forms that don't exist. Fixed in `2c8ae5b`; no census program uses an affected name. No code taken | `tools/shaderrecomp/src/rdna2*` |
| f16 inline constants are the f16 value in the low half and zero in the high half, read as f32 bits by `v_pack_b32_f16` and `v_cvt_f32_f16`; OPSEL on a constant reads it again; 1/2π is 0x3118; packed int16 inline float constants are the 32-bit word | Reported, measured on an RX 6800 by Leonida | [shaders.md](shaders.md) |
| Probes on gfx1036 (RDNA 2): NaN propagation with IEEE mode off, min and max by IEEE mode, `v_med3_f32` with a NaN operand, `s_bfe`'s 7-bit width and sign fill, norm conversions round to nearest with NaN as 0, the fused f16 FMAs (VOP2 0x36 to 0x38) return a signalling NaN unquieted | Reported | [shaders.md](shaders.md) |
| Transcendentals bit-exact on gfx1036: f16 correctly rounded apart from about 20 sin and cos inputs; f32 `rcp`, `rsq` and `sqrt` faithful from a recovered datapath; f32 `log`, `exp`, `sin` and `cos` match AMD's rocjitsu models (MIT) | Reported. Its README and a commit message disagree on the exception count (20 against 18). Refines the gfx1031 row in Leonida's table | [shaders.md](shaders.md) |
| A compute group of one wave may leave out `s_barrier`, so LDS accesses have to keep their order | Reported. Not counted in our programs yet | [shaders.md](shaders.md) |
| `image_gather4_l` lowers to four `SampleLevel` taps, which ignore the sampler's bias and LOD clamp | Reported, tested on WARP | [shaders.md](shaders.md) |
| 64-bit image atomics lower to `RWTexture<uint64_t>` on `R32G32_UINT`, gated on `AtomicInt64OnTypedResourceSupported` (OPTIONS9) and shader model 6.6 | Reported. 685 of our records use 64-bit atomics (checked) | [shaders.md](shaders.md) |
| Desktop D3D12 refuses a mesh pipeline whose primitive topology is missing or undefined, so it has to come from `PSV0`'s mesh information | Reported, tested on a PC | [shaders.md](shaders.md) |
| The console compiler merges root loads into 8- and 16-dword loads that run past the last parameter. Its root-signature inference starts geometry user SGPRs at s22 | Reported. **Conflicts:** Leonida and our register model put them from s8 | [shaders.md](shaders.md) |
| Write-watch: views of physical pages or file mappings can't carry it, and on its Windows build the first read of a page never touched marks it dirty. fp16 and `R11G11B10_FLOAT` blending saturates a finite overflow | Reported, on WARP and its GPU | [d3d12x.md](d3d12x.md), [shaders.md](shaders.md) |
| `XGameSave` calls on a closed handle return `E_GS_HANDLE_EXPIRED` (0x8083000D); it doesn't serve slots 30 to 32 (`XGameSaveFiles*`) | Reported | [hle-status.md](hle-status.md) |
| `XAsync` rules from conformance tests derived from libHttpClient's and also run against the PC Game Runtime: `XAsyncBegin` with no queue and no process queue fails with `E_NO_TASK_QUEUE` (0x800701AB), and on a terminating queue with `E_ABORT`; when the provider's begin step fails, `XAsyncBegin` still returns `S_OK` and completes the call with that failure, and `E_PENDING` from it keeps the call pending; a second `XAsyncSchedule` before the worker has run, and a work step that returns success without completing, give `E_UNEXPECTED`; `XAsyncRun`'s work callback result becomes the call's; `XAsyncGetResult` with a null buffer gives `E_INVALIDARG`, checked before the size, and too small a size `E_NOT_SUFFICIENT_BUFFER` (0x8007007A); `XAsyncGetResultSize` on a pending or failed call returns that status and writes 0 | Reported. Agrees with xcl's rows on the same contracts | Later: `runtime/gdk` |
| The d16 buffer loads write half a VGPR and keep the other: `buffer_load_ubyte_d16`, `_sbyte_d16` and `_short_d16` (MUBUF 0x20, 0x22, 0x24) the low half, their `_hi` forms (0x21, 0x23, 0x25) the high half. `buffer_store_byte_d16_hi` and `buffer_store_short_d16_hi` (0x19, 0x1B) store from the high half | Taken: the MUBUF and FLAT tables name them (the opcode numbers are AMD's RDNA 2 reference), and the register model counts the kept half as read for every d16 load that writes half a VGPR, the LDS `ds_read_*_d16[_hi]` ones included. No 1.0.2.1 program uses the buffer or flat forms; four of its instructions are `ds_read_u16_d16_hi` | `tools/shaderrecomp/src/rdna2*`, `regs.cpp` |
| A freed `XMem` range can be handed out again under a resource that still covers it, so a cache of committed pages has to be cleared on every free path | Reported | Later: `runtime/imports/xmem` and `memory_tracker` (M3) |
| A save container or blob name too long for the host file system is stored under a hashed file name, with the real name in a file beside it | Reported | Later: `runtime/gdk` (`XGameSave`) |
| `CopyFileExW`, `MoveFileWithProgressW` and `ReplaceFileW`, its backup path included, need the same path redirection as `CreateFileW` | Taken: every move, copy and replace function is HLE on the mounts since `2d4307b` | [hle-status.md](hle-status.md), [io.md](io.md) |

The word-scan and structural checks in these tables come from our own scratch scripts. They
match AMD's public instruction encodings against the unpacked programs, and their output stays
local.

### Not taken

* **Its online services:** Xbox Live, PlayFab, title backends, LAN play, and its Trackmania
  and Ubisoft online code. Hard rule 5.
* **Tables derived from console OS images:** the API-set host table and the export lists of OS
  DLLs. We work from what our package contains. The same goes for its console audio service,
  its package-service interface slots, and `MFGetPlatform`, which it gets by loading the
  console's own `mfplat`. Newer ones: the UMS context layout, the wldp watermark and
  production checks, xtffileio and xbtp, and the console texture-layout library's DCC oracle.
* **Its Xbox One register layout** (root parameters packed from s3, compute input s2), and
  its gfx7 buffer formats and texture-address changes. They're for older titles; GTA VI is a
  Series X|S title.
* **The D3D12.X method map and command-list details:** interface slots, MSAA centroid
  handling, and depth-count control. They were measured on console driver binaries that
  aren't in our package. We'll revisit them at M3 against what the game actually calls.
* **Its Opus, SILK, and media decoders.** For audio (M4) we'd take the reference libopus as a
  submodule.
* **Its archive reader.** Of RPF9 it reads only the plain 0x40-byte header. Its RPF8, ZIP and
  PE hardening is for formats and readers we don't use.
* **The DirectStorage request option bits** (zlib, BCPack mode, swizzle mode, physical-page
  flags). They come from a docs.xbox.com page it calls public. We take them only once we've
  confirmed that page is public and not under NDA.
* **Code for later milestones**, not ported yet: the BCPack request validation, the half-wave
  lane mapping and NVIDIA-safe lowerings in `tombox-xshader/src/hlsl.rs`, DRED breadcrumbs,
  the oversize-texture heap, and the headless window and frame capture for local runs. Its
  interpolation-linkage code came from a TomBox fork that is no longer reachable, so we'd
  reimplement that idea rather than port it.
* **Its geometry-shader adjacency inputs.** Where the extra vertex offsets sit is inferred from
  the first three's packing, not measured, and we haven't found where a GTA VI geometry program
  gets its VGPR count. We'd revisit it if one of the 195 takes adjacency.
* **Newer online and console OS material** (`65ca6dd..6e81ffc`): `pixrt10` (an ETW controller
  and kernel-driver IOCTLs), `netprofmsvc_c`, `net_guard` and `netview` (connectivity, WFP and
  socket hooks), and the Trackmania online backend, all under rule 5; the console audio service,
  `chad` and `xapu`; the texture-layout library's DCC oracle; its Game Runtime V7 dispatch and
  pipeline subobjects 25 to 30; and XMem partitions read from an older console OS. Its reading of
  indirect argument 107 as a mesh dispatch is a console enum it calls unverified.
* **Code for later**, from the same range: its gfx1013 opcode table (`tests/isa_table`,
  Apache-2.0 and MIT), as a name-table test, or regenerated by us from LLVM and AMD's XML; the
  XTaskQueue and XAsync edge cases it lists against the PC Game Runtime, which we'd port from
  libHttpClient's own MIT tests instead; the transcendental models, taking AMD's rocjitsu
  headers (MIT) directly for `log`, `exp`, `sin` and `cos`; and its hardware-recorded test
  vectors for VALU and image atomics.
* **Fortnite and Trackmania fixes** (`6e81ffc..de1e1da`). Title-specific, with nothing GTA VI
  or the console's shared behaviour needs.

### TomBox

xbox-emulator's credits now say TomBox publishes no licence, and its repository has no licence
file. Its `tombox-xshader` crate, though, declares `MIT OR Apache-2.0` in its `Cargo.toml`, from
the crate's first commit (`b8fdbe4`, 2026-11-17) to its head. Compared at TomBox `7e8064f`, the
base xbox-emulator names, 483 of the 519 lines of `rdna2.rs` and 267 of the 410 lines of
`rdna2/mnem.rs` are TomBox's. So our three `rdna2` files credit TomBox too, and we use its part
under MIT, whose notice is in [NOTICES.md](../NOTICES.md). Anything else from TomBox gets the
same check first.

### Next review

1. Run `git log de1e1da..origin/main` in a scratch clone outside the repo.
2. Read the diff of `README.md`, `docs/BLOCKERS.md`, and `docs/DEVELOPMENT_LOG.md`. Look for
   GTA VI, and for console behaviour that every title shares.
3. Add each new finding to the table above with its status here, check it when a design
   depends on it, and move the "last reviewed" commit.

## RageForge

<https://github.com/nullbrik/RageForge>, LGPL-3.0-only licence.

A C# toolkit (GUI and CLI) for browsing the game's RPF9 archives and `.ttpf` texture pools. It
carves archives into files, views and exports models, textures, audio, and video, and parses
and disassembles the driver's pipeline cache in `shaders.rpf`. Its format notes are in
`docs/RPF9.md`.

Its licence is LGPL-3.0-only. Taking its code would need asking first, because it would pin our
licence to version 3 (`CLAUDE.md`), and nothing here needs its code. We wrote our own reader,
`g6::rpf9` and `rpfscan` ([rpf.md](rpf.md)), and our own pipeline cache parser,
`shaderrecomp pscache` ([shaders.md](shaders.md)), from the layout it reports.

**Last reviewed: commit `5f2726c` (2026-11-22).** No code has been copied. On 2026-11-23 the
repository was no longer reachable ("Repository not found"), so there was nothing new to
review; the findings below stand as recorded.

### Taken

| Finding | Status here | Used in |
|---|---|---|
| The RPF9 header is 64 bytes; the table of contents follows it, and its size is the word at 0x04. The data area starts at the next 16-byte boundary | Header and table-of-contents size checked on all 22 archives. The data-area start is checked only loosely: carving from there works, but the first file always starts later, after a gap | [rpf.md](rpf.md) |
| Files in the data area are zlib streams on 16-byte boundaries. A file larger than 64 KiB is a run of streams that each inflate to 64 KiB, apart from the last | Checked: `rpfscan` carves every archive that way, with each stream's Adler-32 matching | [rpf.md](rpf.md) |
| The tables of contents are encrypted with AES-CBC, and the key isn't public, so archives are read by carving | Reported. We never decrypt them: the game's own code reads its archives at run time, so the runtime needs no key | [rpf.md](rpf.md) |
| A paged resource has `0DRP` at 0x10 and its type after it | Checked: 290,294 carved files carry it, all of type 0x7A | [rpf.md](rpf.md) |
| The word at 0x24 is the entry count | Doesn't hold up: it is far below the number of files carved (6,534 against 2,127,667 over the install). Its meaning is open | [rpf.md](rpf.md) |
| `shaders.rpf` holds a pipeline cache (`PS\0\t`): header counts, graphics and compute records, metadata and shader indexes, and the metadata and code areas | Checked: `shaderrecomp pscache` parses it to the last byte, and the local `test_shader_census` holds the counts | [shaders.md](shaders.md) |
| Binding records name resources by kind: 0x300 SRV, 0x302 CBV, 0x303 UAV, 0x401 descriptor table, 0x2 sampler | Names reported. The kinds and their counts are checked, and so is the framing of every binding blob | [shaders.md](shaders.md) |
| An AWC stream's sample rate is the 16-bit word at +8 of its format chunk (tag 0x76); in a multichannel bank, at +10 of each channel's 16-byte record in the stream-format chunk (tag 0x48) | Reported | [game-package.md](game-package.md); later `runtime/audio` (M4) |
| BCPack BC1 chunks decode through an rANS layer that uses Microsoft's silicon-rans model tables (MIT), with a BC1 block layer on top | Reported. Agrees with the BCPack rows of Leonida and xbox-emulator; we'd take silicon-rans from upstream | Later: `runtime/io` (M2–M3) |
| A pool texture's mip count is the top four bits of its first header word, plus one except under tile modes 9 to 12. For tile modes 0 to 3 it refits the count so the texture's layout fills the space up to the next texture | Reported. The refit is a heuristic | Later: `runtime/gpu` (M3) |

We found three things it doesn't report: which slot holds which stage (from what each shader's
code exports, with slot 6 holding a vertex fetch subroutine), the key that ties each cache
shader to the shader records it was compiled from, and that the cache's vertex shaders have the
vertex fetch compiled in.

### Not taken

* **Its table-of-contents cipher, key identifiers, and key search.** Archive keys stay out of
  the repo whatever the licence (`CLAUDE.md`), and nothing here needs them.
* **Its exporters and viewers** (glTF models, PNG textures, WAV audio and its batch export,
  Bink files, the 3D world view). Asset exporters stay out.
* **Its name hashes and name lists.** They are derived from the game.
* **Its texture decoder** (GFX10 detiling, BC1 to BC7). For `runtime/gpu` (M3) we'd follow
  AMD's addrlib, which is MIT (below).
* **Its BCPack BC1 block-layer tables.** They don't come from a public source and may be fitted
  to the game's own textures.
* **Its character, world-export, and probe tools.** They read or export the game's assets.

### Next review

1. Check that the repository is reachable again, then run `git log 5f2726c..origin/master` in
   a scratch clone outside the repo.
2. Read the diff of `docs/RPF9.md`, its shader-cache code, and its format readers, as format
   facts only.
3. Add each new finding to the table above with its status here, and move the "last reviewed"
   commit.

## xcl

<https://github.com/ivall/xcl>, GPL-2.0-only licence.

A Rust runtime that runs Xbox Series titles on Windows the way we do. It maps the title's
executable and DLLs itself, routes their imports to HLE modules, other guest DLLs, or the host,
and runs the code natively. It gives the title real D3D12 objects with the console's extra
methods added to their vtables. It translates the console's native shader programs to HLSL and
compiles them with DXC. It runs the side DLLs of the GTA VI package we target (it reports the
same 16 DLLs and `shaders.rpf`), and runs other titles further.

Its licence is GPL-2.0-only, which can't be combined with our GPL-3.0-or-later. It moved there
from LGPL-3.0-or-later before its public history starts, so no earlier snapshot is available
under another licence. We take facts and ideas from it, and write our own code from an
understanding of the behaviour. We never translate its code. The permissive third-party
material it carries (AMD's ISA and addrlib tables, LLVM's test vectors) we take from upstream
when we need it.

**Last reviewed: commit `b4edefb` (2026-11-23).** No code has been copied.

### Taken

| Finding | Status here | Used in |
|---|---|---|
| An executable always has TLS index 0, and code built with `/GA` reads slot 0 of the TLS array without reading its index variable. On a donor's index such code reads another module's block | Checked: clang-cl and cl.exe both emit it, and `test_exe_tls` runs an original `/GA` fixture. Our solution is our own: the host executable lends the start of its own block | [execution-model.md](execution-model.md), "TLS and the TEB" |
| `XSystemGetDeviceType` is computed in the title's thunk library, from the registry value `ConsoleMode` under `HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM`: 0x64 Series X, 0x60 Series S, no value PC | Checked on the thunk in `XCurl.dll`. The registry HLE answers 0x64 | [hle-status.md](hle-status.md) |
| `XMemVirtualQuery` fills a `MEMORY_BASIC_INFORMATION`, then a word at +48 whose bits 0 and 1 give GPU access | Checked at the call sites in the replaced DLLs | [hle-status.md](hle-status.md) |
| The structure `XMemGetWorkingSetStatistics` fills grew between GDK versions, so only +0 (the limit) and +8 (bytes used) may be written | Checked at the call site, which reads +8 | [hle-status.md](hle-status.md) |
| FSSM stage records: from byte 0x100, one 0x88-byte record per stage, holding the hardware and API stage numbers; unused fields hold `0xDEADBEEF` | Checked: `shaderrecomp scan` reads them in all 26,503 records | [shaders.md](shaders.md) |
| The FSSM word at 0x248 is the pixel shader input enable register | Checked: it is `SPI_PS_INPUT_ENA`, and every interpolation in 10,259 of the 10,260 programs that interpolate reads a register it loads. xcl now finds it 8 bytes earlier in images of layout 5 (next row); every 1.0.2.1 image is layout 6 | [shaders.md](shaders.md) |
| The FSSM word at 0x10 is a layout number, 5 or 6, and under 6 the stage tables sit 8 bytes later. `exp param N` and `v_interp attrN` name a signature register, not an element, and the console's compiler numbers only the registers the code still uses. A pixel input table gives each attribute's register, and a vertex output table each param's; a word after each table counts its records | Checked here: all 26,698 programs are layout 6, every pixel program has an input table and every legacy VS stage an output table, read by their count words. Corrections: byte 6 of an input record isn't the register again in 22,168 of 55,937 records, byte 7 has five values rather than two, and 32 records are of kind 7. The register isn't the param number in 8,123 of 9,526 output records | [shaders.md](shaders.md) |
| msvcp140's thread-pool callbacks call `FreeLibraryWhenCallbackReturns` on their own module, and Windows' pool fails on a guest module's handle | Checked: our loader's HLE now ignores guest modules there, and `test_loader` runs callbacks that do the same. Windows 11 ignores the failed unload, so it was a hardening fix here | [hle-status.md](hle-status.md) |
| The console keeps four gfx10.1 opcodes (`v_mac_f32`, `v_madmk_f32`, `v_madak_f32`, `v_mad_f32`), and a VALU instruction fetches a literal whenever any of its source fields is 255, even one it doesn't use | Our decoder names all four and fetches a VOP3 literal for any of the three fields. How often an unused field holds 255 in this build hasn't been counted | [shaders.md](shaders.md) |
| Its archive scan finds 26,503 shader records holding 26,698 programs and 44.4 million instructions | Checked: the same counts as ours (`test_shader_census`). Its earlier 23,869 missed the 2,634 compute records whose names carry an effect prefix with a colon | [shaders.md](shaders.md) |
| A branch beyond `s_branch`'s reach is `s_getpc_b64`, then `s_add_u32` and `s_addc_u32` of a constant, then `s_setpc_b64`: a direct jump, not an indirect one. 12 programs have them | Checked: our decoder follows them and finds 1,450 in 13 programs (57 in 8 shaders of the pipeline cache), every one landing on an instruction inside the code. The 983 programs left with unknown targets are the 788 VS fetch calls and the 195 ES programs, which end by jumping through a copy of s[6:7] | [shaders.md](shaders.md) |
| `ds_swizzle_b32` picks its mode by offset: 0xE000 and up is FFT, 0xC000 and up rotate, 0x8000 and up quad permute, and below that the and, or and xor lane masks. 56 programs use 0x401F, which swaps the 16-lane halves | Reported; the split is AMD's RDNA 2 reference. Our register analysis doesn't depend on the mode | Later: `shaderrecomp` (M3) |
| `XUserGetTokenAndSignatureAsync` fails with `0x80072EFD`, as on a console that can't reach Xbox Live, and titles then take their offline paths | Reported. It fits rule 5: our XUser will fail token requests the same way | Later: `runtime/gdk` |
| Persistent local storage has to be a drive root such as `S:\`: Minecraft keeps only the drive letter of the path, and reads a failed or tiny free-space answer as a full disk | Reported, seen on Minecraft | [io.md](io.md) |
| XDefiant uses `D:\` for its package drive | Reported. The planned pre-flight check of drive-letter strings now looks for `D:` too | [io.md](io.md), [execution-model.md](execution-model.md) |
| Minecraft counts a gamepad as connected only once `XUserRegisterForDeviceAssociationChanged` reports it moving from no user to the local user, on the title's task queue | Reported | [hle-status.md](hle-status.md) |
| Fortnite creates its window with `CreateWindowExW` from `ext-ms-win-rtcore-ntuser-window-l1-1-0`, so hiding a title's window has to cover the A and W forms | Reported | [hle-status.md](hle-status.md) |
| After `s_wqm`, the first lane `v_readfirstlane` picks is often a helper lane on a primitive's edge, in another quad, and before shader model 6.7 a wave read of another quad's helper returns 0. The fix reads inside the lane's own quad first, then reads the wave from a live lane of that quad | Reported, seen on Minecraft. So `QuadReadLaneAt` alone isn't enough | [shaders.md](shaders.md) |
| Float atomic min and max done as integer atomics pick the signed or unsigned path by comparing the bits with +inf's, since a float compare puts -0.0 on the wrong path; the f64 `_x2` forms map to 64-bit integer min and max | Reported, measured on an RX 7900 XTX | [shaders.md](shaders.md) |
| A wave64 program split into two 32-lane halves breaks on `v_mbcnt` over a mask that isn't a constant, a VOP3 `v_cmp` into an SGPR pair, and 32-bit `s_bcnt`, `s_ff1` or `s_flbit` on one word of a mask | Reported, measured on an RTX 3080 | [shaders.md](shaders.md) |
| A Ryzen 7000 integrated GPU (RDNA 2) runs wave64 but has no mesh shader tier, so stages translated to mesh shaders can't run there | Reported | [shaders.md](shaders.md) |
| Vertex inputs the shader doesn't read take no fetch VGPRs | xcl now agrees with what we checked under xbox-emulator's row | [shaders.md](shaders.md) |
| Which kernel32 entries reach which: measured on Windows 11, `MoveFileW` goes only through `MoveFileWithProgressW`; kernel32's `MoveFileA`, `MoveFileExA` and `MoveFileWithProgressA` reach no kernelbase move function; `CopyFileW` and the ANSI copies go through `CopyFileExW`, and `ReplaceFileA` through `ReplaceFileW`. `ReplaceFileW` is the usual last step of a save | Taken as the reason to bind each name itself: since `2d4307b` every move, copy and replace function is HLE, so it doesn't matter which entry the host would route through | [hle-status.md](hle-status.md), [io.md](io.md) |
| `XAsync` and `XTaskQueue` contracts, as libHttpClient has them: `XAsyncCancel` only asks the provider, and a provider that can't cancel runs to its end (completing with `E_ABORT` there freed the payload under a running work step); a failing begin step fails the call through its block while `XAsyncBegin` and `XAsyncRun` return `S_OK`; `XAsyncComplete` with `E_PENDING` completes nothing; `XTaskQueueTerminate` runs the delayed callbacks still waiting, as canceled, before its own callback; a terminated queue refuses submits, delayed submits, waiters and `XAsyncSchedule` with `E_ABORT` | Reported, tested under Wine. Agrees with xbox-emulator's `XAsync` row | Later: `runtime/gdk` |
| Save names that Windows would change get escaped, not refused: `.`, `..`, device names with any extension (`nul .txt` too), and names ending in a dot or a space. Deleting a container named `..` once deleted every save | Reported | Later: `runtime/gdk` (`XGameSave`) |
| Desktop drivers don't keep a 2-D or 3-D compute group's threads in order within a wave: an RX 9070 lays them out in 2x2 quads (in an 8x8 group, lanes 0, 1, 4, 5 hold `SV_GroupIndex` 0 to 3), and NVIDIA's waves of an 8x8x4 group don't hold consecutive indices either; 1-D groups stay in order. The console's do. So a translated 2-D or 3-D compute program that uses DPP, readlane, `ds_bpermute` or `v_mbcnt` reads the wrong lanes unless each host wave takes the next block of console threads itself when it starts | Reported (RX 9070, RTX 5070 Ti) | [shaders.md](shaders.md) |
| Splitting a wave64 program into two 32-lane host waves that exchange through groupshared: NVIDIA (range 32 to 32) refuses a `[WaveSize(64)]` compute pipeline. Only masks, `readlane`/`readfirstlane`, `ds_bpermute` and GLC scalar loads need the exchange; DPP, swizzles and `permlane` stay in a half. It cost 1.3 times with one console wave per group and about 5 times with four. Wave64 vertex and pixel programs that read lanes 32 to 63 stay open on NVIDIA, where those lanes don't exist. The exchange waits on its partner, which assumes co-resident waves make progress; WARP doesn't promise that, and has neither 32 nor 64 lanes, so it can't test this form | Reported (RTX 5070 Ti, RX 9070). Supersedes the row above on splitting into halves | [shaders.md](shaders.md) |
| An AMD driver gave a later `a < b` the unordered result of an earlier `!(a >= b)` on the same operands, so float compares test NaN explicitly; f64 `floor`, `ceil`, `trunc`, `fract`, `ldexp`, `sqrt`, `rsq` and conversions need their own handling past the f32 and int64 ranges | Reported | Later: `shaderrecomp` (M3) |
| `v_fract_f32` of ±inf or NaN is NaN | Reported. So the clamp below 1.0 in Xbox2Windows's row must be a compare and select: HLSL's `min` returns the other operand for a NaN, which would turn NaN into the bound | [shaders.md](shaders.md) |

**For `runtime/gpu` (M3).** Reported, and to be checked against GTA VI's own calls before a
design depends on them:

| Finding | Status here | Used in |
|---|---|---|
| Console command lists: the title's inline code compares the list's write pointer (+0x18) with its end (+0x30), and calls the matching method when the record doesn't fit. So a list object whose buffer never has room turns every inline write into a method call | Checked here on `XboxBvhBuilder.dll`: 205 sites in 30 functions, each falling back to one method. Dispatch records compare with a second end at +0x28. The list object must still be at least 0x564 bytes of writable memory, since one field is written before the check. This removes the Leonida row's risk for the builder; the main executable still needs checking | [d3d12x.md](d3d12x.md); later `runtime/gpu` (M3) |
| Memory-based root arguments: s[0:1] points to a packed table in which a root constant takes 4 bytes, a root descriptor 8, and a descriptor table 4 (a 32-bit address). Compute root arguments fill user SGPRs s4 to s15 until one doesn't fit; it and all after it go to the table | Reported. The compute rule shows in the BVH builder's and FSR 4's code, which we have. Its graphics rule isn't taken (see "Not taken") | Later: `runtime/gpu` (M3) |
| Console descriptors: a view slot is 32 bytes and a sampler slot 16, and descriptor table addresses have a zero high half. Titles copy descriptors as plain memory, so all console view heaps have to live in one shader-visible desktop heap. NVIDIA's holds 1,000,000 descriptors, which Minecraft's eight heaps of 73,727 slots overran, so xcl now takes that heap 256 slots at a time, as each page is first used | Reported. Fits Leonida's 32-bit descriptor pointers and Xbox2Windows's 32-byte entries ([shaders.md](shaders.md)) | Later: `runtime/gpu` (M3) |
| Method slots: device extensions start at slot 44 and command-list extensions at 60, and list slots 28 to 55 are shifted from desktop. `PresentX` is queue slot 30, `SignalX` queue slot 23, and the frame clock device slots 78 to 81 | Reported, mostly observed on Fortnite, an older GDK. **Conflicts:** xcl keeps `ExecuteIndirect` at slot 59 where Leonida says 61, and xcl's own notes give list slot 85 two meanings. **Partly checked** on the BVH builder: desktop compute root slots 29 to 41 sit four higher, while `Dispatch` (14), `SetPipelineState` (25) and `ResourceBarrier` (26) don't move. The builder never calls `ExecuteIndirect`, but the upscaler settles it at 61, against xcl's 59. Queue slots 23 and 30 belong to the older runtime layout (next row) | [d3d12x.md](d3d12x.md); later `runtime/gpu` (M3) |
| A newer console runtime reports the same SDK version, 0x20004, but moves slots: two more queue methods before `Signal`, so `SignalX` is 25 and `PresentX` 32, and the list's console methods from `CopyTextureRegionX` on sit two slots later. The device is unchanged | Reported, seen on Unicorn Overlord. Which layout GTA VI uses shows in the executable's queue calls | [d3d12x.md](d3d12x.md) |
| Unified memory on XDefiant: a first graphics reservation of 8 GiB + 4 MiB; a write-watch scan of about 2 GB costs about 0.8 ms per submission; only the 64-byte chunks the CPU changed may be uploaded; sync at `ExecuteCommandLists`, since titles fill vertex rings after recording; address 0 and the 32-bit descriptor range stay out of title reservations | Reported, measured | [d3d12x.md](d3d12x.md); later `memory_tracker` (M3) |
| A committed console resource that isn't placed needs a desktop description, or creation fails with `E_INVALIDARG`; the console-only state bit 0x100000 has to be masked | Reported, seen on Fortnite, Unicorn Overlord and Minecraft | [d3d12x.md](d3d12x.md) |
| Methods that return a struct use the plain C convention on the console: small results come back in RAX, and larger ones pass the return buffer before `this` | Reported. Fits Leonida's finding that the heap start-handle getters return in RAX | Later: `runtime/gpu` (M3) |
| The console's `D3D12_FEATURE_DATA_D3D12_OPTIONS` is 0x40 bytes (0x3C on desktop), and the console resource description has fields beyond +44 | Reported | Later: `runtime/gpu` (M3) |
| `OpenExistingHeapFromAddress` needs a fully committed allocation smaller than 4 GiB (4,095 MiB works, exactly 4 GiB doesn't). Placeholder reservations replaced by committed sections can each be opened as a heap. Write-watch works on such memory. Mapping tiles onto heaps opened from a file mapping removed the device | Reported, measured on Windows 11 with an RX 9070 | Later: `memory_tracker` (M3) |
| Titles place several render targets, depth buffers, and UAV textures at one address and expect them to share memory, so one committed resource per placement breaks them | Reported, seen on XDefiant | Later: `runtime/gpu` (M3) |
| AMD's shader compiler crashes on DXIL whose bitcode part is `NOBC` (console bytecode) instead of rejecting it | Reported. Console bytecode must never reach the driver | [shaders.md](shaders.md) |
| Ray tracing: the builder's prebuild-size formula; node formats follow AMD's open GPURT for gfx10, apart from the triangle word at +48; rehydrating a structure happens on the CPU and reads the game's own vertex buffers | Reported | [shaders.md](shaders.md), "Ray tracing" |
| Console DirectStorage: a request is 0x48 bytes and an error record 0x290 | Reported | Later: `runtime/io` |
| A title sizes its back buffer from its window's client area, so the window has to look like the console's screen: frameless, 1920x1080, its client area the whole window. On an ordinary overlapped window the client area came out 1904x1041, the title resolved 1920x1080 into it, and the device was removed | Reported, seen on Minecraft | Later: `runtime/gpu` and the window HLE (M3) |
| The console accepts the vertex-and-constant-buffer and index-buffer states on textures, as part of a generic-read combination; desktop D3D12 then fails to close the list, so those bits have to be dropped for anything but buffers | Reported, seen on Minecraft | Later: `runtime/gpu` (M3) |
| A GPU address the title passes is translated only when it lies in the title's own memory: a desktop resource's GPU address can fall in one of the title's ranges by number | Reported, seen on Minecraft | Later: `memory_tracker` (M3) |
| Buffers placed in the title's heaps have to live in unified memory, because console code reads root constant buffers and buffer descriptors from it. Backing each heap when its first buffer is placed fits; backing every heap up front used up 2 GiB | Reported, seen on Minecraft. **Superseded** at `dd713e3`: title graphics memory lives in 4 GiB reserved buffers whose tiles are mapped as it is committed, plus a 512 MB coherent window for readback ([d3d12x.md](d3d12x.md)) | Later: `memory_tracker` (M3) |
| `VirtualFree` has to tell the sparse-tile tracking before it decommits, and uncommitted pages inside a mapped tile are holes: never read, their video memory zeroed | Reported, seen on XDefiant | Later: `memory_tracker` and `runtime/kernel` (M3) |
| An empty sampler slot holds the console's zero sampler: point filtering, wrap, mip 0 | Reported | Later: `runtime/gpu` (M3) |
| The driver links pixel inputs by register, so a pixel shader that declares only the inputs it reads is refused; every input has to be declared | Reported | Later: `shaderrecomp` (M3) |
| Translated shaders reach title memory through window buffers rather than the resources a barrier names, so each batch with a UAV barrier or a transition to `UNORDERED_ACCESS` gets one UAV barrier for all resources | Reported | Later: `runtime/gpu` (M3) |
| `PresentX` has to queue the present copy before it reports the flip, and keep the copy's source until the copy's fence; aliasing barriers must keep placed textures; resetting write-watch when a range is registered drops other users' pending CPU writes | Reported, seen on Unicorn Overlord and XDefiant | Later: `runtime/gpu` and `memory_tracker` (M3) |

The lessons from its HLSL translator are in [shaders.md](shaders.md), "Host constraints that
affect translation".

**Ideas to reimplement ourselves** (`e80ce8d..dd713e3`): hardware-breakpoint call probes and
write watchpoints armed on every thread, new threads arming themselves, for debugging guest code
without patching it; an arena for per-draw data in place of 64 KiB allocations; one DXC compile
at a time when little commit memory is left, with a report of cache hits, misses and the first
distinct errors; and decoding XML character references in `MicrosoftGame.config` once we parse
it. It tells the two runtime layouts apart from behaviour; since we pin one build, a per-build
setting is simpler.

### Not taken

* **Its code.** It is GPL-2.0-only.
* **Its online code.** `xsign.rs` fakes Xbox Live tokens and signatures, `networking.rs`
  implements XNetworking, and network services are forwarded to the desktop Game Runtime. Rule
  5 keeps all of that out.
* **Facts measured on console SDK or OS binaries.** Its tests run the console's own shader
  compiler, which is where its graphics root-argument rule comes from. They compare against the
  console's texture-layout library (pipe configuration, metadata planes), and its MSAA layouts
  are now checked against that library too. Its notes include
  system-partition research. Console SDK material stays out of the repo.
* **Its asset tools:** `rpf.rs --extract` and its shader dumps. Its texture-pool viewer is
  gone upstream since `7396683`.
* **Game shader names and disassembly** in its notes and tests.
* **Its XNetworking review documents** and the `CERT_CHAIN_CONTEXT` offsets it added for
  XNetworking. Hard rule 5.
* **Its tooling changes** (launcher configuration, RPF probe options, shader tool output,
  disassembler byte ranges, the manifest storage quota). They carry no new facts.

### Next review

1. Run `git log b4edefb..origin/main` in a scratch clone outside the repo.
2. Read the diff of `HANDOFF.md` and `docs/specs/` for facts, written up in our own words.
3. Add each new finding to the tables above with its status here, check it when a design
   depends on it, and move the "last reviewed" commit.

## References those projects cite

Public material; listed here so it can be found again.

* [SlimEra](https://github.com/XWine1/SlimEra) (MIT): Win32 reference implementations of
  Xbox One system DLLs. It is the source of the `XALLOC_ATTRIBUTES` layout and the allocation
  hooks in `XMemAlloc`.
* [AMD PAL addrlib](https://github.com/GPUOpen-Drivers/pal) (MIT): GFX10 swizzle patterns and
  mip-tail addressing, for detiling textures.
* [AMD RDNA 2 instruction set reference](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf).
* [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (MIT): the
  public `ffx_api` the console upscaler exports.
* [Microsoft Xbox GDK samples](https://github.com/microsoft/Xbox-GDK-Samples) (MIT): samples
  with Scarlett build configurations. They show publicly how console D3D12 is used, without the
  console headers.
* [GTA6-Asset-Info](https://github.com/walkthestars/GTA6-Asset-Info): community notes on RPF9
  and BCPack. Unvetted.
* [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT): the RADV driver's software BVH
  intersection path and its GFX10 descriptor format tables, and the ACO compiler's rules for
  16-bit results. Leonida cites all three.
* [LLVM](https://llvm.org) (Apache 2.0 with LLVM exceptions): the AMDGPU disassembler, which
  decodes `gfx1013`.
* [rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer): an RPF9 reader and asset
  exporter that Leonida builds on. No licence file in Leonida's copy. Unvetted.
* [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) (University of
  Illinois/NCSA): compiles HLSL to DXIL. xbox-emulator's shader backend uses it.
