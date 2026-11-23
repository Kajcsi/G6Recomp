/* A DLL with static TLS and nothing else, for test_tls_order. Windows loads copies of it under
 * other names before the native loader starts, so they take TLS indices the way host DLLs such
 * as shell32 and combase do; the loader then maps the original as a guest module. No C runtime:
 * the TLS directory below stands in for the one the C runtime would supply. */

#include <intrin.h>
#include <windows.h>

#include "tls_order_api.h"

/* Windows (or the native loader, for the guest) writes this DLL's TLS index here. */
ULONG _tls_index; /* NOLINT(bugprone-reserved-identifier) */

/* The template every thread's block is filled from. */
static const ULONG g_tls_template[4] = {G6_TLS_ORDER_MAGIC, 0, 0, 0};

#pragma comment(linker, "/INCLUDE:_tls_used")
/* NOLINTNEXTLINE(bugprone-reserved-identifier) */
const IMAGE_TLS_DIRECTORY64 _tls_used = {
    .StartAddressOfRawData = (ULONGLONG)g_tls_template,
    .EndAddressOfRawData = (ULONGLONG)(g_tls_template + 4),
    .AddressOfIndex = (ULONGLONG)&_tls_index,
};

/* The TEB's TLS array, at gs:[0x58]. */
static void** tls_array(void) {
    return (void**)__readgsqword(0x58);
}

/* The calling thread's copy of the template's first word, reached the way compiled code
 * reaches static TLS: through the TLS array, at this module's index. */
__declspec(dllexport) ULONG g6_tls_order_value(void) {
    return *(const ULONG*)tls_array()[_tls_index];
}

__declspec(dllexport) ULONG g6_tls_order_index(void) {
    return _tls_index;
}

/* The calling thread's block for this module. */
__declspec(dllexport) void* g6_tls_order_block(void) {
    return tls_array()[_tls_index];
}

/* Windows sets up the static TLS of a DLL loaded at run time while binding its imports, and
 * skips both for a DLL that has none (runtime/loader/support/tls_donor.c), so the entry point
 * makes the one import. */
BOOL WINAPI g6_tls_order_entry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
