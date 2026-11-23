/* Original guest executable for test_exe_tls, built with /GA: its code finds its thread-local
 * data in slot 0 of the TLS array and never reads its TLS index variable, as code in an
 * executable may (docs/execution-model.md, "TLS and the TEB"). It only sees its own data when the
 * loader gave it TLS index 0. The C runtime is linked only for its TLS directory: the entry point
 * is ours, so no runtime startup code runs. */

#include <windows.h>

#include "ga_guest_api.h"

/* Written by the loader, which also writes it in place of Windows. Only g6_ga_tls_index reads
 * it; the other functions never do, which is what /GA means. */
extern ULONG _tls_index;

__declspec(thread) int t_ga_value = G6_GA_INITIAL;
__declspec(thread) char t_ga_name[G6_GA_NAME_SIZE] = G6_GA_NAME;
/* Zero until written, so the loader must clear it on threads whose block held something else. */
__declspec(thread) int t_ga_counter;

__declspec(dllexport) int g6_ga_get(void) {
    return t_ga_value;
}

__declspec(dllexport) void g6_ga_set(int value) {
    t_ga_value = value;
}

__declspec(dllexport) const char* g6_ga_name(void) {
    return t_ga_name;
}

__declspec(dllexport) int g6_ga_bump(void) {
    return ++t_ga_counter;
}

__declspec(dllexport) ULONG g6_ga_tls_index(void) {
    return _tls_index;
}

/* The entry point runs on a thread of its own; it returns a bit mask of checks, and the test
 * expects all of them set. */
DWORD WINAPI g6_ga_entry(void* peb) {
    DWORD result = 0;
    (void)peb;
    if (t_ga_value == G6_GA_INITIAL) {
        result |= 1;
    }
    if (t_ga_name[0] == G6_GA_NAME[0] && t_ga_name[G6_GA_NAME_SIZE - 1] == '\0') {
        result |= 2;
    }
    if (t_ga_counter == 0) {
        result |= 4;
    }
    return result;
}
