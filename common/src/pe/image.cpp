#include "g6/pe/image.h"

#include <algorithm>
#include <fstream>
#include <utility>

#include <fmt/format.h>

#include "g6/text.h"

namespace g6::pe {
namespace {

// Largest image we accept. Generous for a game executable, small enough to reject garbage
// SizeOfImage values before allocating.
constexpr uint32_t kMaxImageSize = 0xC0000000u;

constexpr uint16_t kDosMagic = 0x5A4D;    // "MZ"
constexpr uint32_t kPeSignature = 0x4550; // "PE\0\0"
constexpr uint16_t kPe32PlusMagic = 0x020B;
constexpr uint32_t kFileHeaderSize = 20;
constexpr uint32_t kSectionHeaderSize = 40;
constexpr uint32_t kOptionalHeaderFixedSize = 112; // PE32+ fields before the data directories

// Bounds-checked little-endian reads from the raw file.
class FileReader {
public:
    explicit FileReader(std::span<const uint8_t> file) : file_(file) {}

    template <typename T>
    T read(uint64_t offset) const {
        if (offset > file_.size() || file_.size() - offset < sizeof(T)) {
            throw FormatError(fmt::format("read of {} bytes at file offset {:#x} is past the end "
                                          "of the file ({:#x} bytes)",
                                          sizeof(T), offset, file_.size()));
        }
        T value;
        std::memcpy(&value, file_.data() + offset, sizeof(T));
        return value;
    }

    size_t size() const { return file_.size(); }
    std::span<const uint8_t> span() const { return file_; }

private:
    std::span<const uint8_t> file_;
};

} // namespace

Image Image::load_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw FormatError(fmt::format("cannot open {}", utf8(path.native())));
    }
    const std::streamoff length = stream.tellg();
    if (length < 0 || std::cmp_greater(length, kMaxImageSize)) {
        throw FormatError(fmt::format("{} is too large or unreadable", utf8(path.native())));
    }
    std::vector<uint8_t> file(static_cast<size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(file.data()), length)) {
        throw FormatError(fmt::format("failed to read {}", utf8(path.native())));
    }
    return parse(file);
}

Image Image::parse(std::span<const uint8_t> file) {
    const FileReader reader(file);
    Image image;

    if (reader.read<uint16_t>(0) != kDosMagic) {
        throw FormatError("missing MZ header");
    }
    const auto nt = reader.read<uint32_t>(0x3C);
    if (reader.read<uint32_t>(nt) != kPeSignature) {
        throw FormatError(fmt::format("missing PE signature at {:#x}", nt));
    }

    const uint64_t file_header = uint64_t{nt} + 4;
    image.machine_ = reader.read<uint16_t>(file_header + 0);
    const auto section_count = reader.read<uint16_t>(file_header + 2);
    image.timestamp_ = reader.read<uint32_t>(file_header + 4);
    const auto optional_size = reader.read<uint16_t>(file_header + 16);
    image.characteristics_ = reader.read<uint16_t>(file_header + 18);

    const uint64_t opt = file_header + kFileHeaderSize;
    if (reader.read<uint16_t>(opt) != kPe32PlusMagic) {
        throw FormatError("not a PE32+ (64-bit) image");
    }
    if (optional_size < kOptionalHeaderFixedSize) {
        throw FormatError(fmt::format("optional header too small ({} bytes)", optional_size));
    }
    image.entry_point_ = reader.read<uint32_t>(opt + 16);
    image.image_base_ = reader.read<uint64_t>(opt + 24);
    image.section_alignment_ = reader.read<uint32_t>(opt + 32);
    const auto size_of_image = reader.read<uint32_t>(opt + 56);
    image.size_of_headers_ = reader.read<uint32_t>(opt + 60);
    image.subsystem_ = reader.read<uint16_t>(opt + 68);
    image.dll_characteristics_ = reader.read<uint16_t>(opt + 70);
    image.stack_reserve_ = reader.read<uint64_t>(opt + 72);
    image.stack_commit_ = reader.read<uint64_t>(opt + 80);
    const uint32_t directory_count =
        std::min<uint32_t>(reader.read<uint32_t>(opt + 108), kDirectoryCount);
    if (optional_size < kOptionalHeaderFixedSize + directory_count * 8) {
        throw FormatError("optional header too small for its data directories");
    }
    for (uint32_t i = 0; i < directory_count; ++i) {
        image.directories_[i].rva = reader.read<uint32_t>(opt + kOptionalHeaderFixedSize + i * 8);
        image.directories_[i].size =
            reader.read<uint32_t>(opt + kOptionalHeaderFixedSize + i * 8 + 4);
    }

    if (size_of_image == 0 || size_of_image > kMaxImageSize) {
        throw FormatError(fmt::format("implausible SizeOfImage {:#x}", size_of_image));
    }
    if (image.size_of_headers_ > size_of_image) {
        throw FormatError("SizeOfHeaders is larger than SizeOfImage");
    }

    image.mapped_.assign(size_of_image, 0);
    const size_t header_bytes = std::min<size_t>(image.size_of_headers_, reader.size());
    std::copy_n(file.begin(), header_bytes, image.mapped_.begin());

    const uint64_t section_table = opt + optional_size;
    image.sections_.reserve(section_count);
    for (uint32_t i = 0; i < section_count; ++i) {
        const uint64_t header = section_table + uint64_t{i} * kSectionHeaderSize;
        Section section;
        const auto raw_name = reader.read<std::array<char, 8>>(header);
        section.name.assign(raw_name.data(), strnlen(raw_name.data(), raw_name.size()));
        section.virtual_size = reader.read<uint32_t>(header + 8);
        section.virtual_address = reader.read<uint32_t>(header + 12);
        section.raw_size = reader.read<uint32_t>(header + 16);
        section.raw_offset = reader.read<uint32_t>(header + 20);
        section.characteristics = reader.read<uint32_t>(header + 36);

        const uint64_t mapped_end = uint64_t{section.virtual_address} + section.mapped_size();
        if (mapped_end > size_of_image) {
            throw FormatError(fmt::format("section {} ends at {:#x}, past SizeOfImage {:#x}",
                                          section.name, mapped_end, size_of_image));
        }

        // The raw data checks Windows makes (measured on Windows 11 26200; see "Pre-flight
        // checks" in docs/execution-model.md): a section with no raw data isn't checked, raw
        // data may not end at or past 4 GiB, and the last section's raw data must lie inside
        // the file. Any other section may run or start past the end of the file, and the bytes
        // the file lacks read as zero.
        const uint64_t raw_end = uint64_t{section.raw_offset} + section.raw_size;
        if (section.raw_size != 0) {
            if (raw_end > UINT32_MAX) {
                throw FormatError(fmt::format("section {} raw data ends at {:#x}, past 4 GiB",
                                              section.name, raw_end));
            }
            if (i + 1 == section_count && raw_end > reader.size()) {
                throw FormatError(fmt::format("last section {} raw data ends at {:#x}, past the "
                                              "end of the file at {:#x}",
                                              section.name, raw_end, reader.size()));
            }
        }

        // Copy the initialized part that the file holds.
        const uint64_t wanted = std::min(section.raw_size, section.mapped_size());
        if (wanted != 0 && section.raw_offset < reader.size()) {
            const uint64_t copy = std::min<uint64_t>(wanted, reader.size() - section.raw_offset);
            std::copy_n(file.begin() + section.raw_offset, copy,
                        image.mapped_.begin() + section.virtual_address);
        }
        image.sections_.push_back(std::move(section));
    }
    return image;
}

const Section* Image::section_at(uint32_t rva) const {
    for (const Section& section : sections_) {
        if (section.contains(rva)) {
            return &section;
        }
    }
    return nullptr;
}

bool Image::is_executable(uint32_t rva) const {
    const Section* section = section_at(rva);
    return section != nullptr && section->executable();
}

bool Image::contains(uint32_t rva, uint64_t size) const {
    return rva <= mapped_.size() && mapped_.size() - rva >= size;
}

std::span<const uint8_t> Image::bytes(uint32_t rva, uint32_t size) const {
    if (!contains(rva, size)) {
        throw FormatError(fmt::format("read of {} bytes at RVA {:#x} is outside the image "
                                      "({:#x} bytes)",
                                      size, rva, mapped_.size()));
    }
    return std::span(mapped_).subspan(rva, size);
}

std::string Image::read_cstring(uint32_t rva, size_t max_length) const {
    if (!contains(rva)) {
        throw FormatError(fmt::format("string at RVA {:#x} is outside the image", rva));
    }
    const size_t limit = std::min(max_length, mapped_.size() - rva);
    const auto* begin = mapped_.data() + rva;
    const auto* end = std::find(begin, begin + limit, uint8_t{0});
    if (end == begin + limit) {
        throw FormatError(fmt::format("string at RVA {:#x} is not terminated", rva));
    }
    return {reinterpret_cast<const char*>(begin), static_cast<size_t>(end - begin)};
}

std::optional<uint32_t> Image::try_va_to_rva(uint64_t va) const {
    if (va < image_base_ || va - image_base_ >= mapped_.size()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(va - image_base_);
}

uint32_t Image::va_to_rva(uint64_t va) const {
    if (const auto rva = try_va_to_rva(va)) {
        return *rva;
    }
    throw FormatError(fmt::format("address {:#x} is outside the image", va));
}

} // namespace g6::pe
