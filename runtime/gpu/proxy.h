#pragma once

// The tables behind the proxies the title holds in place of PC D3D12 objects (docs/d3d12x.md,
// "Our device"). Each proxy keeps the PC object at +8. A known slot is a thunk that swaps the
// proxy for the PC object and jumps through one of its slots; a slot not known yet logs its first
// call and fails; the rest are C++ functions of the proxy's own.

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <windows.h>

namespace g6::gpu::detail {

// Slots in every table: more than the highest slot the title is known to call (device 133,
// command list 146).
inline constexpr unsigned kProxySlots = 192;

enum class ProxyKind : uint8_t { device, queue, list };

// One table, built once per kind. It ends at a page boundary, before a page with no access, so a
// slot past it faults at a clear address. Every slot starts as the logging stub.
class ProxyTable {
public:
    explicit ProxyTable(ProxyKind kind);

    // `slot` swaps the proxy for the PC object at +8 and jumps through the PC object's
    // `pc_slot`. The thunk never touches the stack, so arguments and the return value pass
    // straight through and it needs no unwind data.
    void forward(unsigned slot, unsigned pc_slot);
    // Forwards `first` to `end - 1` to the same slots shifted by `shift`.
    void forward_range(unsigned first, unsigned end, int shift = 0);
    void set(unsigned slot, const void* function);
    // Makes the thunks executable and the table read-only, and returns the table.
    const void* const* finish();

private:
    uint8_t* region_;
    const void** table_ = nullptr;
};

// The live proxies of one kind, so a pointer can be checked without reading through it.
class LiveSet {
public:
    void insert(const void* proxy);
    void erase(const void* proxy);
    bool contains(const void* pointer) const;

private:
    mutable std::mutex mutex_;
    std::unordered_set<const void*> proxies_;
};

bool same(const GUID& a, const GUID& b);
bool any_of(const GUID& iid, const GUID* known, size_t count);

template <size_t Count>
bool any_of(const GUID& iid, const std::array<GUID, Count>& known) {
    return any_of(iid, known.data(), Count);
}

// Logs an interface a proxy doesn't serve.
void log_refused_interface(const char* object, const GUID& iid, const void* caller);

} // namespace g6::gpu::detail
