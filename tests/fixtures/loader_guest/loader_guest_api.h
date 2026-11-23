/* What the loader tests and the original guest modules in this folder share. Plain C, so the
 * C parts of the guest can include it too. */

#pragma once

#include <stdint.h>

/* A C header: typedefs are C's spelling. */
/* NOLINTBEGIN(modernize-use-using) */

/* The guest calls these host functions inside its try blocks. Declared outside extern "C",
 * because the compiler may assume extern "C" functions never throw. */
typedef void (*G6HostCallback)(void* context);

#ifdef __cplusplus
extern "C" {
#endif

/* The exception code g6_guest_seh_raise raises (customer bit set). */
#define G6_GUEST_SEH_CODE 0xE0473601u

/* The value g6_guest_tls_get returns on a thread whose TLS block holds the template. */
#define G6_GUEST_TLS_INITIAL 42

/* Ordinals of the made-up console DLL's ordinal-only exports (g6fakeos.def). */
#define G6_FAKE_ORDINAL_BOUND 5
#define G6_FAKE_ORDINAL_MISSING 9

/* What g6replaced_value in the real g6replaced.dll returns. The replaced-DLL tests mark that DLL
 * replaced, so the loader must never map it and this value must never be seen. */
#define G6_REPLACED_ORIGINAL 7

/* Which routine of init_guest.dll reported a notification to init_sink.dll. */
#define G6_INIT_TLS_CALLBACK 1
#define G6_INIT_ENTRY 2

/* What init_sink.dll's g6_init_read hands back per notification: the routine above in the high
 * half, the notification's reason (DLL_PROCESS_ATTACH and so on) in the low half. */
#define G6_INIT_RECORD(routine, reason) (((routine) << 16) | (reason))

typedef struct G6GuestCounts {
    long dll_process_attach;
    long dll_thread_attach;
    long dll_thread_detach;
    long tls_process_attach;
    long tls_thread_attach;
    long tls_thread_detach;
} G6GuestCounts;

/* NOLINTEND(modernize-use-using) */

#ifdef __cplusplus
}
#endif
