#include "g6/mapped_file.h"

#include <stdexcept>
#include <windows.h>

#include <fmt/format.h>

namespace g6 {

MappedFile::MappedFile(const std::filesystem::path& path) {
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = nullptr;
        throw std::runtime_error(
            fmt::format("can't open {} (error {})", path.string(), GetLastError()));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size)) {
        const DWORD error = GetLastError();
        CloseHandle(file_);
        throw std::runtime_error(fmt::format("can't size {} (error {})", path.string(), error));
    }
    size_ = static_cast<size_t>(size.QuadPart);
    if (size_ == 0) {
        return; // Windows can't map an empty file, and there's nothing to map.
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const void* view =
        mapping_ != nullptr ? MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (view == nullptr) {
        const DWORD error = GetLastError();
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
        }
        CloseHandle(file_);
        throw std::runtime_error(fmt::format("can't map {} (error {})", path.string(), error));
    }
    data_ = static_cast<const uint8_t*>(view);
}

MappedFile::~MappedFile() {
    if (data_ != nullptr) {
        UnmapViewOfFile(data_);
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
    }
    if (file_ != nullptr) {
        CloseHandle(file_);
    }
}

} // namespace g6
