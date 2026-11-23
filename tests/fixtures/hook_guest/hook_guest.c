/* The C half of the hook engine's original test guest: SEH callers of the hand-written
 * functions in hook_guest_asm.c, and the data a data patch changes. In C, because __try and
 * __except are simplest there. Without /EHa, clang-cl's __try only catches exceptions raised
 * by calls, so everything that faults or raises is a call. */

#include <windows.h>

#include "hook_guest_api.h"

int g6h_data_value = 5;
static int g_cleanups;

void g6h_note_cleanup(void) {
    ++g_cleanups;
}

int g6h_cleanups(void) {
    return g_cleanups;
}

int g6h_get_data_value(void) {
    return *(volatile int*)&g6h_data_value;
}

static int is_test_code(DWORD code) {
    return code == G6H_SEH_CODE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

__declspec(noinline) static int framed_with_finally(int x) {
    int result = 0;
    __try {
        result = g6h_mid_framed(x);
    } __finally {
        g6h_note_cleanup();
    }
    return result;
}

uint32_t g6h_seh_catch_mid(int x) {
    __try {
        return (uint32_t)framed_with_finally(x);
    } __except (is_test_code(GetExceptionCode())) {
        return GetExceptionCode();
    }
}

static int raise_test_code(int x) {
    (void)x;
    RaiseException(G6H_SEH_CODE, 0, 0, NULL);
    return 0;
}

uint32_t g6h_seh_through_framed(int x) {
    __try {
        g6h_framed(&raise_test_code, x);
    } __except (is_test_code(GetExceptionCode())) {
        return GetExceptionCode();
    }
    return 0;
}

int g6h_load_or_fault(const int* p) {
    __try {
        return g6h_mid_load(p);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

uint32_t g6h_xgetbv_fault(uint32_t index) {
    __try {
        g6h_xgetbv(index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
}
