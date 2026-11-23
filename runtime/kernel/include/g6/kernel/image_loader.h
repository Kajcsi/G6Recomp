#pragma once

// Maps a guest module into the host address space the way the console loader would: headers
// and sections at their RVAs, base relocations applied.
//
// Two users with different needs:
// * The native loader (runtime/loader) maps writable with map_image_writable, binds imports and
//   patches the image, then calls protect_image with CodeAccess::Execute so the code runs.
// * The frozen recompiler's tests map with map_image, which keeps code sections non-executable:
//   there the original code is only data.

#include <cstdint>
#include <stdexcept>

#include "g6/pe/image.h"

namespace g6::kernel {

class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct MappedImage {
    uint64_t base = 0;
    uint64_t size = 0;
};

enum class CodeAccess { NoExecute, Execute };

// Maps the image read-write with relocations applied. `base` 0 means anywhere the system finds
// room. Throws LoadError when the range can't be reserved (for example because something else
// occupies it), when the image would need relocating but has no relocation table, or when the
// image is malformed.
MappedImage map_image_writable(const pe::Image& image, uint64_t base);

// Gives the headers and each section their final protection, and flushes the instruction cache
// when code becomes executable.
void protect_image(const pe::Image& image, const MappedImage& mapped, CodeAccess code);

// map_image_writable at exactly `base`, then protect_image with non-executable code.
MappedImage map_image(const pe::Image& image, uint64_t base);

void unmap_image(const MappedImage& mapped);

} // namespace g6::kernel
