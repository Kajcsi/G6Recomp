/* Records the notifications init_guest.dll's TLS callback and entry point receive, and tells its
 * entry point whether to fail. The test loads it with LoadLibrary before loading init_guest.dll
 * the same way, so it outlives a failed load and the records stay readable. Under the native
 * loader the test marks this DLL replaced and binds the same two imports to host functions. No C
 * runtime and no entry point. */

#include "loader_guest_api.h"

enum { kCapacity = 64 };

static int g_records[kCapacity];
static int g_count;
static int g_refuse;

__declspec(dllexport) void g6_init_record(int routine, unsigned long reason) {
    if (g_count < kCapacity) {
        g_records[g_count++] = G6_INIT_RECORD(routine, (int)reason);
    }
}

__declspec(dllexport) int g6_init_refuse(void) {
    return g_refuse;
}

__declspec(dllexport) void g6_init_set_refuse(int refuse) {
    g_refuse = refuse;
}

/* Copies up to `capacity` records to `out`, clears them, and returns how many there were. */
__declspec(dllexport) int g6_init_read(int* out, int capacity) {
    const int count = g_count;
    for (int i = 0; i < count && i < capacity; ++i) {
        out[i] = g_records[i];
    }
    g_count = 0;
    return count;
}
