#pragma once
// Our xgameruntime.dll (docs/gdk.md, "The Game Runtime DLL"). A title doesn't import the Game
// Runtime's functions by name: code linked from the GDK's xgameruntime.lib loads
// xgameruntime.dll, initializes it, and asks QueryApiImpl for one object per service, then calls
// each function through that object's vtable. These are the DLL's entry points, which the
// default import policy binds; the objects behind them carry the functions in runtime/gdk at the
// slots the console's libraries call.

#include <cstdint>
#include <windows.h>

namespace g6::gdk {

// The services and the interface ID each one is asked for. A thunk passes the same GUID as the
// class and as the interface.
inline constexpr GUID kThreadingId = {
    0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
inline constexpr GUID kErrorId = {
    0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}};
inline constexpr GUID kFeatureId = {
    0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}};

// The object for one service, with its own reference: E_POINTER for a null argument,
// REGDB_E_CLASSNOTREG for a service we don't provide yet (logged once per class), and
// E_NOINTERFACE for an interface the service doesn't have. The objects are static, so the
// reference counts are only for the caller's bookkeeping.
HRESULT QueryApiImpl(const GUID* runtime_class, const GUID* interface_id, void** object);

// The initializers xgameruntime.lib tries, newest first. Each packed version is a high and a low
// 32-bit half (docs/gdk.md gives the values); the plain form takes the first one's halves. Each
// logs what it was given and succeeds.
HRESULT InitializeApiImpl(uint32_t version_high, uint32_t version_low);
HRESULT InitializeApiImplEx(uint64_t version, uint64_t os_version, uint8_t flags);
HRESULT InitializeApiImplEx2(uint64_t version, uint64_t os_version, uint8_t flags,
                             const void* options);
HRESULT UninitializeApiImpl();

// Whether the runtime has a service: always, as on the console. A service we don't provide yet
// fails at QueryApiImpl, where it is logged. Each feature number asked for is logged once.
bool XGameRuntimeIsFeatureAvailable(uint32_t feature);

} // namespace g6::gdk
