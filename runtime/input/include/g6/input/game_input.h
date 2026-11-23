#pragma once

// GameInput, the console's input API. The console's gameinput.dll exports GameInputCreate as
// ordinal 1, as the host's does, and the executable imports it only that way.
//
// For now this is a stand-in (g6/com_stand_in.h), so the boot can go on: an IGameInput with no
// devices. IUnknown and GetCurrentTimestamp (slot 3 in every GameInput version) work; every
// other slot logs once, with its number and caller, and returns E_NOTIMPL. Which GameInput
// version the executable was built against isn't known yet; the slots it calls will tell. The
// real implementation, with keyboard, mouse, and gamepads on the host, comes with runtime/input
// at M4.

#include <windows.h>

namespace g6::input {

// Hands out the one stand-in object, with a reference for the caller.
HRESULT GameInputCreate(void** game_input);

} // namespace g6::input
