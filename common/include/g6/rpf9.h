#pragma once

// RPF9, the archive format of the game's data (docs/rpf.md), read for analysis only. The game's own
// code reads its archives when it runs, so the runtime never needs this.
//
// An archive is a 64-byte header, a table of contents, and a data area that starts at the next
// 16-byte boundary after the table. Most tables of contents are encrypted, and nothing here
// decrypts them. Instead, `carve` walks the data area: files are zlib streams that start on 16-byte
// boundaries, and a file larger than 64 KiB is a run of streams that each inflate to exactly 64
// KiB, apart from the last. Whatever no stream covers is stored data (a gap), such as the shader
// records in shaders.rpf or raw audio and video. Each stream's Adler-32 has to match, so random
// bytes that happen to start like a zlib header aren't taken for a stream.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace g6::rpf9 {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr size_t kHeaderSize = 64;
inline constexpr uint32_t kChunkSize = 64 * 1024; // what each stream but a file's last inflates to
inline constexpr uint64_t kAlignment = 16;

// The header's fields. Only the table-of-contents size is relied on; the others are printed for
// study, and their names are what other readers report (docs/rpf.md).
struct Header {
    uint32_t toc_size = 0;    // 0x04: bytes of table of contents after the header
    uint32_t word_08 = 0;     // 0x08
    uint32_t names_size = 0;  // 0x0C
    uint64_t toc_hash = 0;    // 0x10, read big-endian
    uint32_t word_18 = 0;     // 0x18
    uint32_t word_1c = 0;     // 0x1C
    uint16_t word_20 = 0;     // 0x20
    uint16_t word_22 = 0;     // 0x22
    uint32_t entry_count = 0; // 0x24: reported as the entry count, but far below the files carved
    uint32_t key_id = 0;      // 0x28: which key the table of contents is encrypted with; unused
    uint32_t flags = 0;       // 0x2C
    uint32_t word_30 = 0;     // 0x30
};

// The header, or nothing when the file is too short or lacks the `9FPR` magic.
std::optional<Header> read_header(std::span<const uint8_t> archive);

// Where the data area starts: the first 16-byte boundary after the table of contents.
uint64_t data_start(const Header& header);

// What a file is, judged by its first bytes.
enum class Kind : uint8_t {
    kResource,      // a paged resource: `0DRP` at 0x10, its type at 0x14
    kObject,        // a paged object without that header: a system-page pointer at 0x08
    kPipelineCache, // `PS\0\t`, the console driver's pipeline cache (docs/shaders.md)
    kSchema,        // `PSIN`, a reflection-schema container
    kRbf,           // `RBF0`
    kXml,
    kGfx, // `GFX` or `CFX`
    kDds, // `DDS `
    kPng,
    kText, // almost all printable
    kOther,
};

std::string_view kind_name(Kind kind);

struct Classified {
    Kind kind = Kind::kOther;
    uint32_t tag = 0; // the resource type for kResource, the first u32 for kOther, else 0
};

Classified classify(std::span<const uint8_t> head);

// One zlib stream in the data area.
struct Stream {
    uint64_t offset = 0;
    uint32_t compressed = 0; // bytes the stream occupies, Adler-32 included
    uint32_t size = 0;       // bytes it inflates to
    Classified head;         // what its first bytes look like
};

// One file: a run of streams, each but the last inflating to exactly kChunkSize and each starting
// at the 16-byte boundary after the one before.
struct Entry {
    uint64_t offset = 0;
    uint64_t compressed = 0;
    uint64_t size = 0;
    size_t first_stream = 0;
    size_t streams = 0;
    Classified head; // the first stream's
};

// Bytes of the data area no stream covers, apart from the padding up to each 16-byte boundary.
struct Gap {
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct Carve {
    Header header;
    uint64_t data_start = 0;
    std::vector<Stream> streams; // in offset order
    std::vector<Entry> entries;  // in offset order
    std::vector<Gap> gaps;       // in offset order
};

// Walks the data area on `threads` threads (0: one per processor). Throws FormatError when the
// file isn't an RPF9 archive or its table of contents runs past the end.
Carve carve(std::span<const uint8_t> archive, unsigned threads = 0);

// The entry's bytes, its streams inflated one after another.
std::vector<uint8_t> read_entry(std::span<const uint8_t> archive, const Carve& carve,
                                const Entry& entry);

} // namespace g6::rpf9
