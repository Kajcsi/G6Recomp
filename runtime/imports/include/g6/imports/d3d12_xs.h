#pragma once

// d3d12_xs.dll, the console's D3D12.X. Every export is a stub returning E_NOTIMPL until
// runtime/gpu wraps it at M3 (docs/d3d12x.md), except the ones here, which the executable calls
// before it creates the device. Console-only, so the names come from the import table.

#include <cstdint>
#include <windows.h>

namespace g6::loader {
class ImportRegistry;
}

namespace g6::imports::d3d12_xs {

// Sets up the GPU's virtual address space before the device exists. The executable passes a
// 32-byte description it zeroes first, with a count in the word at +4, and stops at an assert
// unless the result is a success code (docs/boot.md, "Our runs"). The host GPU has its own
// address space, so this succeeds and the description is only logged; runtime/gpu takes it up at
// M3.
HRESULT D3D12ConfigureVirtualMemoryX(const void* description);

// Binds the functions above over d3d12_xs.dll's stubs.
void register_d3d12_xs(loader::ImportRegistry& imports);

} // namespace g6::imports::d3d12_xs
