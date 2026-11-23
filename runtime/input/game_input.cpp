// The GameInput stand-in (g6/input/game_input.h).

#include "g6/input/game_input.h"

#include <cstdint>

#include "g6/com_stand_in.h"
#include "g6/log.h"

namespace g6::input {
namespace {

// Microseconds on the host's performance counter, as GameInput timestamps are.
uint64_t STDMETHODCALLTYPE get_current_timestamp(com::StandIn* /*self*/) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    const auto ticks = static_cast<uint64_t>(now.QuadPart);
    const auto per_second = static_cast<uint64_t>(frequency.QuadPart);
    return ticks / per_second * 1000000 + ticks % per_second * 1000000 / per_second;
}

const com::StandInTable g_table = []() noexcept {
    com::StandInTable table = com::stand_in_table();
    table[3] = reinterpret_cast<const void*>(&get_current_timestamp);
    return table;
}();

// Which IGameInput the executable expects isn't known yet, so only IUnknown is answered.
com::StandIn g_game_input{g_table, "IGameInput", GUID{}};

} // namespace

HRESULT GameInputCreate(void** game_input) {
    if (game_input == nullptr) {
        return E_POINTER;
    }
    LOG_INFO(input, "GameInputCreate: a stand-in with no devices (runtime/input comes at M4)");
    com::add_ref(g_game_input);
    *game_input = &g_game_input;
    return S_OK;
}

} // namespace g6::input
