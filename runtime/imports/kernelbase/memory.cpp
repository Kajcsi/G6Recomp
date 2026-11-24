// The memory functions (g6/imports/memory.h).

#include "g6/imports/memory.h"

#include "g6/gpu/memory_tracker.h"
#include "g6/imports/xmem.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"

namespace g6::imports::kernelbase {

LPVOID WINAPI VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
    if ((type & (MEM_RESET | MEM_RESET_UNDO)) != 0) {
        return ::VirtualAlloc(address, size, type, protection);
    }
    uint32_t console_protection = protection;
    if ((type & kMemGraphics) != 0 && (protection & gpu::kGraphicsProtectionMask) == 0) {
        console_protection |= PAGE_GRAPHICS_READWRITE;
    }
    void* result = xmem::allocate_virtual(address, size, type & ~(kMemGraphics | kMemTitle),
                                          console_protection);
    if (result == nullptr) {
        LOG_WARN(memory, "VirtualAlloc({}, {:#x}, {:#x}, {:#x}) failed (error {})", address, size,
                 type, protection, GetLastError());
    } else {
        LOG_DEBUG(memory, "VirtualAlloc({}, {:#x}, {:#x}, {:#x}) -> {}", address, size, type,
                  protection, result);
    }
    return result;
}

BOOL WINAPI VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    const BOOL freed = gpu::MemoryTracker::instance().free(address, size, type);
    if (freed == FALSE) {
        LOG_WARN(memory, "VirtualFree({}, {:#x}, {:#x}) failed (error {})", address, size, type,
                 GetLastError());
    } else {
        LOG_DEBUG(memory, "VirtualFree({}, {:#x}, {:#x})", address, size, type);
    }
    return freed;
}

void register_memory(loader::ImportRegistry& imports) {
    for (const char* dll : {"api-ms-win-core-memory-*", "kernel32.dll", "kernelbase.dll"}) {
        imports.hle(dll, loader::Symbol::by_name("VirtualAlloc"),
                    reinterpret_cast<const void*>(&VirtualAlloc));
        imports.hle(dll, loader::Symbol::by_name("VirtualFree"),
                    reinterpret_cast<const void*>(&VirtualFree));
        // A query changes nothing, and the host's answer is the truth about a range: the
        // tracker's placeholders and pages are host placeholders and views.
        imports.forward(dll, loader::Symbol::by_name("VirtualQueryEx"), "kernelbase.dll");
    }
}

} // namespace g6::imports::kernelbase
