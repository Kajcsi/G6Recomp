// Stand-in COM objects (g6/com_stand_in.h): IUnknown, the interface they stand for, reference
// counts that never wrap, slots an owner fills in, and E_NOTIMPL from every other slot.

#include <cstdint>
#include <windows.h>

#include "g6/com_stand_in.h"
#include "g6test.h"

using g6::com::StandIn;
using g6::com::StandInTable;

namespace {

constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kStoodFor = {
    0x0a1b2c3d, 0x4e5f, 0x6071, {0x82, 0x93, 0xa4, 0xb5, 0xc6, 0xd7, 0xe8, 0xf9}};
constexpr GUID kOther = {0x11111111, 0x2222, 0x3333, {4, 4, 5, 5, 6, 6, 7, 7}};

using QueryInterface = HRESULT(STDMETHODCALLTYPE*)(StandIn*, const GUID*, void**);
using Count = ULONG(STDMETHODCALLTYPE*)(StandIn*);
using Method = HRESULT(STDMETHODCALLTYPE*)(StandIn*, uint32_t);

template <typename Function>
Function slot(const StandIn& object, size_t index) {
    return reinterpret_cast<Function>(const_cast<void*>(object.vtable[index]));
}

HRESULT STDMETHODCALLTYPE filled_in(StandIn* /*self*/, uint32_t value) {
    return value == 7 ? S_OK : E_INVALIDARG;
}

} // namespace

TEST_CASE("a stand-in answers IUnknown and its own interface") {
    const StandInTable table = g6::com::stand_in_table();
    StandIn object{table, "IStoodFor", kStoodFor};
    const auto query = slot<QueryInterface>(object, 0);

    void* result = nullptr;
    CHECK_EQ(query(&object, &kIUnknown, &result), S_OK);
    CHECK(result == &object);
    CHECK_EQ(query(&object, &kStoodFor, &result), S_OK);
    CHECK(result == &object);
    CHECK_EQ(object.references.load(), ULONG{2});

    CHECK_EQ(query(&object, &kOther, &result), E_NOINTERFACE);
    CHECK(result == nullptr);
    CHECK_EQ(query(&object, nullptr, &result), E_INVALIDARG);
    CHECK_EQ(query(&object, &kIUnknown, nullptr), E_POINTER);
    CHECK_EQ(object.references.load(), ULONG{2});
}

TEST_CASE("a stand-in without an interface answers only IUnknown") {
    const StandInTable table = g6::com::stand_in_table();
    StandIn object{table, "IUnknownYet", GUID{}};
    void* result = nullptr;
    const GUID null_guid{};
    CHECK_EQ(slot<QueryInterface>(object, 0)(&object, &null_guid, &result), E_NOINTERFACE);
    CHECK_EQ(slot<QueryInterface>(object, 0)(&object, &kIUnknown, &result), S_OK);
}

TEST_CASE("references count up and down and stop at zero") {
    const StandInTable table = g6::com::stand_in_table();
    StandIn object{table, "IStoodFor", kStoodFor};
    CHECK_EQ(g6::com::add_ref(object), ULONG{1});
    CHECK_EQ(slot<Count>(object, 1)(&object), ULONG{2});
    CHECK_EQ(slot<Count>(object, 2)(&object), ULONG{1});
    CHECK_EQ(slot<Count>(object, 2)(&object), ULONG{0});
    CHECK_EQ(slot<Count>(object, 2)(&object), ULONG{0}); // one release too many
    CHECK_EQ(object.references.load(), ULONG{0});
}

namespace {

// An object with state of its own, made on the heap, as owners with per-object state make them.
struct Owned {
    StandIn header;
    int* destroyed;
};

void destroy_owned(StandIn* object) noexcept {
    auto* owned = reinterpret_cast<Owned*>(object);
    ++*owned->destroyed;
    delete owned;
}

} // namespace

TEST_CASE("the last release destroys an object made on the heap, once") {
    const StandInTable table = g6::com::stand_in_table();
    int destroyed = 0;
    auto* owned = new Owned{StandIn{table, "IOwned", kStoodFor, &destroy_owned}, &destroyed};
    StandIn& object = owned->header;
    CHECK_EQ(g6::com::add_ref(object), ULONG{1});
    CHECK_EQ(slot<Count>(object, 1)(&object), ULONG{2});
    const auto release = slot<Count>(object, 2);
    CHECK_EQ(release(&object), ULONG{1});
    CHECK_EQ(destroyed, 0);
    CHECK_EQ(release(&object), ULONG{0});
    CHECK_EQ(destroyed, 1);
}

TEST_CASE("filled-in slots work and every other slot is E_NOTIMPL") {
    StandInTable table = g6::com::stand_in_table();
    table[5] = reinterpret_cast<const void*>(&filled_in);
    StandIn object{table, "IStoodFor", kStoodFor};
    CHECK_EQ(slot<Method>(object, 5)(&object, 7), S_OK);
    CHECK_EQ(slot<Method>(object, 5)(&object, 8), E_INVALIDARG);
    for (size_t index = 3; index < g6::com::kStandInSlots; ++index) {
        if (index == 5) {
            continue;
        }
        INFO(fmt::format("slot {}", index));
        CHECK_EQ(slot<Method>(object, index)(&object, 0), E_NOTIMPL);
        CHECK_EQ(slot<Method>(object, index)(&object, 0), E_NOTIMPL); // logged once, same result
        CHECK(object.reported[index].load());
    }
    CHECK(!object.reported[5].load());
}
