#pragma once

// The registry functions the game's modules import, from api-ms-win-core-registry-* and
// advapi32 (docs/hle-status.md, "runtime/imports/kernelbase").
//
// The GDK thunk library linked into every title works out XSystemGetDeviceType by reading one
// value: ConsoleMode under HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM, a REG_DWORD.
// Without the key it reports a PC. We answer that key, and only it, as a Series X; every other
// key keeps failing with ERROR_CALL_NOT_IMPLEMENTED, as the stubs did, and is logged once.

#include <windows.h>

namespace g6::loader {
class ImportRegistry;
}

namespace g6::imports::kernelbase {

// The ConsoleMode values the thunk maps (to XSystemDeviceType in brackets): 0x60 Series S
// (Lockhart, 6), 0x64 Series X (Anaconda, 7), 0x68 Series devkit (8); the Xbox One family uses
// 0x10 to 0x58.
inline constexpr DWORD kConsoleModeSeriesS = 0x60;
inline constexpr DWORD kConsoleModeSeriesX = 0x64;

LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR sub_key, DWORD options, REGSAM access, PHKEY result);
LSTATUS WINAPI RegOpenKeyW(HKEY key, LPCWSTR sub_key, PHKEY result);
LSTATUS WINAPI RegQueryValueExW(HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type,
                                LPBYTE data, LPDWORD data_size);
LSTATUS WINAPI RegCloseKey(HKEY key);

// Binds the functions above for api-ms-win-core-registry-* and advapi32.dll.
void register_registry(loader::ImportRegistry& imports);

} // namespace g6::imports::kernelbase
