#pragma once

// Finding the shader records in shaders.rpf (docs/shaders.md) without the archive's table of
// contents, which the game itself reads. Each record is a name, a NUL, a byte, a u32 compressed
// size, a u32, the compressed size again, a u32 container size, and then that many compressed
// bytes: a raw DEFLATE stream of one DXBC container. The repeated size, a stream that inflates to
// exactly the stated size, and the DXBC magic are what make a record; the meaning of the byte and
// the middle u32 isn't known yet.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "g6/mapped_file.h"

namespace g6::shader {

struct ShaderRecord {
    uint64_t offset = 0; // of the name, in the archive
    std::string name;
    uint8_t tag = 0; // the byte after the name
    uint32_t id = 0; // the u32 between the two copies of the compressed size
    uint32_t compressed_size = 0;
    std::vector<uint8_t> container;
};

struct ArchiveScanStats {
    size_t records = 0;
    // Headers that looked right, but whose data didn't inflate to a DXBC container of the stated
    // size.
    size_t rejected = 0;
};

// Calls `visit` with each record, in archive order.
ArchiveScanStats scan_shader_archive(std::span<const uint8_t> archive,
                                     const std::function<void(const ShaderRecord&)>& visit);

using g6::MappedFile;

} // namespace g6::shader
