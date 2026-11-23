// The pre-flight scan (docs/scan.md) on small synthetic images: the header facts and the
// review notes that a compiled fixture can't easily produce. tests/fixture/test_scan_guest.cpp
// covers the instruction findings on a real DLL.

#include <algorithm>
#include <string>
#include <vector>

#include "g6test.h"
#include "pe_builder.h"
#include "scan.h"

namespace pe = g6::pe;
namespace recomp = g6::recomp;
using g6::test::PeBuilder;

namespace {

bool has_note(const std::vector<std::string>& notes, const std::string& text) {
    return std::ranges::any_of(
        notes, [&](const std::string& note) { return note.find(text) != std::string::npos; });
}

// An image whose entry point is `code`, with an optional relocation block.
PeBuilder image_with(std::initializer_list<uint8_t> code, bool dll) {
    PeBuilder b;
    const uint32_t text = b.add_section(".text", PeBuilder::kCode, 0x100, 0xCC);
    b.put_bytes(text, code);
    b.set_entry_point(text);
    b.set_dll(dll);
    return b;
}

} // namespace

TEST_CASE("scan notes a module without relocations") {
    // mov rax, gs:[0x60]; ret
    const PeBuilder b =
        image_with({0x65, 0x48, 0x8B, 0x04, 0x25, 0x60, 0x00, 0x00, 0x00, 0xC3}, false);
    const recomp::ScanReport report = recomp::scan_module(b.image());
    CHECK(!report.dll);
    CHECK(!report.relocations.present);
    CHECK_EQ(report.image_base, PeBuilder::kImageBase);
    CHECK(!report.tls.has_value());
    CHECK(!report.cfg.has_value());
    REQUIRE(report.segments.size() == 1);
    CHECK(!report.segments[0].fs);
    CHECK_EQ(report.segments[0].offset, int64_t{0x60});
    CHECK_EQ(report.segments[0].reads, size_t{1});
    CHECK_EQ(report.segments[0].writes, size_t{0});

    const std::vector<std::string> notes = recomp::review_notes(report);
    CHECK(has_note(notes, "no base relocations"));
    CHECK(has_note(notes, "reads the PEB"));
}

TEST_CASE("scan counts relocations that aren't 64-bit") {
    PeBuilder b = image_with({0xC3}, true);
    const uint32_t reloc = b.add_section(".reloc", PeBuilder::kRdata, 0x10);
    b.put<uint32_t>(reloc, 0x1000);                 // page
    b.put<uint32_t>(reloc + 4, 12);                 // block size
    b.put<uint16_t>(reloc + 8, (3 << 12) | 0x10);   // HIGHLOW
    b.put<uint16_t>(reloc + 10, (10 << 12) | 0x20); // DIR64
    b.set_directory(pe::Directory::BaseReloc, reloc, 12);
    const recomp::ScanReport report = recomp::scan_module(b.image());
    CHECK(report.relocations.present);
    CHECK_EQ(report.relocations.dir64, size_t{1});
    CHECK_EQ(report.relocations.other, size_t{1});
    const std::vector<std::string> notes = recomp::review_notes(report);
    CHECK(!has_note(notes, "no base relocations"));
    CHECK(has_note(notes, "aren't 64-bit"));
}

TEST_CASE("scan leaves a resolved table's relocated entries out of the taken addresses") {
    PeBuilder b = image_with({0x83, 0xF8, 0x01,                         // cmp eax, 1
                              0x0F, 0x87, 0x37, 0x00, 0x00, 0x00,       // ja 0x1040 (default)
                              0x48, 0x8D, 0x15, 0xF0, 0x0F, 0x00, 0x00, // lea rdx, [0x2000]
                              0xFF, 0x24, 0xC2},                        // jmp [rdx+rax*8]
                             true);
    b.put_bytes(0x1020, {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}); // mov eax, 1; ret
    b.put_bytes(0x1030, {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3}); // mov eax, 2; ret
    b.put_bytes(0x1040, {0x31, 0xC0, 0xC3});                   // xor eax, eax; ret
    const uint32_t rdata = b.add_section(".rdata", PeBuilder::kRdata, 0x20);
    REQUIRE(rdata == 0x2000);
    b.put<uint64_t>(rdata, PeBuilder::kImageBase + 0x1020); // the table
    b.put<uint64_t>(rdata + 8, PeBuilder::kImageBase + 0x1030);
    b.put<uint64_t>(rdata + 16, PeBuilder::kImageBase + 0x1030); // a pointer of its own
    const uint32_t reloc = b.add_section(".reloc", PeBuilder::kRdata, 0x10);
    b.put<uint32_t>(reloc, rdata);                 // page
    b.put<uint32_t>(reloc + 4, 16);                // block size
    b.put<uint16_t>(reloc + 8, (10 << 12) | 0x00); // DIR64 at each 8-byte slot
    b.put<uint16_t>(reloc + 10, (10 << 12) | 0x08);
    b.put<uint16_t>(reloc + 12, (10 << 12) | 0x10);
    b.set_directory(pe::Directory::BaseReloc, reloc, 16);

    const recomp::ScanReport report = recomp::scan_module(b.image());
    REQUIRE(report.code_addresses.size() == 1);
    CHECK_EQ(report.code_addresses[0].site, rdata + 16);
    CHECK_EQ(report.code_addresses[0].target, 0x1030u);
    CHECK(report.code_addresses[0].pointer);
}

TEST_CASE("scan doesn't follow the TLS array past a branch") {
    // mov rax, gs:[0x58]; ret
    const PeBuilder b =
        image_with({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0xC3}, true);
    const recomp::ScanReport report = recomp::scan_module(b.image());
    CHECK_EQ(report.tls_slots.not_followed.walked.size(), size_t{1});
    CHECK(report.tls_slots.fixed_slot.empty());
    CHECK(has_note(recomp::review_notes(report), "weren't followed"));
}

TEST_CASE("scan describes sites and segment accesses") {
    recomp::Sites sites;
    CHECK_EQ(recomp::describe_sites(sites), std::string("none"));
    sites.walked = {0x1000, 0x1010};
    sites.swept = {0x2000, 0x2010};
    CHECK_EQ(recomp::describe_sites(sites, 3),
             std::string("0x1000, 0x1010, 0x2000 (swept), +1 more"));
    CHECK_EQ(recomp::describe_sites(sites, 10),
             std::string("0x1000, 0x1010, 0x2000 (swept), 0x2010 (swept)"));

    recomp::SegmentAccess access;
    access.offset = 0x58;
    CHECK_EQ(recomp::describe(access), std::string("gs:[0x58]"));
    access.computed = true;
    access.offset = 0x1480;
    CHECK_EQ(recomp::describe(access), std::string("gs:[reg+0x1480]"));
    access.computed = false;
    access.via_self = true;
    access.offset = 0x68;
    CHECK_EQ(recomp::describe(access), std::string("[gs:[0x30]+0x68]"));
    access.via_self = false;
    access.fs = true;
    access.offset = 0x30;
    CHECK_EQ(recomp::describe(access), std::string("fs:[0x30]"));
}
