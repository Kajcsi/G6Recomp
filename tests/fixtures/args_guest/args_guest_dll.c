// An original DLL for test_crt_args, imported by args_guest.exe: it copies the command line while
// the loader attaches it, before the executable's entry point runs.

#include <wchar.h>
#include <windows.h>

#include "args_guest_api.h"

static wchar_t g_at_attach[G6_ARGS_TEXT_SIZE];

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        wcsncpy_s(g_at_attach, G6_ARGS_TEXT_SIZE, GetCommandLineW(), _TRUNCATE);
    }
    return TRUE;
}

__declspec(dllexport) const wchar_t* g6_args_at_attach(void) {
    return g_at_attach;
}
