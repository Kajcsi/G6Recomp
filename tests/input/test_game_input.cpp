// The GameInput stand-in (g6/input/game_input.h): one object with a working IUnknown and
// GetCurrentTimestamp, and every other slot an honest E_NOTIMPL.

#include <cstdint>
#include <windows.h>

#include "g6/com_stand_in.h"
#include "g6/input/game_input.h"
#include "g6test.h"

using g6::input::GameInputCreate;

namespace {

struct Object {
    void* const* vtable;
};

template <typename Function>
Function slot(Object* object, size_t index) {
    return reinterpret_cast<Function>(object->vtable[index]);
}

using QueryInterface = HRESULT(STDMETHODCALLTYPE*)(Object*, const GUID*, void**);
using Count = ULONG(STDMETHODCALLTYPE*)(Object*);
using Timestamp = uint64_t(STDMETHODCALLTYPE*)(Object*);
// What the executable passes to slot 9 doesn't matter to a stub; seven arguments, as many as
// any GameInput callback registration takes, check that extra ones are harmless.
using Unimplemented = HRESULT(STDMETHODCALLTYPE*)(Object*, void*, uint32_t, uint32_t, uint32_t,
                                                  void*, void*, uint64_t*);

} // namespace

TEST_CASE("GameInputCreate hands out the stand-in with a reference") {
    CHECK_EQ(GameInputCreate(nullptr), E_POINTER);
    void* first = nullptr;
    void* second = nullptr;
    REQUIRE(GameInputCreate(&first) == S_OK);
    REQUIRE(GameInputCreate(&second) == S_OK);
    CHECK(first != nullptr);
    CHECK(first == second); // one object, as the host's GameInputCreate gives
    auto* object = static_cast<Object*>(first);
    const ULONG after_add = slot<Count>(object, 1)(object);
    CHECK_EQ(slot<Count>(object, 2)(object), after_add - 1);
    slot<Count>(object, 2)(object);
    slot<Count>(object, 2)(object);
}

TEST_CASE("the stand-in answers IUnknown and the timestamp, and nothing else") {
    void* game_input = nullptr;
    REQUIRE(GameInputCreate(&game_input) == S_OK);
    auto* object = static_cast<Object*>(game_input);

    // NOLINTNEXTLINE(misc-const-correctness): QueryInterface writes through it.
    void* other = reinterpret_cast<void*>(1);
    const GUID some_interface = {0x11111111, 0x2222, 0x3333, {4, 4, 5, 5, 6, 6, 7, 7}};
    CHECK_EQ(slot<QueryInterface>(object, 0)(object, &some_interface, &other), E_NOINTERFACE);
    CHECK(other == nullptr);
    CHECK_EQ(slot<QueryInterface>(object, 0)(object, &some_interface, nullptr), E_POINTER);

    // Microseconds on the performance counter: close to QPC's own reading, and never backwards.
    LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    const uint64_t first = slot<Timestamp>(object, 3)(object);
    QueryPerformanceCounter(&now);
    const double expected =
        static_cast<double>(now.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
    CHECK(static_cast<double>(first) <= expected + 1.0);
    CHECK(expected - static_cast<double>(first) < 1e6);
    Sleep(2);
    CHECK(slot<Timestamp>(object, 3)(object) > first);

    for (size_t index = 4; index < g6::com::kStandInSlots; ++index) {
        INFO(fmt::format("slot {}", index));
        uint64_t token = 0;
        CHECK_EQ(
            slot<Unimplemented>(object, index)(object, nullptr, 0, 0, 0, nullptr, nullptr, &token),
            E_NOTIMPL);
        CHECK_EQ(token, uint64_t{0});
    }
    slot<Count>(object, 2)(object);
}
