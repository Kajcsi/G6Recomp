#pragma once

// d3d12_xs.dll, the console's D3D12.X. Every export is a stub returning E_NOTIMPL, except the
// ones here: what the executable calls before it creates the device, and the device itself,
// which runtime/gpu presents (docs/d3d12x.md, "Our device"). Console-only, so the names come
// from the import table.

#include <cstddef>
#include <cstdint>
#include <unknwn.h>
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

// Words of the device parameter block that get logged. Leonida reports a 24-word block; its full
// size and meaning aren't known, so it is only logged.
inline constexpr size_t kCreateDeviceParameterWords = 24;

// Creates the title's device: runtime/gpu's proxy over a PC device on the host's default
// adapter, returned as `riid`. The adapter and the parameter block are logged and not used.
HRESULT D3D12XboxCreateDevice(IUnknown* adapter, const void* parameters, const IID* riid,
                              void** device);

// Binds the functions above over d3d12_xs.dll's stubs.
void register_d3d12_xs(loader::ImportRegistry& imports);

} // namespace g6::imports::d3d12_xs
