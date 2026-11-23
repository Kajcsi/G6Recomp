// PE image parsing on synthetic images: header fields, section mapping, and rejection of
// malformed input.

#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6test.h"
#include "pe_builder.h"

using g6::pe::Directory;
using g6::pe::FormatError;
using g6::pe::Image;
using g6::test::PeBuilder;

namespace {

PeBuilder two_section_image() {
    PeBuilder builder;
    const uint32_t text = builder.add_section(".text", PeBuilder::kCode, 0x30, 0xCC);
    const uint32_t rdata = builder.add_section(".rdata", PeBuilder::kRdata, 0x2400);
    builder.put_bytes(text, {0x31, 0xC0, 0xC3});
    builder.put_string(rdata + 0x10, "hello");
    builder.set_entry_point(text);
    builder.set_directory(Directory::Exception, rdata + 0x100, 0x18);
    return builder;
}

} // namespace

TEST_CASE("image: header fields and section mapping") {
    const Image image = two_section_image().image();
    CHECK_EQ(image.machine(), g6::pe::kMachineAmd64);
    CHECK(image.is_dll());
    CHECK_EQ(image.image_base(), PeBuilder::kImageBase);
    CHECK_EQ(image.entry_point(), 0x1000u);
    CHECK_EQ(image.size_of_image(), 0x5000u); // .rdata at 0x2000 spans 0x2400 bytes
    REQUIRE(image.sections().size() == 2);
    CHECK_EQ(image.sections()[0].name, std::string(".text"));
    CHECK(image.sections()[0].executable());
    CHECK(!image.sections()[1].executable());
    CHECK(image.is_executable(0x1002));
    CHECK(!image.is_executable(0x2000));
    CHECK(!image.is_executable(0x1030)); // past the section's virtual size
    CHECK_EQ(image.read<uint8_t>(0x1002), uint8_t{0xC3});
    CHECK_EQ(image.read<uint8_t>(0x1003), uint8_t{0xCC});
    CHECK_EQ(image.read_cstring(0x2010), std::string("hello"));
    CHECK_EQ(image.directory(Directory::Exception).rva, 0x2100u);
    CHECK(!image.directory(Directory::Import).present());
}

TEST_CASE("image: address conversion and bounds") {
    const Image image = two_section_image().image();
    CHECK_EQ(image.va_to_rva(PeBuilder::kImageBase + 0x1234), 0x1234u);
    CHECK(!image.try_va_to_rva(PeBuilder::kImageBase - 1).has_value());
    CHECK(!image.try_va_to_rva(PeBuilder::kImageBase + image.size_of_image()).has_value());
    CHECK_THROWS_AS(image.va_to_rva(0x1000), FormatError);
    CHECK(image.contains(image.size_of_image() - 4, 4));
    CHECK(!image.contains(image.size_of_image() - 3, 4));
    CHECK_THROWS_AS(image.read<uint32_t>(image.size_of_image() - 2), FormatError);
    CHECK_THROWS_AS(image.bytes(0xFFFFFFF0u, 0x20), FormatError);
}

TEST_CASE("image: unterminated string is rejected") {
    PeBuilder builder;
    const uint32_t data = builder.add_section(".rdata", PeBuilder::kRdata, 0x1000, 'A');
    const Image image = builder.image();
    CHECK_THROWS_AS(image.read_cstring(data), FormatError);
    CHECK_THROWS_AS(image.read_cstring(data, 16), FormatError);
}

TEST_CASE("image: rejects a missing MZ header") {
    std::vector<uint8_t> file = two_section_image().build();
    file[0] = 'X';
    CHECK_THROWS_AS(Image::parse(file), FormatError);
}

TEST_CASE("image: rejects a bad PE signature") {
    std::vector<uint8_t> file = two_section_image().build();
    file[PeBuilder::kNtHeaderOffset + 1] = 'X';
    CHECK_THROWS_AS(Image::parse(file), FormatError);
}

TEST_CASE("image: rejects PE32 (32-bit) images") {
    std::vector<uint8_t> file = two_section_image().build();
    file[PeBuilder::kOptionalHeaderOffset] = 0x0B;
    file[PeBuilder::kOptionalHeaderOffset + 1] = 0x01;
    CHECK_THROWS_AS(Image::parse(file), FormatError);
}

TEST_CASE("image: rejects a section past SizeOfImage") {
    std::vector<uint8_t> file = two_section_image().build();
    const uint32_t too_small = 0x3000;
    std::memcpy(file.data() + PeBuilder::kSizeOfImageOffset, &too_small, 4);
    CHECK_THROWS_AS(Image::parse(file), FormatError);
}

TEST_CASE("image: rejects a truncated header and a zero SizeOfImage") {
    std::vector<uint8_t> file = two_section_image().build();
    std::vector<uint8_t> truncated(file.begin(), file.begin() + 0x90);
    CHECK_THROWS_AS(Image::parse(truncated), FormatError);
    const uint32_t zero = 0;
    std::memcpy(file.data() + PeBuilder::kSizeOfImageOffset, &zero, 4);
    CHECK_THROWS_AS(Image::parse(file), FormatError);
}

TEST_CASE("image: raw data cut off by the end of the file is truncated, not rejected") {
    std::vector<uint8_t> file = two_section_image().build();
    // Drop the tail of the last section's raw data; the loader maps what is there.
    file.resize(file.size() - 0x300);
    const Image image = Image::parse(file);
    CHECK_EQ(image.read_cstring(0x2010), std::string("hello"));
    CHECK_EQ(image.read<uint8_t>(0x43FF), uint8_t{0});
}

TEST_CASE("image: section raw offset past the end of the file is rejected") {
    std::vector<uint8_t> file = two_section_image().build();
    const uint32_t far_away = 0x100000;
    std::memcpy(file.data() + PeBuilder::kSectionTableOffset + 40 + 20, &far_away, 4);
    CHECK_THROWS_AS(Image::parse(file), FormatError);
}

TEST_CASE("relocations: DIR64 entries, padding skipped") {
    PeBuilder builder;
    const uint32_t data = builder.add_section(".rdata", PeBuilder::kRdata, 0x100);
    // One block for page `data`: two DIR64 entries and an ABSOLUTE padding entry.
    builder.put<uint32_t>(data + 0x40, data);
    builder.put<uint32_t>(data + 0x44, 8 + 3 * 2 + 2);
    builder.put<uint16_t>(data + 0x48, (g6::pe::kRelocDir64 << 12) | 0x10);
    builder.put<uint16_t>(data + 0x4A, (g6::pe::kRelocDir64 << 12) | 0x18);
    builder.put<uint16_t>(data + 0x4C, 0);
    builder.set_directory(Directory::BaseReloc, data + 0x40, 16);
    const auto relocations = g6::pe::read_relocations(builder.image());
    REQUIRE(relocations.size() == 2);
    CHECK_EQ(relocations[0].rva, data + 0x10);
    CHECK_EQ(relocations[1].rva, data + 0x18);
    CHECK_EQ(relocations[1].type, g6::pe::kRelocDir64);
}

TEST_CASE("exports: named, ordinal-only and forwarded") {
    PeBuilder builder;
    const uint32_t text = builder.add_section(".text", PeBuilder::kCode, 0x100, 0xCC);
    const uint32_t ed = builder.add_section(".edata", PeBuilder::kRdata, 0x200);
    // Directory at ed, three function slots, two names.
    builder.put<uint32_t>(ed + 12, ed + 0x100);    // dll name
    builder.put<uint32_t>(ed + 16, 5);             // ordinal base
    builder.put<uint32_t>(ed + 20, 3);             // functions
    builder.put<uint32_t>(ed + 24, 2);             // names
    builder.put<uint32_t>(ed + 28, ed + 0x40);     // function RVAs
    builder.put<uint32_t>(ed + 32, ed + 0x60);     // name RVAs
    builder.put<uint32_t>(ed + 36, ed + 0x80);     // name ordinals
    builder.put<uint32_t>(ed + 0x40, text + 0x10); // ordinal 5: named
    builder.put<uint32_t>(ed + 0x44, text + 0x20); // ordinal 6: ordinal only
    builder.put<uint32_t>(ed + 0x48, ed + 0x140);  // ordinal 7: forwarder string
    builder.put<uint32_t>(ed + 0x60, ed + 0x110);
    builder.put<uint32_t>(ed + 0x64, ed + 0x120);
    builder.put<uint16_t>(ed + 0x80, 0);
    builder.put<uint16_t>(ed + 0x82, 2);
    builder.put_string(ed + 0x100, "sample.dll");
    builder.put_string(ed + 0x110, "first");
    builder.put_string(ed + 0x120, "forwarded");
    builder.put_string(ed + 0x140, "other.target");
    builder.set_directory(Directory::Export, ed, 0x180);

    const g6::pe::ExportTable table = g6::pe::read_exports(builder.image());
    CHECK_EQ(table.dll_name, std::string("sample.dll"));
    REQUIRE(table.exports.size() == 3);
    CHECK_EQ(table.exports[0].ordinal, 5u);
    CHECK_EQ(table.exports[0].name, std::string("first"));
    CHECK_EQ(table.exports[0].rva, text + 0x10);
    CHECK(table.exports[1].name.empty());
    CHECK_EQ(table.exports[1].rva, text + 0x20);
    CHECK_EQ(table.exports[2].name, std::string("forwarded"));
    CHECK_EQ(table.exports[2].forwarder, std::string("other.target"));
}
