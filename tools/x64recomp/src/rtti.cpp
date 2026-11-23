// MSVC x64 run-time type information (see rtti.h). The layouts, all little-endian, with every
// pointer between RTTI structures an RVA:
//   complete object locator (24 bytes): signature 1, offset, constructor displacement, type
//     descriptor, class hierarchy, and the locator's own RVA
//   type descriptor: a pointer to type_info's vtable, a spare pointer (zero in the file, where the
//     run time caches the undecorated name), then the decorated name, NUL-terminated
//   class hierarchy (16 bytes): signature 0, attributes, base count, base array
//   base class descriptor (28 bytes): type descriptor, contained bases, mdisp, pdisp, vdisp,
//     attributes, class hierarchy
// A vtable's slot -1 holds the absolute address of its locator.

#include "rtti.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

// dbghelp.h needs windows.h first.
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <fmt/format.h>

namespace g6::recomp {
namespace {

constexpr uint64_t kLocatorSize = 24;
constexpr uint64_t kBaseSize = 28;
constexpr uint64_t kTypeNameAt = 16;
constexpr size_t kMaxTypeName = 4096;
constexpr uint32_t kMaxBases = 4096;
constexpr size_t kMaxSlots = 65536;

template <typename T>
T load(std::span<const uint8_t> image, uint64_t rva) {
    if (rva > image.size() || sizeof(T) > image.size() - rva) {
        throw pe::FormatError(fmt::format("{:#x} is outside the image", rva));
    }
    T value;
    std::memcpy(&value, image.data() + rva, sizeof(T));
    return value;
}

// The decorated name of the type descriptor at `rva`, or empty if there isn't one there.
std::string type_name(std::span<const uint8_t> image, uint64_t rva) {
    const uint64_t at = rva + kTypeNameAt;
    if (at >= image.size() || image.size() - at < 4 || std::memcmp(&image[at], ".?A", 3) != 0) {
        return {};
    }
    const auto* const text = reinterpret_cast<const char*>(&image[at]);
    const size_t max = std::min<size_t>(image.size() - at, kMaxTypeName);
    const size_t length = strnlen(text, max);
    return length == max ? std::string() : std::string(text, length);
}

void read_hierarchy(std::span<const uint8_t> image, RttiClass& cls) {
    if (load<uint32_t>(image, cls.hierarchy) != 0) {
        throw pe::FormatError("class hierarchy signature isn't 0");
    }
    cls.attributes = load<uint32_t>(image, uint64_t{cls.hierarchy} + 4);
    const auto count = load<uint32_t>(image, uint64_t{cls.hierarchy} + 8);
    const auto array = load<uint32_t>(image, uint64_t{cls.hierarchy} + 12);
    if (count == 0 || count > kMaxBases) {
        throw pe::FormatError(fmt::format("class hierarchy has {} bases", count));
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t at = load<uint32_t>(image, uint64_t{array} + (4 * uint64_t{i}));
        load<uint32_t>(image, at + kBaseSize - 4); // the whole descriptor is inside the image
        RttiBase base;
        base.type_descriptor = load<uint32_t>(image, at);
        base.name = type_name(image, base.type_descriptor);
        if (base.name.empty()) {
            throw pe::FormatError(fmt::format("base {} has no type descriptor", i));
        }
        base.contained_bases = load<uint32_t>(image, at + 4);
        base.member_displacement = load<int32_t>(image, at + 8);
        base.vbtable_displacement = load<int32_t>(image, at + 12);
        base.vbtable_index = load<int32_t>(image, at + 16);
        base.attributes = load<uint32_t>(image, at + 20);
        cls.bases.push_back(std::move(base));
    }
    if (cls.bases.front().type_descriptor != cls.type_descriptor) {
        throw pe::FormatError("class hierarchy doesn't start with the class itself");
    }
}

struct Locator {
    uint32_t rva = 0;
    uint32_t type_descriptor = 0;
    uint32_t offset = 0;
    uint32_t constructor_displacement = 0;
    bool has_vtable = false;
};

// Reads the locator at `rva`, adding its class to the report the first time it's seen.
Locator read_locator(std::span<const uint8_t> image, uint32_t rva, RttiReport& report) {
    Locator locator{.rva = rva,
                    .type_descriptor = load<uint32_t>(image, uint64_t{rva} + 12),
                    .offset = load<uint32_t>(image, uint64_t{rva} + 4),
                    .constructor_displacement = load<uint32_t>(image, uint64_t{rva} + 8)};
    const auto hierarchy = load<uint32_t>(image, uint64_t{rva} + 16);
    const auto known = report.classes.find(locator.type_descriptor);
    if (known != report.classes.end()) {
        if (known->second.hierarchy != hierarchy) {
            throw pe::FormatError("a second class hierarchy for one type descriptor");
        }
        return locator;
    }
    RttiClass cls{.type_descriptor = locator.type_descriptor,
                  .name = type_name(image, locator.type_descriptor),
                  .hierarchy = hierarchy,
                  .attributes = 0,
                  .bases = {}};
    if (cls.name.empty()) {
        throw pe::FormatError("no type descriptor");
    }
    read_hierarchy(image, cls);
    report.classes.emplace(cls.type_descriptor, std::move(cls));
    return locator;
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t section_end(const pe::Section& section, std::span<const uint8_t> image) {
    return std::min<uint64_t>(uint64_t{section.virtual_address} + section.mapped_size(),
                              image.size());
}

} // namespace

RttiReport scan_rtti(const pe::Image& image) {
    const std::span<const uint8_t> m = image.mapped();
    RttiReport report;

    // Locators, by their signature, their self-reference, and a named type descriptor. Other data
    // can match the first two (unwind tables do), never all three.
    std::vector<Locator> locators;
    std::unordered_map<uint32_t, size_t> locator_at;
    for (const pe::Section& section : image.sections()) {
        const uint64_t end = section_end(section, m);
        for (uint64_t rva = align_up(section.virtual_address, 4); rva + kLocatorSize <= end;
             rva += 4) {
            if (load<uint32_t>(m, rva) != 1 || load<uint32_t>(m, rva + 20) != rva ||
                type_name(m, load<uint32_t>(m, rva + 12)).empty()) {
                continue;
            }
            ++report.locators;
            try {
                locators.push_back(read_locator(m, static_cast<uint32_t>(rva), report));
                locator_at.emplace(static_cast<uint32_t>(rva), locators.size() - 1);
            } catch (const pe::FormatError& error) {
                if (report.bad_locators++ == 0) {
                    report.first_error = fmt::format("locator at {:#x}: {}", rva, error.what());
                }
            }
        }
    }

    // Vtables: a locator's absolute address, followed by pointers into code.
    const uint64_t base = image.image_base();
    const auto code_rva = [&](uint64_t value) -> std::optional<uint32_t> {
        if (value < base || value - base >= m.size()) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(value - base);
    };
    for (const pe::Section& section : image.sections()) {
        const uint64_t end = section_end(section, m);
        for (uint64_t rva = align_up(section.virtual_address, 8); rva + 8 <= end; rva += 8) {
            const std::optional<uint32_t> target = code_rva(load<uint64_t>(m, rva));
            const auto found = target ? locator_at.find(*target) : locator_at.end();
            if (found == locator_at.end()) {
                continue;
            }
            Locator& locator = locators[found->second];
            locator.has_vtable = true;
            RttiVtable vtable{.rva = static_cast<uint32_t>(rva + 8),
                              .locator = locator.rva,
                              .type_descriptor = locator.type_descriptor,
                              .offset = locator.offset,
                              .constructor_displacement = locator.constructor_displacement,
                              .slots = 0};
            for (uint64_t at = vtable.rva; at + 8 <= m.size() && vtable.slots < kMaxSlots;
                 at += 8) {
                const std::optional<uint32_t> slot = code_rva(load<uint64_t>(m, at));
                if (!slot || !image.is_executable(*slot)) {
                    break;
                }
                ++vtable.slots;
            }
            report.vtables.push_back(vtable);
        }
    }
    report.locators_without_vtable = static_cast<size_t>(
        std::ranges::count_if(locators, [](const Locator& l) { return !l.has_vtable; }));

    // Every type descriptor, by its name and zero spare pointer. They hold a pointer, so they're
    // 8-byte aligned, and the run time writes the spare pointer, so they're never in code.
    for (const pe::Section& section : image.sections()) {
        if (section.executable()) {
            continue;
        }
        const uint64_t end = section_end(section, m);
        for (uint64_t rva = align_up(section.virtual_address, 8); rva + kTypeNameAt + 4 <= end;
             rva += 8) {
            if (m[rva + kTypeNameAt] == '.' && load<uint64_t>(m, rva + 8) == 0 &&
                !type_name(m, rva).empty()) {
                report.type_descriptors.push_back(static_cast<uint32_t>(rva));
            }
        }
    }
    return report;
}

std::string undecorate_type_name(std::string_view decorated) {
    // A type descriptor's name minus its leading '.' is a decorated type that dbghelp undecorates
    // as "class Foo". dbghelp isn't thread-safe, and nothing else here calls it.
    if (!decorated.starts_with(".?A")) {
        return std::string(decorated);
    }
    const std::string symbol(decorated.substr(1));
    std::array<char, 4096> text{};
    const DWORD length =
        UnDecorateSymbolName(symbol.c_str(), text.data(), static_cast<DWORD>(text.size()),
                             UNDNAME_NO_ARGUMENTS | UNDNAME_32_BIT_DECODE);
    if (length == 0 || std::string_view(text.data(), length) == symbol) {
        return std::string(decorated);
    }
    return {text.data(), length};
}

} // namespace g6::recomp
