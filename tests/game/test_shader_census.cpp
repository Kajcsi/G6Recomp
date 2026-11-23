// Shader archive census on the user's files (label `game`, local only, never in CI): scans
// shaders.rpf with shaderrecomp's reader and checks the counts recorded in docs/shaders.md, which
// a second, independent reader found first. The pipeline cache's counts were found first by a
// separate script. It prints counts only; nothing it reads or derives is
// written anywhere. Skips when G6_GAME_DIR is unset or has no shaders.rpf.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "archive.h"
#include "census.h"
#include "console.h"
#include "dxbc.h"
#include "g6/rpf9.h"
#include "g6test.h"
#include "pscache.h"
#include "rdna2.h"

namespace fs = std::filesystem;
namespace sh = g6::shader;
namespace ps = g6::shader::pscache;
namespace rpf9 = g6::rpf9;

namespace {

fs::path game_dir() {
    wchar_t* value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, L"G6_GAME_DIR") != 0 || value == nullptr) {
        return {};
    }
    fs::path path(value);
    std::free(value);
    return path;
}

size_t kind_count(const sh::Census& census, sh::ProgramKind kind) {
    const auto it = census.kinds.find(static_cast<uint16_t>(kind));
    return it == census.kinds.end() ? 0 : it->second;
}

} // namespace

TEST_CASE("shaders.rpf holds the 1.0.2.1 scene shaders, each with a native program") {
    const fs::path path = game_dir() / "shaders.rpf";
    if (game_dir().empty() || !fs::is_regular_file(path)) {
        SKIP("G6_GAME_DIR is not set or has no shaders.rpf");
    }
    const sh::MappedFile archive(path);
    const sh::Census census = sh::take_census(archive.bytes());
    fmt::print("shaders.rpf: {} records ({} rejected), {} names, {} native programs\n",
               census.scan.records, census.scan.rejected, census.names.size(),
               census.native_programs);

    INFO(census.first_error);
    CHECK_EQ(census.malformed, size_t{0});
    CHECK_EQ(census.scan.records, size_t{26503});
    CHECK_EQ(census.names.size(), size_t{8145});
    CHECK_EQ(kind_count(census, sh::ProgramKind::kVertex), size_t{2236});
    CHECK_EQ(kind_count(census, sh::ProgramKind::kPixel), size_t{12101});
    CHECK_EQ(kind_count(census, sh::ProgramKind::kCompute), size_t{2689});
    CHECK_EQ(kind_count(census, sh::ProgramKind::kMesh), size_t{9282});
    CHECK_EQ(kind_count(census, sh::ProgramKind::kGeometry), size_t{195});

    CHECK_EQ(census.shader_models.at("6.6"), census.scan.records);

    // No container carries LLVM bitcode: each has the NOBC marker and FSSM images of native GPU
    // code, so translation starts from the native programs (docs/shaders.md). 195 vertex shaders
    // carry two; every geometry shader carries one.
    CHECK_EQ(census.no_bitcode_marker, census.scan.records);
    CHECK_EQ(census.llvm_bitcode, size_t{0});
    CHECK_EQ(census.no_native_program, size_t{0});
    CHECK_EQ(census.programs_per_shader.at(1), size_t{26308});
    CHECK_EQ(census.programs_per_shader.at(2), size_t{195});
    CHECK_EQ(census.multi_program_shaders.size(), size_t{1});
    CHECK_EQ(census.multi_program_shaders.at(static_cast<uint16_t>(sh::ProgramKind::kVertex)),
             size_t{195});
    CHECK_EQ(census.native_programs, size_t{26698});
    CHECK_EQ(census.fssm_versions.at(0x101), size_t{26698});
    CHECK_EQ(census.xdat_unknown.at(0x24), census.scan.records);

    // The stages each program names (fssm_stages). A vertex shader with two programs is compiled
    // once as the export stage of a geometry pipeline and once as a plain vertex stage, and a
    // geometry program names its copy shader's vertex stage too.
    const auto stages = [&](sh::ProgramKind kind, const std::string& list) {
        const auto it = census.stage_lists.find({static_cast<uint16_t>(kind), list});
        return it == census.stage_lists.end() ? 0 : it->second;
    };
    CHECK_EQ(census.stage_lists.size(), size_t{6});
    CHECK_EQ(stages(sh::ProgramKind::kPixel, "PS"), size_t{12101});
    CHECK_EQ(stages(sh::ProgramKind::kVertex, "VS"), size_t{2041});
    CHECK_EQ(stages(sh::ProgramKind::kVertex, "ES | VS"), size_t{195});
    CHECK_EQ(stages(sh::ProgramKind::kGeometry, "GS+VS"), size_t{195});
    CHECK_EQ(stages(sh::ProgramKind::kCompute, "CS"), size_t{2689});
    CHECK_EQ(stages(sh::ProgramKind::kMesh, "7"), size_t{9282});
    // Every pixel program has an input enable word, in 14 combinations, all with perspective
    // center interpolation.
    size_t enables = 0;
    for (const auto& [enable, count] : census.ps_input_enables) {
        CHECK((enable & 0x2) != 0);
        enables += count;
    }
    CHECK_EQ(enables, size_t{12101});
    CHECK_EQ(census.ps_input_enables.size(), size_t{14});

    // Every program has layout 6, so the stage tables sit where fssm_ps_input_table and
    // fssm_vs_output_table read them. Every pixel program has an input table; a few records are
    // kind 7, so a reader that stops at the first record not of kind 6 misses some. Byte 5 is set
    // only with flags bit 4, and byte 6 often isn't the register byte 1 names.
    CHECK_EQ(census.fssm_layouts.size(), size_t{1});
    CHECK_EQ(census.fssm_layouts.at(sh::kFssmLayout), size_t{26698});
    CHECK_EQ(census.ps_input_tables, size_t{12101});
    CHECK_EQ(census.ps_input_empty_tables, size_t{1784});
    CHECK_EQ(census.ps_input_records, size_t{55937});
    CHECK_EQ(census.ps_input_kinds.at(6), size_t{55905});
    CHECK_EQ(census.ps_input_kinds.at(7), size_t{32});
    CHECK_EQ(census.ps_input_flags.size(), size_t{5});
    CHECK_EQ(census.ps_input_flags.at(0x01), size_t{4214});
    CHECK_EQ(census.ps_input_flags.at(0x02), size_t{40064});
    CHECK_EQ(census.ps_input_flags.at(0x1A), size_t{11239});
    CHECK_EQ(census.ps_input_index_mismatches, size_t{0});
    CHECK_EQ(census.ps_input_byte5, size_t{11069});
    CHECK_EQ(census.ps_input_byte5_unpacked, size_t{0});
    CHECK_EQ(census.ps_input_byte6_differs, size_t{22168});
    // Every program with a legacy vertex stage has an output table: the plain vertex programs,
    // the second program of the vertex shaders that have two, and the geometry programs' copy
    // shaders. The output register usually isn't the param number.
    CHECK_EQ(census.vs_output_tables, size_t{2041 + 195 + 195});
    CHECK_EQ(census.vs_output_records, size_t{9526});
    CHECK_EQ(census.vs_output_kinds.at(6), size_t{9495});
    CHECK_EQ(census.vs_output_index_mismatches, size_t{0});
    CHECK_EQ(census.vs_output_extras, size_t{822});
    CHECK_EQ(census.vs_output_reg_differs, size_t{8123});
}

TEST_CASE("shaders.rpf repeats containers and native programs") {
    const fs::path path = game_dir() / "shaders.rpf";
    if (game_dir().empty() || !fs::is_regular_file(path)) {
        SKIP("G6_GAME_DIR is not set or has no shaders.rpf");
    }
    const sh::MappedFile archive(path);
    const sh::Census census = sh::take_census(archive.bytes());
    const auto distinct = [](const std::map<uint16_t, size_t>& counts, sh::ProgramKind kind) {
        const auto it = counts.find(static_cast<uint16_t>(kind));
        return it == counts.end() ? 0 : it->second;
    };

    // 8,765 distinct mesh containers is the count xbox-emulator reports (docs/related-work.md).
    // They hold 8,291 distinct native programs, which is what a translation cache would key on.
    CHECK_EQ(distinct(census.distinct_containers, sh::ProgramKind::kMesh), size_t{8765});
    CHECK_EQ(distinct(census.distinct_native_programs, sh::ProgramKind::kMesh), size_t{8291});
    CHECK_EQ(distinct(census.distinct_containers, sh::ProgramKind::kPixel), size_t{12056});
    CHECK_EQ(distinct(census.distinct_native_programs, sh::ProgramKind::kPixel), size_t{12037});
    CHECK_EQ(distinct(census.distinct_containers, sh::ProgramKind::kVertex), size_t{2191});
    CHECK_EQ(distinct(census.distinct_native_programs, sh::ProgramKind::kVertex), size_t{2313});
    CHECK_EQ(distinct(census.distinct_containers, sh::ProgramKind::kCompute), size_t{2682});
    CHECK_EQ(distinct(census.distinct_native_programs, sh::ProgramKind::kCompute), size_t{2680});
    CHECK_EQ(distinct(census.distinct_containers, sh::ProgramKind::kGeometry), size_t{195});
    CHECK_EQ(distinct(census.distinct_native_programs, sh::ProgramKind::kGeometry), size_t{195});
}

TEST_CASE("every native program decodes as RDNA 2 to the end of its code") {
    const fs::path path = game_dir() / "shaders.rpf";
    if (game_dir().empty() || !fs::is_regular_file(path)) {
        SKIP("G6_GAME_DIR is not set or has no shaders.rpf");
    }
    const sh::MappedFile archive(path);
    sh::rdna2::DecodeStats stats;
    sh::InterpolationCheck interpolation;
    size_t interpolating = 0;
    size_t outside_programs = 0;
    std::map<uint8_t, sh::StageInputs> inputs;
    sh::GsOutputCheck gs_output;
    sh::F16InputCheck f16_inputs;
    sh::take_census(archive.bytes(), [&](const sh::CensusShader& shader) {
        const std::vector<sh::SignatureElement> signature =
            sh::input_signature(shader.record.container);
        for (const std::vector<uint8_t>& fssm : shader.native_programs) {
            const sh::rdna2::Program program = sh::rdna2::decode_program(sh::fssm_code(fssm));
            stats.add(program);
            sh::count_inputs(inputs, fssm, program, signature);
            sh::check_gs_output(gs_output, fssm, shader.record.container);
            if (const std::optional<uint32_t> enable = sh::fssm_ps_input_enable(fssm)) {
                const sh::InterpolationCheck check =
                    sh::check_interpolation(program, sh::ps_inputs(*enable));
                interpolation.instructions += check.instructions;
                interpolation.outside += check.outside;
                interpolation.vop3 += check.vop3;
                interpolating += check.instructions > 0 ? 1 : 0;
                outside_programs += check.outside > 0 ? 1 : 0;
            }
            if (const auto records = sh::fssm_ps_input_table(fssm)) {
                sh::check_f16_inputs(f16_inputs, program, *records, signature);
            }
        }
    });
    // The f16 interpolations read only f16 input records, and the packing rule in census.h puts
    // all but 6 of their reads on a component the shader reads. Those 6, one per program, read
    // the low half of channel 3 of an attribute whose two registers fill only slots 0 to 5.
    CHECK_EQ(f16_inputs.packed_records, size_t{11239});
    CHECK_EQ(f16_inputs.named_partners, size_t{9859});
    CHECK_EQ(f16_inputs.unnamed_partners, size_t{184});
    CHECK_EQ(f16_inputs.reads, size_t{59191});
    CHECK_EQ(f16_inputs.on_read, size_t{59185});
    CHECK_EQ(f16_inputs.on_unread, size_t{0});
    CHECK_EQ(f16_inputs.outside, size_t{6});
    CHECK_EQ(f16_inputs.not_f16, size_t{0});
    // Pixel programs interpolate from the I and J registers their input enable word loads. The
    // one exception copies the J register to another VGPR first and interpolates from the copy.
    CHECK_EQ(interpolating, size_t{10260});
    CHECK_EQ(interpolation.instructions, size_t{355912});
    CHECK_EQ(interpolation.vop3, size_t{118734});
    CHECK_EQ(interpolation.outside, size_t{4});
    CHECK_EQ(outside_programs, size_t{1});
    size_t unnamed = 0;
    for (const auto& entry : stats.unnamed) {
        unnamed += entry.second;
    }
    fmt::print("decoded {} programs, {} reachable instructions; {} unnamed opcodes in {} "
               "instructions; {} with data after the code, {} with indirect jumps or calls\n",
               stats.programs, stats.instructions, stats.unnamed.size(), unnamed, stats.with_data,
               stats.indirect);

    // Following control flow from the entry, every reachable instruction has a known encoding
    // and fits in the code, every path ends in s_endpgm (or an indirect jump), and every branch
    // lands on an instruction boundary inside the code. Far jumps (s_getpc_b64, then a 64-bit
    // constant added, then s_setpc_b64) land inside the code too.
    CHECK_EQ(stats.programs, size_t{26698});
    CHECK_EQ(stats.instructions, size_t{44333402});
    CHECK_EQ(stats.unknown, size_t{0});
    CHECK_EQ(stats.truncated, size_t{0});
    CHECK_EQ(stats.falls_off, size_t{0});
    CHECK_EQ(stats.bad_branch, size_t{0});
    CHECK_EQ(stats.far_jumps, size_t{1450});
    CHECK_EQ(stats.far_jump_programs, size_t{13});
    CHECK_EQ(stats.indirect, size_t{983});
    CHECK_EQ(stats.with_data, size_t{6779});
    CHECK_EQ(stats.data_bytes, size_t{390176});
    CHECK_EQ(unnamed, size_t{0});
    // No program reaches GDS, GWS, ds_ordered_count, ds_append, or ds_consume.
    CHECK_EQ(stats.gds, size_t{0});
    CHECK_EQ(stats.append_consume, size_t{0});

    // The registers each program reads before anything writes them (regs.h), by the hardware
    // stage of its first record. The register model knows every reached instruction. Wherever
    // the code tells the wave size, it is the setup word's; every program whose registers can be
    // checked reads only registers its setup words load; the fetch bit marks exactly the
    // programs that call the fetch subroutine; and the RSRC2 words give the setup word's user
    // SGPR count.
    for (const auto& [hardware, stage] : inputs) {
        CHECK_EQ(stage.with_unmodelled, size_t{0});
        CHECK_EQ(stage.wave_differs, size_t{0});
        CHECK_EQ(stage.setup_outside, size_t{0});
        CHECK_EQ(stage.setup_checked + stage.setup_unchecked, stage.programs);
        CHECK_EQ(stage.setup_unchecked, stage.dynamic);
        CHECK_EQ(stage.fetch_differs, size_t{0});
        CHECK_EQ(stage.rsrc2_user_differs, size_t{0});
        CHECK_EQ(stage.rsrc2, hardware == sh::kHardwareMesh ? size_t{0} : stage.programs);
    }
    // The register counts word (fssm_register_counts) is what the model makes of the code and
    // the setup words in every program but 114, which count one to three more VGPRs than the
    // code uses: 113 mesh programs and one pixel program. None counts fewer. Per stage: VGPR
    // counts above the model, then the counts the setup words or the stage's minimum set beyond
    // the code's registers, VGPRs and SGPRs. GS programs count only the SGPRs their code uses,
    // and 179 of them fewer than the setup check has the words load.
    const std::map<uint8_t, std::array<size_t, 3>> counts = {
        {sh::kHardwareEs, {0, 0, 0}},    {sh::kHardwareGs, {0, 0, 0}},
        {sh::kHardwareVs, {0, 17, 194}}, {sh::kHardwarePs, {1, 1, 1089}},
        {sh::kHardwareCs, {0, 19, 5}},   {sh::kHardwareMesh, {113, 0, 0}}};
    for (const auto& [hardware, stage] : inputs) {
        INFO(sh::hardware_stage_name(hardware));
        REQUIRE(counts.contains(hardware));
        const std::array<size_t, 3>& expected = counts.at(hardware);
        CHECK_EQ(stage.vgpr_counts.equal + stage.vgpr_counts.above, stage.programs);
        CHECK_EQ(stage.vgpr_counts.above, expected[0]);
        CHECK_EQ(stage.vgpr_counts.beyond_code, expected[1]);
        CHECK_EQ(stage.sgpr_counts.equal, stage.programs);
        CHECK_EQ(stage.sgpr_counts.beyond_code, expected[2]);
        CHECK_EQ(stage.sgpr_count_below_loaded,
                 hardware == sh::kHardwareGs ? size_t{179} : size_t{0});
    }
    // The only jumps and calls left with unknown targets: vertex programs calling the fetch
    // subroutine, and every ES program, which copies s[6:7] to another pair and ends by jumping
    // there (158 of them call the fetch subroutine first).
    size_t indirect = 0;
    for (const auto& [hardware, stage] : inputs) {
        indirect += stage.indirect;
    }
    CHECK_EQ(indirect, stats.indirect);
    CHECK_EQ(inputs[sh::kHardwareVs].indirect, size_t{788});
    CHECK_EQ(inputs[sh::kHardwareEs].indirect, size_t{195});
    CHECK_EQ(inputs[sh::kHardwareEs].scalar[6] + inputs[sh::kHardwareEs].scalar_after_call[6],
             size_t{195});
    const auto wave = [](const sh::StageInputs& stage, unsigned size) {
        const auto it = stage.wave_sizes.find(size);
        return it == stage.wave_sizes.end() ? size_t{0} : it->second;
    };
    // Compute programs read user SGPRs from s4, never s0 to s3.
    const sh::StageInputs& cs = inputs[sh::kHardwareCs];
    CHECK_EQ(cs.programs, size_t{2689});
    CHECK_EQ(cs.setup_unchecked, size_t{16});
    CHECK_EQ(cs.setup_vgprs_checked, size_t{2673});
    CHECK_EQ(cs.scalar[0] + cs.scalar[1] + cs.scalar[2] + cs.scalar[3], size_t{0});
    CHECK_EQ(cs.vector[0], size_t{2605});
    CHECK_EQ(wave(cs, 64), size_t{1154});
    CHECK_EQ(wave(cs, 32), size_t{1535});
    CHECK_EQ(cs.wave_decoded, size_t{2314});
    const sh::StageInputs& ps = inputs[sh::kHardwarePs];
    CHECK_EQ(ps.programs, size_t{12101});
    CHECK_EQ(ps.setup_unchecked, size_t{3});
    CHECK_EQ(ps.setup_vgprs_checked, size_t{12098});
    CHECK_EQ(ps.scalar[9], size_t{11467});
    CHECK_EQ(wave(ps, 64), size_t{10974});
    CHECK_EQ(wave(ps, 32), size_t{1127});
    CHECK_EQ(ps.wave_decoded, size_t{11158});
    // Vertex programs that fetch call the driver's subroutine through s[4:5] and find the
    // attributes from v4 up after the call; ES programs call through s[12:13] and find them from
    // v9 up.
    const sh::StageInputs& vs = inputs[sh::kHardwareVs];
    CHECK_EQ(vs.programs, size_t{2236});
    CHECK_EQ(vs.calls, size_t{788});
    CHECK_EQ(vs.fetch, size_t{788});
    CHECK_EQ(vs.scalar[4], size_t{788});
    CHECK_EQ(vs.scalar[5], size_t{788});
    CHECK_EQ(vs.vector_after_call[4], size_t{787});
    CHECK_EQ(vs.vector[0], size_t{1438});
    CHECK_EQ(vs.vector[3], size_t{386});
    CHECK_EQ(wave(vs, 64), size_t{2236});
    // After the call, each fetching program reads exactly where fetch_layout puts the input
    // elements it reads: four VGPRs per element read, none for an unread one. In 12 programs an
    // unread element comes before a read one, and a layout by input register misplaces the rest.
    CHECK_EQ(vs.fetch_layout_checked, size_t{788});
    CHECK_EQ(vs.fetch_layout_differs, size_t{0});
    CHECK_EQ(vs.fetch_by_register_differs, size_t{12});
    CHECK_EQ(vs.fetch_with_unread, size_t{34});
    // The VGPR component count is 3 where the program or its fetch subroutine needs the instance
    // ID in v3; ES programs load only the vertex ID at v5, mesh programs v5 and v6.
    CHECK_EQ(vs.setup_vgprs_checked, vs.setup_checked);
    CHECK((vs.vgpr_components == std::map<uint32_t, size_t>{{0, 1742}, {3, 494}}));
    const sh::StageInputs& es = inputs[sh::kHardwareEs];
    CHECK_EQ(es.programs, size_t{195});
    CHECK_EQ(es.calls, size_t{158});
    CHECK_EQ(es.fetch, size_t{158});
    CHECK_EQ(es.scalar[3], size_t{195});
    CHECK_EQ(es.scalar[12], size_t{158});
    CHECK_EQ(es.vector_after_call[9], size_t{158});
    CHECK_EQ(es.vector[5], size_t{37});
    CHECK_EQ(wave(es, 64), size_t{195});
    CHECK_EQ(es.fetch_layout_checked, size_t{158});
    CHECK_EQ(es.fetch_layout_differs, size_t{0});
    CHECK_EQ(es.fetch_by_register_differs, size_t{0});
    CHECK_EQ(es.fetch_with_unread, size_t{0});
    CHECK_EQ(es.setup_vgprs_checked, size_t{195});
    CHECK((es.vgpr_components == std::map<uint32_t, size_t>{{0, 195}}));
    const sh::StageInputs& gs = inputs[sh::kHardwareGs];
    CHECK_EQ(gs.programs, size_t{195});
    constexpr std::array<size_t, 5> kGsScalars = {2, 3, 8, 9, 16};
    for (const size_t sgpr : kGsScalars) {
        CHECK_EQ(gs.scalar.at(sgpr), size_t{195});
    }
    CHECK_EQ(gs.vector[0] + gs.vector[1], size_t{390});
    CHECK_EQ(wave(gs, 64), size_t{195});
    CHECK_EQ(gs.setup_vgprs_checked, size_t{0});
    // The merged stage's RSRC2 layout: every ES word asks for LDS_SIZE 2, and its
    // ES_VGPR_COMP_CNT is the component count word's; the GS words ask for no LDS. The halves
    // pass vertices through LDS: every ES program writes it, every GS program reads it, and
    // neither exports. GS programs also write LDS: their output stays on chip.
    CHECK((es.lds_sizes == std::map<unsigned, size_t>{{2, 195}}));
    CHECK_EQ(es.es_components_differ, size_t{0});
    CHECK_EQ(es.lds_writers, size_t{195});
    CHECK_EQ(es.lds_readers, size_t{0});
    CHECK_EQ(es.exporters, size_t{0});
    CHECK((gs.lds_sizes == std::map<unsigned, size_t>{{0, 195}}));
    CHECK_EQ(gs.lds_readers, size_t{195});
    CHECK_EQ(gs.lds_writers, size_t{195});
    CHECK_EQ(gs.exporters, size_t{0});
    // Every GS program's output primitive word says triangle strips on stream 0, as its PSV0's
    // topology does, and 3 on the other streams, which no shader writes.
    constexpr std::array<uint8_t, 4> kStrips = {2, 3, 3, 3};
    CHECK((gs_output.types == std::map<std::array<uint8_t, 4>, size_t>{{kStrips, 195}}));
    CHECK_EQ(gs_output.checked, size_t{195});
    CHECK_EQ(gs_output.differs, size_t{0});
    CHECK((gs_output.streams == std::map<uint32_t, size_t>{{1, 195}}));
    const sh::StageInputs& mesh = inputs[sh::kHardwareMesh];
    CHECK_EQ(mesh.programs, size_t{9282});
    CHECK_EQ(mesh.setup_unchecked, size_t{4});
    CHECK_EQ(mesh.scalar[3], size_t{9282});
    CHECK_EQ(mesh.scalar[17], size_t{9282});
    CHECK_EQ(mesh.vector[5], size_t{9282});
    CHECK_EQ(wave(mesh, 64), size_t{9282});
    CHECK_EQ(mesh.setup_vgprs_checked, size_t{9278});
    CHECK((mesh.vgpr_components == std::map<uint32_t, size_t>{{1, 9282}}));
    // Every VS, pixel, and mesh program exports; otherwise only compute and mesh programs touch
    // LDS.
    CHECK_EQ(vs.exporters, vs.programs);
    CHECK_EQ(ps.exporters, ps.programs);
    CHECK_EQ(mesh.exporters, mesh.programs);
    CHECK_EQ(cs.exporters, size_t{0});
    CHECK_EQ(vs.lds_readers + vs.lds_writers + ps.lds_readers + ps.lds_writers, size_t{0});
    CHECK_EQ(cs.lds_writers, size_t{484});
    CHECK_EQ(cs.lds_readers, size_t{454});
    CHECK_EQ(mesh.lds_writers, size_t{5});
    CHECK_EQ(mesh.lds_readers, size_t{5});
}

TEST_CASE("shaders.rpf holds the driver's pipeline cache, tied to the shader records") {
    const fs::path path = game_dir() / "shaders.rpf";
    if (game_dir().empty() || !fs::is_regular_file(path)) {
        SKIP("G6_GAME_DIR is not set or has no shaders.rpf");
    }
    const sh::MappedFile archive(path);

    // The data area holds the stored shader records, the cache as a run of 64 KiB zlib streams,
    // and as many bytes at the end as the header's names size (docs/rpf.md).
    const rpf9::Carve carve = rpf9::carve(archive.bytes());
    REQUIRE(carve.entries.size() == 1);
    const rpf9::Entry& entry = carve.entries[0];
    CHECK(entry.head.kind == rpf9::Kind::kPipelineCache);
    CHECK_EQ(entry.offset, uint64_t{0x10579000});
    CHECK_EQ(entry.streams, size_t{4205});
    CHECK_EQ(entry.size, uint64_t{275540780});
    REQUIRE(carve.gaps.size() == 2);
    CHECK_EQ(carve.gaps[0].offset, uint64_t{0x8680});
    CHECK_EQ(carve.gaps[0].size, uint64_t{274139520});
    CHECK_EQ(carve.gaps[1].size, uint64_t{carve.header.names_size});

    const std::vector<uint8_t> data = rpf9::read_entry(archive.bytes(), carve, entry);
    const ps::PipelineCache cache = ps::parse(data);
    CHECK_EQ(cache.graphics.size(), size_t{25471});
    CHECK_EQ(cache.compute.size(), size_t{2693});
    CHECK_EQ(cache.shaders.size(), size_t{21522});
    CHECK_EQ(cache.metadata.size(), size_t{54576});

    // Every shader decodes as RDNA 2 to the end of its code, and every binding blob a pipeline
    // refers to frames exactly.
    sh::rdna2::DecodeStats stats;
    std::vector<uint32_t> indirect;
    for (uint32_t i = 0; i < cache.shaders.size(); ++i) {
        const sh::rdna2::Program program = sh::rdna2::decode_program(ps::shader_code(cache, i));
        stats.add(program);
        if (program.indirect) {
            indirect.push_back(i);
        }
    }
    CHECK_EQ(stats.instructions, size_t{42871155});
    CHECK_EQ(stats.unknown, size_t{0});
    CHECK_EQ(stats.truncated, size_t{0});
    CHECK_EQ(stats.falls_off, size_t{0});
    CHECK_EQ(stats.bad_branch, size_t{0});
    CHECK_EQ(stats.far_jumps, size_t{57});
    CHECK_EQ(stats.far_jump_programs, size_t{8});
    CHECK_EQ(stats.indirect, size_t{2});
    // The driver's recompile uses no GDS either, but adds ds_append and ds_consume on LDS to 30
    // compute shaders.
    CHECK_EQ(stats.gds, size_t{0});
    CHECK_EQ(stats.append_consume, size_t{73});
    CHECK_EQ(stats.append_consume_programs, size_t{30});
    std::set<uint32_t> blobs;
    for (const ps::GraphicsPipeline& pipeline : cache.graphics) {
        for (const ps::Slot& slot : pipeline.slots) {
            if (slot.bindings != ps::kNone) {
                blobs.insert(slot.bindings);
            }
        }
    }
    for (const ps::ComputePipeline& pipeline : cache.compute) {
        blobs.insert(pipeline.bindings);
    }
    size_t framed = 0;
    for (const uint32_t blob : blobs) {
        framed += ps::read_bindings(ps::metadata_blob(cache, blob)).has_value() ? 1 : 0;
    }
    CHECK_EQ(blobs.size(), size_t{20318});
    CHECK_EQ(framed, blobs.size());

    // All but 13 cache shaders carry the key of a shader record's native program.
    std::set<uint32_t> keys;
    std::map<uint32_t, uint8_t> fetching; // keys of programs that call the fetch, and their stage
    sh::take_census(archive.bytes(), [&](const sh::CensusShader& shader) {
        for (const std::vector<uint8_t>& fssm : shader.native_programs) {
            keys.insert(sh::fssm_key(fssm));
            const std::vector<sh::FssmStage> stages = sh::fssm_stages(fssm);
            if (sh::fssm_setup(fssm).fetch && !stages.empty()) {
                fetching.emplace(sh::fssm_key(fssm), stages.front().hardware);
            }
        }
    });
    const auto tied = [&](uint32_t shader) {
        const std::optional<uint32_t> key = ps::shader_key(ps::shader_code(cache, shader));
        return key && keys.contains(*key);
    };
    size_t linked = 0;
    for (uint32_t i = 0; i < cache.shaders.size(); ++i) {
        linked += tied(i) ? 1 : 0;
    }
    fmt::print("pipeline cache: {} graphics and {} compute pipelines, {} shaders, {} tied to a "
               "shader record\n",
               cache.graphics.size(), cache.compute.size(), cache.shaders.size(), linked);
    CHECK_EQ(linked, size_t{21509});

    // The two shaders that jump through registers are driver code outside the records: a vertex
    // fetch subroutine, which returns through s[4:5], in slot 6 of the two pipelines that fill
    // that slot, and the vertex shader in their slot 1, which calls it.
    CHECK((indirect == std::vector<uint32_t>{21468, 21469}));
    CHECK(!tied(21468));
    CHECK(!tied(21469));
    size_t slot6 = 0;
    for (const ps::GraphicsPipeline& pipeline : cache.graphics) {
        if (pipeline.slots[6].shader != ps::kNone) {
            ++slot6;
            CHECK_EQ(pipeline.slots[6].shader, uint32_t{21469});
            CHECK_EQ(pipeline.slots[1].shader, uint32_t{21468});
        }
    }
    CHECK_EQ(slot6, size_t{2});

    // Everywhere else the driver compiles the fetch into the vertex shader: the cache's copies of
    // the programs that call it make no call, and read no VGPR at wave start but the vertex ID
    // in v0 and, in 19, the instance ID in v3.
    CHECK_EQ(fetching.size(), size_t{787});
    std::map<uint8_t, size_t> copies;
    size_t calling = 0;
    std::map<std::vector<size_t>, size_t> starts; // the VGPRs read at wave start
    for (uint32_t i = 0; i < cache.shaders.size(); ++i) {
        const std::optional<uint32_t> key = ps::shader_key(ps::shader_code(cache, i));
        const auto it = key ? fetching.find(*key) : fetching.end();
        if (it == fetching.end()) {
            continue;
        }
        ++copies[it->second];
        const sh::rdna2::Program copy = sh::rdna2::decode_program(ps::shader_code(cache, i));
        calling += copy.indirect ? 1 : 0;
        const sh::rdna2::LiveIns live = sh::rdna2::live_ins(copy, 64);
        std::vector<size_t> read;
        for (size_t v = 0; v < live.vector.size(); ++v) {
            if (live.vector[v]) {
                read.push_back(v);
            }
        }
        ++starts[read];
    }
    CHECK((copies == std::map<uint8_t, size_t>{{sh::kHardwareVs, 415}}));
    CHECK_EQ(calling, size_t{0});
    CHECK((starts == std::map<std::vector<size_t>, size_t>{{{0}, 396}, {{0, 3}, 19}}));
}
