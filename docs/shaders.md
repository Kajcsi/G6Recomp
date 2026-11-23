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

With no DXIL, there is nothing to translate here. The plan in [side-dlls.md](side-dlls.md)
provides the FidelityFX API from the public PC FidelityFX SDK instead.

* **Reported** (Leonida, from these 29 programs, which carry both a root signature and their
  `COMPUTE_PGM_RSRC2`): the user SGPR count is 4 + one per descriptor table + two per root
  CBV, in signature order. s0 to s3 are a driver prefix, and tables are 32-bit pointers from
  s4. That is the only place in the package where a root signature and its native program sit
  side by side.

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
    compiled for. Unused fields hold `0xDEADBEEF`. A record opens with a 64-bit word: 0xC00002
    in the top bits, then the API stage in bits 8 to 15 and the hardware stage in bits 0 to 7.
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
    * GS programs read only v0 and v1. Where their own count sits hasn't been found.
* **Registers at wave start.** **Checked here (decode):** `shaderrecomp`'s register model
  (`regs.h`, written from AMD's public RDNA 2 reference) lists the registers each instruction
  reads and writes. From that it finds the registers a program reads before any path writes
  them, which are what the hardware and the driver load when a wave starts. It models every
  instruction the 26,698 programs reach. A program that indexes registers through m0
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
  Leonida reports the same. The scan finds no `s_round_mode` or `s_denorm_mode`.
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
* **Ordered pixel shading.** 10 pixel shaders use primitive-ordered pixel shading (POPS).
  Fragment-shader interlock reproduces the ordering; on D3D12 that means rasterizer-ordered
  views.
* **Ray tracing.** 64 compute shaders (ray-traced shadows) use the hardware BVH intersection
  instruction, which expects AMD's node format.
  * Leonida emulates each intersection step the way Mesa's RADV driver does in software.
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
  it. Leonida measured one case: an SDWA result with `dst_sel:DWORD` zeroes the high half.
* **Narrowing to f16** rounds to nearest even unless the f16 rounding field says toward zero
  (above). Leonida does it in integer arithmetic because AMD's drivers lower GLSL's
  `PackHalf2x16` to a round-toward-zero conversion. About half of the unorm8 values its d16
  loads produced came out one unit low. Xbox2Windows builds the encoding in integers too.
* **Cost of emulating f16.** xbox-emulator found one Fortnite compute shader taking 84 of
  105 ms per frame because its translation emulated f16 rounding in f32. Native 16-bit types
  (`-enable-16bit-types`) with a NaN guard on the conversion took the lobby from about 10 to
  23 frames per second. It checked that conversion against all 2^32 inputs on an RTX 5060.
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
* **NaN handling** (gfx1036 probes, via xbox-emulator):
  * With IEEE mode off, the first NaN operand passes through unchanged.
  * An invalid operation without a NaN operand gives 0xFFC00000, and an fma whose product is
    invalid gives that value even when the addend is a NaN.
  * `v_sub` negates its second source first.
  * min and max NaN rules depend on IEEE mode.
  * `v_med3_f32` with a NaN operand gives the minimum of the other two (from the ISA and
    gfx1030).
  * The fused f16 FMAs (VOP2 0x36 to 0x38) return a signalling NaN unquieted with IEEE off.
* **Conversions.** `v_cvt_pknorm_*` rounds the exact product once to nearest and turns NaN into
  0. d16 image loads of 32-bit integer texels saturate. `v_pk_mad` with clamp saturates.
* **`s_bfe`** takes a 7-bit width, sign-fills signed fields, and sets SCC when the result isn't
  zero (gfx1036).
* **Transcendentals** (gfx1036, bit-exact models published by xbox-emulator): the f16 forms
  are correctly rounded except about 20 sin and cos inputs near a midpoint. The f32 `rcp`,
  `rsq` and `sqrt` are faithfully rounded, and the f64 forms return the unit's unrounded
  value. The f32 `log`, `exp`, `sin` and `cos` match AMD's MIT-licensed rocjitsu models.
* **Buffer bounds** follow Mesa's GFX11 column for each `OOB_SELECT`, not its GFX10 one
  (Leonida, on gfx1036 and gfx1011). In raw mode, soffset counts toward the check.
* **Samplers.**
  * The point mip filter picks floor(λ+0.5), so an exact half rounds up (Leonida, gfx1036).
    The T#'s `MIN_LOD` clamps the fractional LOD before that rounding, counting from the
    resource's level 0.
  * An S#'s compare function is word 0 bits 14:12, in `D3D12_COMPARISON_FUNC` order minus
    one. `TRUNC_COORD` is word 0 bit 27: set, the coordinate rounds down; clear, it rounds to
    nearest even at 6 fraction bits (Xbox2Windows, 104 draws on an RTX 3070 and WARP).
  * `image_get_resinfo` gives depth 0 for 2D and minified array layers (Leonida). Past the
    last level it gives size 0 but keeps the level count (xbox-emulator).
  * `image_gather4_l` lowers to four `SampleLevel` taps, which ignore the sampler's bias and
    LOD clamp (xbox-emulator, on WARP).
* **Helper lanes** (Leonida, RADV on gfx1036, wave64): EXEC at entry holds only covered pixels.
  `s_wqm` turns on whole quads, and lane order within a quad is (x&1) + 2(y&1). A helper's v0
  and v1 already hold its own I/J before `s_wqm`.
* **Float atomic min and max done as integer atomics** (xcl, RX 7900 XTX): pick the signed or
  unsigned path by comparing the bits with +inf's, not with a float compare, since -0.0 ≥ 0 is
  true. The f64 `_x2` forms map onto 64-bit integer min and max.
* **Groups of one wave may leave out `s_barrier`** (xbox-emulator), so a translation that runs
  such a group as several host waves has to keep their LDS accesses in order.

## Host constraints that affect translation

* **Wave size.** Console RDNA shaders may assume 64-lane waves, but desktop NVIDIA GPUs run 32
  lanes and can't be forced to 64.
  * **Reported:** an emulated 64-lane group (two native 32-lane halves combined through group
    shared memory) passes ballot, prefix-count, and lane-read tests on NVIDIA and WARP.
  * **Reported:** native wave64 passes on five Radeon GPUs, RDNA 2 and RDNA 4. RDNA 2 runs 32
    lanes unless a shader asks for 64; RDNA 4 defaults to 64.
  * **Reported** by Xbox2Windows: Intel HD 530 and UHD GPUs report 16 lanes, and WARP 4.
  * **Reported** by xcl (RTX 3080): a wave64 program split into two 32-lane halves breaks on
    `v_mbcnt` over a mask that isn't a constant (lanes 32 to 63 undercount), on a VOP3 `v_cmp`
    that writes an SGPR pair, and on 32-bit `s_bcnt`, `s_ff1` or `s_flbit` applied to one word
    of a mask. Each of these has to see the whole 64-lane mask.
  * A translator has to know which shaders depend on the wave width. Each program's setup word
    gives the width it was compiled for (see "Wave size" above).
* **Mesh shaders.** **Reported** by xcl: a Ryzen 7000 integrated GPU (RDNA 2) runs wave64 but
  has no mesh shader tier, so mesh programs need another path there.
  * **Reported** by xbox-emulator: desktop D3D12 rejects a mesh pipeline whose primitive
    topology is missing or undefined, so the translation fills it from the mesh information in
    `PSV0`.
  * **Reported** by Xbox2Windows: WARP 10.0.26100.9818 removes the device for a mesh pipeline
    without a pixel shader.
* **Feature levels on likely hosts** (Xbox2Windows): an RX 570 on vkd3d-proton exposes shader
  model 6.6, a GTX 1060 runs DXR tier 1.0, and the in-box WARP of Windows 10 19041 has no DXR.
* **64-bit image atomics.** **Reported** by xbox-emulator: `RWTexture<uint64_t>` on an
  `R32G32_UINT` texture, which needs shader model 6.6 and
  `AtomicInt64OnTypedResourceSupported` from `D3D12_FEATURE_DATA_D3D12_OPTIONS9`. 685 of our
  records use 64-bit atomics on typed resources (see `SFI0` above).
* **Interpolation precision.** **Reported** by Xbox2Windows: on an RTX 3070, 133 of 276
  interpolated values came out one or two f32 ulps low, and rounding them toward zero to f16
  then changed the output at 17 pixels. WARP was exact. So the host's interpolation can differ
  by an ulp before the translation's f16 rounding.
* **Sampling.**
  * **Reported** by Xbox2Windows: the `MinLod` blend weight is quantized to 1/256. WARP 1.0.22
    rounds it, and an RTX 3080 Ti truncates it.
  * **Reported** by Xbox2Windows (GTX 1060, WARP): a sampler used both for comparison and for
    ordinary sampling needs two host samplers.
* **Blending.** **Reported** by xbox-emulator: fp16 and `R11G11B10_FLOAT` blending saturates a
  finite overflow on WARP and on its GPU.
* **f16 NaN.** **Reported** by xbox-emulator: an RTX 3080 returns 0x7FFF for an f16 FMA whose
  result is a NaN.
* **Shared-exponent UAVs.** **Reported:** typed UAV stores to `R9G9B9E5_SHAREDEXP` work on
  WARP but not on an NVIDIA driver. A translated shader that writes that format through a UAV
  needs a fallback, such as an `R32_UINT` view with the packing done in the shader.
  * **Reported:** an RDNA 2 integrated GPU supports the typed stores but rounds the mantissas
    toward zero, not to nearest even. So the choice between the typed path and the fallback
    goes by measured result, not by the reported support.
* **`frac`.** **Reported:** `v_fract_f32` clamps its result below 1.0, and D3D's `frac` doesn't.
  Adapters differ on the unclamped value, so a translation adds the clamp itself.
* **Atomics on UAV arrays.** **Reported:** with a non-uniform index, they give four times the
  expected result on RDNA 3 and RDNA 4 and on Windows 10's WARP. A `WaveReadLaneFirst` loop that
  handles one distinct index at a time gives the right result.
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
