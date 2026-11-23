#pragma once

// Stand-in COM objects, for stubbing an interface before implementing it (CLAUDE.md, "Stub
// before implementing"). When a console API hands the title an object, a stand-in lets the boot
// go on and shows which methods the title calls: IUnknown works, the slots an owner fills in do
// what it says, and every other slot logs its number and caller once per object and returns
// E_NOTIMPL. A method that isn't an HRESULT gets that value in rax too, so an owner fills in
// each such slot once the log shows the title calling it.

#include <array>
#include <atomic>
#include <cstddef>
#include <windows.h>

namespace g6::com {

// Slots in a stand-in's table: more than the interfaces stubbed so far have, so a call to a
// slot nobody knows lands in a stub instead of past the table's end.
inline constexpr size_t kStandInSlots = 64;

using StandInTable = std::array<const void*, kStandInSlots>;

// The object. The vtable pointer comes first, as COM lays it out; the rest is ours. An object
// with state of its own puts a StandIn first and its own members after it, and casts the
// StandIn* its slots get back to its own type.
struct StandIn {
    // Called by the Release that drops the last reference, to free an object made on the heap.
    using Destroy = void (*)(StandIn* object) noexcept;

    StandIn(const StandInTable& table, const char* name, const GUID& iid,
            Destroy destroy = nullptr) noexcept
        : vtable(table.data()), name(name), iid(iid), destroy(destroy) {}

    const void* const* vtable;
    const char* name; // the interface's name, for the log
    GUID iid;         // the interface it is: QueryInterface answers this and IUnknown
    Destroy destroy;  // null for a static object, which the last Release leaves alone
    std::atomic<ULONG> references{0};
    std::array<std::atomic<bool>, kStandInSlots> reported{};
};

// A table with IUnknown in slots 0 to 2 and the logging stub in every other slot. Owners replace
// the slots they implement.
StandInTable stand_in_table() noexcept;

// AddRef, for owners handing a stand-in out.
ULONG add_ref(StandIn& object) noexcept;

} // namespace g6::com
