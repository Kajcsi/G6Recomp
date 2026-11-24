#pragma once

// The memory functions the main executable imports from api-ms-win-core-memory-*
// (docs/hle-status.md, "runtime/imports/kernelbase"). The game reserves and frees its
// physical-page ranges and placeholders with them as well as with xmem (docs/boot.md, "Physical
// pages"), so they go through memory_tracker, which has to see every range it hands out.

#include <cstdint>
#include <windows.h>

namespace g6::loader {
class ImportRegistry;
}

namespace g6::imports::kernelbase {

// The console's allocation-type bits that desktop Windows refuses (docs/hle-status.md, "Notes
// for runtime/kernel"): MEM_GRAPHICS asks for GPU access, MEM_TITLE for title memory.
inline constexpr DWORD kMemGraphics = 0x10000000;
inline constexpr DWORD kMemTitle = 0x40000000;

// The console's VirtualAlloc. MEM_TITLE is dropped, since all of a title's memory is title
// memory here. MEM_GRAPHICS becomes GPU read-write access in memory_tracker, unless the
// protection names a PAGE_GRAPHICS_* access itself. The rest is xmem's allocate_virtual.
// MEM_RESET and MEM_RESET_UNDO go to the host as they are.
LPVOID WINAPI VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protection);

// The console's VirtualFree, through memory_tracker: releasing a range the tracker made releases
// every piece of it, and the placeholder forms (MEM_PRESERVE_PLACEHOLDER to split a placeholder
// or put an allocation back into one, MEM_COALESCE_PLACEHOLDERS to join placeholders) keep the
// tracker's table in step with the host's.
BOOL WINAPI VirtualFree(LPVOID address, SIZE_T size, DWORD type);

// Binds VirtualAlloc and VirtualFree above, and the host's VirtualQueryEx, for
// api-ms-win-core-memory-*, kernel32.dll, and kernelbase.dll.
void register_memory(loader::ImportRegistry& imports);

} // namespace g6::imports::kernelbase
