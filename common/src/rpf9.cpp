// RPF9 archives (see rpf9.h). Written from the layout RageForge reports and checked against the
// user's archives (docs/rpf.md); no code is taken from it.

#include "g6/rpf9.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "g6/inflate.h"

namespace g6::rpf9 {
namespace {

constexpr uint8_t kMagic[4] = {'9', 'F', 'P', 'R'};
// Each thread takes the data area this many bytes at a time.
constexpr uint64_t kSegment = 64ull << 20;
// No stream is expected to inflate to more than this; a larger one is reported as a gap.
constexpr size_t kMaxStream = 256u << 20;
constexpr size_t kTextProbe = 128;
constexpr size_t kMinText = 32;

uint64_t align_up(uint64_t value) {
    return (value + kAlignment - 1) & ~(kAlignment - 1);
}

template <typename T>
T read_le(std::span<const uint8_t> data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

bool starts_with(std::span<const uint8_t> data, std::string_view prefix, size_t at = 0) {
    return data.size() >= at + prefix.size() &&
           std::memcmp(data.data() + at, prefix.data(), prefix.size()) == 0;
}

// CMF 0x78 (DEFLATE, 32 KiB window) with one of the four FLG values that pass the header check.
bool zlib_header(uint8_t cmf, uint8_t flg) {
    return cmf == 0x78 && (flg == 0x01 || flg == 0x5E || flg == 0x9C || flg == 0xDA);
}

bool alpha(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

std::vector<Stream> scan_range(std::span<const uint8_t> archive, uint64_t begin, uint64_t end) {
    std::vector<Stream> found;
    uint64_t at = begin;
    while (at < end && at + 2 <= archive.size()) {
        if (zlib_header(archive[at], archive[at + 1])) {
            try {
                const Inflated out = inflate_zlib(archive.subspan(at), kMaxStream);
                if (!out.data.empty()) {
                    found.push_back({at, static_cast<uint32_t>(out.consumed),
                                     static_cast<uint32_t>(out.data.size()), classify(out.data)});
                    at = align_up(at + out.consumed);
                    continue;
                }
            } catch (const InflateError&) { // NOLINT(bugprone-empty-catch): not a stream after all
            }
        }
        at += kAlignment;
    }
    return found;
}

} // namespace

std::optional<Header> read_header(std::span<const uint8_t> archive) {
    if (archive.size() < kHeaderSize || std::memcmp(archive.data(), kMagic, sizeof(kMagic)) != 0) {
        return std::nullopt;
    }
    Header header;
    header.toc_size = read_le<uint32_t>(archive, 0x04);
    header.word_08 = read_le<uint32_t>(archive, 0x08);
    header.names_size = read_le<uint32_t>(archive, 0x0C);
    for (size_t i = 0; i < sizeof(header.toc_hash); ++i) {
        header.toc_hash = (header.toc_hash << 8) | archive[0x10 + i];
    }
    header.word_18 = read_le<uint32_t>(archive, 0x18);
    header.word_1c = read_le<uint32_t>(archive, 0x1C);
    header.word_20 = read_le<uint16_t>(archive, 0x20);
    header.word_22 = read_le<uint16_t>(archive, 0x22);
    header.entry_count = read_le<uint32_t>(archive, 0x24);
    header.key_id = read_le<uint32_t>(archive, 0x28);
    header.flags = read_le<uint32_t>(archive, 0x2C);
    header.word_30 = read_le<uint32_t>(archive, 0x30);
    return header;
}

uint64_t data_start(const Header& header) {
    return align_up(kHeaderSize + uint64_t{header.toc_size});
}

std::string_view kind_name(Kind kind) {
    switch (kind) {
    case Kind::kResource: return "resource";
    case Kind::kObject: return "object";
    case Kind::kPipelineCache: return "pipeline-cache";
    case Kind::kSchema: return "schema";
    case Kind::kRbf: return "rbf";
    case Kind::kXml: return "xml";
    case Kind::kGfx: return "gfx";
    case Kind::kDds: return "dds";
    case Kind::kPng: return "png";
    case Kind::kText: return "text";
    case Kind::kOther: return "other";
    }
    return "?";
}

Classified classify(std::span<const uint8_t> head) {
    const size_t bom = starts_with(head, "\xEF\xBB\xBF") ? 3 : 0;
    if (starts_with(head, "<?xml", bom) ||
        (head.size() > bom + 1 && head[bom] == '<' && alpha(head[bom + 1]))) {
        return {Kind::kXml, 0};
    }
    if (starts_with(head, std::string_view("PS\0\t", 4))) {
        return {Kind::kPipelineCache, 0};
    }
    if (starts_with(head, "PSIN")) {
        return {Kind::kSchema, 0};
    }
    if (starts_with(head, "RBF0")) {
        return {Kind::kRbf, 0};
    }
    if (starts_with(head, "GFX") || starts_with(head, "CFX")) {
        return {Kind::kGfx, 0};
    }
    if (starts_with(head, "DDS ")) {
        return {Kind::kDds, 0};
    }
    if (starts_with(head, "\x89PNG")) {
        return {Kind::kPng, 0};
    }
    if (starts_with(head, "0DRP", 0x10) && head.size() >= 0x18) {
        return {Kind::kResource, read_le<uint32_t>(head, 0x14)};
    }
    // Pointers into a resource's system page are 0x5xxxxxxx.
    if (head.size() >= 16 && (read_le<uint32_t>(head, 8) & 0xF0000000u) == 0x50000000u &&
        read_le<uint32_t>(head, 12) == 0) {
        return {Kind::kObject, 0};
    }
    const size_t probe = std::min(head.size(), kTextProbe);
    const auto printable =
        std::count_if(head.begin(), head.begin() + static_cast<ptrdiff_t>(probe), [](uint8_t c) {
            return c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F);
        });
    if (head.size() >= kMinText && static_cast<size_t>(printable) * 100 > probe * 95) {
        return {Kind::kText, 0};
    }
    return {Kind::kOther, read_le<uint32_t>(head, 0)};
}

Carve carve(std::span<const uint8_t> archive, unsigned threads) {
    const std::optional<Header> header = read_header(archive);
    if (!header) {
        throw FormatError("not an RPF9 archive");
    }
    Carve result;
    result.header = *header;
    result.data_start = data_start(*header);
    if (result.data_start > archive.size()) {
        throw FormatError("the table of contents runs past the end of the file");
    }

    // Each thread takes the next segment; a stream may run past its segment's end, and the next
    // segment's scan then starts inside it, which the merge below sorts out.
    const uint64_t span = archive.size() - result.data_start;
    const auto segments = static_cast<size_t>((span + kSegment - 1) / kSegment);
    std::vector<std::vector<Stream>> found(segments);
    std::atomic<size_t> next{0};
    const auto work = [&] {
        for (size_t i = next++; i < segments; i = next++) {
            const uint64_t begin = result.data_start + i * kSegment;
            found[i] =
                scan_range(archive, begin, std::min<uint64_t>(begin + kSegment, archive.size()));
        }
    };
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }
    threads = static_cast<unsigned>(std::min<size_t>(threads, std::max<size_t>(segments, 1)));
    std::vector<std::jthread> pool;
    for (unsigned i = 1; i < threads; ++i) {
        pool.emplace_back(work);
    }
    work();
    pool.clear(); // joins

    // Segments are in order and each is sorted, so this keeps the streams in offset order; a
    // stream that starts inside the one before is dropped.
    uint64_t covered = result.data_start;
    for (const std::vector<Stream>& segment : found) {
        for (const Stream& stream : segment) {
            if (stream.offset < covered) {
                continue;
            }
            if (stream.offset > covered) {
                result.gaps.push_back({covered, stream.offset - covered});
            }
            const bool continues = !result.streams.empty() && stream.offset == covered &&
                                   result.streams.back().size == kChunkSize;
            if (continues) {
                Entry& entry = result.entries.back();
                entry.compressed += stream.compressed;
                entry.size += stream.size;
                ++entry.streams;
            } else {
                result.entries.push_back({stream.offset, stream.compressed, stream.size,
                                          result.streams.size(), 1, stream.head});
            }
            result.streams.push_back(stream);
            covered = align_up(stream.offset + stream.compressed);
        }
    }
    if (archive.size() > covered) {
        result.gaps.push_back({covered, archive.size() - covered});
    }
    return result;
}

std::vector<uint8_t> read_entry(std::span<const uint8_t> archive, const Carve& carve,
                                const Entry& entry) {
    std::vector<uint8_t> data;
    data.reserve(static_cast<size_t>(entry.size));
    for (size_t i = entry.first_stream; i < entry.first_stream + entry.streams; ++i) {
        const Stream& stream = carve.streams.at(i);
        const Inflated out =
            inflate_zlib(archive.subspan(stream.offset, stream.compressed), stream.size);
        data.insert(data.end(), out.data.begin(), out.data.end());
    }
    return data;
}

} // namespace g6::rpf9
