#pragma once

// A census of shaders.rpf: every record found, what its container holds, and whether its native
// program unpacks. shaderrecomp prints it, and the game suite checks it against docs/shaders.md.
// It holds counts and the archive's own record names, so like the archive it stays on the
// user's machine.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "archive.h"
#include "console.h"
#include "dxbc.h"
#include "rdna2.h"
#include "regs.h"

namespace g6::shader {

// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's map move constructor allocates.
struct Census {
    ArchiveScanStats scan;
    std::set<std::string> names;
    std::map<uint8_t, size_t> record_tags;
    std::map<uint32_t, size_t> parts; // by fourcc, once per container that has the part
    std::map<uint16_t, size_t> kinds;
    std::map<std::string, size_t> shader_models;
    size_t no_dxil = 0;
    size_t no_bitcode_marker = 0;
    size_t llvm_bitcode = 0;
    size_t other_bitcode = 0;
    size_t no_xdat = 0;
    std::map<uint32_t, size_t> xdat_tags;
    std::map<uint32_t, size_t> xdat_unknown; // by the value of the table's unknown word
    size_t no_native_program = 0;
    std::map<size_t, size_t> programs_per_shader; // shaders by how many native programs they carry
    std::map<uint16_t, size_t> multi_program_shaders; // by program kind, those with more than one
    size_t native_programs = 0;
    std::map<uint16_t, size_t> fssm_versions;
    std::vector<uint32_t> fssm_sizes;
    // By program kind, counting byte-identical copies once. A translation cache keyed by the
    // native program needs one entry per distinct program, however many records share it.
    std::map<uint16_t, size_t> distinct_containers;
    std::map<uint16_t, size_t> distinct_native_programs;
    // Shaders by kind and the stages their native programs name (fssm_stages): one program's
    // hardware stages joined by "+", programs by " | ". A vertex shader compiled twice, once for
    // a geometry pipeline, reads "ES | VS".
    std::map<std::pair<uint16_t, std::string>, size_t> stage_lists;
    std::map<uint8_t, std::set<uint8_t>> api_stages; // hardware stage -> API stage values seen
    std::map<uint32_t, size_t> ps_input_enables;     // pixel programs by input enable word
    std::map<uint32_t, size_t> fssm_layouts;         // programs by layout word (fssm_layout)
    // Pixel programs' input tables (fssm_ps_input_table), over all their records.
    size_t ps_input_tables = 0;
    size_t ps_input_empty_tables = 0;
    size_t ps_input_records = 0;
    std::map<uint8_t, size_t> ps_input_kinds;
    std::map<uint8_t, size_t> ps_input_flags;
    size_t ps_input_index_mismatches = 0; // byte 3 isn't the record's index, or byte 4 isn't zero
    size_t ps_input_byte5 = 0;            // byte 5 nonzero
    size_t ps_input_byte5_unpacked = 0;   // of those, the ones whose flags don't say f16
    size_t ps_input_byte6_differs = 0;    // byte 6 isn't byte 1
    // Vertex stages' output tables (fssm_vs_output_table), over all their records.
    size_t vs_output_tables = 0;
    size_t vs_output_records = 0;
    std::map<uint8_t, size_t> vs_output_kinds;
    size_t vs_output_index_mismatches = 0; // byte 2 isn't the record's index
    size_t vs_output_extras = 0;           // bytes 3 to 5 not all zero
    size_t vs_output_reg_differs = 0;      // byte 1 isn't the record's index
    // Containers some part of which didn't parse, and the first such error.
    size_t malformed = 0;
    std::string first_error;
};

struct CensusShader {
    const ShaderRecord& record;
    std::optional<DxilProgram> program;
    std::vector<std::vector<uint8_t>> native_programs; // FSSM images, in table order
};

// Scans the archive and counts. `each`, if given, sees every record once it's unpacked.
Census take_census(std::span<const uint8_t> archive,
                   const std::function<void(const CensusShader&)>& each = {});

// Whether a pixel program interpolates from the VGPRs its input enable word fills (ps_inputs):
// every interpolation that takes an I or a J has to read it from one of them.
struct InterpolationCheck {
    size_t instructions = 0; // interpolations that take an I or a J
    size_t outside = 0;      // of those, the ones that read another register
    size_t vop3 = 0;         // of those, the ones in VOP3 encoding
};
InterpolationCheck check_interpolation(const rdna2::Program& program, const PsInputs& inputs);

// Where the f16 interpolations (v_interp_p1ll_f16, p1lv_f16, p2_f16) read in the shader's input
// signature. An input record with flags bit 3 holds f16 values. With bit 3 alone, the register
// byte 1 names has one f16 per channel, in the low half. With bit 4 as well the record is packed:
// each channel holds two halves, so the attribute has eight slots, slot = 2 * channel + high.
//   * The register byte 1 names fills the slots from 0, one component each.
//   * A second register starts at the next whole channel, slot 2 * ceil(n / 2), where n is the
//     first register's highest declared component plus one.
//   * The second register is the one byte 5 names when it differs from byte 1. When byte 5
//     repeats byte 1, it is the program's only f16 input register that the shader reads and no
//     record names, if there is one; n then counts only the elements the shader reads.
// The rule is fitted to 1.0.2.1's programs (docs/shaders.md).
struct F16InputCheck {
    size_t packed_records = 0;   // input records with flags bit 4
    size_t named_partners = 0;   // of those, the ones whose byte 5 names a second register
    size_t unnamed_partners = 0; // programs with an unnamed second register
    size_t reads = 0;            // distinct attribute, channel and half each program reads
    size_t on_read = 0;          // land on a component the signature marks read
    size_t on_unread = 0;        // land on a declared component it doesn't mark read
    size_t outside = 0;          // land on no component of either register
    size_t not_f16 = 0;          // read a record without flags bit 3, or one past the table
};
// Adds a pixel program, with its input table (fssm_ps_input_table) and its shader's input
// signature (ISG1).
void check_f16_inputs(F16InputCheck& check, const rdna2::Program& program,
                      std::span<const PsInputRecord> records,
                      std::span<const SignatureElement> signature);

// What a compute wave starts with, from COMPUTE_PGM_RSRC2 in AMD's public register reference: the
// user SGPRs from s0, then the workgroup IDs X, Y and Z, the thread-group info and the scratch
// offset, each only if enabled; and the thread IDs in v0, v1 and v2, up to the count enabled.
struct ComputeSetup {
    unsigned user_sgprs = 0;
    unsigned sgprs = 0; // the user SGPRs and the system SGPRs after them
    unsigned vgprs = 0;
};
ComputeSetup compute_setup(uint32_t rsrc2);

// What the register model (regs.h) finds the programs of one hardware stage read before anything
// writes them: the registers the hardware and the driver load when a wave starts.
struct StageInputs {
    size_t programs = 0;
    std::map<unsigned, size_t> wave_sizes; // 32 or 64, by the setup word (fssm_setup)
    size_t wave_decoded = 0;               // programs whose code tells (rdna2::wave_size)
    size_t wave_differs = 0;               // of those, the ones where it isn't the word's
    std::array<size_t, rdna2::kScalarRegisters> scalar{}; // programs that read each register first
    std::array<size_t, rdna2::kVectorRegisters> vector{};
    std::array<size_t, rdna2::kScalarRegisters> scalar_after_call{};
    std::array<size_t, rdna2::kVectorRegisters> vector_after_call{};
    size_t calls = 0;
    size_t indirect = 0; // programs that jump or call to a target not known (rdna2::Program)
    size_t dynamic = 0;
    size_t with_unmodelled = 0;
    std::map<std::string, size_t> unmodelled; // instructions by mnemonic
    // Programs checked against their setup words, and those reading a register the words don't
    // load. Every stage's SGPRs are checked: the user SGPRs fssm_setup gives, after the merged
    // stages' eight system SGPRs, and the system SGPRs after them (COMPUTE_PGM_RSRC2's for CS;
    // the primitive mask, and the collision wave ID when RSRC2 asks for it, for PS). VGPRs are
    // checked for CS (COMPUTE_PGM_RSRC2), PS (the input enable word), and VS, ES, and mesh (the
    // VGPR component count, fssm_vgpr_components); not for GS, whose count hasn't been found.
    // Programs that index registers through m0 (an array kept in registers) aren't checked:
    // which registers they write isn't known before they run.
    size_t setup_checked = 0;
    size_t setup_vgprs_checked = 0;
    size_t setup_outside = 0;
    size_t setup_unchecked = 0;
    size_t rsrc2 = 0;              // programs with an RSRC2 word (fssm_rsrc2)
    size_t rsrc2_user_differs = 0; // whose user SGPR count isn't the setup word's
    size_t fetch = 0;              // programs whose setup word sets the fetch bit
    // Programs where the fetch bit doesn't say whether the code calls, or a program with the bit
    // set doesn't read the pair the word names before writing it.
    size_t fetch_differs = 0;
    // Programs with the fetch bit whose input signature count_inputs was given. In those that
    // differ, the VGPRs read after the call aren't exactly where fetch_layout puts the elements
    // the shader reads, plus vertex and instance IDs it declares. `fetch_by_register_differs`
    // counts the same check against a layout by input register (element r at first_fetched +
    // 4r), which an earlier version of docs/shaders.md gave. `fetch_with_unread` counts the
    // checked programs whose signature declares an element, not a system value, they never read.
    size_t fetch_layout_checked = 0;
    size_t fetch_layout_differs = 0;
    size_t fetch_by_register_differs = 0;
    size_t fetch_with_unread = 0;
    std::map<uint32_t, size_t> vgpr_components; // programs by fssm_vgpr_components
    // ES and GS: programs by the LDS size in their RSRC2 word (gs_rsrc2), and ES programs whose
    // ES_VGPR_COMP_CNT there isn't the component count word.
    std::map<unsigned, size_t> lds_sizes;
    size_t es_components_differ = 0;
    // Programs that read LDS (ds_read*), write it (ds_write*), and export (exp).
    size_t lds_readers = 0;
    size_t lds_writers = 0;
    size_t exporters = 0;
};

struct ProgramInputs {
    uint8_t stage = 0;                    // the first stage record's hardware stage
    unsigned wave = 64;                   // by the setup word
    std::optional<unsigned> decoded_wave; // rdna2::wave_size
    rdna2::LiveIns live;
    bool setup_outside = false;
};

// Adds one FSSM image's program to the counts of its first hardware stage. `signature` is the
// shader's input signature (ISG1), for checking the fetch layout; leave it empty to skip that.
ProgramInputs count_inputs(std::map<uint8_t, StageInputs>& stages, std::span<const uint8_t> fssm,
                           const rdna2::Program& program,
                           std::span<const SignatureElement> signature = {});

// GS programs' output primitive types (fssm_gs_out_prim_types) against their shaders' PSV0
// (psv_geometry): stream 0's type should be the one D3D's output topology names.
struct GsOutputCheck {
    std::map<std::array<uint8_t, 4>, size_t> types; // GS programs by their four streams' types
    std::map<uint32_t, size_t> streams;             // those with a PSV0 part, by its stream mask
    size_t checked = 0;                             // GS programs whose shader has a PSV0 part
    size_t differs = 0; // of those, the ones whose stream 0 type isn't the topology's
};
// Adds an FSSM image whose first stage is GS, with its shader's container; skips others. Throws
// FormatError when the PSV0 part is malformed.
void check_gs_output(GsOutputCheck& check, std::span<const uint8_t> fssm,
                     std::span<const uint8_t> container);

} // namespace g6::shader
