// shaderrecomp's reading of the console's shader archive (docs/shaders.md), on an archive built
// here from made-up shaders: DXBC containers laid out as the console's are, with stored
// (uncompressed) DEFLATE and zlib blocks standing in for compressed ones.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "archive.h"
#include "census.h"
#include "console.h"
#include "dxbc.h"
#include "g6/inflate.h"
#include "g6test.h"
#include "rdna2.h"

namespace sh = g6::shader;

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}
void put16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}
void put(Bytes& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}
void put(Bytes& out, std::span<const uint8_t> data) {
    out.insert(out.end(), data.begin(), data.end());
}
void set32(Bytes& out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

// DEFLATE stored blocks (RFC 1951 3.2.4) holding `data`.
Bytes stored_deflate(std::span<const uint8_t> data) {
    Bytes out;
    size_t at = 0;
    do {
        const size_t length = std::min<size_t>(data.size() - at, 0xFFFF);
        const bool last = at + length == data.size();
        out.push_back(last ? 1 : 0);
        put16(out, static_cast<uint16_t>(length));
        put16(out, static_cast<uint16_t>(~length));
        put(out, data.subspan(at, length));
        at += length;
    } while (at < data.size());
    return out;
}

Bytes stored_zlib(std::span<const uint8_t> data) {
    Bytes out = {0x78, 0x01};
    put(out, stored_deflate(data));
    const uint32_t sum = g6::adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(sum >> shift));
    }
    return out;
}

// A made-up native program: the FSSM magic and version, then filler.
Bytes fssm_image(uint8_t fill, size_t size) {
    Bytes out;
    put(out, "FSSM");
    put16(out, 0x101);
    out.resize(size, fill);
    return out;
}

// A DXIL program header whose bitcode, `bitcode_size` bytes, sits `bitcode_at` bytes into the
// part.
void program_header(Bytes& out, uint16_t kind, size_t part_size, size_t bitcode_at,
                    uint32_t bitcode_size) {
    put32(out, (uint32_t{kind} << 16) | 0x66);
    put32(out, static_cast<uint32_t>(part_size / 4));
    put(out, "DXIL");
    put32(out, 0x106);
    put32(out, static_cast<uint32_t>(bitcode_at - 8));
    put32(out, bitcode_size);
}

Bytes xbch_entry(std::span<const uint8_t> fssm) {
    Bytes xbch;
    const Bytes packed = stored_zlib(fssm);
    put(xbch, "XBCH");
    put32(xbch, 3);
    put32(xbch, static_cast<uint32_t>(packed.size()));
    put32(xbch, 0);
    put32(xbch, static_cast<uint32_t>(fssm.size()));
    put32(xbch, 0);
    put(xbch, packed);
    return xbch;
}

// A DXIL part laid out as the console's: program header, XDAT table (digest, native programs, copy
// of the program header), the entries, and NOBC at the end.
Bytes console_dxil_part(uint16_t kind, const std::vector<Bytes>& programs) {
    std::vector<std::array<size_t, 3>> table; // tag, offset, size
    size_t at = sh::kDxilProgramHeaderSize + 12 + ((programs.size() + 2) * 12);
    table.push_back({sh::kXdatDigest, at, 16});
    at += 16;
    std::vector<Bytes> entries;
    for (const Bytes& fssm : programs) {
        entries.push_back(xbch_entry(fssm));
        table.push_back({sh::kXdatNativeProgram, at, entries.back().size()});
        at += entries.back().size();
    }
    table.push_back({sh::kXdatProgramHeader, at, 28});
    const size_t size = at + 28;

    Bytes part;
    program_header(part, kind, size, size - 4, 4);
    put(part, "XDAT");
    put32(part, static_cast<uint32_t>(table.size()));
    put32(part, 0x24);
    for (const auto& [tag, offset, length] : table) {
        put32(part, static_cast<uint32_t>(tag));
        put32(part, static_cast<uint32_t>(offset));
        put32(part, static_cast<uint32_t>(length));
    }
    part.resize(part.size() + 16, 0x5D); // the digest
    for (const Bytes& entry : entries) {
        put(part, entry);
    }
    program_header(part, kind, 28, 24, 4);
    put(part, "NOBC");
    return part;
}

Bytes dxbc_container(const std::vector<std::pair<std::string_view, Bytes>>& parts) {
    Bytes out;
    put(out, "DXBC");
    out.resize(20, 0xD1); // digest
    put16(out, 1);
    put16(out, 0);
    put32(out, 0); // total, set below
    put32(out, static_cast<uint32_t>(parts.size()));
    size_t offset = out.size() + (4 * parts.size());
    for (const auto& [name, data] : parts) {
        put32(out, static_cast<uint32_t>(offset));
        offset += 8 + data.size();
    }
    for (const auto& [name, data] : parts) {
        put(out, name);
        put32(out, static_cast<uint32_t>(data.size()));
        put(out, data);
    }
    set32(out, 24, static_cast<uint32_t>(out.size()));
    return out;
}

void record(Bytes& archive, std::string_view name, uint8_t tag,
            std::span<const uint8_t> container) {
    const Bytes packed = stored_deflate(container);
    put(archive, name);
    archive.push_back(0);
    archive.push_back(tag);
    put32(archive, static_cast<uint32_t>(packed.size()));
    put32(archive, 0xDDCE7);
    put32(archive, static_cast<uint32_t>(packed.size()));
    put32(archive, static_cast<uint32_t>(container.size()));
    put(archive, packed);
}

struct TestArchive {
    Bytes bytes;
    Bytes vertex_fssm = fssm_image(0xA1, 300);
    Bytes pixel_fssm = fssm_image(0xB2, 70000); // more than one stored block
    Bytes two_program_fssm = fssm_image(0xC3, 200);
    Bytes two_program_second_fssm = fssm_image(0xC4, 100);
    Bytes vertex;
    Bytes pixel;
    Bytes two_program; // a vertex shader with two native programs, like 195 of the console's
};

TestArchive make_archive() {
    TestArchive a;
    a.vertex = dxbc_container({{"SFI0", Bytes(8, 0)},
                               {"PSV0", Bytes(52, 7)},
                               {"DXIL", console_dxil_part(1, {a.vertex_fssm})},
                               {"PRIV", Bytes(40, 9)}});
    a.pixel = dxbc_container({{"DXIL", console_dxil_part(0, {a.pixel_fssm})}});
    a.two_program = dxbc_container(
        {{"DXIL", console_dxil_part(1, {a.two_program_fssm, a.two_program_second_fssm})}});
    a.bytes = {0x00, 0x01, 0x02, 0xFF, 0x00, 0x00};
    record(a.bytes, "VS_Made_Up", 0x32, a.vertex);
    a.bytes.insert(a.bytes.end(), 8, 0); // padding
    // A header with mismatched size copies isn't a record, and one whose data isn't DEFLATE is
    // rejected.
    put(a.bytes, "NotARecord");
    a.bytes.push_back(0);
    a.bytes.insert(a.bytes.end(), {0x32, 40, 0, 0, 0, 1, 2, 3, 4, 41, 0, 0, 0, 64, 0, 0, 0});
    put(a.bytes, "BadStream");
    a.bytes.push_back(0);
    a.bytes.push_back(0x32);
    for (const uint32_t value : {40u, 0u, 40u, 64u}) {
        put32(a.bytes, value);
    }
    a.bytes.insert(a.bytes.end(), 40, 0xFF); // block type 3
    record(a.bytes, "PS_Made_Up", 0x41, a.pixel);
    record(a.bytes, "VS2_Made_Up", 0x3C, a.two_program);
    a.bytes.insert(a.bytes.end(), {0x00, 0x7E, 0x7E});
    return a;
}

} // namespace

TEST_CASE("the archive scan finds every record and rejects look-alikes") {
    const TestArchive a = make_archive();
    std::vector<sh::ShaderRecord> records;
    const sh::ArchiveScanStats stats =
        sh::scan_shader_archive(a.bytes, [&](const sh::ShaderRecord& r) { records.push_back(r); });
    CHECK_EQ(stats.records, size_t{3});
    CHECK_EQ(stats.rejected, size_t{1});
    REQUIRE(records.size() == 3);
    CHECK_EQ(records[0].name, std::string("VS_Made_Up"));
    CHECK_EQ(records[0].offset, uint64_t{6});
    CHECK_EQ(records[0].tag, uint8_t{0x32});
    CHECK_EQ(records[0].id, uint32_t{0xDDCE7});
    CHECK(records[0].container == a.vertex);
    CHECK_EQ(records[1].name, std::string("PS_Made_Up"));
    CHECK_EQ(records[1].tag, uint8_t{0x41});
    CHECK(records[1].container == a.pixel);
    CHECK_EQ(records[2].name, std::string("VS2_Made_Up"));
    CHECK(records[2].container == a.two_program);
}

TEST_CASE("a console container parses down to its native program") {
    const TestArchive a = make_archive();
    const sh::Dxbc dxbc = sh::parse_dxbc(a.vertex);
    CHECK_EQ(dxbc.major, uint16_t{1});
    REQUIRE(dxbc.parts.size() == 4);
    CHECK_EQ(sh::fourcc_text(dxbc.parts[1].fourcc), std::string("PSV0"));
    CHECK_EQ(dxbc.parts[1].data.size(), size_t{52});
    const sh::DxbcPart* dxil = dxbc.find(sh::fourcc("DXIL"));
    REQUIRE(dxil != nullptr);

    const sh::DxilProgram program = sh::parse_dxil_program(dxil->data);
    CHECK_EQ(sh::kind_name(program.kind), std::string("vertex"));
    CHECK_EQ(program.major, uint8_t{6});
    CHECK_EQ(program.minor, uint8_t{6});
    CHECK(program.no_bitcode_marker());
    CHECK(!program.has_llvm_bitcode());

    const auto xdat = sh::read_xdat(dxil->data);
    REQUIRE(xdat.has_value());
    CHECK_EQ(xdat->unknown, uint32_t{0x24});
    const std::vector<sh::XdatEntry>& entries = xdat->entries;
    REQUIRE(entries.size() == 3);
    CHECK_EQ(entries[0].tag, sh::kXdatDigest);
    CHECK_EQ(entries[0].data.size(), size_t{16});
    CHECK_EQ(entries[1].tag, sh::kXdatNativeProgram);
    const sh::NativeProgramHeader header = sh::read_native_program_header(entries[1].data);
    CHECK_EQ(header.version, uint32_t{3});
    CHECK_EQ(header.size, uint32_t{300});
    CHECK(sh::unpack_native_program(entries[1].data) == a.vertex_fssm);
    CHECK_EQ(sh::fssm_version(a.vertex_fssm), uint16_t{0x101});
    const sh::DxilProgram copy = sh::parse_dxil_program(entries[2].data);
    CHECK(copy.no_bitcode_marker());
}

TEST_CASE("a vertex shader's container can carry two native programs") {
    const TestArchive a = make_archive();
    const sh::Dxbc dxbc = sh::parse_dxbc(a.two_program);
    const sh::DxbcPart* dxil = dxbc.find(sh::fourcc("DXIL"));
    REQUIRE(dxil != nullptr);
    CHECK_EQ(sh::kind_name(sh::parse_dxil_program(dxil->data).kind), std::string("vertex"));
    const auto xdat = sh::read_xdat(dxil->data);
    REQUIRE(xdat.has_value());
    const std::vector<sh::XdatEntry>& entries = xdat->entries;
    REQUIRE(entries.size() == 4);
    CHECK_EQ(xdat->unknown, uint32_t{0x24});
    CHECK_EQ(entries[1].tag, sh::kXdatNativeProgram);
    CHECK_EQ(entries[2].tag, sh::kXdatNativeProgram);
    CHECK(sh::unpack_native_program(entries[1].data) == a.two_program_fssm);
    CHECK(sh::unpack_native_program(entries[2].data) == a.two_program_second_fssm);

    // A count larger than the part can hold.
    Bytes part(dxil->data.begin(), dxil->data.end());
    set32(part, sh::kDxilProgramHeaderSize + 4, static_cast<uint32_t>(part.size() / 12));
    CHECK_THROWS_AS(sh::read_xdat(part), sh::FormatError);
}

TEST_CASE("the census counts kinds, bitcode, tags and native programs") {
    const TestArchive a = make_archive();
    std::vector<size_t> programs;
    const sh::Census c = sh::take_census(a.bytes, [&](const sh::CensusShader& shader) {
        CHECK(shader.program.has_value());
        programs.push_back(shader.native_programs.size());
    });
    const std::vector<size_t> expected = {1, 1, 2};
    CHECK(programs == expected);
    CHECK_EQ(c.scan.records, size_t{3});
    CHECK_EQ(c.names.size(), size_t{3});
    CHECK_EQ(c.kinds.at(0), size_t{1});
    CHECK_EQ(c.kinds.at(1), size_t{2});
    CHECK_EQ(c.kinds.size(), size_t{2});
    CHECK_EQ(c.shader_models.at("6.6"), size_t{3});
    CHECK_EQ(c.no_bitcode_marker, size_t{3});
    CHECK_EQ(c.llvm_bitcode, size_t{0});
    CHECK_EQ(c.parts.at(sh::fourcc("DXIL")), size_t{3});
    CHECK_EQ(c.parts.at(sh::fourcc("PSV0")), size_t{1});
    CHECK_EQ(c.xdat_tags.at(sh::kXdatNativeProgram), size_t{4});
    CHECK_EQ(c.xdat_unknown.at(0x24), size_t{3});
    CHECK_EQ(c.programs_per_shader.at(1), size_t{2});
    CHECK_EQ(c.programs_per_shader.at(2), size_t{1});
    CHECK_EQ(c.multi_program_shaders.size(), size_t{1});
    CHECK_EQ(c.multi_program_shaders.at(1), size_t{1});
    CHECK_EQ(c.native_programs, size_t{4});
    CHECK_EQ(c.fssm_versions.at(0x101), size_t{4});
    CHECK_EQ(c.malformed, size_t{0});
}

TEST_CASE("the census counts byte-identical containers and programs once") {
    const TestArchive a = make_archive();
    // A second record of the vertex container, and a vertex container that differs only in its
    // PRIV part, so it is a new container around the same native program.
    const Bytes other_vertex = dxbc_container({{"SFI0", Bytes(8, 0)},
                                               {"PSV0", Bytes(52, 7)},
                                               {"DXIL", console_dxil_part(1, {a.vertex_fssm})},
                                               {"PRIV", Bytes(40, 8)}});
    Bytes archive = a.bytes;
    record(archive, "VS_Made_Up_Again", 0x32, a.vertex);
    record(archive, "VS_Made_Up_Other", 0x32, other_vertex);

    const sh::Census c = sh::take_census(archive);
    CHECK_EQ(c.malformed, size_t{0});
    CHECK_EQ(c.kinds.at(1), size_t{4});
    // The first vertex container, the two-program one, and the one with the other PRIV part.
    CHECK_EQ(c.distinct_containers.at(1), size_t{3});
    // The first vertex program, which two more records repeat, and the two-program shader's two.
    CHECK_EQ(c.distinct_native_programs.at(1), size_t{3});
    CHECK_EQ(c.distinct_containers.at(0), size_t{1});
    CHECK_EQ(c.distinct_native_programs.at(0), size_t{1});
}

namespace {

// A made-up FSSM image with stage records from 0x100, the code at 0x300, and an input enable
// word at 0x248. Each stage is (API stage, hardware stage).
Bytes staged_fssm(std::span<const std::pair<uint8_t, uint8_t>> stages, uint32_t enable) {
    Bytes out = fssm_image(0, 0x400);
    set32(out, 0xC0, 0x300);
    set32(out, 0xC8, 0x40);
    for (size_t i = 0; i < stages.size(); ++i) {
        const size_t at = 0x100 + (0x88 * i);
        set32(out, at, 0x00020000 | (uint32_t{stages[i].first} << 8) | stages[i].second);
        set32(out, at + 4, 0xC0);
        set32(out, at + 8, 0xDEADBEEF);
    }
    set32(out, 0x248, enable);
    return out;
}

} // namespace

TEST_CASE("an FSSM image names its stages, and a pixel program its inputs") {
    const std::array<std::pair<uint8_t, uint8_t>, 2> geometry = {{{2, 3}, {2, 4}}};
    const Bytes gs = staged_fssm(geometry, 0);
    const std::vector<sh::FssmStage> gs_stages = sh::fssm_stages(gs);
    REQUIRE(gs_stages.size() == 2);
    CHECK((gs_stages[0] == sh::FssmStage{.api = 2, .hardware = sh::kHardwareGs}));
    CHECK((gs_stages[1] == sh::FssmStage{.api = 2, .hardware = sh::kHardwareVs}));
    CHECK_EQ(sh::hardware_stage_name(gs_stages[0].hardware), std::string("GS"));
    CHECK_EQ(sh::hardware_stage_name(7), std::string("7"));
    CHECK(!sh::fssm_ps_input_enable(gs).has_value());

    // Perspective center, position X, Y and W, and front face.
    const std::array<std::pair<uint8_t, uint8_t>, 1> pixel = {{{1, 5}}};
    const Bytes ps = staged_fssm(pixel, 0x1B02);
    CHECK_EQ(sh::fssm_stages(ps).size(), size_t{1});
    REQUIRE(sh::fssm_ps_input_enable(ps).has_value());
    const sh::PsInputs inputs = sh::ps_inputs(*sh::fssm_ps_input_enable(ps));
    CHECK_EQ(inputs.vgprs, uint32_t{6});
    CHECK_EQ(inputs.barycentrics, uint32_t{0x3});
    CHECK(inputs.position[0] == std::optional<uint32_t>(2));
    CHECK(inputs.position[1] == std::optional<uint32_t>(3));
    CHECK(!inputs.position[2].has_value());
    CHECK(inputs.position[3] == std::optional<uint32_t>(4));
    CHECK(inputs.front_face == std::optional<uint32_t>(5));

    // Perspective sample, centroid and pull model, linear center: I/J in v0-v1, v2-v3 and v7-v8;
    // the pull model's three VGPRs, v4-v6, aren't barycentrics.
    const sh::PsInputs mixed = sh::ps_inputs(0x2D);
    CHECK_EQ(mixed.vgprs, uint32_t{9});
    CHECK_EQ(mixed.barycentrics, uint32_t{0x18F});

    // A stage record that doesn't open with the tag ends the list; images without one have none.
    CHECK(sh::fssm_stages(fssm_image(0xA1, 0x400)).empty());
    CHECK(sh::fssm_stages(fssm_image(0xA1, 100)).empty());
    CHECK_THROWS_AS(sh::fssm_stages(Bytes(0x400, 0)), sh::FormatError);
}

TEST_CASE("the setup word and the RSRC2 word say how a program starts") {
    // A vertex program that fetches through s[4:5] with 20 user SGPRs, running 64 lanes.
    const std::array<std::pair<uint8_t, uint8_t>, 1> vertex = {{{0, 4}}};
    Bytes vs = staged_fssm(vertex, 0);
    set32(vs, 0xD4, 0x04140004);
    set32(vs, 0x2A0, 20 << 1);
    const sh::FssmSetup vs_setup = sh::fssm_setup(vs);
    CHECK(vs_setup.fetch);
    CHECK(!vs_setup.wave32);
    CHECK(!vs_setup.gs);
    CHECK_EQ(vs_setup.user_sgprs, uint8_t{20});
    CHECK_EQ(vs_setup.fetch_sgpr, uint8_t{4});
    CHECK(sh::fssm_rsrc2(vs) == std::optional<uint32_t>(40));
    // It loads the instance ID: VGPR component count 3.
    set32(vs, 0x2BC, 3);
    CHECK(sh::fssm_vgpr_components(vs) == std::optional<uint32_t>(3));

    // A compute program with 5 user SGPRs, running 32 lanes; its RSRC2 sits at 0x138, and it has
    // no VGPR component count word.
    const std::array<std::pair<uint8_t, uint8_t>, 1> compute = {{{3, 6}}};
    Bytes cs = staged_fssm(compute, 0);
    set32(cs, 0xD4, 0x00050020);
    set32(cs, 0x138, 0x98A);
    set32(cs, 0x2BC, 3);
    CHECK(sh::fssm_setup(cs).wave32);
    CHECK(sh::fssm_rsrc2(cs) == std::optional<uint32_t>(0x98A));
    CHECK_EQ(sh::compute_setup(0x98A).user_sgprs, 5U);
    CHECK(!sh::fssm_vgpr_components(cs).has_value());

    // Mesh programs have no RSRC2 word, but have the component count; an image too short for
    // the setup word throws.
    const std::array<std::pair<uint8_t, uint8_t>, 1> mesh = {{{6, 7}}};
    Bytes ms = staged_fssm(mesh, 0);
    set32(ms, 0x2BC, 1);
    CHECK(!sh::fssm_rsrc2(ms).has_value());
    CHECK(sh::fssm_vgpr_components(ms) == std::optional<uint32_t>(1));
    CHECK_THROWS_AS(sh::fssm_setup(fssm_image(0, 0x40)), sh::FormatError);
}

TEST_CASE("ES and GS words give the LDS size and the GS output primitive types") {
    // The merged stage's RSRC2 layout: 15 user SGPRs and LDS_SIZE 2, as every ES word in 1.0.2.1
    // has; then every field but the user SGPRs set.
    const sh::GsRsrc2 es = sh::gs_rsrc2(0x0010001E);
    CHECK_EQ(es.user_sgprs, 15U);
    CHECK_EQ(es.es_vgpr_components, 0U);
    CHECK_EQ(es.lds_size, 2U);
    const sh::GsRsrc2 full = sh::gs_rsrc2(0x07FF0000);
    CHECK_EQ(full.user_sgprs, 0U);
    CHECK_EQ(full.es_vgpr_components, 3U);
    CHECK_EQ(full.lds_size, 0xFFU);

    // A GS program whose stream 0 writes triangle strips and whose other streams say 3.
    constexpr std::array<uint8_t, 4> kStrips = {2, 3, 3, 3};
    const std::array<std::pair<uint8_t, uint8_t>, 2> geometry = {{{2, 3}, {2, 4}}};
    Bytes gs = staged_fssm(geometry, 0);
    set32(gs, 0x13C, 0x00C30302);
    CHECK((sh::fssm_gs_out_prim_types(gs) == std::optional<std::array<uint8_t, 4>>(kStrips)));
    const std::array<std::pair<uint8_t, uint8_t>, 1> vertex = {{{0, 4}}};
    const Bytes vs = staged_fssm(vertex, 0);
    CHECK(!sh::fssm_gs_out_prim_types(vs).has_value());
    CHECK(sh::gs_out_prim_type(sh::kTopologyPointList) == std::optional<uint8_t>(0));
    CHECK(sh::gs_out_prim_type(sh::kTopologyLineStrip) == std::optional<uint8_t>(1));
    CHECK(sh::gs_out_prim_type(sh::kTopologyTriangleStrip) == std::optional<uint8_t>(2));
    CHECK(!sh::gs_out_prim_type(4).has_value()); // a triangle list isn't a GS output

    // The shader's PSV0: triangles in, triangle strips out on stream 0, with a position.
    Bytes psv;
    put32(psv, 24);
    put32(psv, 3);
    put32(psv, sh::kTopologyTriangleStrip);
    put32(psv, 1);
    psv.resize(28, 0);
    psv[16] = 1;
    const Bytes container = dxbc_container({{"PSV0", psv}});
    const std::optional<sh::PsvGeometry> info = sh::psv_geometry(container);
    REQUIRE(info.has_value());
    CHECK_EQ(info->input_primitive, 3U);
    CHECK_EQ(info->output_topology, sh::kTopologyTriangleStrip);
    CHECK_EQ(info->output_streams, 1U);
    CHECK(info->position);
    CHECK(!sh::psv_geometry(dxbc_container({{"SFI0", Bytes(8, 0)}})).has_value());
    // Runtime info that says it is longer than its part.
    CHECK_THROWS_AS(sh::psv_geometry(dxbc_container({{"PSV0", Bytes(52, 7)}})), sh::FormatError);

    // Checked against PSV0: the strips agree, line strips don't, a shader without PSV0 isn't
    // checked, and a program that isn't GS is skipped.
    Bytes lines = psv;
    set32(lines, 8, sh::kTopologyLineStrip);
    sh::GsOutputCheck check;
    sh::check_gs_output(check, gs, container);
    sh::check_gs_output(check, gs, dxbc_container({{"PSV0", lines}}));
    sh::check_gs_output(check, gs, dxbc_container({{"SFI0", Bytes(8, 0)}}));
    sh::check_gs_output(check, vs, container);
    CHECK((check.types == std::map<std::array<uint8_t, 4>, size_t>{{kStrips, 3}}));
    CHECK((check.streams == std::map<uint32_t, size_t>{{1, 2}}));
    CHECK_EQ(check.checked, size_t{2});
    CHECK_EQ(check.differs, size_t{1});
}

TEST_CASE("the census counts LDS sizes, LDS reads and writes, and exports") {
    namespace r = sh::rdna2;
    Bytes code;
    put32(code, 0xD8340000); // ds_write_b32 v0, v1
    put32(code, 0x00000100);
    put32(code, 0xD8D80000); // ds_read_b32 v2, v0
    put32(code, 0x02000000);
    put32(code, 0xBF810000); // s_endpgm
    const r::Program lds = r::decode_program(code);
    Bytes exp_code;
    put32(exp_code, 0xF800080F); // exp mrt0 v0, v0, v0, v0 done
    put32(exp_code, 0);
    put32(exp_code, 0xBF810000);
    const r::Program exports = r::decode_program(exp_code);

    // An ES program with 15 user SGPRs, LDS_SIZE 2, and the component count word saying 0 like
    // its RSRC2; then one whose component count word says 1.
    const std::array<std::pair<uint8_t, uint8_t>, 1> es_stage = {{{0, 2}}};
    Bytes es = staged_fssm(es_stage, 0);
    set32(es, 0xD4, 15U << 16);
    set32(es, 0x2A0, 0x0010001E);
    set32(es, 0x2BC, 0);
    std::map<uint8_t, sh::StageInputs> stages;
    sh::count_inputs(stages, es, lds);
    set32(es, 0x2BC, 1);
    sh::count_inputs(stages, es, exports);
    const sh::StageInputs& counted = stages[sh::kHardwareEs];
    CHECK((counted.lds_sizes == std::map<unsigned, size_t>{{2, 2}}));
    CHECK_EQ(counted.es_components_differ, size_t{1});
    CHECK_EQ(counted.lds_readers, size_t{1});
    CHECK_EQ(counted.lds_writers, size_t{1});
    CHECK_EQ(counted.exporters, size_t{1});
    // Other stages count LDS and exports, but have no LDS size.
    const std::array<std::pair<uint8_t, uint8_t>, 1> vs_stage = {{{0, 4}}};
    sh::count_inputs(stages, staged_fssm(vs_stage, 0), exports);
    CHECK(stages[sh::kHardwareVs].lds_sizes.empty());
    CHECK_EQ(stages[sh::kHardwareVs].exporters, size_t{1});
}

TEST_CASE("the VGPR component count bounds the VGPRs a vertex or mesh program reads first") {
    namespace r = sh::rdna2;
    const auto decode = [](uint32_t word) {
        Bytes code;
        put32(code, word);
        put32(code, 0xBF810000); // s_endpgm
        return r::decode_program(code);
    };
    const r::Program reads_v3 = decode(0x7E080303); // v_mov_b32 v4, v3
    const r::Program reads_v6 = decode(0x7E0E0306); // v_mov_b32 v7, v6
    const auto image = [](uint8_t hardware, uint32_t components) {
        const uint8_t api = hardware == sh::kHardwareMesh ? 6 : 0;
        const std::array<std::pair<uint8_t, uint8_t>, 1> stage = {{{api, hardware}}};
        Bytes fssm = staged_fssm(stage, 0);
        set32(fssm, 0xD4, 8U << 16);
        set32(fssm, 0x2A0, 8U << 1);
        set32(fssm, 0x2BC, components);
        return fssm;
    };
    std::map<uint8_t, sh::StageInputs> stages;
    // A VS loads v0 up to the count: the instance ID in v3 needs 3.
    CHECK(!sh::count_inputs(stages, image(sh::kHardwareVs, 3), reads_v3).setup_outside);
    CHECK(sh::count_inputs(stages, image(sh::kHardwareVs, 0), reads_v3).setup_outside);
    // A mesh program loads from v5, after the geometry half's five VGPRs.
    CHECK(!sh::count_inputs(stages, image(sh::kHardwareMesh, 1), reads_v6).setup_outside);
    CHECK(sh::count_inputs(stages, image(sh::kHardwareMesh, 0), reads_v6).setup_outside);
    const sh::StageInputs& vs = stages[sh::kHardwareVs];
    CHECK_EQ(vs.setup_vgprs_checked, size_t{2});
    CHECK_EQ(vs.setup_outside, size_t{1});
    CHECK_EQ(vs.vgpr_components.at(0), size_t{1});
    CHECK_EQ(vs.vgpr_components.at(3), size_t{1});
    CHECK_EQ(stages[sh::kHardwareMesh].setup_vgprs_checked, size_t{2});
    CHECK_EQ(stages[sh::kHardwareMesh].setup_outside, size_t{1});
}

namespace {

// An ISG1 part: a count and an offset, then 32-byte elements, whose name offsets all point at one
// made-up name after them.
Bytes signature_part(std::span<const sh::SignatureElement> elements) {
    Bytes part;
    put32(part, static_cast<uint32_t>(elements.size()));
    put32(part, 8);
    const auto name_at = static_cast<uint32_t>(8 + (32 * elements.size()));
    for (const sh::SignatureElement& element : elements) {
        put32(part, element.stream);
        put32(part, name_at);
        put32(part, element.semantic_index);
        put32(part, element.system_value);
        put32(part, element.component_type);
        put32(part, element.reg);
        part.push_back(element.mask);
        part.push_back(element.rw_mask);
        put16(part, 0);
        put32(part, element.min_precision);
    }
    put(part, "ATTR");
    part.push_back(0);
    return part;
}

// Three inputs, the middle one unread, and the instance ID.
const std::array<sh::SignatureElement, 4> kVertexInputs = {{
    {.component_type = 3, .reg = 0, .mask = 0x7, .rw_mask = 0x7},
    {.semantic_index = 1, .component_type = 3, .reg = 1, .mask = 0xF, .rw_mask = 0},
    {.component_type = 3, .reg = 2, .mask = 0x3, .rw_mask = 0x3, .min_precision = 1},
    {.system_value = sh::kSystemValueInstanceId,
     .component_type = 1,
     .reg = 3,
     .mask = 0x1,
     .rw_mask = 0x1},
}};

} // namespace

TEST_CASE("a container's input signature parses from its ISG1 part") {
    const Bytes container =
        dxbc_container({{"SFI0", Bytes(8, 0)}, {"ISG1", signature_part(kVertexInputs)}});
    const std::vector<sh::SignatureElement> inputs = sh::input_signature(container);
    REQUIRE(inputs.size() == 4);
    CHECK_EQ(inputs[1].semantic_index, uint32_t{1});
    CHECK_EQ(inputs[1].mask, uint8_t{0xF});
    CHECK_EQ(inputs[1].rw_mask, uint8_t{0});
    CHECK_EQ(inputs[2].reg, uint32_t{2});
    CHECK_EQ(inputs[2].rw_mask, uint8_t{0x3});
    CHECK_EQ(inputs[2].min_precision, uint32_t{1});
    CHECK_EQ(inputs[3].system_value, sh::kSystemValueInstanceId);
    CHECK(sh::input_signature(dxbc_container({{"SFI0", Bytes(8, 0)}})).empty());
    Bytes too_many = signature_part(kVertexInputs);
    set32(too_many, 0, 5);
    CHECK_THROWS_AS(sh::parse_signature(too_many), sh::FormatError);
}

TEST_CASE("the fetch layout gives each input a vertex program reads four VGPRs") {
    const std::optional<sh::VertexVgprs> vs = sh::vertex_vgprs(sh::kHardwareVs);
    REQUIRE(vs.has_value());
    CHECK_EQ(vs->vertex_id, uint32_t{0});
    CHECK_EQ(vs->instance_id, uint32_t{3});
    CHECK_EQ(vs->first_fetched, uint32_t{4});
    const std::optional<sh::VertexVgprs> es = sh::vertex_vgprs(sh::kHardwareEs);
    REQUIRE(es.has_value());
    CHECK_EQ(es->vertex_id, uint32_t{5});
    CHECK_EQ(es->first_fetched, uint32_t{9});
    CHECK(!sh::vertex_vgprs(sh::kHardwarePs).has_value());

    // Listed out of register order: the elements read go by register, the unread one and the
    // instance ID take nothing.
    const std::array<sh::SignatureElement, 4> shuffled = {kVertexInputs[2], kVertexInputs[3],
                                                          kVertexInputs[1], kVertexInputs[0]};
    const std::vector<sh::FetchedElement> layout = sh::fetch_layout(shuffled, 4);
    REQUIRE(layout.size() == 2);
    CHECK_EQ(layout[0].element, size_t{3});
    CHECK_EQ(layout[0].first_vgpr, uint32_t{4});
    CHECK_EQ(layout[1].element, size_t{0});
    CHECK_EQ(layout[1].first_vgpr, uint32_t{8});
}

TEST_CASE("a fetching program's reads after the call are checked against the fetch layout") {
    namespace r = sh::rdna2;
    // s_swappc_b64 s[4:5], s[4:5], the call the game's programs make, then v_mov_b32 from each
    // VGPR given, then s_endpgm.
    const auto program = [](std::initializer_list<uint32_t> reads) {
        Bytes code;
        put32(code, 0xBE842104);
        uint32_t dst = 20;
        for (const uint32_t vgpr : reads) {
            put32(code, 0x7E000200 | (dst++ << 17) | (0x100 + vgpr));
        }
        put32(code, 0xBF810000);
        return r::decode_program(code);
    };
    // A VS that fetches through s[4:5], with 20 user SGPRs and the instance ID loaded.
    const std::array<std::pair<uint8_t, uint8_t>, 1> vertex = {{{0, 4}}};
    Bytes vs = staged_fssm(vertex, 0);
    set32(vs, 0xD4, 0x04140004);
    set32(vs, 0x2A0, 20 << 1);
    set32(vs, 0x2BC, 3);

    std::map<uint8_t, sh::StageInputs> stages;
    // Inputs 0 and 2 at v4 and v8, and the instance ID it declares: the fetch layout fits, and a
    // layout by register, which puts input 2 at v12, doesn't.
    sh::count_inputs(stages, vs, program({4, 5, 6, 8, 9, 3}), kVertexInputs);
    const sh::StageInputs& counts = stages[sh::kHardwareVs];
    CHECK_EQ(counts.fetch_layout_checked, size_t{1});
    CHECK_EQ(counts.fetch_layout_differs, size_t{0});
    CHECK_EQ(counts.fetch_by_register_differs, size_t{1});
    CHECK_EQ(counts.fetch_with_unread, size_t{1});
    // The other way round.
    sh::count_inputs(stages, vs, program({4, 5, 6, 12, 13}), kVertexInputs);
    CHECK_EQ(counts.fetch_layout_differs, size_t{1});
    CHECK_EQ(counts.fetch_by_register_differs, size_t{1});
    // Reading the vertex ID, which the signature doesn't declare, fits neither.
    sh::count_inputs(stages, vs, program({0, 4, 5, 6, 8, 9}), kVertexInputs);
    CHECK_EQ(counts.fetch_layout_differs, size_t{2});
    CHECK_EQ(counts.fetch_by_register_differs, size_t{2});
    // Without a signature nothing is checked.
    sh::count_inputs(stages, vs, program({4}));
    CHECK_EQ(counts.fetch_layout_checked, size_t{3});
    CHECK_EQ(counts.fetch_with_unread, size_t{3});
    CHECK_EQ(counts.programs, size_t{4});
}

TEST_CASE("a program's inputs are checked against its stage's setup words") {
    namespace r = sh::rdna2;
    // s_mov_b32 m0, s10; v_interp_p1_f32 v2, v0, attr0.x; v_interp_p2_f32 v2, v1, attr0.x;
    // s_endpgm: a pixel program that reads the primitive mask after ten user SGPRs.
    const std::array<uint32_t, 4> words = {0xBEFC030A, 0xC8080000, 0xC8090001, 0xBF810000};
    Bytes code;
    for (const uint32_t word : words) {
        put32(code, word);
    }
    const r::Program program = r::decode_program(code);
    const std::array<std::pair<uint8_t, uint8_t>, 1> pixel = {{{1, 5}}};
    const auto image = [&](uint8_t user_sgprs, uint32_t rsrc2) {
        Bytes ps = staged_fssm(pixel, 0x2); // perspective center: I and J in v0 and v1
        set32(ps, 0xD4, (uint32_t{user_sgprs} << 16) | 0x20);
        set32(ps, 0x23C, rsrc2 | (uint32_t{user_sgprs} << 1));
        return ps;
    };
    std::map<uint8_t, sh::StageInputs> stages;
    const sh::ProgramInputs fits = sh::count_inputs(stages, image(10, 0), program);
    CHECK_EQ(fits.stage, sh::kHardwarePs);
    CHECK_EQ(fits.wave, 32U);
    CHECK(!fits.setup_outside);
    CHECK(fits.live.scalar[10]);
    // With nine user SGPRs, s9 holds the primitive mask and s10 isn't loaded, unless RSRC2 asks
    // for the collision wave ID after it.
    CHECK(sh::count_inputs(stages, image(9, 0), program).setup_outside);
    CHECK(!sh::count_inputs(stages, image(9, sh::kRsrc2PsLoadCollisionWaveId), program)
               .setup_outside);
    const sh::StageInputs& ps = stages[sh::kHardwarePs];
    CHECK_EQ(ps.programs, size_t{3});
    CHECK_EQ(ps.setup_checked, size_t{3});
    CHECK_EQ(ps.setup_vgprs_checked, size_t{3});
    CHECK_EQ(ps.setup_outside, size_t{1});
    CHECK_EQ(ps.rsrc2, size_t{3});
    CHECK_EQ(ps.rsrc2_user_differs, size_t{0});
    CHECK_EQ(ps.wave_sizes.at(32), size_t{3});
    CHECK_EQ(ps.fetch_differs, size_t{0});
}

TEST_CASE("a program's register counts are checked against the registers it uses and loads") {
    namespace r = sh::rdna2;
    // The pixel program above: s_mov_b32 m0, s10; v_interp_p1_f32 v2, v0, attr0.x;
    // v_interp_p2_f32 v2, v1, attr0.x; s_endpgm. It uses s0 to s10 and v0 to v2.
    const std::array<uint32_t, 4> words = {0xBEFC030A, 0xC8080000, 0xC8090001, 0xBF810000};
    Bytes code;
    for (const uint32_t word : words) {
        put32(code, word);
    }
    const r::Program program = r::decode_program(code);
    const r::RegisterExtent extent = r::register_extent(program, 64);
    CHECK_EQ(extent.sgprs, 11U);
    CHECK_EQ(extent.vgprs, 3U);

    // VGPRs in bits 0 to 9 of the word at 0x110, SGPRs in bits 20 to 31.
    const auto image = [](uint8_t hardware, uint8_t user_sgprs, uint32_t vgprs, uint32_t sgprs) {
        const std::array<std::pair<uint8_t, uint8_t>, 1> stage = {{{1, hardware}}};
        Bytes out = staged_fssm(stage, 0x2); // perspective center: I and J in v0 and v1
        set32(out, 0xD4, uint32_t{user_sgprs} << 16);
        set32(out, 0x110, vgprs | (sgprs << 20));
        return out;
    };
    CHECK((sh::fssm_register_counts(image(sh::kHardwarePs, 10, 3, 13)) ==
           sh::FssmRegisterCounts{.vgprs = 3, .sgprs = 13}));
    CHECK(!sh::fssm_register_counts(fssm_image(0, 0x400)).has_value());

    // The model: the code's registers or the loaded ones, whichever reach further, and two more
    // SGPRs. With ten user SGPRs and the primitive mask, the code's s10 and vcc's two give 13;
    // with 14 user SGPRs the 15 loaded do. A count above or below the model is tallied as such.
    std::map<uint8_t, sh::StageInputs> stages;
    sh::count_inputs(stages, image(sh::kHardwarePs, 10, 3, 13), program);
    sh::count_inputs(stages, image(sh::kHardwarePs, 14, 4, 15), program);
    sh::count_inputs(stages, image(sh::kHardwarePs, 14, 3, 13), program);
    const sh::StageInputs& ps = stages[sh::kHardwarePs];
    CHECK_EQ(ps.vgpr_counts.equal, size_t{2});
    CHECK_EQ(ps.vgpr_counts.above, size_t{1});
    CHECK_EQ(ps.vgpr_counts.beyond_code, size_t{0});
    CHECK_EQ(ps.sgpr_counts.equal, size_t{2});
    CHECK_EQ(ps.sgpr_counts.beyond_code, size_t{1});
    CHECK_EQ(ps.sgpr_counts.below, size_t{1});
    CHECK_EQ(ps.sgpr_count_below_loaded, size_t{1});

    // A GS counts only the SGPRs its code uses, though its 20 user SGPRs load from s8.
    sh::count_inputs(stages, image(sh::kHardwareGs, 20, 3, 13), program);
    const sh::StageInputs& gs = stages[sh::kHardwareGs];
    CHECK_EQ(gs.sgpr_counts.equal, size_t{1});
    CHECK_EQ(gs.sgpr_count_below_loaded, size_t{1});
    // A VS counts at least four VGPRs, though this one loads only v0 and uses three.
    sh::count_inputs(stages, image(sh::kHardwareVs, 10, 4, 13), program);
    const sh::StageInputs& vs = stages[sh::kHardwareVs];
    CHECK_EQ(vs.vgpr_counts.equal, size_t{1});
    CHECK_EQ(vs.vgpr_counts.beyond_code, size_t{1});
    CHECK_EQ(vs.sgpr_counts.equal, size_t{1});
}

TEST_CASE("pixel interpolation is checked against the enable word") {
    namespace r = sh::rdna2;
    // v_interp_p1_f32 v2, v0, attr0.x; v_interp_p2_f32 v2, v1, attr0.x; v_interp_p1_f32 v3, v4,
    // attr0.y; s_endpgm. VINTRP: 110010 at bits 26-31, vdst 18-25, op 16-17, attr 10-15,
    // chan 8-9, vsrc 0-7.
    const std::array<uint32_t, 4> words = {0xC8080000, 0xC8090001, 0xC80C0104, 0xBF810000};
    Bytes code;
    for (const uint32_t word : words) {
        put32(code, word);
    }
    const r::Program program = r::decode_program(code);
    const sh::InterpolationCheck center = sh::check_interpolation(program, sh::ps_inputs(0x2));
    CHECK_EQ(center.instructions, size_t{3});
    CHECK_EQ(center.outside, size_t{1}); // v4 isn't an I or a J with only perspective center
    const sh::InterpolationCheck wider = sh::check_interpolation(program, sh::ps_inputs(0x23));
    CHECK_EQ(wider.outside, size_t{0}); // sample and center: v0-v3; linear center: v4-v5
    CHECK_EQ(wider.vop3, size_t{0});

    // The VOP3 forms name the I or J in the src1 field: v_interp_p1ll_f16 v0, v1, attr0.x and
    // v_interp_p2_f16 v0, v6, attr0.x, v0. v_interp_mov_f32 v0, p10, attr0.x reads none.
    const std::array<uint32_t, 7> vop3_words = {0xD7420000, 0x00020200, 0xD75A0000, 0x04020C00,
                                                0xD6020000, 0x00000000, 0xBF810000};
    Bytes vop3_code;
    for (const uint32_t word : vop3_words) {
        put32(vop3_code, word);
    }
    const sh::InterpolationCheck vop3 =
        sh::check_interpolation(r::decode_program(vop3_code), sh::ps_inputs(0x2));
    CHECK_EQ(vop3.instructions, size_t{2});
    CHECK_EQ(vop3.vop3, size_t{2});
    CHECK_EQ(vop3.outside, size_t{1}); // v6
}

TEST_CASE("f16 interpolations are placed by the packing rule") {
    namespace r = sh::rdna2;
    constexpr uint32_t kFloat16 = 6; // D3D's component types
    constexpr uint32_t kFloat32 = 3;
    const auto element = [](uint32_t reg, uint32_t type, uint8_t mask, uint8_t rw,
                            uint32_t system_value = 0) {
        sh::SignatureElement e;
        e.system_value = system_value;
        e.component_type = type;
        e.reg = reg;
        e.mask = mask;
        e.rw_mask = rw;
        return e;
    };
    const auto record = [](uint8_t reg, uint8_t byte5, uint8_t flags) {
        sh::PsInputRecord rec;
        rec.reg = reg;
        rec.byte5 = byte5;
        rec.flags = flags;
        return rec;
    };
    // v_interp_p1ll_f16 v0, v0, attrN.c [high]: src0 holds the attribute in bits 0-5, the
    // channel in 6-7 and the high half in bit 8; src1 the I (v0).
    struct Read {
        unsigned attribute;
        unsigned channel;
        bool high;
    };
    const auto program = [](std::initializer_list<Read> reads) {
        Bytes code;
        for (const Read& read : reads) {
            put32(code, 0xD4000000U | (uint32_t{r::kVop3InterpP1llF16} << 16));
            put32(code,
                  read.attribute | (read.channel << 6) | (read.high ? 0x100U : 0U) | (0x100U << 9));
        }
        put32(code, 0xBF810000); // s_endpgm
        return r::decode_program(code);
    };

    // A named partner. Record 0 packs register 0 (xyz) in slots 0-2 and, from byte 5, register 1
    // (xy, x read) from slot 4. Record 1 is f16 but not packed; record 2 isn't f16.
    const std::vector<sh::SignatureElement> named_signature = {
        element(0, kFloat16, 0x7, 0x7), element(1, kFloat16, 0x3, 0x1),
        element(3, kFloat16, 0x3, 0x3), element(4, kFloat32, 0x1, 0x1)};
    const std::vector<sh::PsInputRecord> named_records = {record(0, 1, 0x1A), record(3, 0, 0x0A),
                                                          record(4, 0, 0x02)};
    sh::F16InputCheck named;
    sh::check_f16_inputs(named,
                         program({{0, 0, false},   // slot 0: register 0 x
                                  {0, 0, false},   // the same read again counts once
                                  {0, 1, false},   // slot 2: register 0 z
                                  {0, 2, false},   // slot 4: register 1 x
                                  {0, 2, true},    // slot 5: register 1 y, not read
                                  {0, 1, true},    // slot 3: between the two registers
                                  {1, 1, false},   // unpacked: register 3 y
                                  {1, 1, true},    // unpacked records have no high halves
                                  {2, 0, false},   // not f16
                                  {5, 0, false}}), // past the table
                         named_records, named_signature);
    CHECK_EQ(named.packed_records, size_t{1});
    CHECK_EQ(named.named_partners, size_t{1});
    CHECK_EQ(named.unnamed_partners, size_t{0});
    CHECK_EQ(named.reads, size_t{9});
    CHECK_EQ(named.on_read, size_t{4});
    CHECK_EQ(named.on_unread, size_t{1});
    CHECK_EQ(named.outside, size_t{2});
    CHECK_EQ(named.not_f16, size_t{2});

    // An unnamed partner: byte 5 repeats byte 1, so the partner is the one f16 register the
    // shader reads that no record names (2), placed after register 0's read elements (xy) rather
    // than all its declared ones. A system value is never the partner.
    std::vector<sh::SignatureElement> unnamed_signature = {
        element(0, kFloat16, 0x3, 0x3), element(0, kFloat16, 0xC, 0x0),
        element(2, kFloat16, 0x1, 0x1), element(3, kFloat16, 0xF, 0xF, 1)};
    const std::vector<sh::PsInputRecord> unnamed_records = {record(0, 0, 0x1A)};
    const r::Program unnamed_program = program({{0, 0, true},   // slot 1: register 0 y
                                                {0, 1, false},  // slot 2: register 2 x
                                                {0, 1, true}}); // slot 3: register 2 has no y
    sh::F16InputCheck unnamed;
    sh::check_f16_inputs(unnamed, unnamed_program, unnamed_records, unnamed_signature);
    CHECK_EQ(unnamed.packed_records, size_t{1});
    CHECK_EQ(unnamed.named_partners, size_t{0});
    CHECK_EQ(unnamed.unnamed_partners, size_t{1});
    CHECK_EQ(unnamed.on_read, size_t{2});
    CHECK_EQ(unnamed.outside, size_t{1});

    // With two candidates there is no partner, so register 0 keeps all four declared slots and
    // slots 2 and 3 land on its unread z and w.
    unnamed_signature.push_back(element(5, kFloat16, 0x1, 0x1));
    sh::F16InputCheck ambiguous;
    sh::check_f16_inputs(ambiguous, unnamed_program, unnamed_records, unnamed_signature);
    CHECK_EQ(ambiguous.unnamed_partners, size_t{0});
    CHECK_EQ(ambiguous.on_read, size_t{1});
    CHECK_EQ(ambiguous.on_unread, size_t{2});
    CHECK_EQ(ambiguous.outside, size_t{0});
}

TEST_CASE("malformed containers throw") {
    const TestArchive a = make_archive();
    CHECK_THROWS_AS(sh::parse_dxbc(std::span(a.vertex).first(20)), sh::FormatError);
    Bytes bad_part = a.vertex;
    set32(bad_part, 32, static_cast<uint32_t>(bad_part.size() - 4)); // first part near the end
    CHECK_THROWS_AS(sh::parse_dxbc(bad_part), sh::FormatError);
    Bytes bad_total = a.vertex;
    set32(bad_total, 24, static_cast<uint32_t>(bad_total.size() + 1));
    CHECK_THROWS_AS(sh::parse_dxbc(bad_total), sh::FormatError);

    // A native program whose stated size doesn't match what it inflates to.
    const sh::Dxbc dxbc = sh::parse_dxbc(a.vertex);
    const auto xdat = sh::read_xdat(dxbc.find(sh::fourcc("DXIL"))->data);
    REQUIRE(xdat.has_value());
    Bytes xbch(xdat->entries[1].data.begin(), xdat->entries[1].data.end());
    set32(xbch, 16, 301);
    CHECK_THROWS_AS(sh::unpack_native_program(xbch), sh::FormatError);
}

TEST_CASE("LLVM bitcode is told apart from the NOBC marker") {
    Bytes part;
    program_header(part, 5, 40, 24, 8);
    part.insert(part.end(), {'B', 'C', 0xC0, 0xDE, 0x35, 0x14, 0x00, 0x00});
    const sh::DxilProgram program = sh::parse_dxil_program(part);
    CHECK_EQ(sh::kind_name(program.kind), std::string("compute"));
    CHECK(program.has_llvm_bitcode());
    CHECK(!program.no_bitcode_marker());
    CHECK(!sh::read_xdat(part).has_value());
}
