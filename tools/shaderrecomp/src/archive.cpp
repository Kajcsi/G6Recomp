// Shader records in shaders.rpf (see archive.h).

#include "archive.h"

#include <algorithm>
#include <cstring>

#include "dxbc.h"
#include "g6/inflate.h"

namespace g6::shader {
namespace {

constexpr size_t kMinName = 3;
constexpr size_t kMaxName = 200;
constexpr size_t kFieldsSize = 18; // the NUL, the byte, and four u32s
constexpr uint32_t kMaxCompressed = 64u << 20;
constexpr uint32_t kMaxContainer = 256u << 20;

// Names are printable ASCII without spaces.
bool name_char(uint8_t c) {
    return c > 0x20 && c < 0x7F;
}

uint32_t u32_at(std::span<const uint8_t> data, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

} // namespace

ArchiveScanStats scan_shader_archive(std::span<const uint8_t> archive,
                                     const std::function<void(const ShaderRecord&)>& visit) {
    ArchiveScanStats stats;
    const uint8_t* const base = archive.data();
    // A name can't start inside the record before it.
    size_t floor = 0;
    for (size_t at = 0; at < archive.size();) {
        const void* const found = std::memchr(base + at, 0, archive.size() - at);
        if (found == nullptr) {
            break;
        }
        const auto nul = static_cast<size_t>(static_cast<const uint8_t*>(found) - base);
        at = nul + 1;
        if (archive.size() - nul < kFieldsSize) {
            break;
        }
        const size_t lowest = std::max(floor, nul > kMaxName ? nul - kMaxName : 0);
        size_t start = nul;
        while (start > lowest && name_char(archive[start - 1])) {
            --start;
        }
        if (nul - start < kMinName) {
            continue;
        }
        const uint32_t compressed = u32_at(archive, nul + 2);
        const uint32_t size = u32_at(archive, nul + 14);
        if (compressed != u32_at(archive, nul + 10) || compressed <= 16 ||
            compressed >= kMaxCompressed || size <= 32 || size >= kMaxContainer ||
            compressed > archive.size() - nul - kFieldsSize) {
            continue;
        }

        ShaderRecord record;
        try {
            record.container =
                inflate_raw(archive.subspan(nul + kFieldsSize, compressed), size).data;
        } catch (const InflateError&) {
            ++stats.rejected;
            continue;
        }
        if (record.container.size() != size || read_u32(record.container, 0) != fourcc("DXBC")) {
            ++stats.rejected;
            continue;
        }
        record.offset = start;
        record.name.assign(reinterpret_cast<const char*>(base + start), nul - start);
        record.tag = archive[nul + 1];
        record.id = u32_at(archive, nul + 6);
        record.compressed_size = compressed;
        visit(record);
        ++stats.records;
        at = nul + kFieldsSize + compressed;
        floor = at;
    }
    return stats;
}

} // namespace g6::shader
