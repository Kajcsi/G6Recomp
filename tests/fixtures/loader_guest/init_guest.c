/* A DLL with one TLS callback and an entry point of its own, each of which reports every
 * notification to init_sink.dll. The entry point fails DLL_PROCESS_ATTACH when the sink says
 * so. No C runtime, so the loader calls exactly these two routines and nothing wraps them: the
 * TLS directory below stands in for the one the C runtime would supply. */

#include <windows.h>

#include "loader_guest_api.h"

__declspec(dllimport) void g6_init_record(int routine, unsigned long reason);
__declspec(dllimport) int g6_init_refuse(void);

static void NTAPI on_tls_callback(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    g6_init_record(G6_INIT_TLS_CALLBACK, reason);
}

BOOL WINAPI g6_init_guest_entry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    g6_init_record(G6_INIT_ENTRY, reason);
    return reason == DLL_PROCESS_ATTACH && g6_init_refuse() ? FALSE : TRUE;
}

/* The linker points the image's TLS directory at the symbol `_tls_used`. The template is a few
 * bytes nothing reads: the loader needs a block to give each thread, not its contents. */
ULONG _tls_index; /* NOLINT(bugprone-reserved-identifier) */
static const char g_tls_template[16] = "g6 init guest";
static PIMAGE_TLS_CALLBACK g_tls_callbacks[] = {on_tls_callback, NULL};

#pragma comment(linker, "/INCLUDE:_tls_used")
/* NOLINTNEXTLINE(bugprone-reserved-identifier) */
const IMAGE_TLS_DIRECTORY64 _tls_used = {
    .StartAddressOfRawData = (ULONGLONG)g_tls_template,
    .EndAddressOfRawData = (ULONGLONG)(g_tls_template + sizeof(g_tls_template)),
    .AddressOfIndex = (ULONGLONG)&_tls_index,
    .AddressOfCallBacks = (ULONGLONG)g_tls_callbacks,
};
