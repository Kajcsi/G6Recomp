// A census of shaders.rpf (see census.h).

#include "census.h"

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace g6::shader {
namespace {

// Identifies content by its size and 64-bit FNV-1a hash. Across the archive's ~27,000 containers
// and programs, a false match is far less likely than a bad disk read.
using ContentKey = std::pair<size_t, uint64_t>;

ContentKey content_key(std::span<const uint8_t> bytes) {
    uint64_t hash = 0xCBF29CE484222325;
    for (const uint8_t byte : bytes) {
        hash = (hash ^ byte) * 0x100000001B3;
    }
    return {bytes.size(), hash};
}

void count_stage_tables(Census& census, std::span<const uint8_t> fssm) {
    constexpr uint8_t kF16High = 0x10; // flags bit 4
    if (const auto inputs = fssm_ps_input_table(fssm)) {
        ++census.ps_input_tables;
        census.ps_input_empty_tables += inputs->empty();
        for (size_t i = 0; i < inputs->size(); ++i) {
            const PsInputRecord& record = (*inputs)[i];
            ++census.ps_input_records;
            ++census.ps_input_kinds[record.kind];
            ++census.ps_input_flags[record.flags];
            census.ps_input_index_mismatches += record.attribute != i || record.byte4 != 0;
            census.ps_input_byte5 += record.byte5 != 0;
            census.ps_input_byte5_unpacked += record.byte5 != 0 && (record.flags & kF16High) == 0;
            census.ps_input_byte6_differs += record.byte6 != record.reg;
        }
    }
    if (const auto outputs = fssm_vs_output_table(fssm)) {
        ++census.vs_output_tables;
        for (size_t i = 0; i < outputs->size(); ++i) {
            const VsOutputRecord& record = (*outputs)[i];
            ++census.vs_output_records;
            ++census.vs_output_kinds[record.kind];
            census.vs_output_index_mismatches += record.param != i;
            census.vs_output_extras +=
                std::ranges::any_of(record.extra, [](uint8_t b) { return b != 0; });
            census.vs_output_reg_differs += record.reg != i;
        }
    }
}

// What the census has seen so far, by program kind.
struct Seen {
    std::map<uint16_t, std::set<ContentKey>> containers;
    std::map<uint16_t, std::set<ContentKey>> native_programs;
};

void count_container(Census& census, Seen& seen, std::span<const uint8_t> container,
                     const Dxbc& dxbc, CensusShader& shader) {
    for (const DxbcPart& part : dxbc.parts) {
        if (dxbc.find(part.fourcc) == &part) {
            ++census.parts[part.fourcc];
        }
    }
    const DxbcPart* const dxil = dxbc.find(fourcc("DXIL"));
    if (dxil == nullptr) {
        ++census.no_dxil;
        return;
    }
    const DxilProgram& program = shader.program.emplace(parse_dxil_program(dxil->data));
    ++census.kinds[program.kind];
    seen.containers[program.kind].insert(content_key(container));
    ++census.shader_models[fmt::format("{}.{}", program.major, program.minor)];
    if (program.no_bitcode_marker()) {
        ++census.no_bitcode_marker;
    } else if (program.has_llvm_bitcode()) {
        ++census.llvm_bitcode;
    } else {
        ++census.other_bitcode;
    }

    const std::optional<Xdat> xdat = read_xdat(dxil->data);
    if (!xdat) {
        ++census.no_xdat;
        return;
    }
    ++census.xdat_unknown[xdat->unknown];
    for (const XdatEntry& entry : xdat->entries) {
        ++census.xdat_tags[entry.tag];
        if (entry.tag == kXdatNativeProgram) {
            shader.native_programs.push_back(unpack_native_program(entry.data));
        }
    }
    if (shader.native_programs.empty()) {
        ++census.no_native_program;
        return;
    }
    ++census.programs_per_shader[shader.native_programs.size()];
    if (shader.native_programs.size() > 1) {
        ++census.multi_program_shaders[program.kind];
    }
    std::string stage_list;
    for (const std::vector<uint8_t>& fssm : shader.native_programs) {
        ++census.native_programs;
        ++census.fssm_versions[fssm_version(fssm)];
        ++census.fssm_layouts[fssm_layout(fssm)];
        count_stage_tables(census, fssm);
        census.fssm_sizes.push_back(static_cast<uint32_t>(fssm.size()));
        seen.native_programs[program.kind].insert(content_key(fssm));
        std::string stages;
        for (const FssmStage& stage : fssm_stages(fssm)) {
            stages += (stages.empty() ? "" : "+") + hardware_stage_name(stage.hardware);
            census.api_stages[stage.hardware].insert(stage.api);
        }
        stage_list += (stage_list.empty() ? "" : " | ") + (stages.empty() ? "none" : stages);
        if (const std::optional<uint32_t> enable = fssm_ps_input_enable(fssm)) {
            ++census.ps_input_enables[*enable];
        }
    }
    ++census.stage_lists[{program.kind, stage_list}];
}

} // namespace

Census take_census(std::span<const uint8_t> archive,
                   const std::function<void(const CensusShader&)>& each) {
    Census census;
    Seen seen;
    census.scan = scan_shader_archive(archive, [&](const ShaderRecord& record) {
        census.names.insert(record.name);
        ++census.record_tags[record.tag];
        CensusShader shader{.record = record, .program = std::nullopt, .native_programs = {}};
        try {
            count_container(census, seen, record.container, parse_dxbc(record.container), shader);
        } catch (const FormatError& error) {
            if (census.malformed++ == 0) {
                census.first_error =
                    fmt::format("{} at {:#x}: {}", record.name, record.offset, error.what());
            }
        }
        if (each) {
            each(shader);
        }
    });
    for (const auto& [kind, keys] : seen.containers) {
        census.distinct_containers[kind] = keys.size();
    }
    for (const auto& [kind, keys] : seen.native_programs) {
        census.distinct_native_programs[kind] = keys.size();
    }
    return census;
}

InterpolationCheck check_interpolation(const rdna2::Program& program, const PsInputs& inputs) {
    // VINTRP v_interp_p1_f32 and v_interp_p2_f32 name the I or J register in their only source.
    constexpr std::array<uint16_t, 2> kVinterp = {0, 1};
    // In VOP3, v_interp_p1_f32, v_interp_p2_f32, v_interp_p1ll_f16, v_interp_p1lv_f16 and
    // v_interp_p2_f16 keep the attribute where src0 would be and name the I or J register in the
    // src1 field. v_interp_mov_f32 (0x202) reads no I or J.
    constexpr std::array<uint16_t, 5> kVop3 = {0x200, 0x201, 0x342, 0x343, 0x35A};
    InterpolationCheck check;
    for (const rdna2::Instruction& insn : program.instructions) {
        const rdna2::Operand* ij = nullptr;
        if (insn.encoding == rdna2::Encoding::kVinterp &&
            std::ranges::find(kVinterp, insn.opcode) != kVinterp.end() && insn.src_count >= 1) {
            ij = &insn.src[0];
        } else if (insn.encoding == rdna2::Encoding::kVop3 &&
                   std::ranges::find(kVop3, insn.opcode) != kVop3.end() && insn.src_count >= 2) {
            ij = &insn.src[1];
            ++check.vop3;
        } else {
            continue;
        }
        ++check.instructions;
        const bool inside = ij->kind == rdna2::OperandKind::kVgpr && ij->value < 32 &&
                            ((inputs.barycentrics >> ij->value) & 1) != 0;
        if (!inside) {
            ++check.outside;
        }
    }
    return check;
}

namespace {

// One input register's elements in the signature, merged.
struct InputRegister {
    uint8_t mask = 0;          // declared components
    uint8_t rw_mask = 0;       // components the shader reads
    uint8_t read_elements = 0; // declared components of the elements the shader reads
    bool sixteen_bit = false;
    bool system_value = false;
};

// The component types D3D's signatures give 16-bit values: uint16, sint16 and float16.
bool sixteen_bit(uint32_t component_type) {
    return component_type >= 4 && component_type <= 6;
}

unsigned width(uint8_t mask) {
    return static_cast<unsigned>(std::bit_width(mask));
}

} // namespace

void check_f16_inputs(F16InputCheck& check, const rdna2::Program& program,
                      std::span<const PsInputRecord> records,
                      std::span<const SignatureElement> signature) {
    constexpr uint8_t kF16 = 0x08;    // flags bit 3
    constexpr uint8_t kPacked = 0x10; // flags bit 4
    std::map<uint32_t, InputRegister> registers;
    for (const SignatureElement& element : signature) {
        InputRegister& reg = registers[element.reg];
        reg.mask |= element.mask;
        reg.rw_mask |= element.rw_mask;
        reg.read_elements |= element.rw_mask != 0 ? element.mask : 0;
        reg.sixteen_bit = reg.sixteen_bit || sixteen_bit(element.component_type);
        reg.system_value = reg.system_value || element.system_value != 0;
    }
    std::set<uint32_t> named;
    for (const PsInputRecord& record : records) {
        named.insert(record.reg);
        if ((record.flags & kPacked) != 0) {
            named.insert(record.byte5);
            ++check.packed_records;
            check.named_partners += record.byte5 != record.reg ? 1 : 0;
        }
    }
    std::optional<uint32_t> unnamed;
    size_t unnamed_count = 0;
    for (const auto& [number, reg] : registers) {
        if (reg.sixteen_bit && !reg.system_value && reg.rw_mask != 0 && !named.contains(number)) {
            unnamed = number;
            ++unnamed_count;
        }
    }
    if (unnamed_count != 1) {
        unnamed.reset();
    }
    check.unnamed_partners += unnamed ? 1 : 0;

    std::set<std::array<uint8_t, 3>> reads;
    for (const rdna2::Instruction& insn : program.instructions) {
        if (insn.encoding != rdna2::Encoding::kVop3 ||
            (insn.opcode != rdna2::kVop3InterpP1llF16 && insn.opcode != rdna2::kVop3InterpP1lvF16 &&
             insn.opcode != rdna2::kVop3InterpP2F16)) {
            continue;
        }
        if (const auto attr = rdna2::interp_attribute(insn)) {
            reads.insert({attr->attribute, attr->channel, static_cast<uint8_t>(attr->high)});
        }
    }
    const auto component_of = [&](const PsInputRecord& record, unsigned channel,
                                  bool high) -> std::optional<std::pair<uint32_t, unsigned>> {
        if ((record.flags & kPacked) == 0) {
            return high ? std::nullopt : std::optional(std::pair{uint32_t{record.reg}, channel});
        }
        const unsigned slot = (2 * channel) + (high ? 1 : 0);
        std::optional<uint32_t> partner;
        unsigned first = width(registers[record.reg].mask);
        if (record.byte5 != record.reg) {
            partner = record.byte5;
        } else if (unnamed) {
            partner = unnamed;
            first = width(registers[record.reg].read_elements);
        }
        const unsigned start = 2 * ((first + 1) / 2);
        if (slot < first) {
            return std::pair{uint32_t{record.reg}, slot};
        }
        if (partner && slot >= start) {
            return std::pair{*partner, slot - start};
        }
        return std::nullopt;
    };
    for (const auto& [attribute, channel, high] : reads) {
        ++check.reads;
        if (attribute >= records.size() || (records[attribute].flags & kF16) == 0) {
            ++check.not_f16;
            continue;
        }
        const auto at = component_of(records[attribute], channel, high != 0);
        const InputRegister reg = at ? registers[at->first] : InputRegister{};
        const unsigned component = at ? at->second : 0;
        if (at && component < 4 && ((reg.rw_mask >> component) & 1) != 0) {
            ++check.on_read;
        } else if (at && component < 4 && ((reg.mask >> component) & 1) != 0) {
            ++check.on_unread;
        } else {
            ++check.outside;
        }
    }
}

ComputeSetup compute_setup(uint32_t rsrc2) {
    const auto bit = [rsrc2](unsigned n) { return (rsrc2 >> n) & 1; };
    ComputeSetup setup;
    setup.user_sgprs = (rsrc2 >> 1) & 0x1F;
    // TGID_X_EN, TGID_Y_EN, TGID_Z_EN, TG_SIZE_EN, SCRATCH_EN
    setup.sgprs = setup.user_sgprs + bit(7) + bit(8) + bit(9) + bit(10) + bit(0);
    setup.vgprs = ((rsrc2 >> 11) & 3) + 1; // TIDIG_COMP_CNT
    return setup;
}

namespace {

using VectorSet = std::bitset<rdna2::kVectorRegisters>;

void set_vgpr(VectorSet& set, uint64_t vgpr) {
    if (vgpr < set.size()) {
        set.set(static_cast<size_t>(vgpr));
    }
}

void set_components(VectorSet& set, uint64_t first, uint8_t mask) {
    for (uint64_t c = 0; c < 4; ++c) {
        if ((mask >> c) & 1U) {
            set_vgpr(set, first + c);
        }
    }
}

// Whether the VGPRs a fetching program reads after the call are those `layout` predicts for the
// elements it reads, besides the vertex and instance IDs its signature declares.
bool fetch_fits(const VectorSet& after_call, const VectorSet& layout, const VertexVgprs& ids,
                std::span<const SignatureElement> signature) {
    VectorSet id_vgprs;
    set_vgpr(id_vgprs, ids.vertex_id);
    set_vgpr(id_vgprs, ids.instance_id);
    VectorSet declared;
    for (const SignatureElement& element : signature) {
        if (element.rw_mask == 0) {
            continue;
        }
        if (element.system_value == kSystemValueVertexId) {
            set_vgpr(declared, ids.vertex_id);
        } else if (element.system_value == kSystemValueInstanceId) {
            set_vgpr(declared, ids.instance_id);
        }
    }
    return (after_call & ~id_vgprs) == layout && (after_call & id_vgprs & ~declared).none();
}

void check_fetch(StageInputs& counts, const rdna2::LiveIns& live, const VertexVgprs& ids,
                 std::span<const SignatureElement> signature) {
    VectorSet layout;
    for (const FetchedElement& fetched : fetch_layout(signature, ids.first_fetched)) {
        set_components(layout, fetched.first_vgpr, signature[fetched.element].rw_mask);
    }
    VectorSet by_register;
    bool unread = false;
    for (const SignatureElement& element : signature) {
        if (element.system_value == 0) {
            set_components(by_register, ids.first_fetched + (4 * uint64_t{element.reg}),
                           element.rw_mask);
            unread = unread || element.rw_mask == 0;
        }
    }
    ++counts.fetch_layout_checked;
    counts.fetch_with_unread += unread ? 1 : 0;
    counts.fetch_layout_differs +=
        fetch_fits(live.vector_after_call, layout, ids, signature) ? 0 : 1;
    counts.fetch_by_register_differs +=
        fetch_fits(live.vector_after_call, by_register, ids, signature) ? 0 : 1;
}

} // namespace

ProgramInputs count_inputs(std::map<uint8_t, StageInputs>& stages, std::span<const uint8_t> fssm,
                           const rdna2::Program& program,
                           std::span<const SignatureElement> signature) {
    ProgramInputs result;
    const std::vector<FssmStage> records = fssm_stages(fssm);
    result.stage = records.empty() ? 0 : records.front().hardware;
    const FssmSetup setup = fssm_setup(fssm);
    result.wave = setup.wave32 ? 32 : 64;
    result.decoded_wave = rdna2::wave_size(program);
    const unsigned wave = result.wave;
    result.live = rdna2::live_ins(program, wave);
    const rdna2::LiveIns& live = result.live;

    StageInputs& counts = stages[result.stage];
    ++counts.programs;
    ++counts.wave_sizes[wave];
    if (result.decoded_wave) {
        ++counts.wave_decoded;
        counts.wave_differs += *result.decoded_wave != wave ? 1 : 0;
    }
    for (size_t i = 0; i < rdna2::kScalarRegisters; ++i) {
        counts.scalar.at(i) += live.scalar[i] ? 1 : 0;
        counts.scalar_after_call.at(i) += live.scalar_after_call[i] ? 1 : 0;
    }
    for (size_t i = 0; i < rdna2::kVectorRegisters; ++i) {
        counts.vector.at(i) += live.vector[i] ? 1 : 0;
        counts.vector_after_call.at(i) += live.vector_after_call[i] ? 1 : 0;
    }
    counts.calls += live.call ? 1 : 0;
    counts.indirect += program.indirect ? 1 : 0;
    counts.dynamic += live.dynamic ? 1 : 0;
    counts.with_unmodelled += live.unmodelled > 0 ? 1 : 0;
    if (live.unmodelled > 0) {
        for (const rdna2::Instruction& insn : program.instructions) {
            if (!rdna2::footprint(insn, wave).modelled) {
                ++counts.unmodelled[rdna2::mnemonic(insn)];
            }
        }
    }

    // The fetch bit says whether the program calls the fetch subroutine, through the pair the
    // word names; the driver loads its address there.
    counts.fetch += setup.fetch ? 1 : 0;
    const size_t pair = setup.fetch_sgpr;
    const bool pair_read =
        pair + 1 < rdna2::kScalarVcc && live.scalar[pair] && live.scalar[pair + 1];
    counts.fetch_differs += setup.fetch != live.call || (setup.fetch && !pair_read) ? 1 : 0;
    const std::optional<VertexVgprs> ids = vertex_vgprs(result.stage);
    if (setup.fetch && ids && !signature.empty()) {
        check_fetch(counts, live, *ids, signature);
    }

    const std::optional<uint32_t> rsrc2 = fssm_rsrc2(fssm);
    if (rsrc2) {
        ++counts.rsrc2;
        counts.rsrc2_user_differs += ((*rsrc2 >> 1) & 0x1F) != setup.user_sgprs ? 1 : 0;
    }
    if (rsrc2 && (result.stage == kHardwareEs || result.stage == kHardwareGs)) {
        const GsRsrc2 merged = gs_rsrc2(*rsrc2);
        ++counts.lds_sizes[merged.lds_size];
        if (result.stage == kHardwareEs) {
            const std::optional<uint32_t> components = fssm_vgpr_components(fssm);
            counts.es_components_differ += components != merged.es_vgpr_components ? 1 : 0;
        }
    }
    bool reads_lds = false;
    bool writes_lds = false;
    bool exports = false;
    for (const rdna2::Instruction& insn : program.instructions) {
        if (insn.encoding == rdna2::Encoding::kExport) {
            exports = true;
        } else if (insn.encoding == rdna2::Encoding::kDs && !rdna2::uses_gds(insn)) {
            const std::string_view name =
                rdna2::opcode_name(insn.encoding, insn.opcode).value_or("");
            reads_lds = reads_lds || name.starts_with("ds_read");
            writes_lds = writes_lds || name.starts_with("ds_write");
        }
    }
    counts.lds_readers += reads_lds ? 1 : 0;
    counts.lds_writers += writes_lds ? 1 : 0;
    counts.exporters += exports ? 1 : 0;

    // The registers the setup words load, and whether the program reads others first.
    constexpr unsigned kMergedSystemSgprs = 8; // ES, GS and mesh start their user SGPRs at s8
    std::optional<unsigned> sgprs;
    std::optional<unsigned> vgprs;
    switch (result.stage) {
    case kHardwareCs:
        if (rsrc2) {
            const ComputeSetup compute = compute_setup(*rsrc2);
            sgprs = compute.sgprs;
            vgprs = compute.vgprs;
        }
        break;
    case kHardwarePs: {
        // After the user SGPRs, the primitive mask that interpolation puts in m0, then the
        // collision wave ID when RSRC2 asks for it.
        const bool collision = rsrc2 && (*rsrc2 & kRsrc2PsLoadCollisionWaveId) != 0;
        sgprs = setup.user_sgprs + 1U + (collision ? 1U : 0U);
        if (const std::optional<uint32_t> enable = fssm_ps_input_enable(fssm)) {
            vgprs = ps_inputs(*enable).vgprs;
        }
        break;
    }
    case kHardwareVs: sgprs = setup.user_sgprs; break;
    case kHardwareEs:
    case kHardwareGs:
    case kHardwareMesh: sgprs = kMergedSystemSgprs + setup.user_sgprs; break;
    default: break;
    }
    // The VGPR component count: that many VGPRs plus one, from v0 in a VS and after the geometry
    // half's VGPRs in the merged stages.
    if (const std::optional<uint32_t> components = fssm_vgpr_components(fssm)) {
        ++counts.vgpr_components[*components];
        const unsigned first = result.stage == kHardwareVs ? 0 : kMergedGsVgprs;
        vgprs = static_cast<unsigned>(
            std::min<uint64_t>(uint64_t{first} + *components + 1, rdna2::kVectorRegisters));
    }
    // The recorded register counts against the model (StageInputs::vgpr_counts).
    if (const std::optional<FssmRegisterCounts> recorded = fssm_register_counts(fssm)) {
        constexpr unsigned kVccSgprs = 2;
        constexpr unsigned kVsVgprs = 4; // VGPR_COMP_CNT 3: vertex ID, two user VGPRs, instance ID
        constexpr unsigned kCsVgprs = 3; // the thread IDs
        const rdna2::RegisterExtent extent = rdna2::register_extent(program, wave);
        unsigned model_vgprs = std::max(extent.vgprs, vgprs.value_or(0));
        unsigned model_sgprs = extent.sgprs + kVccSgprs;
        if (result.stage != kHardwareGs) {
            model_sgprs = std::max(model_sgprs, sgprs.value_or(0));
        }
        if (result.stage == kHardwareVs) {
            model_vgprs = std::max(model_vgprs, kVsVgprs);
        } else if (result.stage == kHardwareCs) {
            model_vgprs = std::max(model_vgprs, kCsVgprs);
        }
        const auto tally = [](CountMatch& match, uint32_t count, unsigned model, unsigned code) {
            ++(count == model ? match.equal : count > model ? match.above : match.below);
            match.beyond_code += count == model && model > code ? 1 : 0;
        };
        tally(counts.vgpr_counts, recorded->vgprs, model_vgprs, extent.vgprs);
        tally(counts.sgpr_counts, recorded->sgprs, model_sgprs, extent.sgprs + kVccSgprs);
        counts.sgpr_count_below_loaded += sgprs && recorded->sgprs < *sgprs ? 1 : 0;
    }

    if (sgprs && live.dynamic) {
        ++counts.setup_unchecked;
    } else if (sgprs) {
        bool outside = false;
        // vcc, m0 and exec aren't loaded from the setup words.
        for (size_t i = *sgprs; i < rdna2::kScalarVcc; ++i) {
            outside = outside || live.scalar[i];
        }
        for (size_t i = vgprs.value_or(rdna2::kVectorRegisters); i < rdna2::kVectorRegisters; ++i) {
            outside = outside || live.vector[i];
        }
        ++counts.setup_checked;
        counts.setup_vgprs_checked += vgprs ? 1 : 0;
        counts.setup_outside += outside ? 1 : 0;
        result.setup_outside = outside;
    }
    return result;
}

void check_gs_output(GsOutputCheck& check, std::span<const uint8_t> fssm,
                     std::span<const uint8_t> container) {
    const std::optional<std::array<uint8_t, 4>> types = fssm_gs_out_prim_types(fssm);
    if (!types) {
        return;
    }
    ++check.types[*types];
    const std::optional<PsvGeometry> geometry = psv_geometry(container);
    if (!geometry) {
        return;
    }
    ++check.checked;
    ++check.streams[geometry->output_streams];
    check.differs += gs_out_prim_type(geometry->output_topology) != types->front() ? 1 : 0;
}

} // namespace g6::shader
