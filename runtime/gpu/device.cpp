// The title's D3D12 device (g6/gpu/device.h, docs/d3d12x.md "Our device").

#include "g6/gpu/device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "g6/log.h"

namespace g6::gpu {
namespace {

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

// The base ID3D12Device methods keep their PC slots on the console. Slots 25 and 26 return
// structures and take the console's argument order (below), and 43 does too but isn't handled
// yet, so the thunks cover 3 to 42 apart from 25 and 26.
constexpr unsigned kFirstForwarded = 3;
constexpr unsigned kEndForwarded = 43;

// ---- Live proxies ----

std::mutex& live_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<const void*>& live() {
    static std::unordered_set<const void*> proxies;
    return proxies;
}

bool is_live(const void* pointer) {
    const std::scoped_lock lock(live_mutex());
    return live().contains(pointer);
}

// ---- IUnknown ----

bool same(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
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
    for (const GUID& known : kInterfaces) {
        if (same(iid, known)) {
            return true;
        }
    }
    return false;
}

void log_refused_interface(const GUID& iid, const void* caller) {
    LOG_WARN(
        d3d12,
        "device QueryInterface({:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}"
        "{:02x}) from {}: not served by the proxy",
        iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
        iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7], caller);
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
        log_refused_interface(*iid, _ReturnAddress());
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
        {
            const std::scoped_lock lock(live_mutex());
            live().erase(self);
        }
        self->real->Release();
        delete self;
    }
    return count - 1;
}

// ---- Methods that return structures ----

// On the PC these methods take the device first and the return buffer second. The title passes
// the buffer first and the device second (docs/boot.md, "D3D12 device calls that differ from the
// PC"). Both forms come here, and the device is whichever of the two is a live proxy. The other
// arguments sit in the same places either way, and the buffer goes back in rax in both.
DeviceProxy* device_of(void*& first, void*& second) {
    if (is_live(first)) {
        return static_cast<DeviceProxy*>(first);
    }
    if (is_live(second)) {
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

// ---- Slots not known yet ----

std::array<std::atomic<bool>, kDeviceSlots>& reported() {
    static std::array<std::atomic<bool>, kDeviceSlots> flags{};
    return flags;
}

template <unsigned Slot>
HRESULT STDMETHODCALLTYPE unknown_slot(void* /*self*/, uint64_t a, uint64_t b, uint64_t c) {
    if (!reported()[Slot].exchange(true, std::memory_order_relaxed)) {
        LOG_WARN(d3d12,
                 "device slot {} called from {} with {:#x}, {:#x}, {:#x}: not implemented, "
                 "E_NOTIMPL",
                 Slot, _ReturnAddress(), a, b, c);
    }
    return E_NOTIMPL;
}

template <unsigned... Slots>
std::array<const void*, kDeviceSlots>
unknown_slots(std::integer_sequence<unsigned, Slots...> /*slots*/) {
    return {reinterpret_cast<const void*>(&unknown_slot<Slots>)...};
}

// ---- The table ----

constexpr size_t kPage = 4096;
constexpr size_t kThunkSize = 16;
static_assert(kEndForwarded * kThunkSize <= kPage);
static_assert(kDeviceSlots * sizeof(void*) <= kPage);

// mov rcx, [rcx+8] ; mov rax, [rcx] ; jmp [rax + slot*8]
// The thunk swaps the proxy for the PC device and jumps to the PC method, which returns straight
// to the title. It never touches the stack, so it needs no unwind data.
void write_forward(uint8_t* at, unsigned slot) {
    constexpr std::array<uint8_t, 9> kHead = {0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0xA0};
    std::memset(at, 0xCC, kThunkSize);
    std::memcpy(at, kHead.data(), kHead.size());
    const uint32_t displacement = slot * sizeof(void*);
    std::memcpy(at + kHead.size(), &displacement, sizeof(displacement));
}

// Three pages: the thunks, then the table at the end of the second page, then a page with no
// access, so a slot past the table faults at a clear address.
const void* const* build_table() {
    auto* region =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, 3 * kPage, MEM_RESERVE, PAGE_NOACCESS));
    if (region == nullptr ||
        VirtualAlloc(region, 2 * kPage, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        throw std::runtime_error("cannot allocate the device proxy's table");
    }
    uint8_t* thunks = region;
    auto* table =
        reinterpret_cast<const void**>(region + 2 * kPage - (kDeviceSlots * sizeof(void*)));

    const std::array<const void*, kDeviceSlots> unknown =
        unknown_slots(std::make_integer_sequence<unsigned, kDeviceSlots>{});
    std::ranges::copy(unknown, table);
    table[0] = reinterpret_cast<const void*>(&query_interface);
    table[1] = reinterpret_cast<const void*>(&add_ref);
    table[2] = reinterpret_cast<const void*>(&release);
    for (unsigned slot = kFirstForwarded; slot < kEndForwarded; ++slot) {
        uint8_t* thunk = thunks + (slot * kThunkSize);
        write_forward(thunk, slot);
        table[slot] = thunk;
    }
    table[25] = reinterpret_cast<const void*>(&resource_allocation_info);
    table[26] = reinterpret_cast<const void*>(&custom_heap_properties);

    DWORD old = 0;
    if (!VirtualProtect(thunks, kPage, PAGE_EXECUTE_READ, &old) ||
        !VirtualProtect(region + kPage, kPage, PAGE_READONLY, &old)) {
        throw std::runtime_error("cannot protect the device proxy's table");
    }
    FlushInstructionCache(GetCurrentProcess(), thunks, kPage);
    return table;
}

const void* const* table() {
    static const void* const* const kTable = build_table();
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
    {
        const std::scoped_lock lock(live_mutex());
        live().insert(proxy);
    }
    const HRESULT result = answer(proxy, riid, out);
    if (FAILED(result)) {
        log_refused_interface(riid, _ReturnAddress());
        {
            const std::scoped_lock lock(live_mutex());
            live().erase(proxy);
        }
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
    if (!is_live(device)) {
        return nullptr;
    }
    return static_cast<const DeviceProxy*>(device)->real;
}

} // namespace g6::gpu
