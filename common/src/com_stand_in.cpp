// Stand-in COM objects (g6/com_stand_in.h).

#include "g6/com_stand_in.h"

#include <cstring>
#include <intrin.h>
#include <utility>

#include "g6/log.h"

namespace g6::com {
namespace {

// IUnknown's IID, spelled out so the common library needs no COM import library.
constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

bool same(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

HRESULT STDMETHODCALLTYPE query_interface(StandIn* self, const GUID* iid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (iid == nullptr) {
        return E_INVALIDARG;
    }
    if (same(*iid, kIUnknown) || (same(*iid, self->iid) && !same(self->iid, GUID{}))) {
        add_ref(*self);
        *object = self;
        return S_OK;
    }
    LOG_WARN(com,
             "{}::QueryInterface({:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}"
             "{:02x}) from {}: not served by the stand-in",
             self->name, iid->Data1, iid->Data2, iid->Data3, iid->Data4[0], iid->Data4[1],
             iid->Data4[2], iid->Data4[3], iid->Data4[4], iid->Data4[5], iid->Data4[6],
             iid->Data4[7], _ReturnAddress());
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE add_ref_slot(StandIn* self) {
    return add_ref(*self);
}

ULONG STDMETHODCALLTYPE release(StandIn* self) {
    ULONG count = self->references.load(std::memory_order_relaxed);
    while (count != 0 &&
           !self->references.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel)) {
    }
    if (count == 0) {
        return 0; // a Release too many: never below zero, and never a second destroy
    }
    if (count == 1 && self->destroy != nullptr) {
        self->destroy(self);
    }
    return count - 1;
}

template <size_t Slot>
HRESULT STDMETHODCALLTYPE unimplemented(StandIn* self) {
    if (!self->reported[Slot].exchange(true, std::memory_order_relaxed)) {
        LOG_WARN(com, "{} slot {} called from {}: not in the stand-in, E_NOTIMPL", self->name, Slot,
                 _ReturnAddress());
    }
    return E_NOTIMPL;
}

template <size_t... Slots>
StandInTable stubs(std::index_sequence<Slots...> /*slots*/) noexcept {
    return {reinterpret_cast<const void*>(&unimplemented<Slots>)...};
}

} // namespace

StandInTable stand_in_table() noexcept {
    StandInTable table = stubs(std::make_index_sequence<kStandInSlots>{});
    table[0] = reinterpret_cast<const void*>(&query_interface);
    table[1] = reinterpret_cast<const void*>(&add_ref_slot);
    table[2] = reinterpret_cast<const void*>(&release);
    return table;
}

ULONG add_ref(StandIn& object) noexcept {
    return object.references.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace g6::com
