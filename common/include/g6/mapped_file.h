#pragma once

// A file mapped read-only into memory, for the tools that read the user's game files in place
// (shaders.rpf, the RPF9 archives). Nothing is copied or written.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace g6 {

class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&&) = delete;
    MappedFile& operator=(MappedFile&&) = delete;

    std::span<const uint8_t> bytes() const { return {data_, size_}; }

private:
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace g6
