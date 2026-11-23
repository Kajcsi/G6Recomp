#pragma once

// What the console adds to a DXBC container, as observed in the 1.0.2.1 scene shaders
// (docs/shaders.md). Its DXIL part holds a program header whose bitcode is the four bytes "NOBC",
// and right after that header a table, "XDAT", of tagged entries. An "XBCH" entry carries a
// precompiled native GPU program, zlib-compressed: an "FSSM" image of RDNA 2 machine code and its
// metadata. Some vertex shaders have two. Malformed input throws FormatError.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dxbc.h"

namespace g6::shader {

// XDAT entry tags seen in every scene shader.
inline constexpr uint32_t kXdatProgramHeader = 0x07; // a copy of the program header, with NOBC
inline constexpr uint32_t kXdatNativeProgram = 0x0D; // XBCH
inline constexpr uint32_t kXdatDigest = 0x12;        // 16 bytes

struct XdatEntry {
    uint32_t tag = 0;
    std::span<const uint8_t> data;
};

// The table after the program header: "XDAT", the entry count, a word whose meaning isn't known
// (0x24 in every 1.0.2.1 shader, with three entries or four), then per entry its tag, offset
// (from the start of the DXIL part), and size.
struct Xdat {
    uint32_t unknown = 0;
    std::vector<XdatEntry> entries;
};

// Returns nullopt when the part has no table there.
std::optional<Xdat> read_xdat(std::span<const uint8_t> dxil_part);

// The XBCH header: "XBCH", a version (3 in 1.0.2.1), the compressed size, a zero, the unpacked
// size, a zero; then the zlib stream.
struct NativeProgramHeader {
    uint32_t version = 0;
    uint32_t compressed_size = 0;
    uint32_t size = 0;
};
inline constexpr size_t kNativeProgramHeaderSize = 24;

NativeProgramHeader read_native_program_header(std::span<const uint8_t> xbch);

// Inflates the program and checks it against the header's sizes and the FSSM magic.
std::vector<uint8_t> unpack_native_program(std::span<const uint8_t> xbch);

// The start of an FSSM image: magic and a 16-bit version (0x101 in 1.0.2.1).
inline constexpr uint32_t kFssmMagic = fourcc("FSSM");
uint16_t fssm_version(std::span<const uint8_t> fssm);

// The RDNA 2 machine code in an FSSM image: the 32-bit word at byte 0xC0 holds an offset and the
// word at 0xC8 a size, and the code runs from the offset plus 0x28 to the offset plus the size.
// The range ends in s_code_end padding.
std::span<const uint8_t> fssm_code(std::span<const uint8_t> fssm);

// The 32-bit word 0x18 bytes into the image's code range (from the offset at 0xC0, before the
// 0x28 bytes fssm_code skips). The driver's pipeline cache carries the same word in the shaders
// it compiled from this program (pscache::shader_key), so it ties the two together. Throws
// FormatError like fssm_code.
uint32_t fssm_key(std::span<const uint8_t> fssm);

// One stage an FSSM image is compiled for. From byte 0x100, the image keeps one 0x88-byte record
// per stage, up to the code. Each record opens with a 64-bit word: 0xC00002 in its top bits, the
// API stage in bits 8 to 15, and the hardware stage in bits 0 to 7. A geometry program names two
// stages, its own and the vertex stage of its copy shader.
struct FssmStage {
    uint8_t api = 0;
    uint8_t hardware = 0;
    bool operator==(const FssmStage&) const = default;
};

// Hardware stages as AMD's public PAL numbers them. Mesh programs use 7, which PAL doesn't name.
inline constexpr uint8_t kHardwareEs = 2;
inline constexpr uint8_t kHardwareGs = 3;
inline constexpr uint8_t kHardwareVs = 4;
inline constexpr uint8_t kHardwarePs = 5;
inline constexpr uint8_t kHardwareCs = 6;
inline constexpr uint8_t kHardwareMesh = 7;
std::string hardware_stage_name(uint8_t stage); // "ES", "PS", ..., or the number

// The stage records, in order; empty when there are none or the image is too short to hold the
// code offset. Throws FormatError when the image isn't FSSM.
std::vector<FssmStage> fssm_stages(std::span<const uint8_t> fssm);

// How the hardware starts the program, from the 32-bit word at byte 0xD4. In 1.0.2.1 no other bit
// of the word is set, and each field agrees with what the code reads first (docs/shaders.md).
struct FssmSetup {
    bool gs = false;     // bit 0: set when the first stage is GS
    bool fetch = false;  // bit 2: the program calls the driver's vertex fetch subroutine
    bool wave32 = false; // bit 5: it runs 32 lanes; clear for 64
    // Bits 16 to 23: how many user SGPRs the driver loads. They start at s0, or at s8 in the
    // merged stages (ES, GS, mesh), whose first eight SGPRs the hardware loads.
    uint8_t user_sgprs = 0;
    uint8_t fetch_sgpr = 0; // bits 24 to 31: the first of the SGPR pair holding the fetch address
};
// Throws FormatError when the image isn't FSSM or is too short to hold the word.
FssmSetup fssm_setup(std::span<const uint8_t> fssm);

// The first stage's program resource word 2, laid out as SPI_SHADER_PGM_RSRC2_* or, for CS,
// COMPUTE_PGM_RSRC2 in AMD's public register reference: the user SGPR count in bits 1 to 5, then
// the stage's own enables. It sits at 0x138 for CS and GS, 0x23C for PS, and 0x2A0 for VS and ES.
// Nothing for mesh programs, which have none there, or an image too short to hold it.
std::optional<uint32_t> fssm_rsrc2(std::span<const uint8_t> fssm);
inline constexpr uint32_t kRsrc2PsLoadCollisionWaveId = 1U << 25; // one more SGPR, for POPS

// The RSRC2 word of an ES or GS program, laid out as SPI_SHADER_PGM_RSRC2_GS in AMD's public
// register reference. GFX10 runs ES as the first half of the merged GS stage and has no ES
// register of its own, so an ES program's word has this layout too.
struct GsRsrc2 {
    unsigned user_sgprs = 0;         // bits 1 to 5
    unsigned es_vgpr_components = 0; // bits 16 and 17: ES_VGPR_COMP_CNT
    unsigned lds_size = 0;           // bits 19 to 26: LDS_SIZE, in the hardware's allocation units
};
GsRsrc2 gs_rsrc2(uint32_t word);

// A GS program's output primitive type for each of its four streams, from the word at byte 0x13C:
// VGT_GS_OUT_PRIM_TYPE in AMD's public register reference, six bits per stream at bits 0, 8, 16,
// and 22. AMD numbers the types 0 point list, 1 line strip, 2 triangle strip, 3 rectangle list.
// Nothing when the first stage isn't GS or the image is too short to hold the word.
std::optional<std::array<uint8_t, 4>> fssm_gs_out_prim_types(std::span<const uint8_t> fssm);
// The type for one of D3D's geometry output topologies (kTopology* in dxbc.h); nothing for others.
std::optional<uint8_t> gs_out_prim_type(uint32_t topology);

// The word at 0x2BC of a program whose first stage is VS, ES, or mesh: the VGPR component count
// (VGPR_COMP_CNT in AMD's public register reference). The hardware loads that many VGPRs plus
// one: from v0 in a VS (vertex ID, then on GFX10 two user VGPRs, then the instance ID), and from
// v5 in the merged ES and mesh stages, after the five VGPRs of the geometry half. Nothing for
// other stages or an image too short to hold it.
std::optional<uint32_t> fssm_vgpr_components(std::span<const uint8_t> fssm);
inline constexpr unsigned kMergedGsVgprs = 5; // v0 to v4 in the merged ES and mesh stages

// The VGPRs a VS or ES program starts with: the vertex ID, two user VGPRs, and the instance ID,
// from v0 in a VS and v5 in an ES; then, in a program that calls the driver's fetch subroutine,
// the inputs it fetches (fetch_layout). Nothing for other stages.
struct VertexVgprs {
    uint32_t vertex_id = 0;
    uint32_t instance_id = 0;
    uint32_t first_fetched = 0;
};
std::optional<VertexVgprs> vertex_vgprs(uint8_t hardware);

// Where the fetch subroutine leaves a program's inputs (docs/shaders.md): the input signature
// elements the shader reads (rw_mask), ordered by register, get four VGPRs each from
// first_fetched, component c in the element's c-th. An element the shader doesn't read takes
// none, and system values aren't fetched.
struct FetchedElement {
    size_t element = 0;      // index in the signature
    uint32_t first_vgpr = 0; // where component x lands
};
std::vector<FetchedElement> fetch_layout(std::span<const SignatureElement> inputs,
                                         uint32_t first_fetched);

// The 32-bit word at byte 0x10, which says where the stage tables below sit. xcl reports 5 and 6
// (github.com/ivall/xcl), the tables sitting 8 bytes later under 6; every 1.0.2.1 image says 6,
// and the offsets below are those of layout 6.
uint32_t fssm_layout(std::span<const uint8_t> fssm);
inline constexpr uint32_t kFssmLayout = 6;

// A pixel program's input enable word, at byte 0x248: SPI_PS_INPUT_ENA in AMD's public register
// reference. Each set bit makes the hardware load one input into the next VGPRs, from v0 on.
// Nothing when the image's first stage isn't PS or the image is too short.
std::optional<uint32_t> fssm_ps_input_enable(std::span<const uint8_t> fssm);

// A pixel program's input table: one 8-byte record per attribute the code can read with
// v_interp attrN, from byte 0x138, with their number in the word at 0x238 (32 at most). The
// record's index is N.
struct PsInputRecord {
    uint8_t kind = 0;      // byte 0: 6 for most inputs, 7 for a few
    uint8_t reg = 0;       // byte 1: the input signature register (xcl)
    uint8_t mask = 0;      // byte 2: the components
    uint8_t attribute = 0; // byte 3: N again
    uint8_t byte4 = 0;     // zero
    uint8_t byte5 = 0;     // packed f16: the second register (check_f16_inputs in census.h)
    uint8_t byte6 = 0;     // xcl reads it as the register again; often one to five lower here
    uint8_t flags = 0;     // byte 7: bit 0 flat, bit 1 interpolated, bit 3 f16, bit 4 packed f16
};
// Nothing when the image isn't layout 6 with PS first, or the count is out of range.
std::optional<std::vector<PsInputRecord>> fssm_ps_input_table(std::span<const uint8_t> fssm);

// A legacy vertex stage's output table: one 6-byte record per exp param N, from byte 0x1DC, with
// their number in the word at 0x29C (32 at most). The record's index is N.
struct VsOutputRecord {
    uint8_t kind = 0;                  // byte 0: 6 for most outputs
    uint8_t reg = 0;                   // byte 1: the output signature register (xcl)
    uint8_t param = 0;                 // byte 2: N again
    std::array<uint8_t, 3> extra = {}; // bytes 3 to 5: zero in most; meaning not worked out
};
// Nothing when the image isn't layout 6, names no VS stage, or the count is out of range.
std::optional<std::vector<VsOutputRecord>> fssm_vs_output_table(std::span<const uint8_t> fssm);

// Where those inputs land. The bits, in register order, are perspective sample, center and
// centroid (two VGPRs each: I and J), perspective pull model (three), linear sample, center and
// centroid (two each), line stipple, then one VGPR each for position X, Y, Z and W, front face,
// ancillary data, sample coverage, and fixed-point position.
struct PsInputs {
    uint32_t vgprs = 0;                  // VGPRs the hardware fills
    uint32_t barycentrics = 0;           // bit n set: VGPR n holds an I or a J
    std::optional<uint32_t> position[4]; // VGPRs of position X, Y, Z and W, if enabled
    std::optional<uint32_t> front_face;
};
PsInputs ps_inputs(uint32_t enable);

} // namespace g6::shader
