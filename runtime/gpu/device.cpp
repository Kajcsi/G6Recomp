// The title's D3D12 device (g6/gpu/device.h, docs/d3d12x.md "Our device").

#include "g6/gpu/device.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <intrin.h>
#include <utility>

#include "command.h"
#include "g6/log.h"
#include "proxy.h"

namespace g6::gpu {
namespace {

static_assert(kDeviceSlots == detail::kProxySlots);

// The object the title holds. The forwarding thunks read the PC device at +8, and the upscaler
// reads the byte at +0x18 before it copies a descriptor (docs/d3d12x.md, "Descriptor copies").
struct DeviceProxy {
    const void* const* vtable;
    ID3D12Device* real;
    std::atomic<ULONG> references;
    uint32_t reserved;
    uint8_t copies_through_device; // nonzero: descriptor copies go through slot 24
    std::array<uint8_t, 7> padding;
};
static_assert(offsetof(DeviceProxy, real) == 8);
static_assert(offsetof(DeviceProxy, copies_through_device) == 0x18);

// ---- Live proxies and IUnknown ----

detail::LiveSet& live() {
    static detail::LiveSet set;
    return set;
}

bool is_device_interface(const GUID& iid) {
    static const std::array<GUID, 17> kInterfaces = {
        __uuidof(IUnknown),       __uuidof(ID3D12Object),   __uuidof(ID3D12Device),
        __uuidof(ID3D12Device1),  __uuidof(ID3D12Device2),  __uuidof(ID3D12Device3),
        __uuidof(ID3D12Device4),  __uuidof(ID3D12Device5),  __uuidof(ID3D12Device6),
        __uuidof(ID3D12Device7),  __uuidof(ID3D12Device8),  __uuidof(ID3D12Device9),
        __uuidof(ID3D12Device10), __uuidof(ID3D12Device11), __uuidof(ID3D12Device12),
        __uuidof(ID3D12Device13), __uuidof(ID3D12Device14),
    };
    return detail::any_of(iid, kInterfaces);
}

// Answers the device interfaces the PC device has, with the proxy itself.
HRESULT answer(DeviceProxy* self, const GUID& iid, void** out) {
    if (!is_device_interface(iid)) {
        return E_NOINTERFACE;
    }
    IUnknown* probe = nullptr;
    if (FAILED(self->real->QueryInterface(iid, reinterpret_cast<void**>(&probe)))) {
        return E_NOINTERFACE;
    }
    probe->Release();
    self->references.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE query_interface(DeviceProxy* self, const GUID* iid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (iid == nullptr) {
        return E_INVALIDARG;
    }
    const HRESULT result = answer(self, *iid, out);
    if (FAILED(result)) {
        detail::log_refused_interface("device", *iid, _ReturnAddress());
    }
    return result;
}

ULONG STDMETHODCALLTYPE add_ref(DeviceProxy* self) {
    return self->references.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE release(DeviceProxy* self) {
    ULONG count = self->references.load(std::memory_order_relaxed);
    while (count != 0 &&
           !self->references.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel)) {
    }
    if (count == 0) {
        return 0; // a Release too many: never below zero, and never a second free
    }
    if (count == 1) {
        live().erase(self);
        self->real->Release();
        delete self;
    }
    return count - 1;
}

// The proxy as a COM object: its table starts with IUnknown's three slots.
IUnknown* as_unknown(DeviceProxy* self) {
    return reinterpret_cast<IUnknown*>(self);
}

// ---- Methods that return structures ----

// On the PC these methods take the device first and the return buffer second. The title passes
// the buffer first and the device second (docs/boot.md, "D3D12 device calls that differ from the
// PC"). Both forms come here, and the device is whichever of the two is a live proxy. The other
// arguments sit in the same places either way, and the buffer goes back in rax in both.
DeviceProxy* device_of(void*& first, void*& second) {
    if (live().contains(first)) {
        return static_cast<DeviceProxy*>(first);
    }
    if (live().contains(second)) {
        std::swap(first, second);
        return static_cast<DeviceProxy*>(first);
    }
    return nullptr;
}

void log_no_device(unsigned slot, const void* caller) {
    LOG_ERROR(d3d12, "device slot {} called from {} without a device in either argument", slot,
              caller);
}

// Slot 25, GetResourceAllocationInfo.
D3D12_RESOURCE_ALLOCATION_INFO* resource_allocation_info(void* first, void* second,
                                                         UINT visible_mask, UINT count,
                                                         const D3D12_RESOURCE_DESC* descs) {
    DeviceProxy* device = device_of(first, second);
    if (device == nullptr) {
        log_no_device(25, _ReturnAddress());
        return nullptr;
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        device->real->GetResourceAllocationInfo(visible_mask, count, descs);
    auto* buffer = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(second);
    *buffer = info;
    return buffer;
}

// Slot 26, GetCustomHeapProperties.
D3D12_HEAP_PROPERTIES* custom_heap_properties(void* first, void* second, UINT node_mask,
                                              D3D12_HEAP_TYPE type) {
    DeviceProxy* device = device_of(first, second);
    if (device == nullptr) {
        log_no_device(26, _ReturnAddress());
        return nullptr;
    }
    const D3D12_HEAP_PROPERTIES properties = device->real->GetCustomHeapProperties(node_mask, type);
    auto* buffer = static_cast<D3D12_HEAP_PROPERTIES*>(second);
    *buffer = properties;
    return buffer;
}

// ---- Command queues and lists (docs/d3d12x.md, "Command queues and lists") ----

// The console's list and queue types add one for its copy engine, which the PC calls COPY.
constexpr uint32_t kConsoleCopyEngine = 0x10;

D3D12_COMMAND_LIST_TYPE pc_list_type(uint32_t type) {
    return type == kConsoleCopyEngine ? D3D12_COMMAND_LIST_TYPE_COPY
                                      : static_cast<D3D12_COMMAND_LIST_TYPE>(type);
}

// One adapter: any node mask the title gives is the first node.
UINT pc_node_mask(uint32_t mask) {
    return mask == 0 ? 0 : 1;
}

// The console's queue description has the PC's four fields. Its flags have bits the PC doesn't
// (run 10 makes compute queues with 2 and 3), which it refuses, so only DISABLE_GPU_TIMEOUT is
// kept; a priority the PC doesn't have becomes NORMAL.
D3D12_COMMAND_QUEUE_DESC pc_queue_desc(const D3D12_COMMAND_QUEUE_DESC& title) {
    D3D12_COMMAND_QUEUE_DESC pc{};
    pc.Type = pc_list_type(static_cast<uint32_t>(title.Type));
    pc.Priority = title.Priority;
    if (title.Priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL &&
        title.Priority != D3D12_COMMAND_QUEUE_PRIORITY_HIGH &&
        title.Priority != D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
        LOG_WARN(d3d12, "command queue priority {} is not the PC's: NORMAL", title.Priority);
        pc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    }
    const auto flags = static_cast<uint32_t>(title.Flags);
    pc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(flags &
                                                      D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT);
    if ((flags & ~static_cast<uint32_t>(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT)) != 0) {
        LOG_DEBUG(d3d12, "command queue flags {:#x} (type {}): the console's bits are dropped",
                  flags, static_cast<int>(title.Type));
    }
    pc.NodeMask = pc_node_mask(title.NodeMask);
    return pc;
}

HRESULT queue_for(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC& title, const GUID* riid,
                  void** out) {
    if (out == nullptr || riid == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    const D3D12_COMMAND_QUEUE_DESC desc = pc_queue_desc(title);
    ID3D12CommandQueue* queue = nullptr;
    HRESULT result = self->real->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                                                    reinterpret_cast<void**>(&queue));
    if (FAILED(result)) {
        LOG_ERROR(d3d12, "CreateCommandQueue(type {}, priority {}, flags {:#x}) failed: {:#x}",
                  static_cast<int>(desc.Type), desc.Priority, static_cast<unsigned>(desc.Flags),
                  static_cast<uint32_t>(result));
        return result;
    }
    result = detail::wrap_queue(queue, as_unknown(self), *riid, out);
    if (FAILED(result)) {
        queue->Release();
    }
    return result;
}

// Slot 8, CreateCommandQueue, with the PC's arguments and the console's types and flags.
HRESULT STDMETHODCALLTYPE create_command_queue(DeviceProxy* self,
                                               const D3D12_COMMAND_QUEUE_DESC* desc,
                                               const GUID* riid, void** out) {
    if (desc == nullptr) {
        return E_INVALIDARG;
    }
    return queue_for(self, *desc, riid, out);
}

// Slot 9, CreateCommandAllocator, with the console's types.
HRESULT STDMETHODCALLTYPE create_command_allocator(DeviceProxy* self, uint32_t type,
                                                   const GUID* riid, void** out) {
    if (out == nullptr || riid == nullptr) {
        return E_POINTER;
    }
    return self->real->CreateCommandAllocator(pc_list_type(type), *riid, out);
}

// Slot 48, the console's CreateCommandQueueX: a description that starts with the PC's and goes
// on with words of the console's own, which are logged.
HRESULT STDMETHODCALLTYPE create_command_queue_x(DeviceProxy* self, const void* desc,
                                                 const GUID* riid, void** out) {
    if (desc == nullptr) {
        return E_INVALIDARG;
    }
    D3D12_COMMAND_QUEUE_DESC title{};
    std::memcpy(&title, desc, sizeof(title));
    std::array<uint32_t, 4> more{};
    std::memcpy(more.data(), static_cast<const uint8_t*>(desc) + sizeof(title), sizeof(more));
    LOG_DEBUG(d3d12,
              "device slot 48 (CreateCommandQueueX): type {}, priority {}, flags {:#x}, node mask "
              "{:#x}, then {:#x} {:#x} {:#x} {:#x}",
              static_cast<int>(title.Type), title.Priority, static_cast<unsigned>(title.Flags),
              title.NodeMask, more[0], more[1], more[2], more[3]);
    return queue_for(self, title, riid, out);
}

HRESULT list_for(DeviceProxy* self, uint32_t node_mask, uint32_t title_type,
                 ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial, const GUID* riid,
                 void** out) {
    if (out == nullptr || riid == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    const D3D12_COMMAND_LIST_TYPE type = pc_list_type(title_type);
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT result = self->real->CreateCommandList(pc_node_mask(node_mask), type, allocator,
                                                   initial, __uuidof(ID3D12GraphicsCommandList),
                                                   reinterpret_cast<void**>(&list));
    if (FAILED(result)) {
        LOG_ERROR(d3d12, "CreateCommandList(type {}) failed: {:#x}", static_cast<int>(type),
                  static_cast<uint32_t>(result));
        return result;
    }
    result = detail::wrap_list(list, as_unknown(self), *riid, out);
    if (FAILED(result)) {
        list->Release();
    }
    return result;
}

// Slot 12, CreateCommandList, with the PC's arguments and the console's types.
HRESULT STDMETHODCALLTYPE create_command_list(DeviceProxy* self, UINT node_mask, uint32_t type,
                                              ID3D12CommandAllocator* allocator,
                                              ID3D12PipelineState* initial, const GUID* riid,
                                              void** out) {
    return list_for(self, node_mask, type, allocator, initial, riid, out);
}

// Slot 73, the console's CreateCommandListX: a 16-byte description with the list type at +0 and
// the node mask at +8 (run 10: 0, 2, 1, 0), then the allocator, the initial pipeline state, and
// an interface the PC SDK doesn't have.
HRESULT STDMETHODCALLTYPE create_command_list_x(DeviceProxy* self, const void* desc,
                                                ID3D12CommandAllocator* allocator,
                                                ID3D12PipelineState* initial, const GUID* riid,
                                                void** out) {
    if (desc == nullptr) {
        return E_INVALIDARG;
    }
    std::array<uint32_t, 4> words{};
    std::memcpy(words.data(), desc, sizeof(words));
    static std::atomic<bool> seen{false};
    if (riid != nullptr && !seen.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(d3d12,
                 "device slot 73 (CreateCommandListX) first called: description {:#x} {:#x} "
                 "{:#x} {:#x}, interface {:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}"
                 "{:02x}{:02x}{:02x}",
                 words[0], words[1], words[2], words[3], riid->Data1, riid->Data2, riid->Data3,
                 riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3], riid->Data4[4],
                 riid->Data4[5], riid->Data4[6], riid->Data4[7]);
    }
    return list_for(self, words[2], words[0], allocator, initial, riid, out);
}

// ---- The table ----

// The base ID3D12Device methods keep their PC slots on the console, 3 to 42 here. Slot 43 returns
// a structure in a form not seen yet, and the console's table differs from the PC's from 44 on, so
// those stay logged until each is measured.
const void* const* table() {
    static const void* const* const kTable = [] {
        detail::ProxyTable table(detail::ProxyKind::device);
        table.set(0, reinterpret_cast<const void*>(&query_interface));
        table.set(1, reinterpret_cast<const void*>(&add_ref));
        table.set(2, reinterpret_cast<const void*>(&release));
        table.forward_range(3, 43);
        table.set(8, reinterpret_cast<const void*>(&create_command_queue));
        table.set(9, reinterpret_cast<const void*>(&create_command_allocator));
        table.set(12, reinterpret_cast<const void*>(&create_command_list));
        table.set(25, reinterpret_cast<const void*>(&resource_allocation_info));
        table.set(26, reinterpret_cast<const void*>(&custom_heap_properties));
        table.set(48, reinterpret_cast<const void*>(&create_command_queue_x));
        table.set(73, reinterpret_cast<const void*>(&create_command_list_x));
        return table.finish();
    }();
    return kTable;
}

} // namespace

HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (real == nullptr) {
        return E_INVALIDARG;
    }
    auto* proxy = new DeviceProxy{};
    proxy->vtable = table();
    proxy->real = real;
    proxy->copies_through_device = 1;
    live().insert(proxy);
    const HRESULT result = answer(proxy, riid, out);
    if (FAILED(result)) {
        detail::log_refused_interface("device", riid, _ReturnAddress());
        live().erase(proxy);
        delete proxy;
    }
    return result;
}

HRESULT create_device(REFIID riid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    ID3D12Device* real = nullptr;
    HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                       reinterpret_cast<void**>(&real));
    if (FAILED(result)) {
        LOG_ERROR(d3d12, "D3D12CreateDevice on the default adapter failed: {:#x}",
                  static_cast<uint32_t>(result));
        return result;
    }
    result = create_device_proxy(real, riid, out);
    if (FAILED(result)) {
        real->Release();
        return result;
    }
    LOG_INFO(d3d12, "device {} over the PC device {}", *out, static_cast<void*>(real));
    return S_OK;
}

ID3D12Device* device_proxy_real(const void* device) {
    if (!live().contains(device)) {
        return nullptr;
    }
    return static_cast<const DeviceProxy*>(device)->real;
}

} // namespace g6::gpu
