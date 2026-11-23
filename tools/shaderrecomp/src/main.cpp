// shaderrecomp command-line front end. For now it reads the console's shader archive: it finds
// every shader record, parses each container, unpacks the native GPU program it carries, and
// decodes the program's RDNA 2 code (docs/shaders.md). Translation to desktop DXIL comes later.
// Everything it prints or writes is derived from the user's files, so it stays on their machine.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "archive.h"
#include "census.h"
#include "console.h"
#include "g6/log.h"
#include "g6/rpf9.h"
#include "pscache.h"
#include "rdna2.h"

namespace {

using namespace g6;

int usage() {
    fmt::print(
        stderr,
        "usage: shaderrecomp <command> <file> [options]\n"
        "\n"
        "commands:\n"
        "  scan        find the shader records in shaders.rpf, parse their containers, and\n"
        "              unpack their native programs; prints a census\n"
        "              --out <dir>     also write each container (<n>.dxbc), its native\n"
        "                              programs (<n>.fssm, <n>_1.fssm), and index.tsv;\n"
        "                              derived from the game, so keep it out of the repo\n"
        "  disasm      decode the RDNA 2 code of one FSSM image (a file scan --out wrote)\n"
        "              and print it, one instruction per line\n"
        "  pscache     find the driver's pipeline cache in shaders.rpf, parse it, decode its\n"
        "              shaders, and match them to the shader records; prints a census\n"
        "              --shader <n>    print shader n's code instead\n");
    return 2;
}

template <typename Key>
std::string join_counts(const std::map<Key, size_t>& counts, auto&& describe) {
    std::string text;
    for (const auto& [key, count] : counts) {
        text += fmt::format("{}{} {}", text.empty() ? "" : ", ", describe(key), count);
    }
    return text.empty() ? "none" : text;
}

void write_file(const std::filesystem::path& path, std::span<const uint8_t> data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out) {
        throw std::runtime_error(fmt::format("can't write {}", path.string()));
    }
}

void print_census(const shader::Census& c) {
    fmt::print("records          {} ({} headers rejected)\n", c.scan.records, c.scan.rejected);
    fmt::print("distinct names   {}\n", c.names.size());
    fmt::print("record tags      {}\n",
               join_counts(c.record_tags, [](uint8_t tag) { return fmt::format("{:#04x}", tag); }));
    fmt::print("program kinds    {}\n", join_counts(c.kinds, shader::kind_name));
    fmt::print("shader models    {}\n",
               join_counts(c.shader_models, [](const std::string& model) { return model; }));
    fmt::print("bitcode          {} NOBC marker, {} LLVM bitcode, {} other, {} without DXIL\n",
               c.no_bitcode_marker, c.llvm_bitcode, c.other_bitcode, c.no_dxil);
    fmt::print("parts            {}\n", join_counts(c.parts, shader::fourcc_text));
    const auto hex = [](uint32_t value) { return fmt::format("{:#04x}", value); };
    fmt::print("XDAT tags        {} ({} DXIL parts without the table)\n",
               join_counts(c.xdat_tags, hex), c.no_xdat);
    fmt::print("XDAT word 2      {}\n", join_counts(c.xdat_unknown, hex));
    fmt::print("native programs  {} unpacked, {} tables without one; per shader {}; FSSM versions "
               "{}\n",
               c.native_programs, c.no_native_program,
               join_counts(c.programs_per_shader, [](size_t n) { return std::to_string(n); }),
               join_counts(c.fssm_versions, hex));
    fmt::print("                 shaders with more than one: {}\n",
               join_counts(c.multi_program_shaders, shader::kind_name));
    if (!c.fssm_sizes.empty()) {
        std::vector<uint32_t> sizes = c.fssm_sizes;
        std::ranges::sort(sizes);
        uint64_t total = 0;
        for (const uint32_t size : sizes) {
            total += size;
        }
        fmt::print("FSSM sizes       min {}, median {}, max {} bytes; {:.1f} MiB in all\n",
                   sizes.front(), sizes[sizes.size() / 2], sizes.back(),
                   static_cast<double>(total) / (1024.0 * 1024.0));
    }
    fmt::print("distinct         containers {}\n",
               join_counts(c.distinct_containers, shader::kind_name));
    fmt::print("                 native programs {}\n",
               join_counts(c.distinct_native_programs, shader::kind_name));
    fmt::print("stages           {}\n",
               join_counts(c.stage_lists, [](const std::pair<uint16_t, std::string>& key) {
                   return fmt::format("{} {}", shader::kind_name(key.first), key.second);
               }));
    std::string api;
    for (const auto& [hardware, values] : c.api_stages) {
        api += fmt::format("{}{} {}", api.empty() ? "" : ", ",
                           shader::hardware_stage_name(hardware), fmt::join(values, "/"));
    }
    fmt::print("                 API stage by hardware stage: {}\n", api.empty() ? "none" : api);
    fmt::print("PS input enable  {}\n", join_counts(c.ps_input_enables, [](uint32_t enable) {
                   return fmt::format("{:#x}", enable);
               }));
    const auto number = [](auto value) { return fmt::format("{}", value); };
    fmt::print("FSSM layouts     {}\n", join_counts(c.fssm_layouts, number));
    fmt::print("PS input tables  {} tables ({} empty), {} records; kinds {}; flags {}\n",
               c.ps_input_tables, c.ps_input_empty_tables, c.ps_input_records,
               join_counts(c.ps_input_kinds, number), join_counts(c.ps_input_flags, number));
    fmt::print(
        "                 {} not repeating their index; byte 5 set in {} ({} without bit 4); "
        "byte 6 isn't byte 1 in {}\n",
        c.ps_input_index_mismatches, c.ps_input_byte5, c.ps_input_byte5_unpacked,
        c.ps_input_byte6_differs);
    fmt::print("VS output tables {} tables, {} records; kinds {}; {} not repeating their index, "
               "{} with bytes 3 to 5 set, {} whose register isn't their index\n",
               c.vs_output_tables, c.vs_output_records, join_counts(c.vs_output_kinds, number),
               c.vs_output_index_mismatches, c.vs_output_extras, c.vs_output_reg_differs);
    fmt::print("malformed        {}{}\n", c.malformed,
               c.first_error.empty() ? "" : fmt::format(" (first: {})", c.first_error));
}

void print_decode(const shader::rdna2::DecodeStats& d, size_t undecodable) {
    namespace rdna2 = shader::rdna2;
    fmt::print("decoded          {} programs, {} reachable instructions; {} of unknown encoding; "
               "{} without readable code\n",
               d.programs, d.instructions, d.unknown, undecodable);
    fmt::print("control flow     {} truncated, {} running off the end, {} with bad branches, {} "
               "with indirect jumps or calls; {} with data after the code ({} bytes)\n",
               d.truncated, d.falls_off, d.bad_branch, d.indirect, d.with_data, d.data_bytes);
    fmt::print("far jumps        {} in {} programs\n", d.far_jumps, d.far_jump_programs);
    fmt::print("GDS and GWS      {} instructions in {} programs; ds_append and ds_consume {} in "
               "{} programs\n",
               d.gds, d.gds_programs, d.append_consume, d.append_consume_programs);
    fmt::print("encodings        {}\n", join_counts(d.encodings, rdna2::encoding_name));
    size_t unnamed = 0;
    std::vector<std::pair<size_t, std::pair<rdna2::Encoding, uint16_t>>> by_count;
    for (const auto& [opcode, count] : d.unnamed) {
        unnamed += count;
        by_count.emplace_back(count, opcode);
    }
    std::ranges::sort(by_count, std::greater{});
    std::string top;
    constexpr size_t kShown = 24;
    for (size_t i = 0; i < std::min(by_count.size(), kShown); ++i) {
        const auto& [count, opcode] = by_count[i];
        top += fmt::format("{}{}<{:#x}> {}", top.empty() ? "" : ", ",
                           rdna2::encoding_name(opcode.first), opcode.second, count);
    }
    fmt::print("unnamed opcodes  {} in {} instructions{}{}\n", d.unnamed.size(), unnamed,
               top.empty() ? "" : ": ", top);
}

// Logs the first few programs that fail a decode check, so they can be looked at with disasm.
void report_decode_problems(size_t shader_number, const shader::rdna2::Program& program) {
    namespace rdna2 = shader::rdna2;
    constexpr size_t kReported = 8;
    static size_t reported = 0;
    const bool unknown =
        std::ranges::any_of(program.instructions, [](const rdna2::Instruction& insn) {
            return insn.encoding == rdna2::Encoding::kUnknown || insn.truncated;
        });
    if ((!unknown && !program.falls_off && !program.bad_branch) || reported >= kReported) {
        return;
    }
    ++reported;
    LOG_WARN(shaderrecomp, "shader {}: {}{}{}", shader_number,
             unknown ? "unknown or cut-off instruction; " : "",
             program.falls_off ? "a path runs off the end; " : "",
             program.bad_branch ? "a bad branch" : "");
}

// A program's inputs for index.tsv: "stage:wave:sgprs:vgprs:after", the wave size from the setup
// word, sgprs the SGPRs below vcc it reads first as hex (bit n for sn), vgprs one past the
// highest VGPR it reads first, and after one past the highest VGPR it reads first after a call.
std::string input_summary(const shader::ProgramInputs& inputs) {
    namespace rdna2 = shader::rdna2;
    std::string sgprs;
    for (size_t nibble = (rdna2::kScalarVcc + 3) / 4; nibble-- > 0;) {
        unsigned value = 0;
        for (size_t bit = 0; bit < 4; ++bit) {
            const size_t i = (4 * nibble) + bit;
            value |= i < rdna2::kScalarVcc && inputs.live.scalar[i] ? 1U << bit : 0;
        }
        if (value != 0 || !sgprs.empty() || nibble == 0) {
            sgprs += fmt::format("{:x}", value);
        }
    }
    const auto end = [](const auto& set) {
        size_t last = 0;
        for (size_t i = 0; i < set.size(); ++i) {
            last = set[i] ? i + 1 : last;
        }
        return last;
    };
    return fmt::format("{}:{}:{}:{}:{}", inputs.stage, inputs.wave, sgprs, end(inputs.live.vector),
                       end(inputs.live.vector_after_call));
}

std::string register_name(bool scalar, size_t i) {
    namespace rdna2 = shader::rdna2;
    if (scalar && i == rdna2::kScalarVcc) {
        return "vcc_lo";
    }
    if (scalar && i == rdna2::kScalarVcc + 1) {
        return "vcc_hi";
    }
    if (scalar && i == rdna2::kScalarM0) {
        return "m0";
    }
    return fmt::format("{}{}", scalar ? 's' : 'v', i);
}

// "s0 2689, s1 2689": the registers some program reads first, with how many programs do.
template <size_t N>
std::string register_counts(const std::array<size_t, N>& counts, bool scalar) {
    std::string out;
    for (size_t i = 0; i < N; ++i) {
        if (counts.at(i) != 0) {
            out += fmt::format("{}{} {}", out.empty() ? "" : ", ", register_name(scalar, i),
                               counts.at(i));
        }
    }
    return out.empty() ? "none" : out;
}

void print_inputs(const std::map<uint8_t, shader::StageInputs>& stages,
                  std::span<const size_t> outside_shaders) {
    std::map<std::string, size_t> unmodelled;
    for (const auto& [stage, s] : stages) {
        const auto wave = [](unsigned size) { return std::to_string(size); };
        fmt::print("inputs {:<9} {} programs; wave {} (the code agrees in {} of {} it tells, "
                   "differs in {}); {} with calls, {} with indirect jumps or calls, {} with "
                   "m0-indexed registers, {} with unmodelled instructions\n",
                   shader::hardware_stage_name(stage), s.programs, join_counts(s.wave_sizes, wave),
                   s.wave_decoded - s.wave_differs, s.wave_decoded, s.wave_differs, s.calls,
                   s.indirect, s.dynamic, s.with_unmodelled);
        fmt::print("  SGPRs          {}\n", register_counts(s.scalar, true));
        fmt::print("  VGPRs          {}\n", register_counts(s.vector, false));
        if (s.calls > 0) {
            fmt::print("  after calls    SGPRs {}; VGPRs {}\n",
                       register_counts(s.scalar_after_call, true),
                       register_counts(s.vector_after_call, false));
        }
        if (s.setup_checked + s.setup_unchecked > 0) {
            fmt::print("  setup words    {} programs checked ({} with VGPRs), {} reading a "
                       "register they don't load; {} indexing registers through m0 not checked\n",
                       s.setup_checked, s.setup_vgprs_checked, s.setup_outside, s.setup_unchecked);
        }
        const auto matches = [](const shader::CountMatch& m) {
            return fmt::format("{} as modelled ({} beyond the code's), {} above, {} below", m.equal,
                               m.beyond_code, m.above, m.below);
        };
        fmt::print("  register count VGPRs {}; SGPRs {}, {} below those loaded\n",
                   matches(s.vgpr_counts), matches(s.sgpr_counts), s.sgpr_count_below_loaded);
        if (s.rsrc2 > 0) {
            fmt::print("  RSRC2 word     in {} programs, user SGPRs differing from the setup word "
                       "in {}\n",
                       s.rsrc2, s.rsrc2_user_differs);
        }
        if (s.fetch + s.fetch_differs > 0) {
            fmt::print("  fetch bit      set in {}, disagreeing with the code in {}\n", s.fetch,
                       s.fetch_differs);
        }
        if (s.fetch_layout_checked > 0) {
            fmt::print("  fetch layout   {} checked against the input signature, {} differing; "
                       "a layout by input register would differ in {}; {} with an unread input\n",
                       s.fetch_layout_checked, s.fetch_layout_differs, s.fetch_by_register_differs,
                       s.fetch_with_unread);
        }
        if (!s.vgpr_components.empty()) {
            fmt::print("  VGPR count     {} (programs by the component count word)\n",
                       join_counts(s.vgpr_components,
                                   [](uint32_t count) { return std::to_string(count); }));
        }
        if (!s.lds_sizes.empty()) {
            fmt::print("  LDS size       {} (programs by the RSRC2 word's LDS_SIZE){}\n",
                       join_counts(s.lds_sizes, [](unsigned size) { return std::to_string(size); }),
                       stage == shader::kHardwareEs
                           ? fmt::format("; ES_VGPR_COMP_CNT differs from the component count "
                                         "word in {}",
                                         s.es_components_differ)
                           : "");
        }
        fmt::print("  LDS, exports   {} read LDS, {} write it, {} export\n", s.lds_readers,
                   s.lds_writers, s.exporters);
        for (const auto& [name, count] : s.unmodelled) {
            unmodelled[name] += count;
        }
    }
    std::vector<std::pair<size_t, std::string>> by_count;
    by_count.reserve(unmodelled.size());
    for (const auto& [name, count] : unmodelled) {
        by_count.emplace_back(count, name);
    }
    std::ranges::sort(by_count, std::greater{});
    std::string top;
    constexpr size_t kShown = 24;
    for (size_t i = 0; i < std::min(by_count.size(), kShown); ++i) {
        top +=
            fmt::format("{}{} {}", top.empty() ? "" : ", ", by_count[i].second, by_count[i].first);
    }
    fmt::print("unmodelled       {} opcodes{}{}\n", unmodelled.size(), top.empty() ? "" : ": ",
               top);
    if (!outside_shaders.empty()) {
        fmt::print("                 outside their setup word: shaders {}\n",
                   fmt::join(outside_shaders, ", "));
    }
}

int cmd_scan(const std::filesystem::path& file, std::span<const std::string_view> flags) {
    std::filesystem::path out_dir;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (flags[i] == "--out" && i + 1 < flags.size()) {
            out_dir = std::filesystem::path(flags[++i]);
        } else {
            LOG_ERROR(shaderrecomp, "bad option {}", flags[i]);
            return usage();
        }
    }

    std::ofstream index;
    if (!out_dir.empty()) {
        std::filesystem::create_directories(out_dir);
        index.open(out_dir / "index.tsv");
        index << "n\toffset\tname\tkind\tmodel\ttag\tid\tcontainer\tfssm\tinputs\n";
    }
    size_t n = 0;
    std::map<uint8_t, shader::StageInputs> stage_inputs;
    std::vector<size_t> setup_outside_shaders;
    shader::rdna2::DecodeStats decode_stats;
    size_t undecodable = 0;
    size_t bad_signatures = 0;
    shader::GsOutputCheck gs_output;
    shader::InterpolationCheck interpolation;
    size_t interpolating = 0;
    size_t interpolation_fits = 0;
    std::vector<size_t> outside_shaders;
    shader::F16InputCheck f16_inputs;
    const shader::MappedFile archive(file);
    const shader::Census census =
        shader::take_census(archive.bytes(), [&](const shader::CensusShader& shader) {
            const size_t number = n++;
            std::string inputs_column;
            std::vector<shader::SignatureElement> signature;
            try {
                signature = shader::input_signature(shader.record.container);
            } catch (const shader::FormatError& error) {
                // The fetch layout goes unchecked for this shader.
                if (bad_signatures++ == 0) {
                    LOG_WARN(shaderrecomp, "shader {}: input signature: {}", number, error.what());
                }
            }
            for (const std::vector<uint8_t>& fssm : shader.native_programs) {
                try {
                    const shader::rdna2::Program program =
                        shader::rdna2::decode_program(shader::fssm_code(fssm));
                    decode_stats.add(program);
                    report_decode_problems(number, program);
                    const shader::ProgramInputs inputs =
                        shader::count_inputs(stage_inputs, fssm, program, signature);
                    shader::check_gs_output(gs_output, fssm, shader.record.container);
                    if (inputs.setup_outside && setup_outside_shaders.size() < 4) {
                        setup_outside_shaders.push_back(number);
                    }
                    inputs_column += fmt::format("{}{}", inputs_column.empty() ? "" : ",",
                                                 input_summary(inputs));
                    if (const std::optional<uint32_t> enable = shader::fssm_ps_input_enable(fssm)) {
                        const shader::InterpolationCheck check =
                            shader::check_interpolation(program, shader::ps_inputs(*enable));
                        interpolation.instructions += check.instructions;
                        interpolation.outside += check.outside;
                        interpolation.vop3 += check.vop3;
                        interpolating += check.instructions > 0 ? 1 : 0;
                        interpolation_fits += check.instructions > 0 && check.outside == 0 ? 1 : 0;
                        if (check.outside > 0 && outside_shaders.size() < 4) {
                            outside_shaders.push_back(number);
                        }
                    }
                    if (const auto records = shader::fssm_ps_input_table(fssm)) {
                        shader::check_f16_inputs(f16_inputs, program, *records, signature);
                    }
                } catch (const shader::FormatError& error) {
                    if (undecodable++ == 0) {
                        LOG_WARN(shaderrecomp, "shader {}: {}", number, error.what());
                    }
                }
            }
            if (out_dir.empty()) {
                return;
            }
            const std::string stem = fmt::format("{:05}", number);
            write_file(out_dir / (stem + ".dxbc"), shader.record.container);
            std::string fssm_sizes;
            for (size_t i = 0; i < shader.native_programs.size(); ++i) {
                const std::vector<uint8_t>& fssm = shader.native_programs[i];
                write_file(out_dir / (i == 0 ? stem + ".fssm" : fmt::format("{}_{}.fssm", stem, i)),
                           fssm);
                fssm_sizes += fmt::format("{}{}", i == 0 ? "" : ",", fssm.size());
            }
            const std::string kind = shader.program ? shader::kind_name(shader.program->kind) : "";
            const std::string model =
                shader.program ? fmt::format("{}.{}", shader.program->major, shader.program->minor)
                               : "";
            index << fmt::format("{}\t{:#x}\t{}\t{}\t{}\t{:#04x}\t{:#x}\t{}\t{}\t{}\n", stem,
                                 shader.record.offset, shader.record.name, kind, model,
                                 shader.record.tag, shader.record.id,
                                 shader.record.container.size(), fssm_sizes, inputs_column);
        });
    print_census(census);
    fmt::print(
        "                 {} pixel programs interpolate; in {}, all {} interpolations ({} of "
        "them VOP3) read an I or J the enable word loads; {} read another register{}\n",
        interpolating, interpolation_fits, interpolation.instructions, interpolation.vop3,
        interpolation.outside,
        outside_shaders.empty() ? ""
                                : fmt::format(" (shaders {})", fmt::join(outside_shaders, ", ")));
    fmt::print("                 f16 inputs: {} packed records, {} with a second register in byte "
               "5, {} programs with an unnamed one; of {} f16 reads, {} land on read components, "
               "{} on unread ones, {} on none, {} on records that aren't f16\n",
               f16_inputs.packed_records, f16_inputs.named_partners, f16_inputs.unnamed_partners,
               f16_inputs.reads, f16_inputs.on_read, f16_inputs.on_unread, f16_inputs.outside,
               f16_inputs.not_f16);
    print_decode(decode_stats, undecodable);
    print_inputs(stage_inputs, setup_outside_shaders);
    fmt::print(
        "GS output        programs by their four streams' primitive types {}; stream 0's "
        "agrees with PSV0's topology in {} of {} checked; PSV0 stream masks {}\n",
        join_counts(gs_output.types,
                    [](const std::array<uint8_t, 4>& types) {
                        return fmt::format("{}", fmt::join(types, "/"));
                    }),
        gs_output.checked - gs_output.differs, gs_output.checked,
        join_counts(gs_output.streams, [](uint32_t mask) { return fmt::format("{:#x}", mask); }));
    return census.malformed == 0 ? 0 : 1;
}

// Prints the code one instruction per line: reached instructions as text, words no path reaches
// as data. `setup_wave` is the setup word's wave size, when the code comes with one.
void print_disasm(std::span<const uint8_t> code, std::optional<unsigned> setup_wave = {}) {
    const shader::rdna2::Program program = shader::rdna2::decode_program(code);
    size_t next = 0;
    size_t at = 0;
    while (at + 4 <= code.size()) {
        while (next < program.instructions.size() && program.instructions[next].offset < at) {
            ++next; // overlaps one already printed: a bad branch
        }
        if (next < program.instructions.size() && program.instructions[next].offset == at) {
            const shader::rdna2::Instruction& insn = program.instructions[next++];
            std::string words;
            for (size_t i = 0; i < insn.dwords; ++i) {
                words += fmt::format("{:08x} ", insn.words[i]);
            }
            fmt::print("{:6x}  {:<45} {}\n", at, words, shader::rdna2::text(insn));
            at += 4 * size_t{insn.dwords};
        } else {
            fmt::print("{:6x}  {:08x}{:38} .data\n", at, shader::read_u32(code, at), "");
            at += 4;
        }
    }
    fmt::print("; {}{}{}{} bytes unreached\n", program.falls_off ? "runs off the end; " : "",
               program.bad_branch ? "bad branch; " : "",
               program.indirect ? "indirect jumps or calls; " : "", program.unreached_bytes);
    namespace rdna2 = shader::rdna2;
    const std::optional<unsigned> wave = setup_wave ? setup_wave : rdna2::wave_size(program);
    const rdna2::LiveIns live = rdna2::live_ins(program, wave.value_or(64));
    const auto list = [](const auto& set, bool scalar) {
        std::string out;
        for (size_t i = 0; i < set.size(); ++i) {
            if (set[i]) {
                out += fmt::format("{}{}", out.empty() ? "" : " ", register_name(scalar, i));
            }
        }
        return out.empty() ? std::string("none") : out;
    };
    fmt::print("; wave {}; inputs {} | {}", wave ? std::to_string(*wave) : "unknown",
               list(live.scalar, true), list(live.vector, false));
    if (live.call) {
        fmt::print("; after calls {} | {}", list(live.scalar_after_call, true),
                   list(live.vector_after_call, false));
    }
    fmt::print("{}\n", live.unmodelled > 0 ? fmt::format("; {} unmodelled", live.unmodelled) : "");
}

int cmd_disasm(const std::filesystem::path& file, std::span<const std::string_view> flags) {
    if (!flags.empty()) {
        LOG_ERROR(shaderrecomp, "bad option {}", flags.front());
        return usage();
    }
    const shader::MappedFile image(file);
    const shader::FssmSetup setup = shader::fssm_setup(image.bytes());
    print_disasm(shader::fssm_code(image.bytes()), setup.wave32 ? 32 : 64);
    const std::optional<uint32_t> components = shader::fssm_vgpr_components(image.bytes());
    fmt::print("; setup word: {} user SGPRs{}{}\n", setup.user_sgprs,
               setup.fetch
                   ? fmt::format("; fetch through s[{}:{}]", setup.fetch_sgpr, setup.fetch_sgpr + 1)
                   : "",
               components ? fmt::format("; VGPR component count {}", *components) : "");
    return 0;
}

// What a program's code shows about its stage: a mesh (NGG) program asks for output space with
// GS_ALLOC_REQ, a vertex program exports a position, a pixel program exports to a color or depth
// target, and a compute program exports nothing.
enum class Role : uint8_t { kMesh, kVertex, kPixel, kNone };

std::string_view role_name(Role role) {
    switch (role) {
    case Role::kMesh: return "mesh";
    case Role::kVertex: return "vertex";
    case Role::kPixel: return "pixel";
    case Role::kNone: return "no exports";
    }
    return "?";
}

Role role_of(const shader::rdna2::Program& program) {
    namespace rdna2 = shader::rdna2;
    constexpr uint16_t kSoppSendmsg = 0x10;
    constexpr uint32_t kMessageMask = 0xF;
    constexpr uint32_t kGsAllocReq = 9;
    constexpr uint32_t kLastPixelTarget = 9; // MRT 0-7, depth (8), null (9)
    constexpr uint32_t kFirstPosition = 12;
    constexpr uint32_t kLastPosition = 15;
    bool position = false;
    bool pixel = false;
    for (const rdna2::Instruction& insn : program.instructions) {
        if (insn.encoding == rdna2::Encoding::kSopp && insn.opcode == kSoppSendmsg &&
            (insn.words[0] & kMessageMask) == kGsAllocReq) {
            return Role::kMesh;
        }
        if (insn.encoding == rdna2::Encoding::kExport) {
            const uint32_t target = (insn.words[0] >> 4) & 0x3F;
            position = position || (target >= kFirstPosition && target <= kLastPosition);
            pixel = pixel || target <= kLastPixelTarget;
        }
    }
    return position ? Role::kVertex : pixel ? Role::kPixel : Role::kNone;
}

// The shader records whose native programs carry each key (shader::fssm_key).
struct KeyedRecords {
    size_t records = 0;
    std::set<std::string> names;
    std::optional<uint8_t> fetch_stage; // the hardware stage, when the program calls the fetch
};
using RecordKeys = std::unordered_map<uint32_t, KeyedRecords>;

RecordKeys record_keys(std::span<const uint8_t> archive) {
    RecordKeys keys;
    shader::take_census(archive, [&](const shader::CensusShader& shader) {
        std::set<uint32_t> seen;
        for (const std::vector<uint8_t>& fssm : shader.native_programs) {
            const uint32_t key = shader::fssm_key(fssm);
            if (!seen.insert(key).second) {
                continue;
            }
            KeyedRecords& entry = keys[key];
            ++entry.records;
            entry.names.insert(shader.record.name);
            const std::vector<shader::FssmStage> stages = shader::fssm_stages(fssm);
            if (!entry.fetch_stage && !stages.empty() && shader::fssm_setup(fssm).fetch) {
                entry.fetch_stage = stages.front().hardware;
            }
        }
    });
    return keys;
}

// The cache's copies of programs that call the vertex fetch subroutine: whether they still call
// it, and which VGPRs they read at wave start.
void print_fetch_copies(const shader::pscache::PipelineCache& cache, const RecordKeys& keys) {
    namespace ps = shader::pscache;
    namespace rdna2 = shader::rdna2;
    std::map<std::string, size_t> programs; // by hardware stage
    for (const auto& [key, entry] : keys) {
        if (entry.fetch_stage) {
            ++programs[shader::hardware_stage_name(*entry.fetch_stage)];
        }
    }
    std::map<std::string, size_t> copies;
    size_t calling = 0;
    std::map<std::string, size_t> starts; // the VGPRs read at wave start -> copies
    for (uint32_t i = 0; i < cache.shaders.size(); ++i) {
        const std::span<const uint8_t> code = ps::shader_code(cache, i);
        const std::optional<uint32_t> key = ps::shader_key(code);
        const auto it = key ? keys.find(*key) : keys.end();
        if (it == keys.end() || !it->second.fetch_stage) {
            continue;
        }
        ++copies[shader::hardware_stage_name(*it->second.fetch_stage)];
        const rdna2::Program copy = rdna2::decode_program(code);
        calling += copy.indirect ? 1 : 0;
        const rdna2::LiveIns live = rdna2::live_ins(copy, 64);
        std::string vgprs;
        for (size_t v = 0; v < live.vector.size(); ++v) {
            if (live.vector[v]) {
                vgprs += fmt::format("{}v{}", vgprs.empty() ? "" : " ", v);
            }
        }
        ++starts[vgprs.empty() ? "none" : vgprs];
    }
    const auto key = [](const std::string& k) { return k; };
    fmt::print("fetch copies     programs that call the fetch subroutine {}; cache shaders with "
               "their keys {}, {} of which still call; VGPRs those read at wave start {}\n",
               join_counts(programs, key), join_counts(copies, key), calling,
               join_counts(starts, key));
}

// Which cache shaders carry the key of a shader record's native program, and how many records
// and names each one matches.
void print_record_links(const shader::pscache::PipelineCache& cache, const RecordKeys& keys) {
    namespace ps = shader::pscache;
    constexpr size_t kShownMost = 6;
    std::vector<bool> linked(cache.shaders.size(), false);
    std::map<size_t, size_t> records;
    std::map<size_t, size_t> names;
    for (uint32_t i = 0; i < cache.shaders.size(); ++i) {
        const std::optional<uint32_t> key = ps::shader_key(ps::shader_code(cache, i));
        const auto it = key ? keys.find(*key) : keys.end();
        if (it == keys.end()) {
            continue;
        }
        linked[i] = true;
        ++records[std::min(it->second.records, kShownMost)];
        ++names[std::min(it->second.names.size(), kShownMost)];
    }
    std::map<std::string, size_t> unlinked;
    for (const ps::GraphicsPipeline& pipeline : cache.graphics) {
        for (size_t slot = 0; slot < ps::kSlots; ++slot) {
            const uint32_t shader = pipeline.slots[slot].shader;
            if (shader != ps::kNone && !linked[shader]) {
                ++unlinked[fmt::format("slot {}", slot)];
            }
        }
    }
    for (const ps::ComputePipeline& pipeline : cache.compute) {
        if (!linked[pipeline.shader]) {
            ++unlinked["compute"];
        }
    }
    const auto count = [](size_t n) {
        return n == kShownMost ? fmt::format("{}+", n) : fmt::format("{}", n);
    };
    fmt::print("shader records   {} of {} shaders carry a record's key; records per shader {}; "
               "names per shader {}; uses of shaders without one {}\n",
               std::ranges::count(linked, true), cache.shaders.size(), join_counts(records, count),
               join_counts(names, count),
               join_counts(unlinked, [](const std::string& use) { return use; }));
}

void print_pipeline_cache(const shader::pscache::PipelineCache& cache) {
    namespace ps = shader::pscache;
    namespace rdna2 = shader::rdna2;
    fmt::print("pipeline cache   id {}, driver {}\n", cache.id, cache.driver);
    fmt::print("                 {} graphics and {} compute pipelines, {} shaders ({:.1f} MiB of "
               "code), {} metadata blobs ({:.1f} MiB)\n",
               cache.graphics.size(), cache.compute.size(), cache.shaders.size(),
               static_cast<double>(cache.code_area.size()) / (1024.0 * 1024.0),
               cache.metadata.size(),
               static_cast<double>(cache.metadata_area.size()) / (1024.0 * 1024.0));

    // Which slots graphics pipelines fill, and what the shaders in each slot export.
    std::map<std::string, size_t> combos;
    std::vector<int> used_by(cache.shaders.size(), 0); // bit 0 graphics, bit 1 compute
    std::map<uint32_t, size_t> binding_blobs;          // metadata index -> uses
    for (const ps::GraphicsPipeline& pipeline : cache.graphics) {
        std::string combo;
        for (size_t slot = 0; slot < ps::kSlots; ++slot) {
            const ps::Slot& s = pipeline.slots[slot];
            if (s.shader == ps::kNone) {
                continue;
            }
            combo += fmt::format("{}{}", combo.empty() ? "" : "+", slot);
            used_by.at(s.shader) |= 1;
            if (s.bindings != ps::kNone) {
                ++binding_blobs[s.bindings];
            }
        }
        ++combos[combo.empty() ? "none" : combo];
    }
    for (const ps::ComputePipeline& pipeline : cache.compute) {
        used_by.at(pipeline.shader) |= 2;
        ++binding_blobs[pipeline.bindings];
    }
    fmt::print("graphics slots   {}\n",
               join_counts(combos, [](const std::string& combo) { return combo; }));
    std::map<int, size_t> sharing;
    for (const int use : used_by) {
        ++sharing[use];
    }
    fmt::print("shaders used by  {}\n", join_counts(sharing, [](int use) {
                   return std::string(use == 1   ? "graphics"
                                      : use == 2 ? "compute"
                                      : use == 3 ? "both"
                                                 : "none");
               }));

    rdna2::DecodeStats stats;
    std::vector<Role> roles(cache.shaders.size(), Role::kNone);
    std::vector<bool> counters(cache.shaders.size(), false); // has ds_append or ds_consume
    std::vector<uint32_t> indirect;                          // the first few, for --shader
    for (uint32_t i = 0; i < cache.shaders.size(); ++i) {
        const rdna2::Program program = rdna2::decode_program(ps::shader_code(cache, i));
        stats.add(program);
        roles[i] = role_of(program);
        counters[i] = std::ranges::any_of(program.instructions, rdna2::appends_or_consumes);
        if (program.indirect && indirect.size() < 4) {
            indirect.push_back(i);
        }
    }
    print_decode(stats, 0);
    if (!indirect.empty()) {
        fmt::print("indirect         shaders {}{}\n", fmt::join(indirect, ", "),
                   stats.indirect > indirect.size() ? ", ..." : "");
    }
    std::set<uint32_t> slot6;
    std::set<uint32_t> slot6_vertex; // what slot 1 holds beside them
    for (const ps::GraphicsPipeline& pipeline : cache.graphics) {
        if (pipeline.slots[6].shader != ps::kNone) {
            slot6.insert(pipeline.slots[6].shader);
            slot6_vertex.insert(pipeline.slots[1].shader);
        }
    }
    if (!slot6.empty()) {
        fmt::print("slot 6           shaders {}, beside slot 1 shaders {}\n",
                   fmt::join(slot6, ", "), fmt::join(slot6_vertex, ", "));
    }
    std::map<std::string, size_t> slot_roles;
    std::map<std::string, size_t> counter_uses;
    const auto use = [&](const std::string& where, uint32_t shader) {
        const std::string key = fmt::format("{} {}", where, role_name(roles[shader]));
        ++slot_roles[key];
        counter_uses[key] += counters[shader] ? 1 : 0;
    };
    for (const ps::GraphicsPipeline& pipeline : cache.graphics) {
        for (size_t slot = 0; slot < ps::kSlots; ++slot) {
            if (pipeline.slots[slot].shader != ps::kNone) {
                use(fmt::format("slot {}", slot), pipeline.slots[slot].shader);
            }
        }
    }
    for (const ps::ComputePipeline& pipeline : cache.compute) {
        use("compute", pipeline.shader);
    }
    std::erase_if(counter_uses, [](const auto& entry) { return entry.second == 0; });
    const auto key = [](const std::string& k) { return k; };
    fmt::print("code by slot     {}\n", join_counts(slot_roles, key));
    fmt::print("append, consume  used in {}\n",
               counter_uses.empty() ? std::string("no slot") : join_counts(counter_uses, key));

    // Binding blobs: how many frame cleanly, and what their records hold.
    size_t framed = 0;
    std::map<uint32_t, size_t> tags;
    std::map<uint32_t, size_t> resources;
    for (const auto& [index, uses] : binding_blobs) {
        const std::optional<ps::Bindings> bindings =
            ps::read_bindings(ps::metadata_blob(cache, index));
        if (!bindings) {
            continue;
        }
        ++framed;
        for (const ps::BindingRecord& record : bindings->records) {
            ++tags[record.tag];
            if (record.tag == ps::kRecordResource) {
                ++resources[record.words[0]];
            }
        }
    }
    const auto hex = [](uint32_t value) { return fmt::format("{:#x}", value); };
    fmt::print("binding blobs    {} referenced, {} framed; records {}; resource kinds {}\n",
               binding_blobs.size(), framed, join_counts(tags, hex), join_counts(resources, hex));
}

int cmd_pscache(const std::filesystem::path& file, std::span<const std::string_view> flags) {
    std::optional<uint32_t> shader_number;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (flags[i] == "--shader" && i + 1 < flags.size()) {
            shader_number = static_cast<uint32_t>(std::stoul(std::string(flags[++i])));
        } else {
            LOG_ERROR(shaderrecomp, "bad option {}", flags[i]);
            return usage();
        }
    }
    const shader::MappedFile archive(file);
    const rpf9::Carve carve = rpf9::carve(archive.bytes());
    size_t found = 0;
    for (const rpf9::Entry& entry : carve.entries) {
        if (entry.head.kind != rpf9::Kind::kPipelineCache) {
            continue;
        }
        ++found;
        const std::vector<uint8_t> data = rpf9::read_entry(archive.bytes(), carve, entry);
        const shader::pscache::PipelineCache cache = shader::pscache::parse(data);
        if (shader_number) {
            print_disasm(shader::pscache::shader_code(cache, *shader_number));
            continue;
        }
        fmt::print("entry            at {:#x}, {} streams, {} bytes\n", entry.offset, entry.streams,
                   entry.size);
        print_pipeline_cache(cache);
        const RecordKeys keys = record_keys(archive.bytes());
        print_record_links(cache, keys);
        print_fetch_copies(cache, keys);
    }
    if (found == 0) {
        LOG_ERROR(shaderrecomp, "no pipeline cache in {}", file.string());
        return 1;
    }
    return 0;
}

int run(int argc, wchar_t** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::string command = std::filesystem::path(argv[1]).string();
    const std::filesystem::path file(argv[2]);
    std::vector<std::string> flag_storage;
    for (int i = 3; i < argc; ++i) {
        flag_storage.push_back(std::filesystem::path(argv[i]).string());
    }
    const std::vector<std::string_view> flags(flag_storage.begin(), flag_storage.end());
    if (command == "scan") {
        return cmd_scan(file, flags);
    }
    if (command == "disasm") {
        return cmd_disasm(file, flags);
    }
    if (command == "pscache") {
        return cmd_pscache(file, flags);
    }
    return usage();
}

} // namespace

int wmain(int argc, wchar_t** argv) { // NOLINT(misc-use-internal-linkage): process entry point
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        LOG_ERROR(shaderrecomp, "{}", error.what());
    }
    return 1;
}
