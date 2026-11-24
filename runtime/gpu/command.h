#pragma once

// Command queue and command list proxies (docs/d3d12x.md, "Command queues and lists"). The device
// proxy makes them; each holds a reference to the device proxy, which its GetDevice hands out.

#include <d3d12.h>
#include <windows.h>

namespace g6::gpu::detail {

// Wraps `real` in a queue proxy and returns it as `riid`, which the proxy then also answers. On
// success the proxy owns the caller's reference to `real`; on failure the caller keeps it.
HRESULT wrap_queue(ID3D12CommandQueue* real, IUnknown* device, REFIID riid, void** out);

// The same for a command list. The title asks for its lists by an interface the PC SDK doesn't
// have, so `riid` isn't checked: the proxy answers it along with the PC list interfaces.
HRESULT wrap_list(ID3D12GraphicsCommandList* real, IUnknown* device, REFIID riid, void** out);

} // namespace g6::gpu::detail
