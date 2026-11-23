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
/* Set by this thread's own DLL_THREAD_ATTACH callback, so it is only set when the loader filled
 * the block before the callback ran. */
__declspec(thread) int t_ga_attached;

/* What the executable's TLS callback saw. A started executable's callbacks get process attach
 * first, then thread attach and detach for every thread that starts after it, as under a Windows
 * launch. A thread's detach is counted only when it marked itself with G6_GA_DETACH_MARK, so
 * threads the test didn't start never count. */
static volatile LONG g_process_attaches;
static volatile LONG g_marked_detaches;

static void NTAPI g6_ga_on_tls(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InterlockedIncrement(&g_process_attaches);
    } else if (reason == DLL_THREAD_ATTACH) {
        t_ga_attached = t_ga_value == G6_GA_INITIAL ? 1 : 0;
    } else if (reason == DLL_THREAD_DETACH && t_ga_value == G6_GA_DETACH_MARK) {
        InterlockedIncrement(&g_marked_detaches);
    }
}

#pragma comment(linker, "/INCLUDE:g6_ga_tls_callback")
#pragma section(".CRT$XLF", long, read)
__declspec(allocate(".CRT$XLF")) const PIMAGE_TLS_CALLBACK g6_ga_tls_callback = g6_ga_on_tls;

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

__declspec(dllexport) int g6_ga_attached(void) {
    return t_ga_attached;
}

__declspec(dllexport) LONG g6_ga_process_attaches(void) {
    return g_process_attaches;
}

__declspec(dllexport) LONG g6_ga_marked_detaches(void) {
    return g_marked_detaches;
}

/* The entry point runs on a thread of its own; it returns a bit mask of checks, and the test
 * expects all of them set. It marks its thread, so that thread's detach counts. */
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
    if (t_ga_attached == 1) {
        result |= 8;
    }
    t_ga_value = G6_GA_DETACH_MARK;
    return result;
}
