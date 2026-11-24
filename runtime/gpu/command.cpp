// Command queue and command list proxies (command.h, docs/d3d12x.md "Command queues and lists").

#include "command.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <intrin.h>
#include <vector>

#include "g6/gpu/device.h"
#include "g6/log.h"
#include "proxy.h"

namespace g6::gpu {
namespace detail {
namespace {

struct QueueProxy {
    const void* const* vtable;
    ID3D12CommandQueue* real;
    std::atomic<ULONG> references;
    IUnknown* device;
    GUID title_interface;
};
static_assert(offsetof(QueueProxy, real) == 8);

// The title's own fields fill a list object up to +0x564 (docs/d3d12x.md, "Command lists write
// records inline"), and ours come after them. They start zeroed, so the write pointer at +0x18 is
// never below the ends at +0x28 and +0x30, and every record goes to a method.
constexpr size_t kListTitleBytes = 0x580;

struct ListProxy {
    const void* const* vtable;
    ID3D12GraphicsCommandList* real;
    std::array<uint8_t, kListTitleBytes - 0x10> title;
    std::atomic<ULONG> references;
    IUnknown* device;
    GUID title_interface;
};
static_assert(offsetof(ListProxy, real) == 8);
static_assert(offsetof(ListProxy, references) == kListTitleBytes);

LiveSet& live_set(const QueueProxy* /*kind*/) {
    static LiveSet set;
    return set;
}

LiveSet& live_set(const ListProxy* /*kind*/) {
    static LiveSet set;
    return set;
}

const char* name_of(const QueueProxy* /*kind*/) {
    return "command queue";
}

const char* name_of(const ListProxy* /*kind*/) {
    return "command list";
}

bool answers(const QueueProxy* self, const GUID& iid) {
    static const std::array<GUID, 5> kInterfaces = {
        __uuidof(IUnknown),       __uuidof(ID3D12Object),       __uuidof(ID3D12DeviceChild),
        __uuidof(ID3D12Pageable), __uuidof(ID3D12CommandQueue),
    };
    return same(iid, self->title_interface) || any_of(iid, kInterfaces);
}

bool answers(const ListProxy* self, const GUID& iid) {
    static const std::array<GUID, 15> kInterfaces = {
        __uuidof(IUnknown),
        __uuidof(ID3D12Object),
        __uuidof(ID3D12DeviceChild),
        __uuidof(ID3D12CommandList),
        __uuidof(ID3D12GraphicsCommandList),
        __uuidof(ID3D12GraphicsCommandList1),
        __uuidof(ID3D12GraphicsCommandList2),
        __uuidof(ID3D12GraphicsCommandList3),
        __uuidof(ID3D12GraphicsCommandList4),
        __uuidof(ID3D12GraphicsCommandList5),
        __uuidof(ID3D12GraphicsCommandList6),
        __uuidof(ID3D12GraphicsCommandList7),
        __uuidof(ID3D12GraphicsCommandList8),
        __uuidof(ID3D12GraphicsCommandList9),
        __uuidof(ID3D12GraphicsCommandList10),
    };
    return same(iid, self->title_interface) || any_of(iid, kInterfaces);
}

// ---- IUnknown and GetDevice, shared by both ----

template <typename Proxy>
ULONG STDMETHODCALLTYPE add_ref(Proxy* self) {
    return self->references.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename Proxy>
ULONG STDMETHODCALLTYPE release(Proxy* self) {
    ULONG count = self->references.load(std::memory_order_relaxed);
    while (count != 0 &&
           !self->references.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel)) {
    }
    if (count == 0) {
        return 0; // a Release too many: never below zero, and never a second free
    }
    if (count == 1) {
        live_set(self).erase(self);
        self->real->Release();
        self->device->Release();
        delete self;
    }
    return count - 1;
}

template <typename Proxy>
HRESULT STDMETHODCALLTYPE query_interface(Proxy* self, const GUID* iid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (iid == nullptr) {
        return E_INVALIDARG;
    }
    if (!answers(self, *iid)) {
        log_refused_interface(name_of(self), *iid, _ReturnAddress());
        return E_NOINTERFACE;
    }
    add_ref(self);
    *out = self;
    return S_OK;
}

// Slot 7, ID3D12DeviceChild::GetDevice: the device proxy, never the PC device behind it.
template <typename Proxy>
HRESULT STDMETHODCALLTYPE get_device(Proxy* self, const GUID* iid, void** out) {
    if (out == nullptr || iid == nullptr) {
        return E_POINTER;
    }
    return self->device->QueryInterface(*iid, out);
}

const void* const* table(const QueueProxy* kind);
const void* const* table(const ListProxy* kind);

template <typename Proxy, typename Real>
HRESULT wrap(Real* real, IUnknown* device, REFIID riid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (real == nullptr || device == nullptr) {
        return E_INVALIDARG;
    }
    auto* proxy = new Proxy{};
    proxy->vtable = table(proxy);
    proxy->real = real;
    proxy->device = device;
    proxy->title_interface = riid;
    device->AddRef();
    live_set(proxy).insert(proxy);
    return query_interface(proxy, &riid, out); // always answered: riid is the title interface
}

// ---- The queue ----

// Slot 10, ExecuteCommandLists: the title hands in list proxies, and the PC queue gets the lists
// behind them. Anything else is left out and logged.
void STDMETHODCALLTYPE execute_command_lists(QueueProxy* self, UINT count,
                                             ID3D12CommandList* const* given) {
    static std::atomic<bool> seen{false};
    if (!seen.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(d3d12,
                 "command queue slot 10 (ExecuteCommandLists) first called from {}: {} lists",
                 _ReturnAddress(), count);
    }
    std::vector<ID3D12CommandList*> real;
    real.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const void* list = given[i];
        if (live_set(static_cast<const ListProxy*>(nullptr)).contains(list)) {
            real.push_back(static_cast<const ListProxy*>(list)->real);
        } else {
            LOG_ERROR(d3d12,
                      "ExecuteCommandLists from {}: {} is not a command list proxy; left out",
                      _ReturnAddress(), list);
        }
    }
    if (!real.empty()) {
        self->real->ExecuteCommandLists(static_cast<UINT>(real.size()), real.data());
    }
}

// The queue keeps the PC order up to slot 13. From 14 on, which of the console's two queue layouts
// the title uses decides what each slot is (docs/d3d12x.md, "Reported by other projects"), so
// those slots stay logged until a call shows the layout.
const void* const* table(const QueueProxy* /*kind*/) {
    static const void* const* const kTable = [] {
        ProxyTable table(ProxyKind::queue);
        table.set(0, reinterpret_cast<const void*>(&query_interface<QueueProxy>));
        table.set(1, reinterpret_cast<const void*>(&add_ref<QueueProxy>));
        table.set(2, reinterpret_cast<const void*>(&release<QueueProxy>));
        table.forward_range(3, 7); // the private data methods and SetName
        table.set(7, reinterpret_cast<const void*>(&get_device<QueueProxy>));
        table.forward_range(8, 10); // UpdateTileMappings, CopyTileMappings
        table.set(10, reinterpret_cast<const void*>(&execute_command_lists));
        table.forward_range(11, 14); // SetMarker, BeginEvent, EndEvent
        return table.finish();
    }();
    return kTable;
}

// The list keeps the PC order up to slot 26, ResourceBarrier. Slots 27 to 30 are the console's
// own. From 32, SetDescriptorHeaps, to 59 the PC methods sit four slots later, as the builder's
// compute root methods do, and 61 is ExecuteIndirect, as the upscaler calls it. Slot 31 would be
// ExecuteBundle, which takes a list, so it stays logged until the title calls it.
const void* const* table(const ListProxy* /*kind*/) {
    static const void* const* const kTable = [] {
        ProxyTable table(ProxyKind::list);
        table.set(0, reinterpret_cast<const void*>(&query_interface<ListProxy>));
        table.set(1, reinterpret_cast<const void*>(&add_ref<ListProxy>));
        table.set(2, reinterpret_cast<const void*>(&release<ListProxy>));
        table.forward_range(3, 7);
        table.set(7, reinterpret_cast<const void*>(&get_device<ListProxy>));
        table.forward_range(8, 27);
        table.forward_range(32, 60, -4);
        table.forward(61, 59);
        return table.finish();
    }();
    return kTable;
}

} // namespace

HRESULT wrap_queue(ID3D12CommandQueue* real, IUnknown* device, REFIID riid, void** out) {
    return wrap<QueueProxy>(real, device, riid, out);
}

HRESULT wrap_list(ID3D12GraphicsCommandList* real, IUnknown* device, REFIID riid, void** out) {
    return wrap<ListProxy>(real, device, riid, out);
}

} // namespace detail

ID3D12CommandQueue* command_queue_proxy_real(const void* queue) {
    using detail::QueueProxy;
    if (!detail::live_set(static_cast<const QueueProxy*>(nullptr)).contains(queue)) {
        return nullptr;
    }
    return static_cast<const QueueProxy*>(queue)->real;
}

ID3D12GraphicsCommandList* command_list_proxy_real(const void* list) {
    using detail::ListProxy;
    if (!detail::live_set(static_cast<const ListProxy*>(nullptr)).contains(list)) {
        return nullptr;
    }
    return static_cast<const ListProxy*>(list)->real;
}

} // namespace g6::gpu
