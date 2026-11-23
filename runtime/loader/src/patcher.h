#pragma once

// The hook engine (docs/hooks.md). The loader calls apply_patches while a module is mapped
// writable and before any of its code runs: after binding its imports, before making its code
// executable. Nothing else is running the module's code yet, so no thread has to be suspended.

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>
#include <windows.h>

#include "g6/kernel/image_loader.h"
#include "g6/loader/patches.h"
#include "g6/pe/image.h"

namespace g6::loader {

class PatchArena;

// What patching a module leaves behind: the arena with its stubs and trampolines, and the cpuid
// and xgetbv sites the trap handler answers. Destroying it frees the arena and resets the
// `original` pointers it filled in.
class AppliedPatches {
public:
    struct Site {
        uint32_t rva = 0;
        bool xgetbv = false;
    };

    AppliedPatches();
    ~AppliedPatches();
    AppliedPatches(const AppliedPatches&) = delete;
    AppliedPatches& operator=(const AppliedPatches&) = delete;

    // Answers a trapped cpuid or xgetbv at `rva`: sets the result registers in `context` and
    // moves rip past the instruction. False when no trapped site is at `rva`, or when the host
    // rejects the xgetbv index (the guest then sees the illegal-instruction fault).
    bool answer_trap(uint32_t rva, CONTEXT& context) const;
    bool has_traps() const { return !sites.empty(); }

    std::unique_ptr<PatchArena> arena;
    std::vector<Site> sites; // sorted by rva
    std::vector<void**> originals;
};

// Applies `patches` to the module `image` mapped at `mapped`. Throws kernel::LoadError naming
// the rule a patch breaks; the caller then discards the module, so nothing stays half patched.
std::unique_ptr<AppliedPatches> apply_patches(std::string_view module_name, const pe::Image& image,
                                              const kernel::MappedImage& mapped,
                                              const ModulePatches& patches);

} // namespace g6::loader
