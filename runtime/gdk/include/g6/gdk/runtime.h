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

// The services, each a class, and the interfaces the console's thunks ask them for. The
// threading, error, and feature thunks pass the same GUID as the class and as the interface.
inline constexpr GUID kThreadingId = {
    0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
inline constexpr GUID kErrorId = {
    0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}};
inline constexpr GUID kFeatureId = {
    0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}};
// XUser: one class with two interfaces, the user functions and the gamertag.
inline constexpr GUID kUserClassId = {
    0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
inline constexpr GUID kUserInterfaceId = {
    0x26f3c674, 0xa2fe, 0x44fa, {0xb6, 0xc4, 0xa3, 0x23, 0xbc, 0x94, 0xff, 0x53}};
inline constexpr GUID kUserGamertagInterfaceId = {
    0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};
// XUser's device functions.
inline constexpr GUID kUserDeviceClassId = {
    0x7d824997, 0x10dc, 0x45ab, {0x86, 0xb7, 0x27, 0x37, 0x76, 0x7c, 0x0b, 0xf1}};
inline constexpr GUID kUserDeviceInterfaceId = {
    0x0cc6a956, 0xe7e1, 0x4fdf, {0x93, 0x41, 0x9d, 0x5d, 0xa6, 0x49, 0xeb, 0xc8}};
// XGameSave: one class with two interfaces, whose thunks call one vtable, slots 3 to 32 through
// the first and 33 to 49 through the second.
inline constexpr GUID kGameSaveClassId = {
    0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2}};
inline constexpr GUID kGameSaveInterfaceId = {
    0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}};
inline constexpr GUID kGameSave2InterfaceId = {
    0xab4ae4fb, 0x6508, 0x4950, {0xa0, 0x32, 0x45, 0xfd, 0x4b, 0xf8, 0xc4, 0x3b}};

// The object for one interface of a service, with its own reference: E_POINTER for a null
// argument, REGDB_E_CLASSNOTREG for a service we don't provide yet, and E_NOINTERFACE for an
// interface the service doesn't have, each logged once. The objects are static, so the reference
// counts are only for the caller's bookkeeping.
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
