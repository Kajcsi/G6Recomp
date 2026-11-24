// d3d12_xs.dll's device creation (g6/imports/d3d12_xs.h).

#include "g6/gpu/device.h"

#include <array>
#include <cstdint>
#include <cstring>

#include <fmt/ranges.h>

#include "g6/imports/d3d12_xs.h"
#include "g6/log.h"

namespace g6::imports::d3d12_xs {

HRESULT D3D12XboxCreateDevice(IUnknown* adapter, const void* parameters, const IID* riid,
                              void** device) {
    if (device == nullptr || riid == nullptr) {
        return E_POINTER;
    }
    *device = nullptr;
    LOG_INFO(d3d12,
             "D3D12XboxCreateDevice(adapter {}, parameters {}, {:08x}-{:04x}-{:04x}-{:02x}{:02x}-"
             "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x})",
             static_cast<void*>(adapter), parameters, riid->Data1, riid->Data2, riid->Data3,
             riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3], riid->Data4[4],
             riid->Data4[5], riid->Data4[6], riid->Data4[7]);
    if (parameters != nullptr) {
        std::array<uint32_t, kCreateDeviceParameterWords> words{};
        std::memcpy(words.data(), parameters, sizeof(words));
        LOG_DEBUG(d3d12, "D3D12XboxCreateDevice parameters: {:#010x}", fmt::join(words, " "));
    }
    if (adapter != nullptr) {
        LOG_WARN(d3d12,
                 "D3D12XboxCreateDevice: the title's adapter {} is not used; the device "
                 "goes on the host's default adapter",
                 static_cast<void*>(adapter));
    }
    return gpu::create_device(*riid, device);
}

} // namespace g6::imports::d3d12_xs
