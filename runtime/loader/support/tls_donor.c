/* A TLS donor: a DLL that holds one static TLS block and nothing else. Built several times
 * (g6tls0.dll, g6tls1.dll, ...) and loaded normally, so Windows gives every thread one block
 * per donor. The loader lends each guest module with a TLS directory one donor: it writes the
 * donor's TLS index into the guest's index variable and copies the guest's template into the
 * donor's block on every thread (docs/execution-model.md, "TLS and the TEB").
 *
 * The TLS directory is written out here instead of coming from the C runtime, because the
 * guest's data must start exactly at the beginning of the block. The CRT's version puts a
 * marker byte there. */

#include <windows.h>

#ifndef G6_TLS_DONOR_CAPACITY
#error "G6_TLS_DONOR_CAPACITY must be defined"
#endif

/* Windows writes this DLL's TLS index here when it loads the DLL. */
ULONG _tls_index = 0;

/* The template Windows copies into every new thread's block. What it holds doesn't matter: the
 * loader copies the guest's template over it. The nonzero byte keeps it in initialized data,
 * so the whole block is raw data rather than a zero-fill count. */
#pragma section(".tls", read, write)
__declspec(allocate(".tls")) unsigned char g6_tls_template[G6_TLS_DONOR_CAPACITY] = {1};

#pragma section(".rdata$T", read)
__declspec(allocate(".rdata$T")) const IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG)g6_tls_template,
    (ULONGLONG)(g6_tls_template + G6_TLS_DONOR_CAPACITY),
    (ULONGLONG)&_tls_index,
    0, /* no TLS callbacks */
    0,
    {0},
};

__declspec(dllexport) ULONG g6_tls_donor_index(void) {
    return _tls_index;
}

__declspec(dllexport) ULONG g6_tls_donor_capacity(void) {
    return G6_TLS_DONOR_CAPACITY;
}

/* The donor must import at least one function. Windows sets up the static TLS of a DLL loaded
 * at run time as part of binding its imports, and skips both for a DLL that has none: such a
 * DLL keeps index 0 and gets no blocks (observed on Windows 11 24H2). Turning off thread
 * notifications is the import, and harmless: Windows allocates TLS blocks regardless, and the
 * donor has no use for the notifications. */
BOOL WINAPI g6_tls_donor_entry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
