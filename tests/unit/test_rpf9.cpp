// RPF9 archives (common/include/g6/rpf9.h) on an archive built here from made-up files: a header,
// a table of contents of filler bytes standing in for an encrypted one, and a data area of stored
// (uncompressed) zlib streams with gaps between some of them.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "g6/inflate.h"
#include "g6/rpf9.h"
#include "g6test.h"

namespace rpf9 = g6::rpf9;

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes& out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}
void put(Bytes& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}
void put(Bytes& out, std::span<const uint8_t> data) {
    out.insert(out.end(), data.begin(), data.end());
}
void pad16(Bytes& out) {
    out.resize((out.size() + 15) & ~size_t{15}, 0);
}
Bytes text_bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

// A zlib stream of DEFLATE stored blocks (RFC 1951 3.2.4) holding `data`.
Bytes stored_zlib(std::span<const uint8_t> data) {
    Bytes out = {0x78, 0x01};
    size_t at = 0;
    do {
        const size_t length = std::min<size_t>(data.size() - at, 0xFFFF);
        out.push_back(at + length == data.size() ? 1 : 0);
        out.push_back(static_cast<uint8_t>(length));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.push_back(static_cast<uint8_t>(~length));
        out.push_back(static_cast<uint8_t>(~length >> 8));
        put(out, data.subspan(at, length));
        at += length;
    } while (at < data.size());
    const uint32_t sum = g6::adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(sum >> shift));
    }
    return out;
}

Bytes header(uint32_t toc_size) {
    Bytes out(rpf9::kHeaderSize, 0);
    out[0] = '9';
    out[1] = 'F';
    out[2] = 'P';
    out[3] = 'R';
    put32(out, 0x04, toc_size);
    put32(out, 0x0C, 0x60);
    for (uint8_t i = 0; i < 8; ++i) {
        out[0x10 + i] = static_cast<uint8_t>(i + 1);
    }
    put32(out, 0x24, 5);
    put32(out, 0x28, 0x0FFFFFF9);
    put32(out, 0x2C, 0x80000000);
    return out;
}

// Five files and two gaps, laid out the way the game's archives are.
struct Sample {
    Bytes archive;
    Bytes xml;
    Bytes resource; // 64 KiB and 100 bytes, so two streams
    Bytes dds;      // exactly 64 KiB, one stream
    Bytes cache;
    Bytes text;
    uint64_t xml_at = 0;
    uint64_t resource_at = 0;
    uint64_t resource_tail_at = 0;
    uint64_t stored_at = 0;
    uint64_t dds_at = 0;
    uint64_t zeros_at = 0;
    uint64_t cache_at = 0;
    uint64_t text_at = 0;
};

Sample sample() {
    Sample s;
    s.xml = text_bytes("<?xml version=\"1.0\"?>\n<settings />\n");
    s.resource.resize(rpf9::kChunkSize + 100);
    for (size_t i = 0; i < s.resource.size(); ++i) {
        s.resource[i] = static_cast<uint8_t>(i * 7);
    }
    std::copy_n("0DRP", 4, s.resource.begin() + 0x10);
    put32(s.resource, 0x14, 0x2B);
    s.dds.assign(rpf9::kChunkSize, 0x11);
    std::copy_n("DDS ", 4, s.dds.begin());
    s.cache = {'P', 'S', 0, '\t', 1, 2, 3, 4};
    s.text = text_bytes("a made-up line of text that is long enough to count as text\n");

    // A 40-byte table of contents puts the data area at 0x70.
    s.archive = header(40);
    s.archive.resize(rpf9::kHeaderSize + 40, 0xEE);
    pad16(s.archive);

    s.xml_at = s.archive.size();
    put(s.archive, stored_zlib(s.xml));
    pad16(s.archive);

    s.resource_at = s.archive.size();
    put(s.archive, stored_zlib(std::span(s.resource).first(rpf9::kChunkSize)));
    pad16(s.archive);
    s.resource_tail_at = s.archive.size();
    put(s.archive, stored_zlib(std::span(s.resource).subspan(rpf9::kChunkSize)));
    pad16(s.archive);

    // Stored data: a made-up magic, then bytes that start like a zlib stream but aren't one (the
    // first block has the reserved type 3).
    s.stored_at = s.archive.size();
    put(s.archive, "KB2a");
    s.archive.resize(s.archive.size() + 12, 0);
    s.archive.push_back(0x78);
    s.archive.push_back(0xDA);
    s.archive.resize(s.archive.size() + 14, 0xFF);

    s.dds_at = s.archive.size();
    put(s.archive, stored_zlib(s.dds));
    pad16(s.archive);
    s.zeros_at = s.archive.size();
    s.archive.resize(s.archive.size() + 16, 0);

    s.cache_at = s.archive.size();
    put(s.archive, stored_zlib(s.cache));
    pad16(s.archive);

    // The last file ends the archive without padding.
    s.text_at = s.archive.size();
    put(s.archive, stored_zlib(s.text));
    return s;
}

} // namespace

TEST_CASE("rpf9 reads the header") {
    const Sample s = sample();
    const std::optional<rpf9::Header> header = rpf9::read_header(s.archive);
    REQUIRE(header.has_value());
    CHECK_EQ(header->toc_size, uint32_t{40});
    CHECK_EQ(header->names_size, uint32_t{0x60});
    CHECK_EQ(header->toc_hash, uint64_t{0x0102030405060708});
    CHECK_EQ(header->entry_count, uint32_t{5});
    CHECK_EQ(header->key_id, uint32_t{0x0FFFFFF9});
    CHECK_EQ(header->flags, uint32_t{0x80000000});
    CHECK_EQ(rpf9::data_start(*header), uint64_t{0x70});

    // Exactly on a 16-byte boundary already.
    rpf9::Header aligned;
    aligned.toc_size = 0x40;
    CHECK_EQ(rpf9::data_start(aligned), uint64_t{0x80});
}

TEST_CASE("rpf9 rejects other files") {
    Bytes other = header(0);
    other[0] = 'R';
    CHECK(!rpf9::read_header(other).has_value());
    const Bytes complete = header(0);
    CHECK(!rpf9::read_header(std::span(complete).first(rpf9::kHeaderSize - 1)).has_value());
    CHECK_THROWS_AS(rpf9::carve(other), rpf9::FormatError);
    // A table of contents that runs past the end of the file.
    CHECK_THROWS_AS(rpf9::carve(header(0x100)), rpf9::FormatError);
}

TEST_CASE("rpf9 carves the data area into files and gaps") {
    const Sample s = sample();
    for (const unsigned threads : {1u, 4u}) {
        const rpf9::Carve carve = rpf9::carve(s.archive, threads);
        CHECK_EQ(carve.data_start, uint64_t{0x70});
        CHECK_EQ(carve.header.entry_count, uint32_t{5});

        REQUIRE(carve.streams.size() == 6);
        CHECK_EQ(carve.streams[1].offset, s.resource_at);
        CHECK_EQ(carve.streams[1].size, rpf9::kChunkSize);
        CHECK_EQ(carve.streams[2].offset, s.resource_tail_at);
        CHECK_EQ(carve.streams[2].size, uint32_t{100});
        CHECK_EQ(carve.streams[2].compressed, uint32_t{2 + 5 + 100 + 4});

        REQUIRE(carve.entries.size() == 5);
        const rpf9::Entry& xml = carve.entries[0];
        CHECK_EQ(xml.offset, s.xml_at);
        CHECK_EQ(xml.size, uint64_t{s.xml.size()});
        CHECK(xml.head.kind == rpf9::Kind::kXml);

        // The two streams of a file larger than 64 KiB make one entry.
        const rpf9::Entry& resource = carve.entries[1];
        CHECK_EQ(resource.offset, s.resource_at);
        CHECK_EQ(resource.first_stream, size_t{1});
        CHECK_EQ(resource.streams, size_t{2});
        CHECK_EQ(resource.size, uint64_t{s.resource.size()});
        CHECK_EQ(resource.compressed,
                 uint64_t{carve.streams[1].compressed} + carve.streams[2].compressed);
        CHECK(resource.head.kind == rpf9::Kind::kResource);
        CHECK_EQ(resource.head.tag, uint32_t{0x2B});

        // A 64 KiB file followed by a gap stays one entry of its own.
        const rpf9::Entry& dds = carve.entries[2];
        CHECK_EQ(dds.offset, s.dds_at);
        CHECK_EQ(dds.streams, size_t{1});
        CHECK_EQ(dds.size, uint64_t{rpf9::kChunkSize});
        CHECK(dds.head.kind == rpf9::Kind::kDds);

        CHECK_EQ(carve.entries[3].offset, s.cache_at);
        CHECK(carve.entries[3].head.kind == rpf9::Kind::kPipelineCache);
        CHECK_EQ(carve.entries[4].offset, s.text_at);
        CHECK(carve.entries[4].head.kind == rpf9::Kind::kText);

        // The bytes that only look like a zlib stream stay part of the gap they're in.
        REQUIRE(carve.gaps.size() == 2);
        CHECK_EQ(carve.gaps[0].offset, s.stored_at);
        CHECK_EQ(carve.gaps[0].size, uint64_t{32});
        CHECK_EQ(carve.gaps[1].offset, s.zeros_at);
        CHECK_EQ(carve.gaps[1].size, uint64_t{16});

        CHECK(rpf9::read_entry(s.archive, carve, resource) == s.resource);
        CHECK(rpf9::read_entry(s.archive, carve, carve.entries[4]) == s.text);
    }
}

TEST_CASE("rpf9 reports stored data after the last stream as a gap") {
    Bytes archive = header(0);
    put(archive, stored_zlib(text_bytes("<root/>")));
    pad16(archive);
    const uint64_t tail = archive.size();
    put(archive, "AWC tail of stored data");

    const rpf9::Carve carve = rpf9::carve(archive);
    CHECK_EQ(carve.entries.size(), size_t{1});
    REQUIRE(carve.gaps.size() == 1);
    CHECK_EQ(carve.gaps[0].offset, tail);
    CHECK_EQ(carve.gaps[0].size, uint64_t{archive.size() - tail});

    // An archive with an empty data area.
    const rpf9::Carve empty = rpf9::carve(header(0));
    CHECK(empty.streams.empty());
    CHECK(empty.gaps.empty());
}

TEST_CASE("rpf9 classifies files by their first bytes") {
    const auto kind = [](std::string_view head) {
        return rpf9::classify({reinterpret_cast<const uint8_t*>(head.data()), head.size()}).kind;
    };
    CHECK(kind("\xEF\xBB\xBF<?xml version=\"1.0\"?>") == rpf9::Kind::kXml);
    CHECK(kind("<settings>") == rpf9::Kind::kXml);
    CHECK(kind("PSIN\x01\x00\x00\x00") == rpf9::Kind::kSchema);
    CHECK(kind("RBF0\x00") == rpf9::Kind::kRbf);
    CHECK(kind("GFX\x0F") == rpf9::Kind::kGfx);
    CHECK(kind("CFX\x0F") == rpf9::Kind::kGfx);
    CHECK(kind("\x89PNG\r\n") == rpf9::Kind::kPng);
    CHECK(kind("a < b, so this line of plain text is still text and not xml") == rpf9::Kind::kText);
    // Too short to call text.
    CHECK(kind("short") == rpf9::Kind::kOther);

    // A paged object: a pointer into the system page at 0x08.
    Bytes object(32, 0);
    put32(object, 8, 0x50001230);
    CHECK(rpf9::classify(object).kind == rpf9::Kind::kObject);

    Bytes other(32, 0x80);
    put32(other, 0, 0xCAFEF00D);
    const rpf9::Classified classified = rpf9::classify(other);
    CHECK(classified.kind == rpf9::Kind::kOther);
    CHECK_EQ(classified.tag, uint32_t{0xCAFEF00D});

    CHECK_EQ(rpf9::kind_name(rpf9::Kind::kPipelineCache), std::string_view("pipeline-cache"));
}
