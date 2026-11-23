#pragma once

// PE32+ image parsing for the recompiler and runtime. Written from the public PE/COFF
// specification. An Image holds the file laid out the way the loader maps it (headers and
// sections at their RVAs, the rest zero-filled), and every read is bounds-checked: malformed
// input throws FormatError instead of reading out of range.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace g6::pe {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr uint16_t kMachineAmd64 = 0x8664;
inline constexpr uint16_t kCharacteristicDll = 0x2000;
inline constexpr uint32_t kSectionCode = 0x00000020;
inline constexpr uint32_t kSectionExecute = 0x20000000;
inline constexpr uint32_t kSectionRead = 0x40000000;
inline constexpr uint32_t kSectionWrite = 0x80000000;

enum class Directory : uint32_t {
    Export = 0,
    Import = 1,
    Resource = 2,
    Exception = 3,
    Security = 4,
    BaseReloc = 5,
    Debug = 6,
    Architecture = 7,
    GlobalPtr = 8,
    Tls = 9,
    LoadConfig = 10,
    BoundImport = 11,
    Iat = 12,
    DelayImport = 13,
    ClrRuntime = 14,
};
inline constexpr size_t kDirectoryCount = 16;

struct DataDirectory {
    uint32_t rva = 0;
    uint32_t size = 0;

    bool present() const { return rva != 0 && size != 0; }
};

struct Section {
    std::string name;
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_offset = 0;
    uint32_t raw_size = 0;
    uint32_t characteristics = 0;

    // Bytes the section occupies once mapped (the loader uses SizeOfRawData when VirtualSize
    // is zero).
    uint32_t mapped_size() const { return virtual_size != 0 ? virtual_size : raw_size; }
    bool contains(uint32_t rva) const {
        return rva >= virtual_address && rva - virtual_address < mapped_size();
    }
    bool executable() const { return (characteristics & kSectionExecute) != 0; }
    bool writable() const { return (characteristics & kSectionWrite) != 0; }
};

class Image {
public:
    static Image load_file(const std::filesystem::path& path);
    static Image parse(std::span<const uint8_t> file);

    uint16_t machine() const { return machine_; }
    uint16_t characteristics() const { return characteristics_; }
    bool is_dll() const { return (characteristics_ & kCharacteristicDll) != 0; }
    uint32_t timestamp() const { return timestamp_; }
    uint64_t image_base() const { return image_base_; }
    uint32_t size_of_image() const { return static_cast<uint32_t>(mapped_.size()); }
    uint32_t size_of_headers() const { return size_of_headers_; }
    uint32_t entry_point() const { return entry_point_; }
    uint16_t subsystem() const { return subsystem_; }
    uint16_t dll_characteristics() const { return dll_characteristics_; }
    uint32_t section_alignment() const { return section_alignment_; }
    uint64_t stack_reserve() const { return stack_reserve_; }
    uint64_t stack_commit() const { return stack_commit_; }

    const std::vector<Section>& sections() const { return sections_; }
    DataDirectory directory(Directory which) const {
        return directories_[static_cast<size_t>(which)];
    }

    const Section* section_at(uint32_t rva) const;
    bool is_executable(uint32_t rva) const;
    bool contains(uint32_t rva, uint64_t size = 1) const;

    // The whole mapped image, for decoders that want a raw view.
    std::span<const uint8_t> mapped() const { return mapped_; }

    // Bounds-checked accessors on the mapped image. They throw FormatError when out of range.
    std::span<const uint8_t> bytes(uint32_t rva, uint32_t size) const;
    std::string read_cstring(uint32_t rva, size_t max_length = 4096) const;

    template <typename T>
    T read(uint32_t rva) const {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        std::memcpy(&value, bytes(rva, sizeof(T)).data(), sizeof(T));
        return value;
    }

    // Converts a virtual address at the preferred base to an RVA inside the image.
    std::optional<uint32_t> try_va_to_rva(uint64_t va) const;
    uint32_t va_to_rva(uint64_t va) const;

private:
    Image() = default;

    std::vector<uint8_t> mapped_;
    std::vector<Section> sections_;
    std::array<DataDirectory, kDirectoryCount> directories_{};
    uint64_t image_base_ = 0;
    uint64_t stack_reserve_ = 0;
    uint64_t stack_commit_ = 0;
    uint32_t timestamp_ = 0;
    uint32_t entry_point_ = 0;
    uint32_t size_of_headers_ = 0;
    uint32_t section_alignment_ = 0;
    uint16_t machine_ = 0;
    uint16_t characteristics_ = 0;
    uint16_t subsystem_ = 0;
    uint16_t dll_characteristics_ = 0;
};

} // namespace g6::pe
