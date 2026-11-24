// d3d12_xs.dll before the device (g6/imports/d3d12_xs.h).

#include <cstdint>
#include <cstring>

#include "g6/imports/d3d12_xs.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"

namespace g6::imports::d3d12_xs {

HRESULT D3D12ConfigureVirtualMemoryX(const void* description) {
    if (description == nullptr) {
        return E_INVALIDARG;
    }
    uint32_t words[8];
    std::memcpy(words, description, sizeof(words));
    LOG_INFO(d3d12,
             "D3D12ConfigureVirtualMemoryX({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x}, "
             "{:#x}): accepted, nothing to configure on the host",
             words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
    return S_OK;
}

void register_d3d12_xs(loader::ImportRegistry& imports) {
    imports.hle("d3d12_xs.dll", loader::Symbol::by_name("D3D12ConfigureVirtualMemoryX"),
                reinterpret_cast<const void*>(&D3D12ConfigureVirtualMemoryX));
    imports.hle("d3d12_xs.dll", loader::Symbol::by_name("D3D12XboxCreateDevice"),
                reinterpret_cast<const void*>(&D3D12XboxCreateDevice));
}

} // namespace g6::imports::d3d12_xs
