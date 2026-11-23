#pragma once

// Builds small PE32+ images in memory for unit tests, so the PE readers and the recompiler's
// analysis passes can be tested without any real binary. Sections are laid out back to back at
// 0x1000-aligned RVAs; build() produces the file bytes, which tests may corrupt on purpose.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "g6/pe/image.h"

namespace g6::test {

class PeBuilder {
public:
    static constexpr uint32_t kFileAlignment = 0x200;
    static constexpr uint32_t kSectionAlignment = 0x1000;
    static constexpr uint64_t kImageBase = 0x180000000ull;
    static constexpr uint32_t kNtHeaderOffset = 0x80;
    static constexpr uint32_t kOptionalHeaderOffset = kNtHeaderOffset + 4 + 20;
    static constexpr uint32_t kOptionalHeaderSize = 112 + 16 * 8;
    static constexpr uint32_t kSizeOfImageOffset = kOptionalHeaderOffset + 56;
    static constexpr uint32_t kSectionTableOffset = kOptionalHeaderOffset + kOptionalHeaderSize;

    static constexpr uint32_t kCode = 0x60000020;  // code | execute | read
    static constexpr uint32_t kRdata = 0x40000040; // initialized data | read
    static constexpr uint32_t kData = 0xC0000040;  // initialized data | read | write

    // Adds a section at the next free aligned RVA, filled with `fill`, and returns its RVA.
    uint32_t add_section(std::string name, uint32_t characteristics, uint32_t size,
                         uint8_t fill = 0) {
        uint32_t rva = kSectionAlignment;
        if (!sections_.empty()) {
            const SectionData& last = sections_.back();
            rva = align(last.rva + static_cast<uint32_t>(last.data.size()), kSectionAlignment);
        }
        sections_.push_back(
            {std::move(name), characteristics, rva, std::vector<uint8_t>(size, fill)});
        return rva;
    }

    template <typename T>
    void put(uint32_t rva, const T& value) {
        std::memcpy(at(rva, sizeof(T)), &value, sizeof(T));
    }

    void put_bytes(uint32_t rva, std::initializer_list<uint8_t> bytes) {
        std::memcpy(at(rva, bytes.size()), bytes.begin(), bytes.size());
    }

    void put_string(uint32_t rva, const std::string& text) {
        std::memcpy(at(rva, text.size() + 1), text.c_str(), text.size() + 1);
    }

    // Writes the rel32 of a call/jmp whose opcode bytes end at `rva` (the rel32 sits at rva and
    // the instruction ends at rva + 4).
    void put_rel32(uint32_t rva, uint32_t target) {
        put<int32_t>(rva, static_cast<int32_t>(int64_t{target} - (int64_t{rva} + 4)));
    }

    void set_directory(pe::Directory which, uint32_t rva, uint32_t size) {
        directories_[static_cast<size_t>(which)] = {rva, size};
    }
    void set_entry_point(uint32_t rva) { entry_point_ = rva; }
    void set_dll(bool dll) { dll_ = dll; }

    std::vector<uint8_t> build() const {
        const uint32_t headers_size = align(
            kSectionTableOffset + 40 * static_cast<uint32_t>(sections_.size()), kFileAlignment);
        uint32_t file_size = headers_size;
        uint32_t image_size = kSectionAlignment;
        for (const SectionData& section : sections_) {
            file_size += align(static_cast<uint32_t>(section.data.size()), kFileAlignment);
            image_size =
                align(section.rva + static_cast<uint32_t>(section.data.size()), kSectionAlignment);
        }
        std::vector<uint8_t> file(file_size, 0);
        const auto write = [&](uint32_t offset, auto value) {
            std::memcpy(file.data() + offset, &value, sizeof(value));
        };

        write(0, uint16_t{0x5A4D}); // MZ
        write(0x3C, kNtHeaderOffset);
        write(kNtHeaderOffset, uint32_t{0x4550}); // PE\0\0

        const uint32_t fh = kNtHeaderOffset + 4;
        write(fh + 0, pe::kMachineAmd64);
        write(fh + 2, static_cast<uint16_t>(sections_.size()));
        write(fh + 4, uint32_t{0x12345678});
        write(fh + 16, static_cast<uint16_t>(kOptionalHeaderSize));
        write(fh + 18, static_cast<uint16_t>(dll_ ? 0x2022 : 0x0022));

        const uint32_t oh = kOptionalHeaderOffset;
        write(oh + 0, uint16_t{0x020B});
        write(oh + 16, entry_point_);
        write(oh + 24, kImageBase);
        write(oh + 32, kSectionAlignment);
        write(oh + 36, kFileAlignment);
        write(oh + 40, uint16_t{6}); // OS version
        write(oh + 48, uint16_t{6}); // subsystem version
        write(oh + 56, image_size);
        write(oh + 60, headers_size);
        write(oh + 68, dll_ ? uint16_t{2} : uint16_t{3});
        write(oh + 70, uint16_t{0x0160});
        write(oh + 72, uint64_t{0x100000});
        write(oh + 80, uint64_t{0x1000});
        write(oh + 88, uint64_t{0x100000});
        write(oh + 96, uint64_t{0x1000});
        write(oh + 108, uint32_t{16});
        for (size_t i = 0; i < directories_.size(); ++i) {
            write(oh + 112 + static_cast<uint32_t>(i) * 8, directories_[i].rva);
            write(oh + 116 + static_cast<uint32_t>(i) * 8, directories_[i].size);
        }

        uint32_t raw = headers_size;
        for (size_t i = 0; i < sections_.size(); ++i) {
            const SectionData& section = sections_[i];
            const uint32_t sh = kSectionTableOffset + static_cast<uint32_t>(i) * 40;
            char name[8] = {};
            std::copy_n(section.name.data(), std::min<size_t>(section.name.size(), 8), name);
            std::memcpy(file.data() + sh, name, 8);
            const auto size = static_cast<uint32_t>(section.data.size());
            const uint32_t raw_size = align(size, kFileAlignment);
            write(sh + 8, size);
            write(sh + 12, section.rva);
            write(sh + 16, raw_size);
            write(sh + 20, raw);
            write(sh + 36, section.characteristics);
            std::memcpy(file.data() + raw, section.data.data(), size);
            raw += raw_size;
        }
        return file;
    }

    pe::Image image() const { return pe::Image::parse(build()); }

    static uint32_t align(uint32_t value, uint32_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

private:
    struct SectionData {
        std::string name;
        uint32_t characteristics = 0;
        uint32_t rva = 0;
        std::vector<uint8_t> data;
    };

    uint8_t* at(uint32_t rva, size_t size) {
        for (SectionData& section : sections_) {
            if (rva >= section.rva && rva - section.rva + size <= section.data.size()) {
                return section.data.data() + (rva - section.rva);
            }
        }
        throw std::out_of_range("PeBuilder: write outside every section");
    }

    std::vector<SectionData> sections_;
    std::array<pe::DataDirectory, pe::kDirectoryCount> directories_{};
    uint32_t entry_point_ = 0;
    bool dll_ = true;
};

} // namespace g6::test
