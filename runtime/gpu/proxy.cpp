// The proxies' tables (proxy.h).

#include "proxy.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <stdexcept>
#include <utility>

#include "g6/log.h"

namespace g6::gpu::detail {
namespace {

constexpr size_t kPage = 4096;
constexpr size_t kThunkSize = 16;
static_assert(kProxySlots * kThunkSize <= kPage);
static_assert(kProxySlots * sizeof(void*) <= kPage);

constexpr std::array<const char*, 3> kKindNames = {"device", "command queue", "command list"};

std::array<std::array<std::atomic<bool>, kProxySlots>, kKindNames.size()>& reported() {
    static std::array<std::array<std::atomic<bool>, kProxySlots>, kKindNames.size()> flags{};
    return flags;
}

template <ProxyKind Kind, unsigned Slot>
HRESULT STDMETHODCALLTYPE unknown_slot(void* /*self*/, uint64_t a, uint64_t b, uint64_t c) {
    constexpr auto kind = static_cast<size_t>(Kind);
    if (!reported()[kind][Slot].exchange(true, std::memory_order_relaxed)) {
        LOG_WARN(d3d12,
                 "{} slot {} called from {} with {:#x}, {:#x}, {:#x}: not implemented, E_NOTIMPL",
                 kKindNames[kind], Slot, _ReturnAddress(), a, b, c);
    }
    return E_NOTIMPL;
}

template <ProxyKind Kind, unsigned... Slots>
std::array<const void*, kProxySlots>
unknown_slots(std::integer_sequence<unsigned, Slots...> /*slots*/) {
    return {reinterpret_cast<const void*>(&unknown_slot<Kind, Slots>)...};
}

std::array<const void*, kProxySlots> unknown_slots(ProxyKind kind) {
    constexpr auto slots = std::make_integer_sequence<unsigned, kProxySlots>{};
    switch (kind) {
    case ProxyKind::device: return unknown_slots<ProxyKind::device>(slots);
    case ProxyKind::queue: return unknown_slots<ProxyKind::queue>(slots);
    case ProxyKind::list: return unknown_slots<ProxyKind::list>(slots);
    }
    throw std::logic_error("unknown proxy kind");
}

} // namespace

// Three pages: the thunks, then the table at the end of the second page, then a page with no
// access.
ProxyTable::ProxyTable(ProxyKind kind)
    : region_(static_cast<uint8_t*>(VirtualAlloc(nullptr, 3 * kPage, MEM_RESERVE, PAGE_NOACCESS))) {
    if (region_ == nullptr ||
        VirtualAlloc(region_, 2 * kPage, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        throw std::runtime_error("cannot allocate a D3D12 proxy table");
    }
    table_ = reinterpret_cast<const void**>(region_ + (2 * kPage) - (kProxySlots * sizeof(void*)));
    std::ranges::copy(unknown_slots(kind), table_);
}

// mov rcx, [rcx+8] ; mov rax, [rcx] ; jmp [rax + pc_slot*8]
void ProxyTable::forward(unsigned slot, unsigned pc_slot) {
    constexpr std::array<uint8_t, 9> kHead = {0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0xA0};
    uint8_t* thunk = region_ + (static_cast<size_t>(slot) * kThunkSize);
    std::memset(thunk, 0xCC, kThunkSize);
    std::memcpy(thunk, kHead.data(), kHead.size());
    const uint32_t displacement = pc_slot * sizeof(void*);
    std::memcpy(thunk + kHead.size(), &displacement, sizeof(displacement));
    table_[slot] = thunk;
}

void ProxyTable::forward_range(unsigned first, unsigned end, int shift) {
    for (unsigned slot = first; slot < end; ++slot) {
        forward(slot, static_cast<unsigned>(static_cast<int>(slot) + shift));
    }
}

void ProxyTable::set(unsigned slot, const void* function) {
    table_[slot] = function;
}

const void* const* ProxyTable::finish() {
    DWORD old = 0;
    if (!VirtualProtect(region_, kPage, PAGE_EXECUTE_READ, &old) ||
        !VirtualProtect(region_ + kPage, kPage, PAGE_READONLY, &old)) {
        throw std::runtime_error("cannot protect a D3D12 proxy table");
    }
    FlushInstructionCache(GetCurrentProcess(), region_, kPage);
    return table_;
}

void LiveSet::insert(const void* proxy) {
    const std::scoped_lock lock(mutex_);
    proxies_.insert(proxy);
}

void LiveSet::erase(const void* proxy) {
    const std::scoped_lock lock(mutex_);
    proxies_.erase(proxy);
}

bool LiveSet::contains(const void* pointer) const {
    const std::scoped_lock lock(mutex_);
    return proxies_.contains(pointer);
}

bool same(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

bool any_of(const GUID& iid, const GUID* known, size_t count) {
    return std::any_of(known, known + count, [&](const GUID& each) { return same(iid, each); });
}

void log_refused_interface(const char* object, const GUID& iid, const void* caller) {
    LOG_WARN(d3d12,
             "{} QueryInterface({:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}"
             "{:02x}) from {}: not served by the proxy",
             object, iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2],
             iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7], caller);
}

} // namespace g6::gpu::detail
