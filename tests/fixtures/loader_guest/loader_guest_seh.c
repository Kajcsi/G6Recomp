/* The SEH half of the original loader test guest (loader_guest.cpp has the rest). In C,
 * because __try and __except are simplest there. */

#include <windows.h>

#include "loader_guest_api.h"

void g6_guest_note_finally(void);

/* Raises G6_GUEST_SEH_CODE from inside a __try/__finally, so the unwind runs guest cleanup. */
__declspec(dllexport) void g6_guest_seh_raise(void) {
    __try {
        RaiseException(G6_GUEST_SEH_CODE, 0, 0, NULL);
    } __finally {
        g6_guest_note_finally();
    }
}

static int is_guest_code(DWORD code) {
    return code == G6_GUEST_SEH_CODE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

/* Calls the host, which is expected to raise G6_GUEST_SEH_CODE back through its own frames.
 * Returns the code it caught, or 0. */
__declspec(dllexport) DWORD g6_guest_seh_catch_through(G6HostCallback callback, void* context) {
    __try {
        callback(context);
    } __except (is_guest_code(GetExceptionCode())) {
        return GetExceptionCode();
    }
    return 0;
}

static volatile int* volatile g_null_pointer = NULL;

__declspec(noinline) static void write_through_null(void) {
    *g_null_pointer = 1;
}

/* A hardware exception in guest code, handled in guest code. The fault is in a function of its
 * own: without /EHa, clang-cl's __try only catches exceptions raised by calls. */
__declspec(dllexport) int g6_guest_fault_caught(void) {
    __try {
        write_through_null();
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
    return 0;
}
