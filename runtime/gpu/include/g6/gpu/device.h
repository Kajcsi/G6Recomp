#pragma once

// The title's D3D12 device (docs/d3d12x.md, "Our device"). The title calls its device through
// the console's vtable, which keeps the PC order for the base ID3D12Device methods and differs
// after them. So it gets a proxy over a PC device: the base slots go straight to the PC device,
// the slots that differ are handled here, and every slot not known yet logs its first call and
// fails.

#include <cstdint>
#include <d3d12.h>
#include <windows.h>

namespace g6::gpu {

// Slots in the proxy's table: more than the highest slot the title is known to call (133).
inline constexpr unsigned kDeviceSlots = 192;

// Creates a PC device on the default adapter and returns its proxy as `riid`, which has to be
// IUnknown, ID3D12Object, or an ID3D12Device interface the PC device has.
HRESULT create_device(REFIID riid, void** out);

// Wraps `real` and returns the proxy as `riid`. On success the proxy owns the reference the
// caller passed in; on failure the caller keeps it.
HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out);

// The PC device behind a proxy (no reference added), or null when `device` isn't a live proxy.
ID3D12Device* device_proxy_real(const void* device);

// The same for the command queues and lists the device makes (docs/d3d12x.md, "Command queues
// and lists").
ID3D12CommandQueue* command_queue_proxy_real(const void* queue);
ID3D12GraphicsCommandList* command_list_proxy_real(const void* list);

} // namespace g6::gpu
