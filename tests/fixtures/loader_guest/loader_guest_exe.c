/* Original guest executable for the loader tests. It imports the guest DLL and has static TLS
 * of its own. The C runtime is linked only for its TLS support: the entry point is ours, so no
 * runtime startup code runs.
 *
 * The entry point returns a bit mask of checks; the test expects all of them set. */

#include <intrin.h>
#include <windows.h>

#include "loader_guest_api.h"

__declspec(dllimport) int g6_guest_tls_get(void);

extern IMAGE_DOS_HEADER __ImageBase;

__declspec(thread) int t_exe_value = 7;

#define G6_EXE_STACK_RESERVE 0x200000

DWORD WINAPI g6_guest_exe_entry(void* peb) {
    DWORD result = 0;
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;

    /* The executable's own TLS. */
    if (t_exe_value == 7) {
        result |= 1;
    }
    /* The DLL's TLS, on this new thread. */
    if (g6_guest_tls_get() == G6_GUEST_TLS_INITIAL) {
        result |= 2;
    }
    /* The stack reserve from the PE header (/STACK). */
    GetCurrentThreadStackLimits(&low, &high);
    if (high - low >= G6_EXE_STACK_RESERVE) {
        result |= 4;
    }
    /* The PEB, as Windows passes it. */
    if (peb == (void*)__readgsqword(0x60)) {
        result |= 8;
    }
    /* GetModuleHandle(NULL) is the guest executable, not the host's. */
    if (GetModuleHandleW(NULL) == (HMODULE)&__ImageBase) {
        result |= 16;
    }
    return result;
}
