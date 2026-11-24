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

**Last reviewed: commit `be8624e` (2026-11-24).** No code has been copied. If code is ever
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
| `RtlCaptureStackBackTrace` stops at the first frame in a manually mapped image; walking with `RtlLookupFunctionEntry` and `RtlVirtualUnwind` over the registered tables gets through | Checked here: it returns the first guest frame, then stops. Taken, our own code: the crash report walks with those two functions | [execution-model.md](execution-model.md), "Crash reports"; `runtime/loader/src/crash_report.cpp` |
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
| NVIDIA drivers reject some shaders compiled with `-denorm preserve`: pipeline creation returns `E_OUTOFMEMORY` on an RTX 5080 (32.0.16.2101) and an RTX 5060 Ti (32.0.16.2124), where WARP accepts them | Reported. Matters when `shaderrecomp` picks a denormal mode. Narrowed by `c0f4d9c` (row below): `log2` under `preserve` is what fails | Later: `shaderrecomp` (M3) |
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
| Windows 10's in-box WARP (10.0.19041.5794) picks mip 1 for a per-sample `MinLod` clamp of 1.5625 under point mip filtering, where `SampleLevel` at the same LOD picks mip 2; linear mip filtering blends as expected | Reported, on that one adapter: a host difference, not a claim about the console. Upstream later found that only that WARP reads the mip below (the `MinLod` row further down) | [shaders.md](shaders.md) |
| Creating a mesh pipeline takes much longer as the mesh shader grows, compiling with DXC's `-Od` doesn't reliably shorten it, and WARP crashed creating some of the larger ones. Measured on an RTX 5080, a Radeon integrated GPU, and WARP, with generated shaders | Reported. Adds to the pipeline-time rows: GTA VI has 8,765 mesh shaders | Later: `shaderrecomp` (M3) |
| `v_cvt_flr_i32_f32` and `v_cvt_rpi_i32_f32` lower to a plain HLSL conversion only where the input is proven finite and in range, since HLSL leaves the conversion of other values undefined | Reported | Later: `shaderrecomp` (M3) |
| On NVIDIA a NaN-shaped word loses its bits in a float-typed HLSL value: an RTX 4060 Laptop (32.0.16.1656) and an RTX 3060 (32.0.16.2101) returned 0x7FFFFFFF for 0xFFFFFFFF. Declaring the constant buffer `uint4` and converting to float only where a float operation reads the value keeps them (`24936e2`, merged in `046a2bd`) | Reported. A design rule for `shaderrecomp`: registers and loads stay `uint` | [shaders.md](shaders.md), "Host constraints" |
| Division helpers: `v_div_scale_f32` depends on the numerator too (a denominator of 1 with a numerator of 2^100 scales by 2^64 and sets VCC); `v_div_fmas_f32` is one fused rounding, scaled by 2^32 when VCC is set, before the rounding to binary32; `v_fma_f16` can't be an f32 FMA narrowed afterwards. Windows 10's in-box WARP (10.0.19041) faults at the first dispatch of a double `>` or `<=`, so a translation emits `<`, `>=`, `==` or `!=` with the operands swapped (`9fff1ee`, `4a19cec`, `766276f`) | Reported. The fixed 2^32 factor isn't confirmed: later AMD guides tie the scale to the third operand's exponent. Their MIT helper `div_fmas32_core` isn't copied; it waits for a census of these instructions | [shaders.md](shaders.md) |
| f16 add, sub and mul computed in f32 and narrowed once to nearest even are exact for every finite input, subnormals and signed zeros included (24 ≥ 2·11+2); an exhaustive run over all finite pairs of add and mul found no mismatch. `v_subrev_f16` is src1 − src0, from LLVM (`7e0be98`) | Reported. Not for a fused f16 FMA (the row above) | [shaders.md](shaders.md) |
| An integer inline constant read as an f32 operand is its raw bits, so 1 is the smallest denormal and −1 a NaN. A compiler may fold an ordered compare with abs or neg modifiers (−\|x\| ≤ 2) to true for a NaN, so upstream adds an explicit ordered test on the bits (`e114e89`) | Reported | [shaders.md](shaders.md) |
| `MinLod` under point mip filtering: only Windows 10's in-box WARP reads the mip below (0.5625 gives 0, 1.5 gives 1, 2.5625 gives 2, at any shader model); Windows 11's in-box WARP 10.0.26100.9818, an RX 7900 XTX, an RTX 3080 Ti and WARP 1.0.22 pick the nearest. Under linear mip filtering WARP rounds the blend weight to the nearest 1/256, ties to even, as the RX 7900 XTX and Windows 11's WARP do; the RTX truncates, storing step k as k·256/65535 (`851b68c`, `eab58b5`) | Reported. Supersedes the `MinLod` row above | [shaders.md](shaders.md) |
| Atomics on a non-uniformly indexed UAV array also give four times the result on RDNA 2: an RX 6500 (32.0.21045.29003) read 256 for 64. A loop in which a leader lane counts the lanes sharing its index and makes one `InterlockedAdd` of that count read 64 there and on WARP; the cause is open (`224d6fd`) | Reported. Widens the atomics row above | [shaders.md](shaders.md) |
| An RTX 5080 (32.0.16.2101 and .2124) fails pipeline creation with `E_OUTOFMEMORY` for any pixel shader that uses `log2` under `-denorm preserve`; `ftz` and `any` pass, and WARP and a Radeon integrated GPU accept `preserve` (`c0f4d9c`) | Reported. Narrows the NVIDIA `-denorm preserve` row above | Later: `shaderrecomp` (M3) |
| `XMemGetWorkingSetStatistics` +8 is GPU memory in bytes: the upscaler's seven calls use only its change around D3D12 heap and committed-resource creation and release, as FidelityFX SDK v2.3.0's DX12 backend counts VRAM. Upstream reads DXGI `QueryVideoMemoryInfo`, local plus non-local on every node, and counts a GPU that Windows lists under several LUIDs once, by its PnP key (`4c78073`, `a35f8f0`, `1ca4ad1`, `629eb61`) | Reported. Ours reports private bytes until `memory_tracker` owns GPU memory; the executable imports the function, so this comes back with M3. Their MIT code (`xmem_gpu_usage`, `adapter_identity`) isn't copied | [hle-status.md](hle-status.md) |
| In upstream's own mapper, which has no `RtlPcToFileHeader` HLE, a C++ exception thrown and caught inside a manually mapped DLL escaped as 0xE06D7363 while SEH worked (`3fb24dc`) | Agrees with our design: our HLE `RtlPcToFileHeader` answers for guest modules, and `test_loader` throws and catches C++ exceptions across guest and host frames | [hooks.md](hooks.md) |
| The compute-to-vertex mesh fallback also holds with normals interpolated and lighting applied, at shader model 6.0, on a GTX 1060 and both WARPs (`301bfae`) | Reported. Extends the fallback row above | [shaders.md](shaders.md) |
| Exec-masked regions: one forward region from `s_and_saveexec_b64` to the `s_or_b64 exec` that restores it can be lowered as per-lane selects; `s_cbranch_execz` can go where every operation in the body is safe on every lane; SCC after the save or restore is a whole-wave value, so instructions that read it are refused (`03ece06`, `1a252c8`) | Reported. A lowering idea for `shaderrecomp` | [shaders.md](shaders.md) |
| Operand codes 235–239 and 251–254 are hardware values (`src_shared_base` to `src_pops_exiting_wave_id`, `src_vccz`, `src_execz`, `src_scc`, `src_lds_direct`), and inline constant 248 in a 64-bit operand is 1/(2π) as an f64 (`a99e8da`) | Taken, our own code: the decoder names them and gives the f64 bits of 1/(2π), tested by `test_rdna2` | `tools/shaderrecomp/src/rdna2.cpp`, [shaders.md](shaders.md) |
| When two products are NaN, which NaN `dpps` and `dppd` return is vendor-defined, so an Intel host can differ from Zen 2 though `cpuid` masking makes the feature bits the same (`e2526e1`) | Reported | [execution-model.md](execution-model.md) |
| The 24-bit MAD truncates both multiplicands (`ee5535a`) | Reported. Agrees with the integer row above | Later: `shaderrecomp` (M3) |
| Intel UHD drivers 32.0.101.7026 and .7092 got emulated 64-bit (`uint2`) arithmetic wrong inside a large shader, where the same operation alone was exact (`688aa85`) | Reported | [shaders.md](shaders.md) |
| `D3D12ConfigureVirtualMemoryX` is called once before the device, with a 32-byte block that is zero but for a DWORD at +4 (3 or 4). A failure only breaks into the debugger. Two reserves of type `0x20402000` follow whatever the result, 16 GiB each for 3 and 24 GiB for 4, with attribute and protection words 0x21 and 4, then 3 and 0x801 (`6eb3341`, `9adf97c`) | Checked on our run 6 at debug level: the DWORD is 4, the reserves are 24 GiB each, and the words are as stated. Ours logs the block and succeeds | [boot.md](boot.md), "Our runs" |
| `XMemAllocatePhysicalPages(flags, count*, pages)` and `XMemFreePhysicalPages(count, pages)` return BOOL. The count is in 64 KiB pages, in and out, and unchanged on success. That much comes from Microsoft's public GDK samples; the flag check (only `0x20400000`), the all-or-nothing rule, and whole-grant release are their own policy (`729bfa1`, `d0bf29a`) | Shapes reported, and they agree with Leonida's measurements. The flag check is not taken: Leonida saw the game pass 0x280 and 0x380 | [boot.md](boot.md), "Physical pages" |
| Windows 11 seeds an x64 image's `/GS` cookie only when the load-config directory and its `Size` are both at least 0x70, `IMAGE_GUARD_SECURITY_COOKIE_UNUSED` (0x800) is clear, and the slot holds the linker's default. A pre-seeded value is kept (`62c7536`) | Taken, our own code: the loader follows the same three conditions (`test_loader`, with patched copies of an original fixture) | [execution-model.md](execution-model.md), step 5 |
| Windows 10 calls two TLS callbacks in table order and then the entry point on every notification, and gives each thread a fresh copy of a template that crosses a page (`02cd2ac`, `54b6781`) | Reported. Our loader does the same; no fixture has two callbacks yet | [execution-model.md](execution-model.md) |
| `XMemAllocDefault` and `XMemFreeDefault` take `XMemAlloc`'s and `XMemFree`'s arguments and share their allocator; `XMemIsToolsMemAvailable` is `BOOL (void)`, FALSE on retail (`713c3ba`, `8e85283`) | Taken: ours does the same | [hle-status.md](hle-status.md) |
| Returning LDS updates write the word's value from before the update, with the inputs read first since the destination may overlap them; `ds_rsub_u32`, and signed or unsigned min and max; `v_mul_lo_u32` is the low 32 bits of the full product; f16 floor, ceil, trunc and round-to-even go exactly through f32 and keep the sign of zero (`daf87ee`, `80c4d6e`, `a329153`, `cc09358`, `8872308`) | Reported, from AMD's ISA reference and tests on desktop GPUs, not RDNA 2 | Later: `shaderrecomp` (M3) |
| Once it has a device, the executable keeps its first 24 GiB reservation as a CPU range A and the second as a GPU range B of the same size, and translates by offset: `gpu = cpu - A + B`, the reverse inside `[B, B + size)`, other addresses unchanged. Desktop D3D12's `OpenExistingHeapFromAddress` opens only a fully committed range, so A would be committed and wrapped once, and D3D12 objects over it are released before it is freed (`b68ef35`, `af0590a`) | Reported (a static read of the executable, and a desktop test). Our run 1 shows the two reserves; how `memory_tracker` gives the GPU its side is M3 work | [boot.md](boot.md), "Our runs" |
| A started executable's TLS callbacks get `DLL_THREAD_ATTACH` and `DLL_THREAD_DETACH` for every thread once all its process-attach callbacks have returned, and its entry point never gets them (measured on Windows 10, `41b1de6`, `c631998`) | Checked: our loader already did this, and `test_exe_tls` now covers it with an original fixture | [execution-model.md](execution-model.md) |
| A synthetic caller and provider for `XMemMapPhysicalPages` and `XMemMapPhysicalPagesScatter`: 64 KiB address alignment, contiguous and scatter maps, and a null page array that unmaps. Map returning the base and scatter returning TRUE are its proposed contracts, not measurements of the game (`d3a0aae`, `24e58eb`, `379b0d6`) | Reported. Agrees with Leonida's measurements, which our physical pages follow | [boot.md](boot.md), "Physical pages" |
| LDS d16 reads replace one half of a VGPR; `ds_inc_u32` and `ds_dec_u32` wrap at a limit, and so does `ds_wrap_rtn_b32`; `ds_cmpst_b32` replaces a word equal to its first operand, `ds_mskor_b32` clears the first operand's bits and ORs in the second, and `ds_wrxchg_rtn_b32` replaces the word. Returning forms write the old word (`dc7118f`, `03a101a`, `67b21b4`) | Reported, from AMD's ISA reference | Later: `shaderrecomp` (M3) |
| DXC's GVN pass grows about fourfold per doubling on shaders with many conditional UAV stores; `-opt-disable gvn` compiles a 16,384-store case in seconds instead of minutes (`47be65f`) | Reported. A note for `shaderrecomp`'s compile budget | Later: `shaderrecomp` (M3) |

### Not taken

* **The static translators and their generated C++,** including the new x87, SSE2 to SSE4,
  AES-NI, PCLMULQDQ and FMA3 lowering. We run guest code natively. Our frozen recompiler
  covers the cases patching can't.
* **Package verification and provenance material.** Hard rule 6.
* **Script archive and camera-state probes.** Game logic data. Not needed before M4.
* **The FSR4 render-graph traces** (passes 24 to 26 and RCAS), for now. We planned to replace
  the upscaler with the public FidelityFX SDK at M3, so its passes would never run here. xcl's
  report that its shaders translate (`fdeacf0`) reopened that choice
  ([side-dlls.md](side-dlls.md)); if they are translated, these traces become a reference.
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
* **Its translator coverage at `275c399`:** `pixel_aot` lowerings for the 16-bit three-operand
  integer operations, `v_mul_lo_u32`, Wave32 `exec_lo` operations, and `image_sample_lz`, `_l`
  and `_b`, a small integer ALU subset between a compute read and its store, and a further
  hardware BVH report. They extend its own translator, which doesn't carry over; the rules
  behind them are AMD's public ones.
* **Its changes at `2aa81a2` with nothing for us:** the static x64 translator's new lowerings
  (AVX integer, FMA3, SSE2 to SSE4.1, unused memory reads, RCX branches); `pixel_aot` coverage
  (quad-perm `ds_swizzle`, `ds_nop`, FLAT scratch, which GTA VI doesn't use, dot products,
  carry-in adds); compute LDS lowering; disassembler and tooling changes; archive-texture
  fixtures; and README and disclaimer edits.
* **Its changes at `ec5d004` with nothing for us:** the physical-page registry (bookkeeping
  with no memory behind it), the package import-closure tool, stack outputs through imports, and
  the static translator's AVX, x87 (including BCD), and fuzzing work.
* **Its changes at `be8624e` with nothing for us:** the static translator's AVX broadcast,
  permute, shuffle, blend, dot, round, and `vzeroall` lowerings, its CPU fuzz test's fixed-frame
  generators, desktop tools-memory reporting (ours already answers FALSE), and Fedora setup
  notes.

### Next review

1. Run `git log be8624e..origin/main` in a scratch clone outside the repo.
2. Read the diff of `docs/experiments.md` and `README.md`.
3. Add each new finding to the table above with its status here, check it when a design
   depends on it, and move the "last reviewed" commit.

## Leonida

<https://github.com/ubiquitarianism/leonida>, GPL-3.0-or-later licence. The original
repository, github.com/ItsNotRexed/Leonida, was removed by its owner on 2026-11-23 (a 404, not
a DMCA block). The repository above continues it with the full history: `dde82f27`, the last
commit we reviewed on the original, is an ancestor of its `main`.

Groundwork for running the same 1.0.2.1 package natively on Windows. It has Python analysis
tools, a small C++ loader, and a translator from the console's RDNA 2 machine code to Vulkan
SPIR-V, tested on a GPU with hand-written test programs. It also exports the game's assets
(models, textures, audio) to files with tools built on rpf9-explorer.

Its execution model matches ours. It differs in two ways: it targets Vulkan where we target
D3D12, and it extracts assets where we read archives in place. Its licence matches ours, so its
code can be ported with its notices kept. Little of it fits as it is, though: its tools are
Python and its translator emits SPIR-V. We take its measurements as facts, check them on our
copy, and port code only where it fits our design.

**Last reviewed: commit `dde82f27` (2026-11-23).** Code ported: the physical pages in
`memory_tracker`, from `dde82f27`'s `runtime/shims/xmem.cpp` ([NOTICES.md](../NOTICES.md)).
The continuation added about 1,100 commits after it by 2026-11-24 (`c023a365`), not reviewed
yet. Upstream's
`57c17539` reverts its console-ABI command-list proxy, import routing, and two console-ABI
documents; the revert is for broken tests, so the facts from them below still stand.

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
| Its shim serves `GetCommandLineA/W` and the UCRT's WinMain command-line helpers, so the game sees its own command line rather than the host's | Taken, our own code: the runner sets the guest's line before it loads anything, and `GetCommandLineA/W`, the WinMain helpers, and the rest of the CRT's argument functions answer from it. The parsing rules were measured on the host UCRT and are tested against it line by line | [execution-model.md](execution-model.md) phase 5, "Command line"; `runtime/imports/command_line.cpp` |
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
| Threads that exist before the loader starts (pool workers, host libraries' threads) need the guest's static TLS too; Leonida lists the process's threads and writes the template through each TEB | Taken as a problem, solved our own way: the thread notifier became a TLS callback in the host executable, which sees every thread from the start of the process. We measured that an exiting thread clears its TLS array pointer outside the loader lock, so writing through threads we were never told about could hit freed memory. `test_tls_order` checks a thread started before anything else | [execution-model.md](execution-model.md) |
| Its `CopyFile2` fallback refuses copy flags `CopyFileExW` can't honour instead of dropping them | Taken as a rule, our own code: the fallback refuses copying a directory or a reparse point as such, and only when the source is one; on an ordinary file those flags change nothing. Windows 11's own parameter checks, measured here, now come first on every host | [hle-status.md](hle-status.md) |
| Gaming Services installs an online-capable `xgameruntime.dll` in System32, and a loader that lets unserved imports reach it gives the title Xbox services | Checked here: present on this machine, with `gameinput.dll`. Taken: the default policy never forwards it, and `test_imports` checks that ([gdk.md](gdk.md)) | [hle-status.md](hle-status.md) |
| Its XTaskQueue is no longer Microsoft's unmodified: waiter tokens are published before the callback can fire, a waiter callback may unregister itself or a peer or close the queue, and wait lifetimes are fixed. `XTaskQueueTerminate(wait=true)` on a Manual queue, from the thread that dispatches it, deadlocks in Microsoft's code, which its documentation warns of | Reported. Our own queue keeps Microsoft's rule: a Manual port has to be dispatched for termination to end | [gdk.md](gdk.md) |
| Host error codes where `runtime/io` answers itself: listing below a file gives `ERROR_DIRECTORY` (267), a missing folder `ERROR_PATH_NOT_FOUND`; `MoveFile` onto its own name succeeds and changes nothing; a write-open in a missing folder gives `ERROR_PATH_NOT_FOUND` and creates nothing | Reported, measured on the host | Later: `runtime/io` (mount roots and unmounted drives) |
| `XNetworking`'s two change registrations (`XNetworkingImpl` slots 6 and 16) send one initial callback on the queue's completion port and none after | Reported; Microsoft documents it | Later: `runtime/gdk` (offline networking) |
| `OutputDebugStringW` with no debugger raises the wide debug-print exception, then calls the ANSI form on a lossy copy, which raises a second | Reported, measured. A startup trace logs each line once | Later: a startup trace |
| Its float-mode reading of FSSM +0x104 (0xC0 in all 26,503 headers: round to nearest, f32 denormals flushed) | Checked here: not a float mode. That dword is the top of the first stage record's tag, `0xC00002`, which every record we parse carries. Also reported: +0x10C rounded up to 512 bytes is the compute LDS size in all 2,689 compute programs, and the corpus has 102,365 f32 output modifiers and no f16 ones | [shaders.md](shaders.md) |
| Measured on RDNA 2 (gfx1036 and gfx1030): an f16 output modifier is ignored while f16 denormals are kept, but clamp still applies; 16_FLOAT typed loads keep a NaN's payload; f32 to f16 in 16_FLOAT stores and d16 loads from 32_FLOAT rounds toward zero; `v_fract_f32` passes NaN through and gives 0xFFC00000 for ±inf; wave64 `ds_bpermute_b32` stays within each 32-lane half; `s_buffer_load` checks bounds per dword; an LDS halfword crossing the end of the allocation reads 0; `image_bvh_intersect_ray` returns the raw watertight values, tests t ≥ 0 but not t max, and breaks edge ties RDNA 2's own way, not RADV's | Reported. Corrects our note that Leonida follows RADV ([shaders.md](shaders.md)) | Later: `shaderrecomp` (M3) |
| The console command queue has two vtable layouts, the newer shifted by 2 (`Signal` 14 or 16, `PresentX` 30 or 32), and the command list has 160 slots | Not taken yet: reverted upstream, and they come from realdody/americas-recomp, not from this package. Until their source is known they stay out, as xbox-emulator's method map does, since a layout measured on console driver binaries is console SDK material | — |
| Microsoft's public Xbox GDK graphics reference gives the PC shapes for its d3d12_xs pages; `PresentX`, `D3D12XboxCreateDevice` and the root-signature flags have no page. The upscaler's pipeline slot 11 can answer 0, since none of its 29 programs uses scratch | Reported | Later: `runtime/gpu` (M3) |
| Its retail boot chain (`docs/retail_boot_chain.md`): one run of the 1.0.2.1 executable on Windows 11 with an RTX 4090, 13 stops, each the first failure with every fix before it in place: `XMemVirtualAlloc` forms and graphics bits, `XErrorReport`, `/GA` TLS index 0, `DStorageGetFactory`, physical pages, device slots 26, 48, 73 and 133, a placeholder reserve carved into physical ranges, and a descriptor-heap start the game predicts | Reported, one run; most of its fixes are local stand-ins. Two stops are done here, the rest are our boot plan | [boot.md](boot.md) |
| The executable's own DXBC containers | Checked here: 10 | [shaders.md](shaders.md), "The executable's own containers" |
| Pixel export valid mask: a pixel is written only if its lane was on in EXEC at the last export carrying `vm` and at the last export to that target (with no export, EXEC at the end decides); a later export to a target replaces the earlier for the whole wave; a lane with no data in the last color export stays unwritten even when a later null `done vm` export marks it valid. Measuring it needs `s_waitcnt expcnt(0)` after each export (`c26d78af`) | Reported, measured on gfx1036 | [shaders.md](shaders.md) |
| The output modifier applies only with IEEE mode off and denormals flushed for the destination's precision, so f64 and f16 destinations ignore it under the denormal setting Leonida reads for the game, while clamp still applies; `v_cvt_f32_f64` scales. `v_div_fixup`: a NaN first source gives ±inf, an f64 underflow cutoff, no overflow check, a signalling NaN passing through with IEEE off (`73fce549`) | Reported. Extends the f16 omod rows above to f64, and conflicts with xcl (`0056934`) on f64 destinations and on `v_cvt_f16_f32` | [shaders.md](shaders.md) |
| NaN in min, max and med3: with quiet NaNs the result is the same in all four IEEE and DX10_CLAMP combinations. One NaN operand gives the other operand's bits; all NaN gives src0's exact bits, payload and signalling bit included; −0 orders below +0; min3 and max3 are two pairwise steps; med3 with any NaN equals min3. The same holds for the f16 three-source and packed forms (`a31e3c68`) | Reported, measured. Corrects our earlier note, from xbox-emulator's probes, that the rules depend on IEEE mode. DXIL `FMin`/`FMax` promise neither payload nor −0 order, so these lower to selects | [shaders.md](shaders.md) |
| `v_cmp_lg` is ordered: false on NaN, (a<b) or (a>b), where HLSL's `!=` is unordered (`5bc7e8b0`) | Reported | [shaders.md](shaders.md) |
| 8- and 16-bit MUBUF: `_d16` loads write one half and keep the other (`_hi` the high half); ubyte and sbyte loads extend to 16 bits; byte and short stores write the low 8 or 16 bits, `_d16_hi` stores the high half; bounds cover the access's own 1 or 2 bytes, an out-of-bounds load returns 0 and a store writes nothing. HLSL has no 8-bit stores, so they need dword `InterlockedAnd`/`Or` merges (`8f208d09`) | Reported; out-of-bounds d16 loads not measured | [shaders.md](shaders.md) |
| The float compare-and-swaps (`ds_cmpst_f32`, global and buffer atomic cmpswap) compare as floats: −0 matches +0 and NaN never matches, even with identical bits, so D3D12's bitwise `InterlockedCompareExchangeFloatBitwise` needs a loop. Operand order: CMP then DATA for `ds_cmpst_f32`, DATA then CMP in global and buffer vdata (`0f2b802a`) | Reported, not measured on hardware | [shaders.md](shaders.md) |
| permlane: op_sel bit 0 is FI and bit 1 BOUND_CTRL. From an inactive source lane, FI=1 returns the value anyway; FI=0 keeps the old value with BOUND_CTRL=0 and returns 0 with BOUND_CTRL=1. Shuffles and ballots go before any predication (`5a78b4b7`) | Reported, measured on RDNA 2 and RDNA 3.5 | [shaders.md](shaders.md) |
| SDWA: 172 distinct forms (about 79k instructions) in its corpus, about 21k cases measured on gfx1036. Operand-select narrowing comes before neg and abs; `dst_unused` pads, sign-extends or preserves; a DWORD `dst_sel` on a 16-bit result zeroes the high half; a NaN result keeps the input's sign and payload; div:2 of −0 gives +0 (`f4cd3061`) | Reported | [shaders.md](shaders.md) |
| AMD driver 26.10.1 folds an ordered compare against a literal NaN to true in wave32 and mis-evaluated a med3 i16 with a constant operand (sum − min − max avoids it); omod turns a hardware −0 into +0; without float-controls preservation AMD folds `x != x` (`a284bad1`, `399fa980`) | Reported (Vulkan). For DXC: mark such operations `precise`, avoid literal NaN constants | [shaders.md](shaders.md), "Host constraints" |
| `s_memtime` reads a per-CU shader clock and `s_memrealtime` a constant-rate clock, both 64 bits into an SGPR pair, low word first; upstream turns both off by default (`bd3f061a`) | Reported. D3D12 has no portable shader clock | [shaders.md](shaders.md) |
| It refuses the `ds_*_src2` add, sub, inc and dec forms, the u64 add and sub forms, `ds_condxchg32_rtn_b64`, and LDS outside compute (`db22f2fa`) | Reported. Our census has 5 mesh programs and the ES and GS programs using LDS | [shaders.md](shaders.md) |
| FSSM stage records: 0x88 bytes from 0x100 with our hardware stage IDs, but unused fields are zero in 25,643 records; paired `0xDEADBEEF` qwords appear only in 1,055 records (860 blobs), those whose directory words at 0x120, 0x12C and 0x130 are `0xDEADBEEF`, and 25,643 records satisfy word 0x120 + word 0x12C = word 0x130 (`f6c6d11e`) | Reported. Conflicts with xcl's "unused fields hold `0xDEADBEEF`" row below; not yet checked against our census | [shaders.md](shaders.md) |
| Magic numbers its game-file checker rejects (`9b13eead`) | Taken as an idea, our own code: `scripts/check-clean.sh` now refuses FSSM, XBCH, the pipeline cache's header, PSIN, AWC, YSC, PAMH, MHIU, SPS1 and SGD5 | `scripts/check-clean.sh` |
| Windows' `CopyFile2` with the cancel flag already set returns `ERROR_REQUEST_ABORTED` and writes nothing; Wine 11.20's `CopyFile2` and Wine 9's `CopyFileExW` copy anyway. Wine 11.20's `MoveFileW` to a name differing only in case reports success without renaming (`6d55b156`) | Taken, our own code: `CopyFile2` and the `CopyFileExW` fallback check the flag themselves first (`test_files`). The rename is a Proton note | [io.md](io.md), "Under Proton"; `runtime/imports/kernelbase/files.cpp` |
| MSVC's `path::string()` throws for characters outside the ANSI code page, so module names are keyed by their wide name (`3092fdfc`) | Taken, our own code: our keys were UTF-8 already, and the error paths in `common`, the loader and `rpfscan` that used `string()` now convert with `g6::utf8`. No test runs from a non-ANSI folder yet | `common/include/g6/text.h` |
| Windows 10's d3d12.dll has no `ID3D12Device10`, and Windows 11 23H2's in-box WARP reports `RelaxedFormatCastingSupported` 0, so `CreateCommittedResource3` with castable formats fails there (`0e1ecbb8`) | Reported | [d3d12x.md](d3d12x.md) |
| Without an interactive window station (SSH, a service), DXGI returns `NOT_CURRENTLY_AVAILABLE` for `EnumOutputs` and `CreateSwapChainForHwnd`, so GPU tests skip there (`55200be1`) | Reported | [d3d12x.md](d3d12x.md) |
| Its parallel carve dropped a zlib header starting one byte before a window's end (`30356710`); a resource's final compressed system page can inflate to only 48 bytes (`6bb95d7c`) | Our 16-byte-aligned 64 MiB segments can't hit the first, but `test_rpf9` never crosses a segment boundary; we have no minimum stream size, so the second is fine | [rpf.md](rpf.md) |
| A NaN made on x86 or RDNA is 0xFFC00000 but 0x7FC00000 on ARM64 (`ea2cf301`); D3D12 can't `Sample` integer SRVs, so sampling integer formats goes through derivatives and a matching view (`64890ac2`) | Reported | [shaders.md](shaders.md) |
| The executable calls `XMemTransferMemory` once at start with direction 1 and 16 MiB | Taken: ours succeeds and moves nothing | [hle-status.md](hle-status.md) |
| A closed `XGameSave` container stays expired when its address is reused (`2cc83a80`) | Taken, our own code: our handles were object addresses, so a stale one could validate again. Handles are now values in reserved address space, never handed out twice, and each call holds the object it found (`test_game_save`) | [gdk.md](gdk.md), "XGameSave" |
| The four physical-page functions, measured at the title's call sites: allocate `(attributes, count*, pages)` with a count of 64 KiB pages, in and out, freed by the caller on a mismatch; free `(count, pages)`; map `(address, count, pages)` returning the address, where a null array unmaps `count` pages and a 0 entry one page; scatter `(addresses, UINT32 count, pages)`. Page numbers are 4 KiB frame numbers, nonzero multiples of 16 below 2^25. One section per allocation, one view per page; a page takes its window's protection, and pages in `PAGE_NOACCESS` windows get read-write views (`042c5f37`) | Ported into `memory_tracker` from `dde82f27`, with unit tests; the `VirtualAlloc` and `VirtualFree` side is still to do | [boot.md](boot.md), "Physical pages" |
| The title releases its page windows with plain `VirtualFree`, which an xmem shim doesn't see (`042c5f37`, inferred from overlapping reservations) | Reported. Agrees with xcl's row. Our `VirtualFree` is a stub, and becomes HLE through `memory_tracker` | [hle-status.md](hle-status.md) |
| `QueryApiImpl` classes: `37e56907` XNetworkingImpl, `973a344e` XGameImpl, `e349bd1a` XSystemImpl, `b884675d` XSystemAnalyticsImpl, `01acd177` XUserImpl, `8836fe87` XGameRuntimeFeatureImpl. Each class ID is also its base interface's ID, and one vtable answers every derived interface. XUserImpl's IDs run `01acd177`, `eb9bf948`, `1bf2f8c5`, `079415e3`, `26f3c674`, `5131d685` | Reported. Our runs ask for the first five and three classes Leonida doesn't serve (`af406016`, `f4faf4d4`, `03f0fe74`); we accept only `26f3c674` for XUser. Next for `runtime/gdk`, offline where it touches networking | [boot.md](boot.md), "Our runs" |
| `DllMain` gets a non-null `lpvReserved` on process attach for modules loaded at start, and null for `LoadLibrary` loads (`e0816d6a`) | Reported (from Microsoft's documentation). Ours always passes null; a loader change with a fixture test, to do | [execution-model.md](execution-model.md) |
| A signalling NaN stays signalling through `abs` and `neg` on `v_min` and `v_max`; a wave sees its own LDS writes in order without a barrier, which one AMD driver broke; `*_atomic_csub` subtracts clamped at zero; packed 16-bit `clamp` does nothing on multiply, min, and max; `s_bfm_b64` gives data, not a lane mask; a uniform value in a wave32 program on a 64-wide host comes from a lane of its own half; inactive lanes skip `image_atomic_*` (`07f81e76`, `0433058a`, `5dc7bb38`, `bc773d30`, `e0a8293f`, `4ce16cd5`, `f469596f`) | Reported; the csub, packed-clamp, and LDS-order rows are measured on AMD GPUs | Later: `shaderrecomp` (M3) |
| Wine before 10.1 laid out `_Mtx_t` differently, so a host `/MD` binary's timed condition-variable wait under Wine 9 never releases its mutex and runs out its timeout (`ee56bf93`) | Reported (measured). Later, for our host binaries under Proton (M5); the guest uses the console's own C++ runtime | [execution-model.md](execution-model.md) |

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
  arbitrary content packages, stays out** (hard rule 6). Its copy of Microsoft's MIT
  XTaskQueue and XAsync, vendored unmodified from libHttpClient, isn't needed: `runtime/gdk` has
  its own ([gdk.md](gdk.md)). Also for later: its startup trace and "stop at the first
  unimplemented import" switch, as ideas for phase 5; and its static scan of Game
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
* **Its changes at `86fd2fda` with nothing for us:** Vulkan and MoltenVK work (queue sharing,
  pipeline statistics, mesh pipeline streams, an f16 interpolant fallback, an implicit-layer
  crash); its wait-counter text parser; the new asset exporters (models, animation, textures,
  audio, navmesh, world, peds, PSIN, texture detiling, the RPF dump tool), of which only the
  48-byte page fact is kept; the harness that runs xcl's binaries for comparison; and CLI,
  build, launcher, translation and README changes.
* **Its changes at `976b3ec9` with nothing for us:** device slots 78 to 80 as frame-interval
  and frame-event methods, whose source is another project of unknown provenance (our own run
  will show them at M3); its Vulkan validation and barycentric fallback; the XMA2 crate (FFmpeg's
  C decoder is the more direct source at M4); and its asset, BVH, and tooling changes.
* **Its changes at `dde82f27` with nothing for us:** help-text pointers in its docs, option
  checks in its tools, a readback check in its cross-GPU sweep, and fixes to its movie notes and
  audio tag tooling.

### Next review

1. Run `git log dde82f27..origin/main` in a scratch clone of the continuation, outside the
   repo.
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

**Last reviewed: commit `f89e410` (2026-11-23).** Upstream rewrote its history on 2026-11-22:
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
| `XTaskQueue` behaves like libHttpClient's open-source (MIT) task queue | Taken: `runtime/gdk` follows its behavior, with no code taken, and `test_gdk` checks it | `runtime/gdk`, [gdk.md](gdk.md) |
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
| `XGameSaveReadBlobDataResult`'s buffer is the array of 24-byte blob records followed by the data; `XPersistentLocalStoragePromptUserForSpace` completes only with `S_OK` or `E_ABORT` | Reported; both follow from the public GDK | Taken for `XGameSave` ([gdk.md](gdk.md), "XGameSave"); later for `XPersistentLocalStorage` |
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
| `XGameSave` calls on a closed handle return `E_GS_HANDLE_EXPIRED` (0x8083000D); it doesn't serve slots 30 to 32 (`XGameSaveFiles*`) | Reported | Taken: [gdk.md](gdk.md), "XGameSave" |
| `XAsync` rules from conformance tests derived from libHttpClient's and also run against the PC Game Runtime: `XAsyncBegin` with no queue and no process queue fails with `E_NO_TASK_QUEUE` (0x800701AB), and on a terminating queue with `E_ABORT`; when the provider's begin step fails, `XAsyncBegin` still returns `S_OK` and completes the call with that failure, and `E_PENDING` from it keeps the call pending; a second `XAsyncSchedule` before the worker has run, and a work step that returns success without completing, give `E_UNEXPECTED`; `XAsyncRun`'s work callback result becomes the call's; `XAsyncGetResult` with a null buffer gives `E_INVALIDARG`, checked before the size, and too small a size `E_NOT_SUFFICIENT_BUFFER` (0x8007007A); `XAsyncGetResultSize` on a pending or failed call returns that status and writes 0 | Taken: `runtime/gdk` behaves this way, and `test_gdk` checks each rule with tests of our own, so none of its test code is taken. Agrees with xcl's rows on the same contracts | `runtime/gdk`, [gdk.md](gdk.md) |
| The d16 buffer loads write half a VGPR and keep the other: `buffer_load_ubyte_d16`, `_sbyte_d16` and `_short_d16` (MUBUF 0x20, 0x22, 0x24) the low half, their `_hi` forms (0x21, 0x23, 0x25) the high half. `buffer_store_byte_d16_hi` and `buffer_store_short_d16_hi` (0x19, 0x1B) store from the high half | Taken: the MUBUF and FLAT tables name them (the opcode numbers are AMD's RDNA 2 reference), and the register model counts the kept half as read for every d16 load that writes half a VGPR, the LDS `ds_read_*_d16[_hi]` ones included. No 1.0.2.1 program uses the buffer or flat forms; four of its instructions are `ds_read_u16_d16_hi` | `tools/shaderrecomp/src/rdna2*`, `regs.cpp` |
| A freed `XMem` range can be handed out again under a resource that still covers it, so a cache of committed pages has to be cleared on every free path | Reported | Later: `runtime/imports/xmem` and `memory_tracker` (M3) |
| A save container or blob name too long for the host file system is stored under a hashed file name, with the real name in a file beside it | Reported | Taken in our own form: a long name keeps 80 characters and a hash, and the manifest holds the real name ([gdk.md](gdk.md), "XGameSave") |
| `CopyFileExW`, `MoveFileWithProgressW` and `ReplaceFileW`, its backup path included, need the same path redirection as `CreateFileW` | Taken: every move, copy and replace function is HLE on the mounts since `2d4307b` | [hle-status.md](hle-status.md), [io.md](io.md) |
| A gamepad's user association: plugging the pad sends `XUserDeviceAssociationChanged` from no user to the default user, unplugging the reverse; `XUserFindForDevice` resolves only the connected pad and gives `E_GAMEUSER_USER_NOT_FOUND` for any other ID; `XUserFindControllerForUserWithUiAsync` returns the pad's ID, and with no pad completes with `E_ABORT`, as a dismissed pairing dialog does; GameInput reports the same ID as device and root device | Reported. Agrees with xcl's Minecraft row | Later: `runtime/gdk` and `runtime/input` (M4) |
| Unregistering an XUser event callback waits for a call still running on another thread, and a callback that unregisters itself doesn't deadlock | Reported. Our task-queue monitors follow the same rule ([gdk.md](gdk.md)) | Later: `runtime/gdk` |
| Console topology type 0x13 becomes a triangle topology in a non-mesh pipeline state stream, and rect-list draws (IA topology 17) through such a pipeline need the expansion too | Reported | Later: `runtime/gpu` (M3) |

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
  transcendental models, taking AMD's rocjitsu headers (MIT) directly for `log`, `exp`, `sin`
  and `cos`; and its hardware-recorded test vectors for VALU and image atomics.
* **Fortnite and Trackmania fixes** (`6e81ffc..de1e1da`). Title-specific, with nothing GTA VI
  or the console's shared behaviour needs.
* **Its signed token requests** (`XUserGetTokenAndSignature`, at `4eec5e8`). They sign requests
  to Xbox services; ours report an offline error (hard rule 5).
* **Its profiler and Trackmania work at `f89e410`.** Title-specific.

### TomBox

xbox-emulator's credits now say TomBox publishes no licence, and its repository has no licence
file. Its `tombox-xshader` crate, though, declares `MIT OR Apache-2.0` in its `Cargo.toml`, from
the crate's first commit (`b8fdbe4`, 2026-11-17) to its head. Compared at TomBox `7e8064f`, the
base xbox-emulator names, 483 of the 519 lines of `rdna2.rs` and 267 of the 410 lines of
`rdna2/mnem.rs` are TomBox's. So our three `rdna2` files credit TomBox too, and we use its part
under MIT, whose notice is in [NOTICES.md](../NOTICES.md). Anything else from TomBox gets the
same check first.

### Next review

1. Run `git log f89e410..origin/main` in a scratch clone outside the repo.
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
repository came back with its history replaced by one commit, `39a9145`, which holds only a
notice that the project has closed. There is nothing new to review; the findings below stand as
recorded.

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

**Last reviewed: commit `db5dc26` (2026-11-23).** No code has been copied.

### Taken

| Finding | Status here | Used in |
|---|---|---|
| An executable always has TLS index 0, and code built with `/GA` reads slot 0 of the TLS array without reading its index variable. On a donor's index such code reads another module's block | Checked: clang-cl and cl.exe both emit it, and `test_exe_tls` runs an original `/GA` fixture. Our solution is our own: the host executable lends the start of its own block | [execution-model.md](execution-model.md), "TLS and the TEB" |
| `XSystemGetDeviceType` is computed in the title's thunk library, from the registry value `ConsoleMode` under `HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM`: 0x64 Series X, 0x60 Series S, no value PC | Checked on the thunk in `XCurl.dll`. The registry HLE answers 0x64 | [hle-status.md](hle-status.md) |
| `XMemVirtualQuery` fills a `MEMORY_BASIC_INFORMATION`, then a word at +48 whose bits 0 and 1 give GPU access | Checked at the call sites in the replaced DLLs | [hle-status.md](hle-status.md) |
| The structure `XMemGetWorkingSetStatistics` fills grew between GDK versions, so only +0 (the limit) and +8 (bytes used) may be written | Checked at the call site, which reads +8 | [hle-status.md](hle-status.md) |
| FSSM stage records: from byte 0x100, one 0x88-byte record per stage, holding the hardware and API stage numbers; unused fields hold `0xDEADBEEF` | Checked: `shaderrecomp scan` reads them in all 26,503 records. Conflict: Leonida (`f6c6d11e`) finds unused fields zero in 25,643 records, with `0xDEADBEEF` only in 1,055; the record layout stands, the filler isn't settled | [shaders.md](shaders.md) |
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
| `XAsync` and `XTaskQueue` contracts, as libHttpClient has them: `XAsyncCancel` only asks the provider, and a provider that can't cancel runs to its end (completing with `E_ABORT` there freed the payload under a running work step); a failing begin step fails the call through its block while `XAsyncBegin` and `XAsyncRun` return `S_OK`; `XAsyncComplete` with `E_PENDING` completes nothing; `XTaskQueueTerminate` runs the delayed callbacks still waiting, as canceled, before its own callback; a terminated queue refuses submits, delayed submits, waiters and `XAsyncSchedule` with `E_ABORT` | Taken: `runtime/gdk` behaves this way, written from libHttpClient's behavior, and `test_gdk` checks each contract. No code taken | `runtime/gdk`, [gdk.md](gdk.md) |
| Save names that Windows would change get escaped, not refused: `.`, `..`, device names with any extension (`nul .txt` too), and names ending in a dot or a space. Deleting a container named `..` once deleted every save | Reported | Taken: folder names are prefixed and escaped, and invalid names are refused ([gdk.md](gdk.md), "XGameSave") |
| Desktop drivers don't keep a 2-D or 3-D compute group's threads in order within a wave: an RX 9070 lays them out in 2x2 quads (in an 8x8 group, lanes 0, 1, 4, 5 hold `SV_GroupIndex` 0 to 3), and NVIDIA's waves of an 8x8x4 group don't hold consecutive indices either; 1-D groups stay in order. The console's do. So a translated 2-D or 3-D compute program that uses DPP, readlane, `ds_bpermute` or `v_mbcnt` reads the wrong lanes unless each host wave takes the next block of console threads itself when it starts | Reported (RX 9070, RTX 5070 Ti) | [shaders.md](shaders.md) |
| Splitting a wave64 program into two 32-lane host waves that exchange through groupshared: NVIDIA (range 32 to 32) refuses a `[WaveSize(64)]` compute pipeline. Only masks, `readlane`/`readfirstlane`, `ds_bpermute` and GLC scalar loads need the exchange; DPP, swizzles and `permlane` stay in a half. It cost 1.3 times with one console wave per group and about 5 times with four. Wave64 vertex and pixel programs that read lanes 32 to 63 stay open on NVIDIA, where those lanes don't exist. The exchange waits on its partner, which assumes co-resident waves make progress; WARP doesn't promise that, and has neither 32 nor 64 lanes, so it can't test this form | Reported (RTX 5070 Ti, RX 9070). Supersedes the row above on splitting into halves | [shaders.md](shaders.md) |
| An AMD driver gave a later `a < b` the unordered result of an earlier `!(a >= b)` on the same operands, so float compares test NaN explicitly; f64 `floor`, `ceil`, `trunc`, `fract`, `ldexp`, `sqrt`, `rsq` and conversions need their own handling past the f32 and int64 ranges | Reported | Later: `shaderrecomp` (M3) |
| `v_fract_f32` of ±inf or NaN is NaN | Reported. So the clamp below 1.0 in Xbox2Windows's row must be a compare and select: HLSL's `min` returns the other operand for a NaN, which would turn NaN into the bound | [shaders.md](shaders.md) |
| The FSR 4 upscaler's shaders translate: each of its 29 `ACCC` parts holds an `FSPX` container (an entry count at +8, then 16-byte entries from +0x10; type 0 holds the thread-group size and RSRC1 and RSRC2, type 6 the RDNA 2 code after a 0x24-byte header, type 7 32-byte resource records). The DLL tries `DeserializeComputePipelineStateX` first. One upscaled frame is 20 dispatches; translating `v_dot4` to the SM 6.4 `dot4add` intrinsics cut DXC time on the largest pass from 604 to 74 s (`fdeacf0`) | Reported, not checked on our copy. Reopens replacing the upscaler with the PC FidelityFX SDK | [shaders.md](shaders.md), [side-dlls.md](side-dlls.md) |
| Wave64 vertex and pixel programs on 32-lane GPUs: its 13,210 match our 2,236 VS and 10,974 PS. Reads of lanes 32 to 63: 1,082 whole-wave reductions in 436 programs, 355 waterfall reads staying below lane 32, and 200 reads of SGPR spill slots in lanes 32 to 47 in 11 programs. On every GPU, 287 pixel programs lose spill slots in lanes a partial wave never launched, and min reductions read 0 from unlaunched lanes on an RTX 5070 Ti (325 of the 436) (`af0311d`, `fc60bf5`, `55156f0`) | Reported; the 13,210 agrees with our census. For `shaderrecomp`: spill slots become scalar variables, reductions are recognised whole | [shaders.md](shaders.md), "Wave size" |
| The clamp bit saturates the integer adds and subtracts (`v_add_nc_u32`, `v_sub_nc_u32`, `v_subrev_nc_u32`, `v_add3_u32`), the packed 16-bit add, sub and mad, and the integer dot products; omod applies to f64 results and `v_cvt_f32_f64`, and on `v_cvt_f16_f32` in f32 before rounding; a cube's face coordinate is face + 8 × cube index, every cube is declared as a cube array, and `image_get_resinfo` gives 6 layers per cube (`34efd81`, `0056934`) | Reported. The omod rule conflicts with Leonida's `73fce549` on f64 and f16 destinations | [shaders.md](shaders.md) |
| Integer pixel inputs went back to being converted to float, because raw bits made XDefiant's UI text disappear; the real cause is open (`58309b9`) | Reported. Conflicts with the raw-bits rule in the ISA row above; neither is settled | [shaders.md](shaders.md) |
| CPU check: it refuses to start without AVX, AVX2, FMA, F16C, BMI1 and BMI2, LZCNT, POPCNT, MOVBE and SSE4.2, with an override; it logs `std::exception::what()`; a thread dump only reads contexts while threads are suspended and unwinds after `ResumeThread`, since unwinding takes locks a suspended thread may hold (`1a51a82`) | CPU check taken, our own code: the runner refuses a host missing any feature the guest may use, and names them. `what()` in our crash report is optional for later; the thread-dump rule is a note for when we add one | [execution-model.md](execution-model.md), "The runner" |

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
| `GetModuleHandle` on a DLL the title never imported or loaded returns null on Windows, so a stand-in for every DLL the policy covers made an optional DLL look present | Taken, our own code: a policy DLL counts as loaded once a guest module imports or loads it, and an API set while its host DLL is loaded, as measured on the host. `test_loader` checks both | [hle-status.md](hle-status.md) |
| `CreateFile` with `TRUNCATE_EXISTING` needs `GENERIC_WRITE` and fails with `ERROR_INVALID_PARAMETER` otherwise | Checked here on Windows 11 26200: the check comes before the file is looked for, and neither `GENERIC_ALL`, `FILE_WRITE_DATA` nor `MAXIMUM_ALLOWED` passes it. Taken, our own code, for the read-only mount, which also reports a missing directory or file before refusing a write; the host does the rest | [hle-status.md](hle-status.md) |
| A truncated ANSI result is cut at a whole character | Taken: `GetModuleFileNameA` and the `WIN32_FIND_DATAA` names now cut after a whole character in the active code page (`g6::narrow_prefix`, tested by `test_text` on code pages 932 and UTF-8) | [io.md](io.md) |
| Wine's loader looks `PEB.ImageBaseAddress` up in its own module list whenever a thread starts, so pointing it at a manually mapped image crashed the first guest thread under Wine | Checked here: our loader never writes the field, and `GetModuleHandle(NULL)` is HLE, so Proton is safe from it | `runtime/loader` |
| `TlsGetValue` clears the last error when it succeeds, so a hook that works after calling the original has to restore `GetLastError` | Reported; Microsoft documents it. Our file HLE ends on the host call | `runtime/imports` |
| From its own stress tests of `XTaskQueue` and `XAsync`: `XTaskQueueDispatch` with an infinite timeout returns false once the queue is terminated, and at once on a terminated queue; a waiter on a port above 1 gets `E_INVALIDARG`; concurrent `XAsyncGetResult` calls run cleanup once; `XAsyncRun` on a terminated queue returns `S_OK`, ends the block with `E_ABORT`, and never runs the completion routine; `XErrorReport` calls its callback outside its lock, so the callback may call it again | Partly agrees: ours returns false from dispatch on a terminated port, refuses a bad port with `E_INVALIDARG` (`test_gdk` checks it), and runs cleanup once. **Conflicts** on `XAsyncRun`: xbox-emulator's row, run against the PC Game Runtime, has `XAsyncBegin` fail with `E_ABORT` on a terminating queue, and ours follows that. Its `XTaskQueueTerminate(wait=true)` also runs a Manual port's items itself, where ours keeps Microsoft's rule | [gdk.md](gdk.md) |
| `XGameSave`: a container name must be 1 to 256 characters, otherwise `E_GS_INVALID_CONTAINER_NAME`, sync and async (an empty name deleted the provider's whole save folder); the async delete runs when the queue dispatches it, in order with blob updates, and on a terminated queue gives `E_ABORT` and deletes nothing; a blob is written to a unique temporary file beside it and moved over it with write-through, and leftovers are never listed; names match case-insensitively; `XGameSaveInitializeProviderResult` with a null out pointer returns `E_INVALIDARG` and leaves the result | Reported | Taken, except that a whole update, not each blob, is made safe: new files, then one manifest rename ([gdk.md](gdk.md), "XGameSave") |
| `XPackageGetIdentifier` takes three arguments (path or name, buffer size, buffer); `XSystemGetConsoleId` gives the documented PC id, `00000000.00000000.00000000.00000000.00` (38 characters), and titles size the buffer for exactly that; `XUserGetGamertag` cuts classic to 15 characters, unique modern to 16 and modern to 96 bytes, at a character boundary; `XUserFindControllerForUserWithUi` returns the pad's device ID; `XPackageGetUserLocale` matches the OS locale against the declared languages, exactly, then by primary language, then takes the first (its own choice) | Reported. The console ID agrees with xbox-emulator's 39-byte row | Later: `runtime/gdk` |
| Most of the `s_round_mode` writes in a pack of the same 26,698 programs set f16/f64 rounding toward zero ("95 %"; 122 of 670 sampled programs change the mode) | Checked here: our decoder finds no `s_round_mode` or `s_denorm_mode` in any of the 26,698 programs. The writes are `s_setreg_imm32_b32` to the f16/f64 rounding field, toward zero in 4,776 pixel programs and nearest even in 1,147 pixel and 1,251 other programs, so these are the same writes under another name | [shaders.md](shaders.md) |
| ISA rules from AMD's reference and one NVIDIA measurement: `v_mad_f16` isn't fused (the product rounds to f16 first); `ds_swizzle` and `ds_bpermute` read 0 from inactive source lanes; an f64 instruction takes a 32-bit literal as the high word, integer and bitmask ones as the low word; f64 negate and absolute value only flip or clear the sign bit (DXC writes `-x` as `-0.0 - x`, and NVIDIA kept a NaN's sign); `v_cvt_pknorm` from f16 works on the f16 bits; small unsigned float formats turn negatives into 0 and keep NaN; `v_cmp_class_f16` mustn't go through f32, which quiets signalling NaNs; `v_interp_mov` and `v_interp_p2` read integer inputs as raw bits (conflicting: xcl's `58309b9` went back to converting them to float, see below); `ds_*_addtid_b32` addresses `M0[15:0]` plus the offset plus 4 times the lane; signed BFE keeps the sign when the field runs past the end | Reported | Later: `shaderrecomp` (M3) |
| Masks read one lane at a time (`v_cndmask` selectors, carry-ins) need no exchange between the halves of a split wave64 program, which brings the cost from 1.6 to 4.1 times down to 1.04 to 1.16 times (RTX 3090) | Reported. Updates the splitting row above | [shaders.md](shaders.md) |
| Desktop D3D12: after device removal, fences report `UINT64_MAX`, so check for removal before trusting a readback; Windows 10's built-in D3D12 stops at shader model 6.5, and `ID3D12SDKConfiguration1::CreateDeviceFactory` loads the Agility SDK at run time from an ANSI path with a trailing backslash; Agility 1.620 and later swap a command list's vtable on `Close` and `Reset`, so patched vtables have to be re-adopted; a placed texture whose initial state the PC refuses is retried with `COMMON` | Reported | Later: `runtime/gpu` (M3) |
| Title memory: console heap type 4 is CBV/SRV/UAV in `CopyDescriptors` too; buffers placed in an `OpenExistingHeapFromAddress` heap are row-major, with console-only flags and state bits stripped; uncommitted 4 KiB pages inside a 64 KiB tile, guard and `PAGE_NOACCESS` pages included, are holes; reservation tracking changes only after `VirtualFree` succeeds, and `VirtualFreeEx` on the current process takes the same path | Reported | Later: `memory_tracker` and `runtime/kernel` (M3) |
| Frame pacing: `SetFrameIntervalX` accepts values either side of the nominal length (16,666 and 66,666 µs are both valid), and `PresentX` can arrive on another queue than the first present. With one descriptor range, `OMSetRenderTargets` has one handle for N targets | Reported | Later: `runtime/gpu` (M3) |
| NTFS ignores `FindExSearchLimitToDirectories` and returns files too | Checked here: ours passes the option to the host, so the guest sees the same | [io.md](io.md) |
| `MicrosoftGame.config` has to be read with its comments stripped, since a commented-out `Executable` or `Identity` was taken as the real one, and may be UTF-16 big-endian | Reported | Later: when we parse it |
| DirectStorage on the PC: `GetErrorEvent` returns a duplicate the caller closes; `RetrieveErrorRecord` clears the record and resets the event; refused requests are reported in queue order at `Submit`; `Close` waits for requests in flight and fails pending status entries | Reported, its reading of the PC API | Later: `runtime/io` (M2) |
| GameInput's `UnregisterCallback` waits for a call still running, and mustn't deadlock from inside the callback; `QueryInterface` answers only the v0 interface IDs; `XInputGetState` on an empty slot asks the driver every time, so empty slots are polled at most once a second; keyboard input by physical scancode works on AZERTY layouts, and is reset when the window loses focus | Reported | Later: `runtime/input` (M4, keyboard and mouse) |
| Video-memory arenas: a later placement can straddle an existing arena, so each title address must map to one backing heap; xcl merges overlapping arenas (`70d7e0a`). Queue slot 18 is `GetDesc` (16 bytes through a hidden return pointer) in the older layout and `GetTimestampFrequency` (8 bytes) in the newer, and can come before the first `Signal` (`a01d61c`). Call the native `Reset` before dropping a list's tracked state, since a failed one keeps the old recording (`c806a0b`). Frame statistics for a token carry that frame's own origin time with 2 to 3 frames in flight (`23c53fc`) | Reported | [d3d12x.md](d3d12x.md) |
| Memory windows: picking one from a 64-bit address must include the carry out of the low dword (a fix reverted because AMD's shader compiler crashed on one large program); HLSL 2021's `?:` short-circuits into a branch, so use `select()`; mesh and geometry programs need the windows too. DXC: programs translated with program-counter loops (13–17%) take 53–75% of compile time, a compile peaks at 0.6–0.7 GB, and workers stay at normal priority. At start it checks SM 6.6, the mesh tier, the wave width and DXC (`a787c52`, `70f8dc4`, `07f12e8`, `3304bb1`, `56fa1dc`, `8ba374c`) | Reported | [d3d12x.md](d3d12x.md), [shaders.md](shaders.md) |
| A physical page's number counts 4 KiB frames: a 64 KiB page's number is a nonzero multiple of 16, and 0 unmaps. In `XMemMapPhysicalPagesScatter(addresses, count, pages)`, a 0 entry unmaps that one address (`9add721`) | Reported. Agrees with Leonida's measurement; taken with the physical pages | [boot.md](boot.md), "Physical pages" |
| The title allocates about 5.7 GB of physical pages at once and maps them into a reservation of about 6 GB of its own (`66e8e97`) | Reported (measured on its run). So pages are backed one section per allocation call, not one per page | [boot.md](boot.md), "Physical pages" |
| The title manages placeholders through `VirtualAlloc` and `VirtualFree`: it splits one with `MEM_RELEASE` and `MEM_PRESERVE_PLACEHOLDER`, replaces each piece with a physical reserve, returns pieces to placeholders, and coalesces them. Its first placeholder starts with its view descriptor heap, which shaders address with 32 bits, so placeholders go below 4 GiB (`9add721`) | Reported. **Checked here:** the executable imports `VirtualAlloc`, `VirtualFree`, and `VirtualQueryEx` from the memory API set, which is still stubbed; they become HLE through `memory_tracker` | [boot.md](boot.md), [hle-status.md](hle-status.md) |
| The executable exports `__XMemAllocationHooks__`, the title's allocate and free functions. The console's D3D runtime takes descriptor heaps and display targets from it, and the game asserts that a heap starts at the memory its hook gave (`9add721`) | **Checked here:** it is the executable's only export, in `.data`: two pointers into `.text`, one into `.rdata`, then zero. It explains Leonida's stop 13. At M3 those objects are built at the hook's memory | [side-dlls.md](side-dlls.md), [boot.md](boot.md) |
| `chad.dll` is XDsp, the audio processor's DSP. The game stops at a breakpoint when `XDspConnectWithMaximumStreamLimit` fails (`66e8e97`) | **Checked here:** the executable imports five XDsp functions. They are honest `E_NOTIMPL` stubs for now, with `AcpHal.dll`, `xapu.dll`, and `xg_xs.dll`; before, their stubs returned zero, which reads as `S_OK` | [hle-status.md](hle-status.md) |
| The newer DirectStorage factory (`62959d11-…`) and queue (`3c02cb0c-…`): factory slots 3 to 8, and 10 with a 0x20-byte description; queue, file, and status array slots; the 0x48-byte request and 0x290-byte error record (`9add721` and its earlier notes) | Taken, our own code: `runtime/io` serves those slots (`test_io`). The names of the request options bits come from the console SDK's header, so they are not taken | [io.md](io.md), "DirectStorage" |
| The game's crash reporter sends a minidump to an outside server at every crash, over WinHTTP and Winsock (`9add721`) | Reported. Rule 5 already covers it: those imports bind offline here | [hle-status.md](hle-status.md) |
| Fence slot 12 in the newer layout returns the address of the fence's completed value, which the title waits on with `monitorx` and `mwaitx` (`9add721`) | Reported. Our scan's 4 MONITORX sites; this host lacks MONITORX, so they need emulating on Intel | [scan.md](scan.md) |
| GTA VI uses the newer D3D12.X layout. Device slots 70 and 71 make views of memory with no resource, 108 is `GetResourceAllocationInfo2`, and 127 and 133 take a barrier layout where the PC takes a state. Compute queues come with flags 2 and 3, list slot 139 is `Reset` with one more argument, full-screen passes use rect lists (topology 17, type 19), and pipeline streams may lack a topology type (`9add721`, `66e8e97`) | Reported. Later (M3) | [boot.md](boot.md), slot 133 |
| During loading the game creates about 3,500 shaders, 2,076 of them compute; a mesh program can sit in a stream's AS subobject (`db5dc26`) | Reported. Later: `shaderrecomp` (M3) | [shaders.md](shaders.md) |

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
* **Its changes at `ea4bd62` with nothing for us:** networking (rule 5), its write overlay
  (we have none), host-wide hooking, audio internals, and its tests and tooling.
* **Its changes at `484dd57` with nothing for us:** Ogg Opus trimming (GTA VI's AWC Opus
  streams are raw packets), issue templates, and precompile tooling. Its held f16
  directed-rounding change fails its own GPU test, so there is nothing to take from it yet.
* **Its changes at `db5dc26` with nothing for us:** the 1D and 3D texture layouts and
  `XGSuggestSwizzleMode`'s choice, checked against the console's texture-layout library (console
  SDK material); the names of DirectStorage's request options, from the console SDK's header;
  game shader names in its notes; and its trace and dump switches.

### Next review

1. Run `git log db5dc26..origin/main` in a scratch clone outside the repo.
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
* [americas-recomp](https://github.com/realdody/americas-recomp) (GPL-2.0-only, with 88 files
  relicensed MIT in its `REUSE.toml`): another project on Xbox Series titles, which Leonida
  cites for console command queue and list vtable layouts. Not tracked yet; its GPL-2.0-only
  parts would be facts only, as with xcl.
* [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) (University of
  Illinois/NCSA): compiles HLSL to DXIL. xbox-emulator's shader backend uses it.
* [xgameruntime-rs](https://github.com/minecraft-linux/xgameruntime-rs) (MIT, reviewed at
  `c523673a`): a Rust Game Runtime for running GDK titles on Linux. Its threading vtable order
  matches the slots we checked on the console's libraries, except slot 28, which it calls
  `XTaskQueueIsEmpty` while the thunk there passes no arguments ([gdk.md](gdk.md), "Slots"). Its
  entry-point signatures (`QueryApiImpl`, the three initializers, `UninitializeApiImpl`) agree
  with the start-up code we read, except that the code passes 64-bit packed versions to the two
  newer initializers, where it takes 32-bit ones. Its XUser vtables agree with the console's
  thunks up to slot 36, at 42, and on the device interface, but not at 37 and 38, where it puts
  device association and the console's thunks pass too few arguments for it ([gdk.md](gdk.md),
  "XUser"). Its XGameSave order agrees with the console's thunks through slot 32; it names slots
  33 to 49 only in comments, which we haven't confirmed ([gdk.md](gdk.md), "XGameSave"). Facts
  only so far; its sign-in and token code is never taken (hard rule 5).
* [xodus-gaming/xgameruntime](https://github.com/xodus-gaming/xgameruntime) (LGPL-2.1) and
  [xodus-gaming/xgameruntime-docs](https://github.com/xodus-gaming/xgameruntime-docs) (GPL-3.0):
  the Wine IDL and the class and slot documentation that hle-status.md compares. Neither is
  copied; GPL-3.0-only text would need asking first.
* JPTuba9/xlaunch-vi: a compatibility layer for this game, published 2026-11-23 with no
  licence, so nothing is taken from it.
