#include "g6/kernel/image_loader.h"

#include <cstring>
#include <windows.h>

#include "g6/log.h"
#include "g6/pe/directories.h"

namespace g6::kernel {
namespace {

DWORD protection_for(const pe::Section& section, CodeAccess code) {
    if (section.executable() && code == CodeAccess::Execute) {
        return section.writable() ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    }
    return section.writable() ? PAGE_READWRITE : PAGE_READONLY;
}

void apply_relocations(const pe::Image& image, uint8_t* mapped, uint64_t base) {
    const uint64_t delta = base - image.image_base();
    if (delta == 0) {
        return;
    }
    if (!image.directory(pe::Directory::BaseReloc).present()) {
        throw LoadError(
            fmt::format("the image prefers {:#x} but has no relocations to move to {:#x}",
                        image.image_base(), base));
    }
    for (const pe::Relocation& reloc : pe::read_relocations(image)) {
        uint8_t* const at = mapped + reloc.rva;
        switch (reloc.type) {
        case pe::kRelocDir64: {
            uint64_t value = 0;
            std::memcpy(&value, at, sizeof(value));
            value += delta;
            std::memcpy(at, &value, sizeof(value));
            break;
        }
        case pe::kRelocHighLow: {
            uint32_t value = 0;
            std::memcpy(&value, at, sizeof(value));
            value += static_cast<uint32_t>(delta);
            std::memcpy(at, &value, sizeof(value));
            break;
        }
        default:
            throw LoadError(
                fmt::format("unsupported relocation type {} at RVA {:#x}", reloc.type, reloc.rva));
        }
    }
}

} // namespace

MappedImage map_image_writable(const pe::Image& image, uint64_t base) {
    const uint64_t size = image.size_of_image();
    void* const memory =
        VirtualAlloc(reinterpret_cast<void*>(base), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory == nullptr) {
        throw LoadError(fmt::format("cannot reserve {:#x} bytes at {:#x} (error {})", size, base,
                                    GetLastError()));
    }
    const auto actual = reinterpret_cast<uint64_t>(memory);
    if (base != 0 && actual != base) {
        VirtualFree(memory, 0, MEM_RELEASE);
        throw LoadError(
            fmt::format("the system placed the image at {:#x} instead of {:#x}", actual, base));
    }

    auto* const bytes = static_cast<uint8_t*>(memory);
    const auto source = image.mapped();
    std::memcpy(bytes, source.data(), source.size());
    try {
        apply_relocations(image, bytes, actual);
    } catch (...) {
        VirtualFree(memory, 0, MEM_RELEASE);
        throw;
    }
    LOG_DEBUG(kernel, "mapped {:#x} bytes at {:#x} (preferred {:#x})", size, actual,
              image.image_base());
    return MappedImage{.base = actual, .size = size};
}

void protect_image(const pe::Image& image, const MappedImage& mapped, CodeAccess code) {
    auto* const bytes = reinterpret_cast<uint8_t*>(mapped.base);
    DWORD old = 0;
    VirtualProtect(bytes, image.size_of_headers(), PAGE_READONLY, &old);
    bool any_code = false;
    for (const pe::Section& section : image.sections()) {
        if (section.mapped_size() == 0) {
            continue;
        }
        any_code = any_code || section.executable();
        if (!VirtualProtect(bytes + section.virtual_address, section.mapped_size(),
                            protection_for(section, code), &old)) {
            LOG_WARN(kernel, "cannot protect section {} at {:#x} (error {})", section.name,
                     mapped.base + section.virtual_address, GetLastError());
        }
    }
    if (any_code && code == CodeAccess::Execute) {
        FlushInstructionCache(GetCurrentProcess(), bytes, mapped.size);
    }
}

MappedImage map_image(const pe::Image& image, uint64_t base) {
    if (base == 0) {
        throw LoadError("map_image needs an explicit base");
    }
    const MappedImage mapped = map_image_writable(image, base);
    protect_image(image, mapped, CodeAccess::NoExecute);
    return mapped;
}

void unmap_image(const MappedImage& mapped) {
    if (mapped.base != 0) {
        VirtualFree(reinterpret_cast<void*>(mapped.base), 0, MEM_RELEASE);
    }
}

} // namespace g6::kernel
