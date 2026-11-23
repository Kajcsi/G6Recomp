# Shaders

What the game's shader blobs contain, and what that means for `shaderrecomp` and `runtime/gpu`.
`CLAUDE.md` asks for this before a translation path is chosen. Prose and counts only: no
disassembly, no dumped shaders.

Each finding says where it comes from. **Checked here** means we measured it on the 1.0.2.1
files with our own scripts (kept outside the repo). **Reported** means another project
published it and we haven't checked it yet (see [related-work.md](related-work.md)). A reported
finding must be checked here before a design depends on it.

## Side DLLs

Two side DLLs carry compute shaders as DXBC containers in their data.

### XboxBvhBuilder

* **Checked here.** 52 containers. Each holds Shader Model 6.5 DXIL compute programs and a
  root signature part (`RTS0`, version 1.1). All 52 root signatures set flags `0x03000000`.
  Those bits aren't defined in desktop D3D12. There are 16 distinct root signatures.
* **Reported.** The desktop serializer rejects those root signatures with `E_INVALIDARG`.
  Clearing exactly those two bits makes serialization and `CreateRootSignature` succeed. The
  DXIL modules target `dxil-ms-xb2` and carry `xbox.compiler.options` metadata, so the desktop
  validator rejects all 52. Most modules call console-only `xbox.` intrinsics:
  * cache-controlled loads and stores;
  * a lane swizzle;
  * float atomic min and max on global memory;
  * `Med3_F32` used as a clamp;
  * three-input min, median and max;
  * packed half-precision operations.

  33 distinct intrinsics in all.

The builder writes the console GPU's own BVH format, and desktop DXR acceleration structures
are opaque. That makes the plan in [side-dlls.md](side-dlls.md) to replace the DLL with
desktop DXR builds rather than translate these shaders.

* **Reported** (xbox-emulator): `0x02000000` is a spill area for compute root arguments, and the
  pair `0x03000000` marks a root signature internal to XDXR, the console's DXR layer.
  vkd3d-proton accepts the pair; Microsoft's D3D12 refuses each bit on its own (Xbox2Windows).
* **Reported** (Leonida): it counts 53 containers where we count 52, and describes the layout
  of the builder's batched build descriptor. The container it counted twice is the one for
  `GetBVHCompactedSize`, created at the 47th and 53rd creations. Its translator now handles 51
  of the 52 programs.
* **Risk, reported** (Leonida): `core_bvh4.rpf` holds about 272,000 prebuilt bottom-level
  acceleration structures in the console's node format (64-byte nodes, version 6), with no
  source geometry. The builder's exported class has a rehydrate method
  ([side-dlls.md](side-dlls.md)), which fits a game that loads those structures rather than
  building them. Then a desktop DXR build has nothing to build from, and the ray-traced passes
  would need the console format kept and intersected in software instead (see "Ray tracing"
  below). Check what the game does with that archive before M3 commits to either.
* **Reported** (Leonida, later): those dehydrated structures hold primitive references only;
  the triangles come from the drawables in `levels.rpf`. The rehydrate method runs on the CPU
  and makes no device calls. A structure's size field means a total of 64·(field+1) bytes.
  Destinations must lie below 2^40 and be 256-byte aligned, and the builder reads geometry
  descriptors with CPU loads. So the geometry does exist, which weakens the risk above.
  Whether the game takes the rehydrate path is still open.

### amd_fidelityfx_upscaler_xbox

* **Checked here.** 29 containers, each with an `RTS0` part (version 1.1, flags 0, 3 distinct)
  and an `ACCC` part, and no DXIL. The internal name and the exports say this is FSR 4 for
  Scarlett, using the public FidelityFX API.
* **Reported.** All three root signatures serialize and create unchanged on desktop D3D12. The
  provider reports version `4.1.1`. The frame-generation provider the loader DLL can ask for,
  `amd_fidelityfx_framegeneration_xbox.dll`, isn't in the package. **Checked here:** it isn't in
  our copy either.

With no DXIL, there is no DXIL to translate. The plan in [side-dlls.md](side-dlls.md) provides
the FidelityFX API from the public PC FidelityFX SDK instead. xcl's report below reopens that
choice: the `ACCC` parts hold native programs, which a gfx1013 translation can take.

* **Reported** (xcl, `fdeacf0`), not checked on our copy:
  * Each of the 29 `ACCC` parts holds an `FSPX` container, the console driver's format. The
    entry count is at +8, then 16-byte entries from +0x10: a type, an offset and a size.
  * A type 0 entry holds the thread-group size as three 16-bit words at +0x0C, and
    `COMPUTE_PGM_RSRC1` and `COMPUTE_PGM_RSRC2` at +0x14 and +0x18. A type 6 entry holds the
    RDNA 2 code after a 0x24-byte header, ending at the last `s_endpgm`. A type 7 entry holds
    32-byte resource records.
  * The DLL first tries `DeserializeComputePipelineStateX` and falls back to
    `CreateComputePipelineState`.
  * One upscaled frame is 20 dispatches, 24 barriers, 2 clears and 4 copies: about 11 ms on an
    RX 9070.
  * xcl translates these programs. Lowering `v_dot4_*_i8` and `_u8` to shader model 6.4's
    `dot4add_*packed` shrank the biggest pass's DXIL from 1.55 to 1.23 MB and its DXC time from
    604 to 74 s. Before that, AMD's driver refused the pass with `E_OUTOFMEMORY`. Casting the
    result of `dot4add_i8packed` to a 64-bit integer crashes DXC.
  * So translating the upscaler's own programs is an option again, beside the PC FidelityFX
    SDK. Which one M3 takes is open.
* **Reported** (Leonida, from these 29 programs, which carry both a root signature and their
  `COMPUTE_PGM_RSRC2`): the user SGPR count is 4 + one per descriptor table + two per root
  CBV, in signature order. s0 to s3 are a driver prefix, and tables are 32-bit pointers from
  s4. That is the only place in the package where a root signature and its native program sit
  side by side.

## The executable's own containers

**Reported** by Leonida (`86fd2fda`), then **checked here** on the 1.0.2.1 executable:

* It carries 10 DXBC containers in its data, 924 to 992 bytes each and 9,548 bytes in all by
  our reading (Leonida counts 9,520). One more `DXBC` match isn't a container.
* One is a vertex shader and nine are pixel shaders.
* Each has the parts `SFI0`, `ISG1`, `OSG1`, `PSV0`, `HASH`, `DXIL` and `PRIV`. None has a root
  signature or LLVM bitcode: every `DXIL` part holds an `XBCH` record instead, so these are
  console programs like the scene shaders, not DXIL a PC driver could take.
* So the executable's few built-in shaders go through the same translation as `shaders.rpf`.
  Which draws use them isn't known yet.

## Scene shaders (`shaders.rpf`)

### The archive and the containers

**Checked here** with `shaderrecomp scan` (our own reader) and held by the local
`test_shader_census`:

* The shaders are named records in the archive's stored entry. Each record has the name, a NUL,
  a tag byte, the compressed size, a 32-bit ID, the compressed size again, the unpacked size,
  and then a raw DEFLATE stream.
  * There are 26,503 records with 8,145 distinct names.
  * The tag byte takes five values. Its meaning is unknown.
* Every stream inflates to a DXBC container with the same seven parts: `SFI0`, `PSV0`, `ISG1`,
  `OSG1`, `HASH`, `DXIL`, and `PRIV`. Every program header says shader model 6.6.
* The stages are 12,101 pixel, 9,282 mesh, 2,689 compute, 2,236 vertex, and 195 geometry.
  There are no hull, domain, amplification, or library shaders.
  * xcl's scan now finds the same 26,503. Until 2026-11-22 it found 23,869, missing the
    compute records whose names carry an effect prefix ([related-work.md](related-work.md)). A
    coverage figure from another project has to be read against its own record count.
* **None of them carries LLVM bitcode.** In every container, the DXIL program's bitcode is the
  four bytes `NOBC`. That settles the question this section used to leave open: translation
  starts from native machine code, gfx1013 to DXIL, which is the larger compiler. Console DXIL
  to desktop DXIL isn't possible.
* Right after the program header sits a table, `XDAT`.
  * It holds an entry count, then a word whose meaning is unknown (0x24 in all 26,503), then
    entries of tag, offset, and size.
  * Every table has a 16-byte digest (tag 0x12), a copy of the program header with `NOBC`
    (tag 0x07), and native programs (tag 0x0D).
  * 26,308 shaders carry one native program. 195 vertex shaders carry two; they have 13
    distinct names. Every geometry shader carries one.
* **No container has a root signature part** (`RTS0`). Nothing in the shader records says
  which root argument a shader finds in which register. The driver's pipeline cache (below)
  has a binding table for each shader of each pipeline, which is the lead for that.
* A native program entry starts with an `XBCH` header (version 3, compressed size, unpacked
  size), followed by a zlib stream.
  * All 26,698 programs inflate to `FSSM` images, version 0x101.
  * They range from 578 bytes to 482 KiB, and total 412 MiB.
* Many records repeat others byte for byte. Counting each copy once leaves 25,889 distinct
  containers and 25,516 distinct native programs. For mesh shaders, the 9,282 records hold
  8,765 distinct containers and 8,291 distinct programs.

### The driver's pipeline cache

RageForge reported the layout ([related-work.md](related-work.md)). **Checked here** with
`shaderrecomp pscache` and held by the local `test_shader_census`, except where noted:

* `shaders.rpf` has one compressed file besides the stored shader records: 275,540,780 bytes in
  4,205 zlib streams ([rpf.md](rpf.md)). It is a pipeline cache from the console's graphics
  driver. It starts with `PS\0\t`, then an ID (`0x65F41EDD`) and a driver version
  (`930.048.020`) as text.
* **Layout.** At 0x50, the header gives the counts of graphics pipelines, compute pipelines,
  metadata blobs, and shaders, then the sizes of the metadata and code areas. After the
  0x80-byte header come:
  * the graphics pipeline records (0x70 bytes each);
  * the compute pipeline records (0x18 bytes);
  * the metadata index and the shader index, an offset and a size each;
  * the metadata area;
  * the code area, which ends exactly at the end of the file.
* There are 25,471 graphics pipelines, 2,693 compute pipelines, 21,522 shaders (229.7 MiB of
  code), and 54,576 metadata blobs (29.7 MiB). Each metadata blob belongs to exactly one field
  of one pipeline.
* **A graphics record** has a 64-bit hash, a state blob, and a description blob, then more
  hashes, then seven slots. Each slot holds a shader and its binding blob, with 0xFFFFFFFF for
  none. The description blob starts with a hash and names the input-layout semantics when the
  pipeline has an input layout.
* **A compute record** has a hash, a state blob (which holds the thread-group size), a
  description blob, a shader, and a binding blob.
* **Slots**, told apart by what the code exports:
  * Slot 0 is the pixel shader (23,910 uses, and 201 more whose code exports nothing).
  * Slot 1 is the vertex shader (13,409).
  * Slot 2 is the mesh shader (12,062).
  * Slot 6 holds a vertex fetch subroutine, in 2 pipelines. Both have shader 21469 there and
    shader 21468 in slot 1, and neither shader carries a record's key (below), so both are the
    driver's own code. 21468 calls through s[4:5], as the game's vertex programs do, and 21469
    returns through s[4:5] (see "Vertex fetch" below).
  * The combinations are pixel and vertex (13,383), pixel and mesh (10,726), mesh alone (1,336),
    vertex alone (24), and slots 0, 1, and 6 (2).
  * 18,838 shaders are used only by graphics pipelines, 2,684 only by compute pipelines.
* **The code is final RDNA 2 machine code.** Each shader opens with three `v_nop`s that carry
  32-bit literals. In 21,463 shaders an `s_inst_prefetch` follows, and the code proper starts
  at 0x28. The decoder reads all 21,522 shaders to the end: 42,871,155 reachable instructions,
  none of unknown encoding or cut short, and no branch that misses an instruction. 8 shaders
  make 57 far jumps (see "Decode" below), and 2 jump or call to a target the code doesn't give:
  the slot 6 pair above.
  21,521 shaders have bytes after their last reachable instruction (695,680 in all), inside the
  `s_code_end` padding.
* **It isn't the records' code.** No cache shader's code equals a record's native program byte
  for byte: 13,962 distinct cache programs against 15,110 distinct record programs, none
  shared (checked with a one-off script). The driver compiled each shader again for each
  pipeline.
* **A key ties them together.** The first `v_nop` literal equals the 32-bit word 0x18 bytes
  into a native program's code range (from the offset at FSSM word 0xC0).
  * 21,509 of the 21,522 cache shaders carry the key of a record's native program. 20,696 of
    them match one record, and 21,501 match records of a single name.
  * The 13 without a match are used by 4 compute pipelines and in 44 graphics slots.
  * So for nearly every pipeline the game ships, we know which named shaders it combines.
* **The vertex fetch is compiled in.** 787 distinct native programs call the fetch subroutine
  (629 VS and 158 ES). 415 cache shaders carry the key of one, all of them VS programs, and
  none of them calls anything. At wave start each one reads only the vertex ID in v0 (396
  shaders) or v0 and the instance ID in v3 (19). No attribute VGPR is an input any more, so the
  driver compiled each pipeline's input layout into its vertex shader.
  * In the copies looked at (not counted), each element is one buffer load at its byte offset:
    an untyped dword load for 32-bit floats, and a typed load where the format needs
    converting, such as four 8-bit unorm components. Those copies also reach the program's own
    structured buffers by byte offset where the record's program indexes them: each copy is a
    fresh compile.
* **Binding blobs.** All 20,318 binding blobs that pipelines refer to have the same framing:
  * A header of four words for a graphics shader (the first word is 2) or six for a compute
    shader (0x403). The second word is the blob's size, counted from the last header word.
  * Then 32-byte records tagged 0x41 (402,328 records), 0x42 (371,387), or 0x43 (one per
    blob).
  * In a 0x41 record the next word is a resource kind: 0x300 (289,590), 0x302 (46,186), 0x401
    (33,596), 0x2 (13,897), 0x303 (11,328), or 0x100 (7,731). RageForge names the first five
    SRV, CBV, descriptor table, sampler, and UAV; 0x100 has no name yet.
  * The other fields haven't been worked out. They are where to look for which root argument
    the driver loads into which register.

### What the native programs do

**Reported** by Leonida and xbox-emulator ([related-work.md](related-work.md)). Leonida's
translator targets Vulkan and xbox-emulator's targets HLSL, so their notes are useful here as
facts about the console side.

Some claims are marked **checked here (word scan)**. For those, we searched each program's code
one 32-bit word at a time for the instruction encodings in AMD's public RDNA 2 reference. A
scan like that also looks at operand words, so it counts as evidence, not as a decode.

* **Disassembly.** LLVM's AMDGPU disassembler decodes the code as `gfx1013`.
* **Where the code is.** **Checked here (structural scan):** FSSM word 0xC0 holds the code's
  offset and word 0xC8 its size in bytes. The code runs from the offset plus 0x28 to the offset
  plus the size, and the range ends in `s_code_end` padding right after an `s_endpgm`. That
  holds in all 26,698 programs. A program can end in more than one place: 6,189 pixel shaders
  have two to five `s_endpgm`. So the size word marks the end, not the first `s_endpgm`. No
  `s_endpgm` follows the range, so the rule this section gave before, "to the last
  `s_endpgm`", found the same end. Xbox2Windows reported the size word and the 6,189.
* **Decode.** **Checked here (decode):** `shaderrecomp`'s RDNA 2 decoder (ported from
  xbox-emulator, see [related-work.md](related-work.md)) follows control flow from the first
  word of the code range: through `s_branch`, the `s_cbranch` family, `s_call_b64`, the
  subvector loop, and far jumps, until each path ends in an `s_endpgm` variant or an
  `s_setpc_b64` whose target the code doesn't give. In all 26,698 programs:
  * 44,333,402 reachable instructions, every one with a known encoding and fitting inside the
    code range;
  * no path runs off the end of the range, and every branch lands on an instruction boundary;
  * a far jump reaches past `s_branch`'s 16-bit offset: `s_getpc_b64` into a pair, then
    `s_add_u32` and `s_addc_u32` of a 64-bit constant into it (or `s_sub_u32` and
    `s_subb_u32`), then `s_setpc_b64` through it. The target is the address after
    `s_getpc_b64` plus the constant, wherever the code is loaded, so it is a direct jump. 13
    programs make 1,450 far jumps, one of them 1,377; all land on an instruction inside the
    code, and none is a call. xcl reported the pattern and counts 12 programs;
  * 983 programs still jump or call to a target the code doesn't give: the 788 VS programs
    that call the driver's fetch subroutine, and all 195 ES programs, which end by jumping on
    to the GS half (see "Registers at wave start" below);
  * 6,779 programs keep constant data after their last `s_endpgm` (390,176 bytes in all). A
    linear sweep decodes that data as instructions, which is why the decoder follows control
    flow;
  * every reached opcode has a name. The last two named, the LDS opcodes `0xA1` and `0xA7`
    (four instructions each), are `ds_write_b16_d16_hi` and `ds_read_u16_d16_hi` in LLVM's
    GFX10 tables.

  The local `test_shader_census` holds these numbers. `shaderrecomp disasm` prints one
  program from `scan --out`, with unreached words marked as data; its output stays local.
* **Wave size.** **Checked here:** bit 5 of the FSSM setup word at 0xD4 (see "Setup words"
  below) is set when a program runs 32 lanes and clear when it runs 64. `wave_size` in `regs.h`
  reads the size on its own, from how the reached code uses exec and vcc: as 64-bit lane masks,
  or only as 32-bit ones. All 24,323 programs whose code tells agree with the word. The other
  2,375 (943 pixel, 1,057 VS, 375 compute) don't use exec or vcc in a way that tells. By the
  hardware stage of the first record:
  * all 9,282 mesh, 2,236 VS, 195 ES, and 195 GS programs run 64 lanes;
  * 10,974 of the 12,101 pixel programs run 64 and 1,127 run 32;
  * 1,154 of the 2,689 compute programs run 64 and 1,535 run 32.

  So 24,036 programs run 64 lanes and 2,662 run 32. An earlier word scan here read the
  geometry programs as 32 lanes. Their setup words and their code say 64, and AMD's open drivers
  run the legacy geometry stages only in wave64. Leonida counts 21,545 wave64 programs of 26,503
  with broader markers; the decode alone finds 21,711 of 26,698.
* **Mesh shaders** run as NGG wave64 programs. `PSV0` gives 128 threads and at most 128
  vertices and 128 primitives.
  * v5 holds the workgroup's first vertex plus the thread index: the group index times the
    thread count, plus the thread's index. Both projects now agree; Leonida earlier read it as
    the workgroup index.
  * xbox-emulator: bits 24 to 27 of s3 hold the wave index.
  * xbox-emulator: a program reserves its output with `s_sendmsg` message 9 (`GS_ALLOC_REQ`),
    with the primitive count shifted left by 12, ORed with the vertex count, in m0.
  * xbox-emulator: positions go to export target 12, clip and cull distances to 13 to 15, and
    primitives to target 20. Each primitive packs its three vertex indices at bits 0, 10, and
    20, and bit 31 marks it culled.
  * **Checked here (word scan):**
    * Each of the 8,291 distinct mesh programs sends `GS_ALLOC_REQ`, exports a position and a
      primitive, and has an `s_endpgm` after the code start given above.
    * The register contents and bit layouts aren't checked.
* **Geometry shaders** are permutations of one voxelization shader: legacy merged ES/GS
  programs that pass data through LDS rings.
  * **Checked here (word scan):** each carries one native program, and none of the 195
    exports a position or a primitive. That fits a legacy geometry stage that writes to a
    ring.
  * **Checked here (word scan):** the 195 vertex shaders with two native programs have as
    many distinct names as the geometry shaders, 13. In each one, the first program never
    exports a position and the second always does.
  * That fits a vertex shader compiled twice: once as the export stage the driver merges with
    a geometry shader, and once as an ordinary vertex program. Leonida reads them the same way.
    **Checked here** by the stage records (below): the first program names hardware stage ES
    and the second VS.
* **Vertex fetch.** Vertex shaders that read vertex buffers call a fetch subroutine that the
  console driver generates, and that subroutine isn't in the blob.
  * **Checked here (decode and input signatures):** the subroutine packs the attributes by the
    elements the shader reads, as xbox-emulator reported. Take the elements of the container's
    `ISG1` input signature that the shader reads and that aren't system values, in register
    order: each gets the next four VGPRs, from v4 in a VS and from v9 in an ES, and component
    *c* goes in the *c*-th of them. Unread elements and the vertex and instance IDs take none
    (`fetch_layout` in `console.h`; `parse_signature` in `dxbc.h` reads the public signature
    format and keeps no names).
    * All 946 programs that call the subroutine (788 VS, 158 ES) read after the call exactly
      the components their signature marks as read, in the VGPRs that layout gives them. Besides
      those they read only the vertex and instance IDs (v0 and v3 in a VS, v5 and v8 in an ES),
      and only when the signature declares them.
    * 34 VS programs declare an element they never read. In 12 of them the unread element comes
      before one that is read. There, the layout these notes gave before, element *r* at
      4 + 4*r*, puts the later elements four VGPRs too high.
    * `scan` prints these counts by stage, and the local `test_shader_census` holds them.
  * **Checked here (one-off word scan):** each program that fetches calls once, with
    `s_swappc_b64` through the pair the setup word names, and gets the return address back in
    the same pair: s[4:5] in all 788 VS programs, s[12:13] in all 158 ES programs.
  * The pipeline cache keeps one subroutine as a shader of its own (slot 6 above). It adds s2
    to the vertex ID to index the vertex buffer and reads the buffer's descriptor through the
    pointer in s[0:1]. It then makes one typed load per element, straight into the element's
    VGPRs (a three-float element at byte 0 into v4 to v6, four 8-bit unorm components at byte
    12 into v8 to v11), and jumps back through s[4:5]. Its caller reads exactly those seven
    VGPRs after the call. Elsewhere the cache compiles the fetch into the vertex shader (see
    "The vertex fetch is compiled in" above).
  * So a desktop input layout can stand in for the subroutine.
* **Pixel shader inputs.** v0 and v1 hold the barycentrics. After them come the `SV_Position`
  components the shader reads, packed in order, then the front-face flag. When a shader reads
  the raw barycentrics, those are the weights of vertices 1 and 2.
  * **Checked here (decode):** the FSSM word at 0x248 says which inputs the hardware loads. It is
    the `SPI_PS_INPUT_ENA` register from AMD's public register reference, which also gives the
    order and size of each input (xcl reported where the word sits; `ps_inputs` in `console.h`).
    * Every pixel program has the word. It takes 14 values, and every one enables perspective
      center interpolation, which puts I and J in v0 and v1.
    * 10,260 pixel programs interpolate. In 10,259 of them, all 355,912 interpolations read
      their I or J from a register the word loads: 237,178 in VINTRP encoding and 118,734 in
      VOP3. The remaining program copies v1 to another VGPR and interpolates from the copy.
* **Stage records.** xcl reported these ([related-work.md](related-work.md)). **Checked here**
  with `shaderrecomp scan` and held by the local `test_shader_census`:
  * From FSSM byte 0x100 up to the code, each program keeps one 0x88-byte record per stage it is
    compiled for. A record opens with a 64-bit word: 0xC00002 in the top bits, then the API
    stage in bits 8 to 15 and the hardware stage in bits 0 to 7.
  * **Unused fields: conflicting reports,** neither checked on our census. xcl reports that they
    hold `0xDEADBEEF`. Leonida (`f6c6d11e`), whose records match ours (0x88 bytes from 0x100,
    the same hardware stage numbers), finds them zero in 25,643 records. Paired `0xDEADBEEF`
    qwords appear in only 1,055 records (860 blobs): those of the blobs whose directory words at
    0x120, 0x12C and 0x130 are `0xDEADBEEF`. In 25,643 records, the word at 0x120 plus the word
    at 0x12C equals the word at 0x130.
  * The hardware stage numbers match AMD's public PAL numbering:
    * 12,101 pixel programs name PS (5).
    * 2,689 compute programs name CS (6).
    * 2,041 vertex shaders name VS (4).
    * The 195 vertex shaders with two programs name ES (2), then VS.
    * The 195 geometry programs each name two stages, GS (3) and VS. The VS is the copy shader
      that moves a legacy geometry stage's output from the ring to the rasterizer.
    * All 9,282 mesh programs name 7, which PAL doesn't name.
  * So the plain vertex shaders run on the legacy hardware VS stage, not as NGG primitive
    shaders. Only the mesh shaders use NGG.
  * The API stage is 0 for vertex, 1 for pixel, 2 for geometry (both of its records), 3 for
    compute, and 6 for mesh. ES and VS records of vertex shaders say 0.
  * Leonida: the position's w component holds 1/w, as on desktop AMD hardware.
  * Leonida, later: the FSSM word at 0xFC, just before the records, isn't a render-target
    count. It is 5 or 6 in some programs and 16 or 18 in others. The count comes from `OSG1`.
  * **Layout word.** xcl reports that the FSSM word at byte 0x10 says where the stage tables
    below sit: 5 or 6 in the titles it ran, with the tables 8 bytes later under 6. **Checked
    here:** all 26,698 programs in 1.0.2.1 say 6, and the offsets below are those of layout 6
    (`fssm_layout`).
  * **Pixel input table** (`fssm_ps_input_table`). **Checked here:** a pixel program's FSSM
    image has a table of 8-byte records from offset 0x138, one per attribute N the code can read
    with `v_interp ... attrN`. The word at 0x238 holds their number, at most 32.
    * All 12,101 pixel programs have one, 1,784 of them empty. There are 55,937 records:
      55,905 of kind 6 (byte 0) and 32 of kind 7.
    * An earlier version of this note read records while byte 0 was 6, which stops at a kind 7
      record. It found 55,791 records and 1,794 empty tables. Use the count word.
    * Byte 1 is the input signature register (xcl), byte 2 the components, and byte 3 repeats N.
      Byte 4 is always zero.
    * Byte 7 is a set of flags. Exactly one of bits 0 and 1 is set in every record: bit 1 in
      51,327 and bit 0 in 4,610. Bit 3 is set in 11,659 records, and bit 4 in 11,239 of those.
      The values are 1 (4,214), 2 (40,064), 9 (396), 10 (24), and 26 (11,239), so a test for a
      flat input has to look at bit 0, not compare the byte with 1.
    * Byte 5 is nonzero in 11,069 records, all with bit 4 set. **Checked here** (decoder,
      `check_f16_inputs`): it names the second register packed into an f16 attribute.
      * Bit 3 marks f16 values. With bit 3 alone (flags 9 or 10), the record's register has one
        f16 per channel, in the low half. With bit 4 as well (flags 26), each channel holds two
        halves, so the attribute has eight slots: channel c's low half is slot 2c, its high
        half slot 2c + 1. Byte 2 is 3 in every packed record.
      * Byte 1's register fills the slots from 0, one declared component each. A second
        register starts at the next whole channel, slot 2 × ⌈n / 2⌉, where n is byte 1's
        highest declared component plus one. A register with x, y and z puts its partner's x in
        channel 2's low half.
      * The partner is byte 5's register in the 9,859 packed records where byte 5 differs from
        byte 1. In the other 1,380, byte 5 repeats byte 1 (both are 0 in 170). There the
        partner, if any, is the program's one f16 input register that the shader reads, that no
        record names, and that isn't a system value, and n counts only the elements the shader
        reads. 184 programs have such a register, always byte 1's register plus one. In 170 of
        them byte 1's register declares x to w but the shader reads only its xy element, and
        the partner starts at slot 2, not 4.
      * Of the 59,191 distinct attribute, channel and half each program's f16 interpolations
        read, 59,185 land on a component the signature marks read and none on an unread one.
        The other 6 are in 6 programs that read slot 6 of an attribute whose registers fill
        slots 0 to 5 (byte 1's x to w, byte 5's x and y). The f16 interpolations read only
        records with bit 3, 24 of them unpacked.
      * The 6 are one shader family. In each, the previous attribute's byte 1 register holds
        two elements, xyz and a separate w, and nothing reads that attribute's slot 3, where
        the w would go. That w is the only read f16 element no slot holds, so slot 6 most
        likely holds it. If an element has to start on a channel's low half, the w can't sit
        at slot 3, and slot 6 is the first whole free channel in record order. Elsewhere a
        second element starts on a low half: in the 12 unnamed-partner programs whose byte 1
        register holds an xy and a zw element, both read, the zw sits at slots 2 and 3 and the
        partner at 4. With one family, this stays a likely reading, not a rule.
    * xcl reads byte 6 as the register again. Here it differs from byte 1 in 22,168 records,
      usually by one to five lower, so don't rely on it.
  * Leonida reports what the flag bits mean: bit 0 marks a position the code reads with
    `v_interp_mov_f32` (a flat input) and bit 1 one it reads with `v_interp_p1`/`p2` or their f16
    forms. They give the read method, not the D3D interpolation mode. Bits 3 and 4 mark f16
    inputs. The table sometimes lists positions the code never reads (775 shaders).
  * **How the interpolation unit reads its parameters** (Leonida, measured on gfx1035 and
    gfx1036 with parameter words a compute wave wrote, not a real draw's setup):
    * Attribute N takes 48 bytes from M0 + 48N. For channel c, P0 and P10 sit at +8c and
      +8c+4, and P20 at +32+4c.
    * The f32 `mov`, `p1` and `p2` forms read a packed channel's raw dword as f32, with no
      unpacking.
    * `v_interp_p1ll_f16` widens the selected half (the `high` bit picks bits 31:16) and does
      one fma rounded to f32. `v_interp_p1lv_f16` takes P0's half from its third source.
    * `v_interp_p2_f16` rounds once, straight to f16, by the f16 rounding field of MODE. It
      writes bits 15:0 and keeps the destination's high half. Denormal halves survive, and NaN
      halves are quieted with their payload kept.
    * What a real draw's interpolation setup writes for a packed channel is still open there.
      On D3D12, the per-vertex equivalent is a `nointerpolation` input read with
      `GetAttributeAtVertex`, plus `SV_Barycentrics`.
  * **The f16 forms are VOP3** (Leonida, from LLVM's encoding for gfx1013): the attribute is in
    bits 0 to 5 of the first source field, the channel in bits 6 and 7, and `high` in bit 8.
    p1ll and p2 always agree on attribute, channel and `high`, and 4,731 of the 4,776 programs
    that use `high` read both halves of the same channel. xbox-emulator and xcl decode only
    the attribute and channel, and xcl never reads bit 8. Our decoder reads all three
    (`rdna2::interp_attribute`), and the disassembly prints them as `attrN.c high`.
  * **Which attribute is which** (xbox-emulator, a guess that still fails on one Trackmania
    shader): attribute k is the k-th input register holding a value that isn't a system value.
    A register counts if any element in it is read, even a system value that shares it.
    Registers holding `SV_RenderTargetArrayIndex` or `SV_ViewportArrayIndex` come after, so a
    shader that reads only the layer reads it as attr0. A channel with no signature element
    reads 0, except w, which reads 1.0. Byte 1 of our records names the register directly, so
    we don't need the guess. Whether our 32 kind-7 pixel records, and the 29 kind-7 and 2
    kind-8 vertex output records, are layer or viewport values is unchecked.
  * xbox-emulator also reports where a vertex stage puts those two values: the layer in bits
    10:0 of the z dword of position export 13, and the viewport index in bits 19:16. It took
    this from register documentation and tested it only on a shader it wrote.
  * **Vertex output table** (`fssm_vs_output_table`). **Checked here:** an image that names a
    legacy VS stage has a table of 6-byte records from offset 0x1DC, one per `exp param N` the
    code writes. The word at 0x29C holds their number. All 2,431 such programs have one: the
    2,041 plain vertex programs, the second program of the 195 vertex shaders that have two,
    and the copy shaders of the 195 geometry programs.
    * There are 9,526 records: 9,495 of kind 6, 29 of kind 7, and 2 of kind 8.
    * Byte 1 is the output signature register (xcl), and byte 2 repeats N. Bytes 3 to 5 are zero
      except in 822 records.
    * The register isn't N in 8,123 records. A translator has to map each `exp param N` to the
      signature through the table, not assume the two numbers agree.
  * Leonida: the interpolation modes in `PSV0` agree. Every interpolated input in this build is
    perspective-correct at the pixel center, and every other input is flat. So the one
    barycentric pair the code reads is the perspective pair at the center.
* **Compute shaders.**
  * FSSM word 0x138 is `COMPUTE_PGM_RSRC2`, which gives the user-SGPR count, the workgroup-ID
    enables, and the LDS size.
  * Thread-group sizes come from `PSV0`.
  * **Checked here:** FSSM words 0x13C, 0x140, and 0x144 hold the same group size. They equal
    `PSV0`'s thread counts in all 2,689 compute records. Leonida reports 2,281 matches.
* **Setup words.** **Checked here** against the code of all 26,698 programs (see "Registers at
  wave start" below): FSSM word 0xD4 says how a program starts (`fssm_setup` in `console.h`).
  Each stage but mesh also has a word laid out like AMD's public `SPI_SHADER_PGM_RSRC2`
  registers (`fssm_rsrc2`). By the hardware stage of the first record:
  * Bit 5 of 0xD4 is set when the program runs 32 lanes (see "Wave size" above).
  * Bits 16 to 23 give the number of user SGPRs: 4 to 14 in compute programs, 8 to 20 in pixel
    and VS programs, and 13 to 17 in ES, GS, and mesh programs. They start at s0, except in the
    merged ES, GS, and mesh stages, where the hardware loads 8 system SGPRs first and the user
    SGPRs start at s8.
  * Bit 2 is set in the 788 VS and 158 ES programs that call the driver's fetch subroutine, and
    in no others. Bits 24 to 31 then give the first of the SGPR pair the call goes through: s4
    in VS programs and s12 in ES programs, the fifth user SGPR in both.
  * Bit 0 is set in the 195 GS programs and in no others. No other bit of the word is set in
    any program.
  * The RSRC2 word sits at 0x138 in compute and GS programs, at 0x23C in pixel programs, and at
    0x2A0 in VS and ES programs. Its user-SGPR count (bits 1 to 5) equals the setup word's in
    all 17,416 programs that have one.
  * In the pixel word, bit 25 is `LOAD_COLLISION_WAVEID`. It is set in 10 programs and loads one
    more SGPR, for rasterizer-ordered pixel shading.
  * The ES and GS words are laid out like `SPI_SHADER_PGM_RSRC2_GS` (`gs_rsrc2`). GFX10 runs ES
    as the first half of the merged GS stage and has no ES register of its own. The field
    positions come from AMD's public register headers in the Linux kernel
    (`gc_10_3_0_sh_mask.h`).
    * Bits 19 to 26 are `LDS_SIZE`. Every ES word sets it to 2 (bit 20), and every GS word
      leaves it at 0. The field counts LDS allocation units. How the driver sizes the merged
      stage's LDS from it hasn't been worked out; the halves use LDS to pass vertices (see "ES
      and GS" under "Registers at wave start").
    * Bits 16 and 17 are `ES_VGPR_COMP_CNT`. They are 0 in all 195 ES words, as the word at
      0x2BC says.
  * In GS programs, the word at 0x13C is `VGT_GS_OUT_PRIM_TYPE` (`fssm_gs_out_prim_types`),
    with six bits per output stream at bits 0, 8, 16, and 22. AMD numbers the types 0 point
    list, 1 line strip, 2 triangle strip, and 3 rectangle list.
    * All 195 say 2 for stream 0 and 3 for streams 1 to 3.
    * Every one of their shaders' `PSV0` declares triangles in, triangle strips out, and stream
      0 alone. So stream 0 agrees with D3D's output topology in all 195, and the 3s sit on
      streams no shader writes.
  * In VS, ES, and mesh programs, the word at 0x2BC is the VGPR component count
    (`VGPR_COMP_CNT` in AMD's register reference, `fssm_vgpr_components`). The hardware loads
    one more VGPR than it says. In a VS they start at v0. AMD's open drivers give the GFX10
    order as the vertex ID, two user VGPRs, then the instance ID. In the merged ES and mesh
    stages they start at v5, after the geometry half's five.
    * It is 3 in 494 VS programs and 0 in the other 1,742. The 3s are the 386 programs that
      read v3 themselves and 108 that call the fetch subroutine and read no VGPR before the
      call. There the 3 must be for the subroutine, presumably for an instance-rate input.
    * It is 0 in all 195 ES programs and 1 in all 9,282 mesh programs.
    * GS programs read only v0 and v1. Where their own count sits hasn't been settled. The GS
      stage record's word at 0x15C is the candidate: it is 1 in all 195 GS programs and 0 in
      every ES, VS, and mesh program (pixel images keep their input table there, and compute
      images their code). AMD's open drivers set `GS_VGPR_COMP_CNT` to 1 for triangle input,
      which loads v0 and v1, and every GS here takes triangles and reads exactly those two.
      With one value in every program that is a fit, not proof, so the checks below leave GS
      VGPRs out. The driver's pipeline cache can't settle it either: none of its pipelines has
      a geometry stage.
  * The first stage record's word at 0x110 holds the program's register counts
    (`fssm_register_counts`): VGPRs in bits 0 to 9 and SGPRs in bits 20 to 31. They are plain
    counts, not the granules `SPI_SHADER_PGM_RSRC1` takes, and no other bit is set in any
    program. "Against the register counts" under "Registers at wave start" checks them.
* **Registers at wave start.** **Checked here (decode):** `shaderrecomp`'s register model
  (`regs.h`, written from AMD's public RDNA 2 reference) lists the registers each instruction
  reads and writes. From that it finds the registers a program reads before any path writes
  them, which are what the hardware and the driver load when a wave starts. It models every
  instruction the 26,698 programs reach. A d16 load that writes half a VGPR (the byte and short
  buffer, flat, and LDS loads with `_d16` in their names) keeps the other half, so the model
  counts that register as read too, as LLVM's AMDGPU target ties them; none of the counts below
  moves for it. A program that indexes registers through m0
  (`v_movreld` and its kin) can't be checked this way, because which registers it touches is
  only known when it runs: 16 compute, 3 pixel, 14 VS, and 4 mesh programs do. `scan` prints the
  counts by stage, `--out` adds each program's inputs to the local index, and the local
  `test_shader_census` holds the counts below. By the hardware stage of the first record:
  * **Against the setup words.** Each of the other 26,661 programs reads only registers its
    stage's setup words load (`count_inputs` in `census.h`):
    * compute: the user SGPRs, then the workgroup IDs and other system SGPRs that
      `COMPUTE_PGM_RSRC2` enables (`compute_setup`), and the thread IDs in v0 up to its count;
    * pixel: the user SGPRs, then the primitive mask the hardware puts right after them (the
      code moves it to m0 to interpolate), then the collision wave ID where bit 25 asks for it,
      and only VGPRs the input enable word loads;
    * VS: only its user SGPRs, and the VGPRs from v0 its component count loads;
    * ES, GS, and mesh: the 8 system SGPRs, then the user SGPRs from s8; in ES and mesh
      programs, VGPRs up to the ones the component count loads from v5.

    VGPRs are checked in every stage but GS: 26,466 programs.
  * **Against the register counts.** The counts at 0x110 follow from the register model in all
    26,698 programs for SGPRs, and in all but 114 for VGPRs (`vgpr_counts` and `sgpr_counts`
    in `StageInputs`). Programs that index registers through m0 are included.
    * The VGPR count is the higher of the VGPRs the code reads or writes (`register_extent` in
      `regs.h`) and the VGPRs the setup words load. It is at least 4 in a VS, as many as its
      component count can load, and at least 3 in a compute program, its thread IDs. That
      minimum sets the count in 17 VS and 19 compute programs that use fewer, and the loaded
      VGPRs set it in 1 pixel program.
    * 113 mesh programs and 1 pixel program count one to three VGPRs more than that, and no
      program counts fewer. Nothing in their code was found to account for the extra ones. The
      instructions that write their highest registers are ordinary one-register ones, and the
      model already counts the extra VGPR a TFE load writes.
    * The SGPR count is two more than the highest SGPR below vcc that the code reads or writes,
      since AMD's compilers count vcc's pair. It is at least as many as the setup words load,
      and the loaded SGPRs set it in 1,089 pixel, 194 VS, and 5 compute programs.
    * GS programs are the exception: all 195 count only the SGPRs their code uses. In 179 of
      them that is one SGPR fewer than the setup check has them load, the 8 system SGPRs plus
      the user SGPRs. Either a GS isn't loaded the way that check assumes, or the compiler
      leaves loaded SGPRs the GS half doesn't touch out of its count.
  * **Compute.** The SGPRs they read start at s4 (s5 in 2,598 programs); none reads s0 to s3.
    v0 is read in 2,605, v1 in 893, and v2 in 57.
  * **Pixel.** Their SGPRs run from s8 (s9 in 11,467) to s20; none reads s0 to s7.
  * **VS.** The 788 programs that fetch vertices call the driver's subroutine with
    `s_swappc_b64` through s[4:5], which they read first, so the driver loads its address
    there. After the call they read the attributes from v4 up (v4 and v5 in 787). Before it,
    they read user SGPRs s8 to s19, and v0 (1,438 programs) and v3 (386), where AMD's open
    drivers put a legacy VS's vertex and instance IDs.
  * **ES and GS** run as one merged hardware program. All 195 ES programs read s3. The 158 that
    call do it through s[12:13] and read the attributes back from v9 up (v9 to v11 in 158, v13
    and v14 in 153); the other 37 read v5. All 195 GS programs read s2, s3, s8, s9, s16, v0, and
    v1.
    * No ES program ends in `s_endpgm`. Each one copies s[6:7], one of the 8 system SGPRs, to
      another pair (s[24:25] in 160, s[26:27] in 28, s[22:23] in 7; after the fetch call in
      the 158 that make one), puts the copy back in s[6:7] before its one `s_setpc_b64`, and
      jumps through it. So the ES half hands the wave over to code at an address loaded at
      wave start, with the registers the GS program reads left in place. The blob doesn't say
      what the address is; presumably the driver loads the GS program's address there. A
      translator that compiles the two halves into one shader can drop the jump.
    * The halves pass vertices through LDS. Every ES program writes LDS, and none reads it or
      exports. Every GS program reads LDS. AMD's open drivers keep this ES-to-GS ring in LDS
      on GFX9 and later.
    * Every GS program also writes LDS and none exports, so its output vertices stay in LDS
      too. Presumably the copy shader of its second stage record reads them from there. A
      translator maps both rings to the D3D signatures instead.
    * By contrast, every VS, pixel, and mesh program exports. Elsewhere only compute and mesh
      programs touch LDS: 484 compute programs write it and 454 read it, and 5 mesh programs
      write it and 5 read it.
  * **Mesh.** All 9,282 programs read s3, s17, and v5, and the others they read lie in s16 to
    s24.
  * What each user SGPR holds is the driver's and the game's choice (see "Descriptors" below);
    the model only says which ones are read.
* **Descriptors.** Shaders read raw hardware descriptors (T#, S#, V#) through pointers in user
  SGPRs, which the console driver fills from the root signature. That mapping comes from game
  code and driver behaviour, not from the blobs.
  * Xbox2Windows: pixel shaders find their resource table through s[8:9], mesh shaders through
    s17 and s[18:19], and compute shaders through s[4:5], with entries 32 bytes apart. A stored
    vertex-shader group can hold a mesh shader (DXIL kind 13), so the DXIL kind decides the
    stage, not the group.
  * Leonida: some shaders truncate descriptor pointers to 32 bits, so shader-visible
    descriptor heaps have to sit in the low 4 GiB of GPU addresses. Large root signatures spill
    their arguments to memory.
  * Xbox2Windows: in the pixel shaders its translator tried, the first unresolved read is a
    16-byte scalar descriptor load in 2,927 records (1,925 distinct programs). The table
    pointer comes from s8 in 1,896, s9 in 1,011, and s10 in 20. The most common places are
    s9+0 (565), s8+0xA80 (430), s8+0xAC0 (379), s8+0x6E0 (268), and s9+0x20 (229). In 529 of
    the 565, the shader loads the descriptor at byte 0 of the s9 table and then reads four
    dwords at bytes 16 to 28 through it. The `PSV0` resource declarations vary between those
    shaders and don't give the original HLSL register or space.
  * Xbox2Windows, tracing 4,411 pixel records: 4,099 read four or more descriptors through
    `s_buffer_load` (up to 32), and 311 read one to three. 3,711 use three entry registers, 606
    two, and 93 one. The trace stops at an entry SGPR nothing has set in 1,553 records, and at a
    16-byte descriptor no `s_buffer_load` reads (probably a V#) in 582.
  * Leonida's heap-index census: 99.86% of 770,412 descriptor loads take their base from a
    wave-start SGPR used as a 32-bit pointer. The resource heap and the sampler heap each have
    their own base, and the stride is compiled in: T# offsets are multiples of 32, S# offsets
    multiples of 16.
  * xbox-emulator: the console compiler merges root loads into 8- and 16-dword loads that run
    past the last parameter, so the block a translation reads root arguments from needs slack
    after it. Its optional root-signature inference starts geometry user SGPRs at s22, where
    Leonida and our register model ("Setup words" above) put them from s8.
  * Since no container carries a root signature (checked above), xbox-emulator's translator
    stops at the first resource load in 8,749 of the mesh shaders it tried.
  * It stops 16 more at a wave-reduction prologue it doesn't handle yet.
* **Other state the programs assume** (Leonida): 1,473 shaders read constants embedded in
  their own code; IEEE mode is off. Leonida reads 32-bit denormals as flushed to zero and
  16-bit denormals as kept. Xbox2Windows calls the 32-bit denormal mode unmeasured, and its
  checks assume denormals are kept.
* **Float mode changes.** **Checked here (word scan):** the only field of the MODE register the
  programs set with `s_setreg_imm32_b32` is the f16/f64 rounding field (bits 3:2 of FP_ROUND).
  Leonida reports the same. The scan finds no `s_round_mode` or `s_denorm_mode`, and the decoder
  finds neither in any of the 26,698 programs.
  * 4,776 pixel shaders set that field to round toward zero, and 1,147 set it to nearest even.
    Other stages set it only to nearest even: 571 compute, 595 mesh, and 85 vertex programs.
  * So f16 results in those pixel shaders round toward zero from that point on. Xbox2Windows
    reported a pixel shader that exports binary16 rounded toward zero.
  * **Checked here (decoder):** the mode is there for packed f16 interpolation and nothing
    else. The 4,776 are exactly the pixel programs whose f16 interpolations read a high half
    (see "Pixel input table") and whose input table has a record with flag bit 4. In every
    one, the first toward-zero write comes before the first f16 interpolation. In program
    order, no 16-bit instruction that rounds, other than `v_interp_p2_f16`, runs while the
    field holds toward zero. Xbox2Windows found the same over 4,187 records. So a translation
    rounds `v_interp_p2_f16` results toward zero and can treat the rest of the program as
    round-to-nearest. 21 more pixel programs have a bit-4 record but neither the mode change
    nor a high-half read.
  * The scan also finds 19 words that would be `s_setreg_b32` of the f32 rounding field, in 11
    compute and 2 pixel programs. A word scan can't tell whether those are instructions.
  * 8,317 of the 9,282 mesh shaders, and 4,424 shaders of other stages, set hardware registers
    20 and 21, which LLVM names `FLAT_SCR_LO` and `FLAT_SCR_HI`, to 0 and 1024. **Checked
    here (decoder):** none of those 12,741 programs (8,317 mesh, 2,249 pixel, 1,225 compute,
    876 plain vertex, and 37 two-program vertex shaders in both their ES and VS programs)
    contains a scratch or flat instruction, so the writes have no effect and a translation
    drops them. Xbox2Windows reported the same for 2,011 pixel records.
* **Features in `SFI0`.** **Checked here:** 685 records use 64-bit atomics, all on typed
  resources; 30 use tiled resources; 10 use rasterizer-ordered views; 18,227 index descriptor
  heaps directly. The first three match Leonida's counts.
* **Features without a D3D12 counterpart** (xbox-emulator): `ds_ordered_count` has none, and
  the GWS semaphores only a memory counter for straight-line 32-lane compute; GDS `ds_append`
  and `ds_consume`, and SDWA negate and absolute-value modifiers, need their own lowering.
  * **Checked here (decode):** none of the 26,698 programs reaches a DS instruction with the
    gds bit, a GWS op, `ds_ordered_count`, `ds_append`, or `ds_consume`, as Leonida reports.
    So 1.0.2.1 needs none of these lowerings.
  * The driver's pipeline cache has no GDS or GWS either, but 30 of its compute shaders use
    `ds_append` or `ds_consume` on LDS (73 instructions), which no archive program does: the
    driver's recompile adds them. On LDS each adds the number of active lanes to a
    group-shared counter (`ds_consume` subtracts it) and returns the old value, which D3D12
    can express with a group-shared atomic.
  * Leonida's translator (`db22f2fa`) refuses the `ds_*_src2` add, sub, inc and dec forms, the
    u64 add and sub forms, `ds_condxchg32_rtn_b64`, and LDS outside compute. LDS outside compute
    does occur here: in 5 mesh programs, and in the ES and GS halves of the merged geometry
    stage (see "ES and GS" under "Registers at wave start"). This page doesn't count those DS
    forms yet.
* **Ordered pixel shading.** 10 pixel shaders use primitive-ordered pixel shading (POPS).
  Fragment-shader interlock reproduces the ordering; on D3D12 that means rasterizer-ordered
  views.
* **Ray tracing.** 64 compute shaders (ray-traced shadows) use the hardware BVH intersection
  instruction, which expects AMD's node format.
  * Leonida first emulated each intersection step the way Mesa's RADV driver does in software.
    It has since measured the instruction on an RX 6800: it returns the raw watertight values,
    and breaks edge ties RDNA 2's own way, not RADV's ([related-work.md](related-work.md),
    Leonida).
  * Either way, the acceleration structures have to be built in AMD's node format.
  * Leonida: these compute shaders compile slowly, so their translation belongs on a background
    thread.

### What it means for `shaderrecomp`

* **Input.** `shaderrecomp scan` already finds every record, unpacks every native program, and
  decodes its code. `shaderrecomp pscache` reads the driver's pipeline cache, decodes its code,
  and matches each shader to its records.
* **The pipeline cache lists every precompiled pipeline ahead of time:** which named shaders it
  combines and a binding table for each. Once the binding tables are understood, they may
  answer the root-argument question below without waiting for the game's calls. Whether to
  translate the cache's per-pipeline code or the records' shared native programs is open.
* **Translation happens when the game creates a pipeline, not ahead of time.** Which register
  holds which root argument depends on the root signature, and no shader carries one. The game
  supplies root signatures when it creates them at run time. Running the game natively, we see
  those calls directly.
  * Decoding and analysis can still happen ahead of time.
  * The final translation is keyed by the native program and the root signature, and cached.
  * The cache holds at most one entry per distinct program and root signature. There are
    25,516 distinct programs.
* **Next pieces:**
  * a gfx1013 decoder: done, ported from xbox-emulator and extended against AMD's public RDNA 2
    instruction set reference (see "Decode" above);
  * the per-stage register setup: done for which registers each stage reads at start, checked
    against the setup words (see "Setup words" and "Registers at wave start" above). Still
    open: where GS programs give their VGPR count;
  * a backend.
    * One option writes DXIL directly.
    * The other writes HLSL and compiles it with DXC, as xbox-emulator does. It reported
      compiling all 105,506 shaders of another title that way, and about a third of a
      4,000-shader GTA VI vertex sample. Its later notes call the 105,506 a DXC compile sweep
      rather than checked output, and count 0 of 8 stages as verified.
    * DXC 1.8.2502 is the first version whose module-only validation passes (Xbox2Windows).
    * We choose when the translator starts.
* **Exec-masked regions** (Xbox2Windows, `03ece06` and `1a252c8`, a backend design idea):
  * A single forward region from `s_and_saveexec_b64` to the `s_or_b64` that restores exec can
    be lowered as per-lane selects where exec is restored.
  * The `s_cbranch_execz` that skips the region can be dropped when every operation in its body
    is safe on every lane.
  * SCC after the save and restore is a property of the whole wave, so upstream refuses
    instructions that read it there.
* **Wave64 shaders.** They need 64-lane waves or emulation on 32-lane hardware. Shader model
  6.6's `WaveSize` attribute asks for 64 lanes where the hardware supports it (see the host
  constraints below).

### Instruction behaviour a translation has to keep

**Reported** by the projects named, from AMD's public reference, LLVM and Mesa source, and
their own GPU measurements. None is checked against our decoder yet; each will be before a
translation depends on it.

* **16-bit results.** On GFX10, f16 ALU instructions (VOP1 and VOP2, `v_cvt_f16_f32` included)
  and the VOP3 instructions that take `op_sel` write only the low half of the destination and
  keep the high half. The VOP2 16-bit integer instructions define the whole register. Leonida
  takes this from Mesa's ACO compiler and Xbox2Windows from LLVM. Xbox2Windows now marks it
  unmeasured, `v_cvt_f16_f32` included, so treat it as an assumption until a GPU test settles
  it. Leonida measured one case: an SDWA result with `dst_sel:DWORD` zeroes the high half (see
  "SDWA" below).
* **SDWA** (Leonida, `f4cd3061`: 172 distinct forms and about 79,000 instructions in its
  corpus, about 21,000 cases measured on gfx1036):
  * Operand-select narrowing happens before negate and absolute value.
  * `dst_unused` pads, sign-extends, or preserves the bits the result doesn't fill.
  * A NaN result keeps the input's sign and payload.
  * `div:2` on −0 gives +0.
* **Narrowing to f16** rounds to nearest even unless the f16 rounding field says toward zero
  (above). Leonida does it in integer arithmetic because AMD's drivers lower GLSL's
  `PackHalf2x16` to a round-toward-zero conversion. About half of the unorm8 values its d16
  loads produced came out one unit low. Xbox2Windows builds the encoding in integers too.
* **Cost of emulating f16.** xbox-emulator found one Fortnite compute shader taking 84 of
  105 ms per frame because its translation emulated f16 rounding in f32. Native 16-bit types
  (`-enable-16bit-types`) with a NaN guard on the conversion took the lobby from about 10 to
  23 frames per second. It checked that conversion against all 2^32 inputs on an RTX 5060.
* **f16 arithmetic in f32** (Xbox2Windows, `7e0be98`): f16 add, subtract and multiply can be
  computed in f32 and narrowed once to nearest even. That is exact for every finite input,
  subnormals and signed zeros included, since f32's 24 bits are at least 2 × 11 + 2. Upstream
  checked add and multiply exhaustively.
  * It doesn't hold for the fused f16 FMA: `v_fma_f16` can't be an f32 FMA followed by
    narrowing (Xbox2Windows, `9fff1ee`, `4a19cec` and `766276f`).
  * `v_subrev_f16` computes the second source minus the first (from LLVM; unmeasured).
* **`v_sin` and `v_cos`** take turns, not radians: they compute sin(2πx). Xbox2Windows removes
  the nearest whole number of turns before multiplying by 2π, which keeps the HLSL argument
  within ±π. Leonida measured the f16 forms on a gfx1031 GPU: they reduce exactly over the
  whole f16 range, and the other f16 transcendentals are correctly rounded.
* **`v_cvt_u32_f32`** truncates toward zero, gives 0 for NaN and negative inputs, and
  saturates at 0xFFFFFFFF from 2^32 up (Xbox2Windows).
* **Stores to normalized buffer formats** write a NaN as zero (Leonida).
* **Typed buffers.** A typed buffer's element type has to come from the descriptor's numeric
  format. xbox-emulator guesses it from how the shader uses the values, and an integer view
  read as float then returns garbage (its issue 105).
* **Vertex format 189,** `R10G10B10_SNORM_A2_UNORM`, exists only on the console.
  xbox-emulator fetches it as `R10G10B10A2_UNORM` and restores the signs in the vertex shader.
  That is exact for vertex fetch, but not for anything that filters the raw bits.
* **gfx1013 is not gfx103x** (Leonida, from an llvm-mc sweep of every opcode). Only gfx1013 has
  `v_mad_f32`, `v_mac_f32` and their `_legacy` forms, `v_mul_lo_i32` as VOP3 0x16B,
  `s_get_waveid_in_workgroup`, scalar stores, atomics and scratch, `s_dcache_wb` and
  `s_dcache_discard`, and the 31 `ds_*_src2_*` forms. VOP2 0x06 and VOP3 0x106 and 0x140 decode
  as fused legacy instructions on gfx103x. So a listing made for the wrong target misreads them.
  **Checked here:** compared against xbox-emulator's gfx1013 table built from LLVM and AMD's
  reference, our names have no conflict. That comparison found three decoder errors, now fixed:
  the SMEM offset is signed, eight VOP2- or VOP1-only opcodes had VOP3 names, and gather4 has no
  `_d` forms.
* **`v_mad_f32`, `v_mac_f32` and `v_mad_legacy_f32`** flush denormal operands, products and
  sums whatever MODE says, before and after the output modifier (Leonida on gfx1010,
  xbox-emulator on gfx1010 too). Leonida counts 9,297 shaders that use at least one of them.
* **f16 inline constants** (RX 6800, via Leonida and xbox-emulator): the constant's f16 value
  in the low half and zero in the high half. `v_pack_b32_f16` and `v_cvt_f32_f16` read the f32
  bits instead. OPSEL on a constant reads the constant again, and 1/2π is 0x3118. Packed
  16-bit integer instructions read a float constant as the whole 32-bit word.
* **Other operand codes** (Xbox2Windows):
  * An integer inline constant used as an f32 operand is its raw bit pattern: 1 is the smallest
    denormal and −1 a NaN pattern (`e114e89`).
  * Operand codes 235 to 239 and 251 to 254 are hardware values, not constants:
    `src_shared_base` to `src_pops_exiting_wave_id`, then `src_vccz`, `src_execz`, `src_scc`
    and `src_lds_direct` (`a99e8da`).
  * Inline constant 248 in a 64-bit operand is the f64 value of 1/(2π) (`a99e8da`).
  * Ours: `shaderrecomp`'s decoder names those hardware sources, and `inline_float64` gives an
    inline float constant's 64-bit value, that one included (tested by `test_rdna2`).
* **NaN handling** (gfx1036 probes, via xbox-emulator, except where another project is named):
  * With IEEE mode off, the first NaN operand passes through unchanged.
  * An invalid operation without a NaN operand gives 0xFFC00000, and an fma whose product is
    invalid gives that value even when the addend is a NaN. A CPU reference model has to set
    that value itself: x86 creates the same 0xFFC00000, but ARM64 creates 0x7FC00000 (Leonida,
    `ea2cf301`).
  * `v_sub` negates its second source first.
  * **min, max and med3** (Leonida, `a31e3c68`, measured). With quiet NaNs, all four
    combinations of IEEE mode and `DX10_CLAMP` give the same result. That corrects xbox-emulator's
    report, given here before, that the min and max NaN rules depend on IEEE mode.
    * One NaN operand: the other operand's bits, unchanged.
    * All operands NaN: src0's exact bits, payload included, and with IEEE mode off its
      signalling bit too.
    * The same holds for the f16 three-source forms, and per half for the packed forms.
    * −0 orders below +0.
    * `min3` and `max3` are two pairwise steps. `med3` with any NaN operand gives `min3`, which
      with one NaN is the minimum of the other two, as the ISA and gfx1030 say for
      `v_med3_f32`.
    * DXIL's `FMin` and `FMax` promise neither the payload nor the −0 ordering, so a translation
      lowers these to explicit selects.
  * **Ordered compares.** `v_cmp_lg` is ordered, false when either operand is NaN: it is
    (a < b) || (a > b). HLSL's `!=` is unordered and would be wrong (Leonida, `5bc7e8b0`).
    * A compiler backend may fold an ordered compare with abs or neg modifiers, such as
      −|x| ≤ 2, to true for a NaN. Xbox2Windows (`e114e89`) adds an explicit ordered test on
      the bits to `lt`, `eq`, `le`, `gt`, `lg` and `ge`. See also "AMD driver folding" under the
      host constraints.
  * The fused f16 FMAs (VOP2 0x36 to 0x38) return a signalling NaN unquieted with IEEE off.
* **Output modifier (omod).** Two reports, which partly disagree on f64; neither is checked
  here:
  * Leonida (`73fce549`): omod applies only when IEEE mode is off and denormals are flushed for
    the destination's precision. Under the f16 and f64 denormal setting Leonida reads for the
    game (kept; see "Other state the programs assume" above), f64 and f16 destinations ignore
    omod: conversions to f64, `v_frexp_mant_f64`, and `v_div_fixup_f64` and `_f16`. Clamp still
    applies to them. `v_cvt_f32_f64`, whose destination is f32, does scale.
  * xcl (`34efd81`, `0056934`): omod applies to f64 instructions too, and to `v_cvt_f32_f64`. On
    `v_cvt_f16_f32` it is applied in f32, before rounding to f16.
  * They agree that `v_cvt_f32_f64` scales. They disagree on f64 destinations, and on
    `v_cvt_f16_f32`, whose destination is f16. The f16 and f64 denormal mode GTA VI's programs
    run in hasn't been read from the images yet.
* **Division helpers** (Xbox2Windows, `9fff1ee`, `4a19cec` and `766276f`, except where
  another project is named):
  * `v_div_scale_f32` depends on the numerator as well as the denominator. A denominator of 1
    with a numerator of 2^100 scales by 2^64 and sets VCC, so no rule that looks only at the
    denominator works.
  * `v_div_fmas_f32` is a single-rounded FMA, scaled by 2^32 when VCC is set, with the scaling
    done before the rounding to binary32. Doing it in f64 needs a midpoint correction that
    covers the binary32 subnormal range. The fixed 2^32 factor isn't confirmed: later AMD
    guides tie the scale to the third operand's exponent.
  * `v_div_fixup` (Leonida, `73fce549`): a NaN in the first source gives ±inf. There is an f64
    underflow cutoff and no overflow check. A signalling NaN passes through with IEEE mode off.
* **Conversions.** `v_cvt_pknorm_*` rounds the exact product once to nearest and turns NaN into
  0. d16 image loads of 32-bit integer texels saturate. `v_pk_mad` with clamp saturates.
* **Clamp on integer instructions** (xcl, `34efd81` and `0056934`): the clamp bit saturates
  instead of wrapping on `v_add_nc_u32`, `v_sub_nc_u32`, `v_subrev_nc_u32`, `v_add3_u32`, the
  packed 16-bit add, subtract and mad (`v_pk_mad` above among them), and the integer dot
  products.
* **24-bit MAD** (Xbox2Windows, `ee5535a`): it truncates both multiplicands to 24 bits.
* **`s_bfe`** takes a 7-bit width, sign-fills signed fields, and sets SCC when the result isn't
  zero (gfx1036).
* **Transcendentals** (gfx1036, bit-exact models published by xbox-emulator): the f16 forms
  are correctly rounded except about 20 sin and cos inputs near a midpoint. The f32 `rcp`,
  `rsq` and `sqrt` are faithfully rounded, and the f64 forms return the unit's unrounded
  value. The f32 `log`, `exp`, `sin` and `cos` match AMD's MIT-licensed rocjitsu models.
* **Buffer bounds** follow Mesa's GFX11 column for each `OOB_SELECT`, not its GFX10 one
  (Leonida, on gfx1036 and gfx1011). In raw mode, soffset counts toward the check.
* **Byte and short buffer accesses** (Leonida, `8f208d09`):
  * `_d16` loads write one 16-bit half, the high half for `_d16_hi`, and keep the other. The
    `ubyte` and `sbyte` forms among them extend the byte to 16 bits.
  * Byte and short stores write the low 8 or 16 bits; `_d16_hi` stores take the high half.
  * The bounds check covers the access's own 1 or 2 bytes. An out-of-bounds load returns 0
    (unmeasured for the d16 forms), and an out-of-bounds store writes nothing.
  * HLSL has no 8-bit stores, so a byte store becomes a merge into its dword with
    `InterlockedAnd` and `InterlockedOr`.
* **Samplers.**
  * The point mip filter picks floor(λ+0.5), so an exact half rounds up (Leonida, gfx1036).
    The T#'s `MIN_LOD` clamps the fractional LOD before that rounding, counting from the
    resource's level 0.
  * An S#'s compare function is word 0 bits 14:12, in `D3D12_COMPARISON_FUNC` order minus
    one. `TRUNC_COORD` is word 0 bit 27: set, the coordinate rounds down; clear, it rounds to
    nearest even at 6 fraction bits (Xbox2Windows, 104 draws on an RTX 3070 and WARP).
  * `image_get_resinfo` gives depth 0 for 2D and minified array layers (Leonida). Past the
    last level it gives size 0 but keeps the level count (xbox-emulator). For a cube it gives
    the layer count, 6 per cube (xcl, `34efd81` and `0056934`).
  * **Cube textures** (xcl, the same commits): the face coordinate is the face plus 8 times the
    cube index. The T# doesn't say how many cubes a view holds, so xcl declares every cube as a
    cube array and takes the LOD from the direction only.
  * `image_gather4_l` lowers to four `SampleLevel` taps, which ignore the sampler's bias and
    LOD clamp (xbox-emulator, on WARP).
* **Integer pixel inputs: conflicting reports.** xcl reported earlier, from the ISA, that
  `v_interp_mov` and `v_interp_p2` read integer inputs as raw bits
  ([related-work.md](related-work.md)). xcl (`58309b9`) has since gone back to converting them
  to float, because raw bits made XDefiant's UI text disappear. The real cause is still open,
  so neither reading is settled.
* **Helper lanes** (Leonida, RADV on gfx1036, wave64): EXEC at entry holds only covered pixels.
  `s_wqm` turns on whole quads, and lane order within a quad is (x&1) + 2(y&1). A helper's v0
  and v1 already hold its own I/J before `s_wqm`.
* **Which pixels a pixel shader writes** (Leonida, `c26d78af`, measured on gfx1036 with 18
  programs):
  * A pixel is written only if its lane was on in EXEC both at the last export that carries
    `vm`, if there is one, and at the last export to that color target. With no export, EXEC at
    the end decides.
  * An export carries data only for active lanes. A later export to the same target replaces
    the earlier one for the whole wave.
  * A lane with no data in the last color export stays unwritten, even if a later null export
    with `done` and `vm` marks it valid.
  * Measuring this needs `s_waitcnt expcnt(0)` after each export.
* **Float atomic min and max done as integer atomics** (xcl, RX 7900 XTX): pick the signed or
  unsigned path by comparing the bits with +inf's, not with a float compare, since -0.0 ≥ 0 is
  true. The f64 `_x2` forms map onto 64-bit integer min and max.
* **Float compare-and-swap** (Leonida, `0f2b802a`, unmeasured on hardware): the float forms of
  `ds_cmpst`, `global_atomic_fcmpswap` and `buffer_atomic_fcmpswap` compare as floats. −0
  matches +0, and a NaN never matches, even with identical bits. D3D12's
  `InterlockedCompareExchangeFloatBitwise` compares bits, so it needs a loop around it. The
  operand order differs: `ds_cmpst_f32` takes the compare value, then the data; in the global
  and buffer forms, vdata holds the data first, then the compare value.
* **Groups of one wave may leave out `s_barrier`** (xbox-emulator), so a translation that runs
  such a group as several host waves has to keep their LDS accesses in order.
* **permlane** (Leonida, `5a78b4b7`, measured on RDNA 2 and RDNA 3.5): `op_sel` bit 0 is FI
  (fetch inactive) and bit 1 is BOUND_CTRL. When the source lane is off, FI=1 returns the
  source value anyway. With FI=0, the old value stays when BOUND_CTRL is 0, and the result is 0
  when it is 1. Shuffles and ballots have to run before any predication: upstream, a
  short-circuit select broke this.
* **Shader clocks** (Leonida, `bd3f061a`): `s_memtime` reads a per-CU shader clock and
  `s_memrealtime` a reference clock at a constant rate. Both return 64 bits into an SGPR pair,
  low word first. D3D12 has no portable shader clock, only vendor extensions.

## Host constraints that affect translation

* **Wave size.** Console RDNA shaders may assume 64-lane waves, but desktop NVIDIA GPUs run 32
  lanes and can't be forced to 64.
  * **Reported:** an emulated 64-lane group (two native 32-lane halves combined through group
    shared memory) passes ballot, prefix-count, and lane-read tests on NVIDIA and WARP.
  * **Reported:** native wave64 passes on five Radeon GPUs, RDNA 2 and RDNA 4. RDNA 2 runs 32
    lanes unless a shader asks for 64; RDNA 4 defaults to 64.
  * **Reported** by Xbox2Windows: Intel HD 530 and UHD GPUs report 16 lanes. WARP's width
    depends on its build: 4 lanes for Windows 10's in-box WARP, 8 for app-local WARP 1.0.22.
  * **Reported** by xcl (RTX 3080): a wave64 program split into two 32-lane halves breaks on
    `v_mbcnt` over a mask that isn't a constant (lanes 32 to 63 undercount), on a VOP3 `v_cmp`
    that writes an SGPR pair, and on 32-bit `s_bcnt`, `s_ff1` or `s_flbit` applied to one word
    of a mask. Each of these has to see the whole 64-lane mask.
  * **Reported** by xcl (RTX 5070 Ti, RX 9070): NVIDIA, whose range is 32 to 32, refuses a
    `[WaveSize(64)]` compute pipeline. xcl now runs each console wave as two 32-lane host waves
    that exchange through groupshared memory. Only masks, `readlane`/`readfirstlane`,
    `ds_bpermute` and GLC scalar loads need the exchange; DPP, swizzles and `permlane` stay in a
    half. It cost 1.3 times with one console wave per group and about 5 times with four. The
    exchange waits on its partner, which assumes the waves of a group make progress together;
    WARP doesn't promise that and runs neither 32 nor 64 lanes, so WARP can't test this form.
  * **Reported** by xcl (`af0311d`, `fc60bf5`, `55156f0`), for wave64 vertex and pixel programs
    on 32-lane GPUs. Its 13,210 wave64 VS and PS programs match our 2,236 VS and 10,974 PS (see
    "Wave size" under "What the native programs do").
    * Reads of lanes 32 to 63: 1,082 whole-wave reductions (unsigned min, max, or bitwise or)
      in 436 programs; 355 waterfall reads in 353 programs, which stay below lane 32; and 200
      reads of SGPR spill slots in lanes 32 to 47 in 11 programs, which come out wrong on NVIDIA
      today. None of these programs uses `ds_bpermute`.
    * Two problems hit every GPU, native AMD wave64 included. Spill slots get lost: 287 pixel
      shaders (3,540 reads) spill to lanes below 32, and in a partial wave at a primitive's edge
      a lane the PC never launched loses its slot. Min reductions go wrong: on an RTX 5070 Ti,
      lanes that weren't launched read 0, so a min reduction gives 0. 325 of the 436 reduction
      programs have one.
    * xcl's plan: turn `v_writelane` and `v_readlane` spill slots into scalar variables, and
      recognise a reduction as a whole rather than lane by lane.
* **Compute lane order.** **Reported** by xcl: desktop drivers don't keep a 2-D or 3-D compute
  group's threads in order within a wave. An RX 9070 lays them out in 2x2 quads, and NVIDIA's
  waves of an 8x8x4 group don't hold consecutive indices either; 1-D groups stay in order. The
  console's waves hold consecutive threads, so a translated 2-D or 3-D compute program that uses
  DPP, readlane, `ds_bpermute` or `v_mbcnt` has to hand each host wave the next block of console
  threads itself.
  * A translator has to know which shaders depend on the wave width. Each program's setup word
    gives the width it was compiled for (see "Wave size" above).
* **Mesh shaders.** **Reported** by xcl: a Ryzen 7000 integrated GPU (RDNA 2) runs wave64 but
  has no mesh shader tier, so mesh programs need another path there.
  * **Reported** by xbox-emulator: desktop D3D12 rejects a mesh pipeline whose primitive
    topology is missing or undefined, so the translation fills it from the mesh information in
    `PSV0`.
  * **Reported** by Xbox2Windows: WARP 10.0.26100.9818 removes the device for a mesh pipeline
    without a pixel shader.
  * **Reported** by Xbox2Windows: without mesh shaders, compute dispatches can write each
    triangle into its own range of one buffer, and vertex draws then read the ranges in order
    into shared colour and depth targets. Depth order held on a GTX 1060 and on Windows 10's
    in-box WARP, with the draw order reversed and the depths swapped. Xbox2Windows (`301bfae`)
    then lit it at shader model 6.0: with normals interpolated and lighting applied, it matched
    on a GTX 1060 and on both WARPs.
* **Feature levels on likely hosts** (Xbox2Windows): an RX 570 on vkd3d-proton exposes shader
  model 6.6, a GTX 1060 runs DXR tier 1.0, and the in-box WARP of Windows 10 19041 has no DXR.
  Tier 1.0 runs `DispatchRays` but not inline `RayQuery`, which needs tier 1.1 and shader model
  6.5, so translating the 64 ray-traced compute shaders to `RayQuery` leaves tier 1.0 hosts out.
  Refitting a structure needs `ALLOW_UPDATE` on the build as well as `PERFORM_UPDATE`; without
  it the GTX 1060 removed the device.
* **64-bit image atomics.** **Reported** by xbox-emulator: `RWTexture<uint64_t>` on an
  `R32G32_UINT` texture, which needs shader model 6.6 and
  `AtomicInt64OnTypedResourceSupported` from `D3D12_FEATURE_DATA_D3D12_OPTIONS9`. 685 of our
  records use 64-bit atomics on typed resources (see `SFI0` above).
* **Interpolation precision.** **Reported** by Xbox2Windows: on an RTX 3070, 133 of 276
  interpolated values came out one or two f32 ulps low, and rounding them toward zero to f16
  then changed the output at 17 pixels. WARP was exact. So the host's interpolation can differ
  by an ulp before the translation's f16 rounding.
* **Sampling.**
  * **Reported** by Xbox2Windows (`851b68c`, `eab58b5`): with linear mip filtering, the
    `MinLod` blend weight is quantized to 1/256. WARP 1.0.22 rounds it to the nearest 1/256,
    ties to even, and an RX 7900 XTX and Windows 11's in-box WARP agree. An RTX 3080 Ti
    truncates instead, storing step k as k·256/65535, an offset of up to 7.6e-6.
  * **Reported** by Xbox2Windows (GTX 1060, WARP): a sampler used both for comparison and for
    ordinary sampling needs two host samplers.
  * **Reported** by Xbox2Windows (`851b68c`, `eab58b5`): under point mip filtering with a
    `MinLod` clamp, only Windows 10's in-box WARP reads the mip below: a clamp of 0.5625 gives
    mip 0, 1.5 mip 1, and 2.5625 mip 2, at any shader model. (An earlier report had 10.0.19041.5794
    pick mip 1 for 1.5625, where `SampleLevel` at the same LOD picks mip 2.) Windows 11's
    in-box WARP 10.0.26100.9818, an RX 7900 XTX, an RTX 3080 Ti and WARP 1.0.22 pick the nearest
    mip. So only Windows 10's in-box WARP is no reference for the clamp-then-round rule above.
  * **Reported** by Leonida (`64890ac2`): D3D12 can't `Sample` an integer SRV, so sampling an
    integer format goes through derivatives and a matching view.
* **Blending.** **Reported** by xbox-emulator: fp16 and `R11G11B10_FLOAT` blending saturates a
  finite overflow on WARP and on its GPU.
* **f16 NaN.** **Reported** by xbox-emulator: an RTX 3080 returns 0x7FFF for an f16 FMA whose
  result is a NaN.
* **NaN bits through float values.** **Reported** by Xbox2Windows (`24936e2`, merged in
  `046a2bd`): on NVIDIA, a NaN-shaped word loses bits after passing through a float-typed HLSL
  value. An RTX 4060 Laptop GPU (32.0.16.1656) and an RTX 3060 (32.0.16.2101) returned
  0x7FFFFFFF where 0xFFFFFFFF was expected. Declaring the constant buffer `uint4` and converting
  to float only where a float operation reads it kept the bits. Our rule for the `shaderrecomp`
  backend: keep registers and loads as `uint`, and convert only where a float operation reads
  them.
* **Denormal mode on NVIDIA.** **Reported** by Xbox2Windows: NVIDIA drivers reject some shaders
  compiled with `-denorm preserve`, failing pipeline creation with `E_OUTOFMEMORY`
  ([related-work.md](related-work.md)). Its later measurement (`c0f4d9c`) narrows that: on an
  RTX 5080 (32.0.16.2101 and 32.0.16.2124), any pixel shader that uses `log2` fails under
  `preserve`, while `ftz` and `any` are accepted. WARP and a Radeon integrated GPU accept
  `preserve`.
* **f64 compares on Windows 10's WARP.** **Reported** by Xbox2Windows (`9fff1ee`, `4a19cec`,
  `766276f`): Windows 10's in-box WARP (10.0.19041) faults at the first dispatch of a double
  `>` or `<=` (DXIL `fcmp ogt` and `ole`). Its translator emits only `<`, `>=`, `==` and `!=`,
  swapping the operands where needed.
* **No f32 FMA in DXIL.** **Reported** by Leonida (`ea2cf301`): DXIL's `mad` may be split into a
  multiply and an add, and `dx.op.fma` takes only doubles. So a single-rounded f32 FMA needs
  exact emulation. Xbox2Windows' f64 form of `v_div_fmas_f32`, with its midpoint correction, is
  one (see "Division helpers" above).
* **AMD driver folding.** **Reported** by Leonida (`a284bad1`, `399fa980`), on AMD driver
  26.10.1:
  * In wave32 it folds an ordered compare against a literal NaN to true, so a translation avoids
    literal NaN constants.
  * It mis-evaluated a 16-bit integer `med3` with a constant operand. The sum-minus-min-minus-max
    form avoids it.
  * An output modifier turns a −0 the hardware produced into +0.
  * Without float-controls preservation, it folds `x != x`.
  * Our inference, not upstream's: DXC emits fast-math flags by default, so a translation marks
    such operations `precise` or compiles with `-Gis`.
* **Emulated 64-bit arithmetic on Intel.** **Reported** by Xbox2Windows (`688aa85`): Intel UHD
  drivers 32.0.101.7026 and 32.0.101.7092 got emulated 64-bit (`uint2`) arithmetic wrong inside
  a large shader, while the same operation alone was exact.
* **Shared-exponent UAVs.** **Reported:** typed UAV stores to `R9G9B9E5_SHAREDEXP` work on
  WARP but not on an NVIDIA driver. A translated shader that writes that format through a UAV
  needs a fallback, such as an `R32_UINT` view with the packing done in the shader.
  * **Reported:** an RDNA 2 integrated GPU supports the typed stores but rounds the mantissas
    toward zero, not to nearest even. So the choice between the typed path and the fallback
    goes by measured result, not by the reported support.
* **`frac`.** **Reported:** `v_fract_f32` clamps its result below 1.0, and D3D's `frac` doesn't.
  Adapters differ on the unclamped value, so a translation adds the clamp itself. xcl reports
  that ±inf and NaN give NaN, so the clamp has to be a compare and select: HLSL's `min` returns
  the other operand for a NaN, which would turn NaN into the bound.
* **Atomics on UAV arrays.** **Reported:** with a non-uniform index, they give four times the
  expected result on RDNA 3 and RDNA 4 and on Windows 10's WARP. A `WaveReadLaneFirst` loop that
  handles one distinct index at a time gives the right result.
  * **Reported** by Xbox2Windows (`224d6fd`): RDNA 2 too. An RX 6500 (32.0.21045.29003) read
    256 where 64 was expected. Its loop makes one pass per distinct index: each pass picks a
    leader lane, counts the lanes that share its index, and makes one `InterlockedAdd` of that
    count. That read 64 on the RX 6500 and on WARP. The cause is still open.
* **Root signature version 1.2** isn't in the in-box D3D12 of Windows 10 or Windows 11 23H2
  (reported), so a runtime that needs it ships the Agility SDK.
* **Constant buffer alignment.** **Reported** by xbox-emulator: the console accepts a constant
  buffer view at a 16-byte aligned address, and desktop D3D12 requires 256 bytes; a Terraria
  view at such an address removed the device. xbox-emulator binds root constant buffers as raw
  buffer views instead, which the translated shader reads the same way.
* **Pipeline compile time.** **Reported** by xbox-emulator: large translated pixel shaders took
  1.6 to 3 s in DXC and 0.4 to 1.9 s in the driver. It hands the title a placeholder pipeline
  that draws nothing and finishes any pipeline not ready within 12 ms on a worker thread. It
  doesn't use `ID3D12PipelineLibrary` yet, so the driver's part recurs on every run.
  * **Reported** by Xbox2Windows (Ryzen 5 5500, DXC 1.9.2609.5): in a shader with 8,192
    sample branches, DXC's GVN pass takes 82% of the pass time. `-Od` cuts the wall time by 84%
    there and by more than 95% at 16,384. A 2.77 MB DXIL pipeline didn't finish creation within
    about 9 minutes on an RTX 4060.
* **Branches in generated HLSL.** **Reported** by xcl: in HLSL 2021, `?:`, `&&`, and `||` on
  scalars short-circuit, so each one is a real branch. Its translator emitted them at nearly
  every instruction (lane activity, condition codes, selects), and DXC couldn't cope. One
  1,589-instruction shader took about 6 minutes and over 30 GB of memory, and the memory use
  ended the user's Windows session. Emitting `select()`, `and()`, and `or()` instead brought that
  shader to 0.3 s. So a translator emits branch-free selects, and runs DXC under a memory limit
  (a job object, or a process of its own).
* **Lane reads in pixel shaders.** **Reported** by xcl: console pixel shaders take derivatives
  by hand across a quad's lanes, so they read helper lanes. `WaveReadLaneAt` leaves helper lanes
  out before shader model 6.7, which drew seams along triangle edges. `QuadReadLaneAt` includes
  them and fixed it. `[WaveOpsIncludeHelperLanes]` (shader model 6.7) hung the device instead:
  the mask of live lanes then took in helper lanes, which never finished.
  * `QuadReadLaneAt` alone isn't enough (xcl, later). After `s_wqm`, the first lane in EXEC,
    which `v_readfirstlane` picks, is often a helper on a primitive's edge, in another quad,
    and before shader model 6.7 a wave read of it returns 0. xcl reads the value inside the
    lane's own quad first, then does the wave read from a live lane of that quad.
* **Coherence in compute shaders.** **Reported** by xcl: its translated compute shaders reach
  memory through one large UAV. That view had to be `globallycoherent`, or a tree build that
  reads what other groups wrote got wrong pointers. The console code relies on GLC loads and
  cache invalidation there.
* **Console bytecode crashes AMD's compiler.** **Reported** by xcl: AMD's desktop shader
  compiler crashes on a DXIL part whose bitcode is `NOBC`, which every GTA VI container has
  (above), instead of rejecting it. So a shader that fails to translate gets a replacement,
  and the original container never reaches the driver.
* **Root signature flags.** Console root signatures may set bits desktop D3D12 rejects (see
  above). The translator clears them only after confirming what they mean.
  * **Reported** by xbox-emulator: bit `0x01000000` is the console flag that forces
    memory-based root arguments. With it, a shader reads its root arguments from a block whose
    address is in s[0:1], rather than from user SGPRs. The console requires it whenever an
    amplification shader is used, and WARP rejects it.
  * The BVH builder's root signatures set that bit and `0x02000000`. xbox-emulator now names
    the second a compute spill area and the pair an XDXR-internal root signature (reported).
  * A shader used with such a root signature has to be translated for that ABI before the bit
    can be cleared.
