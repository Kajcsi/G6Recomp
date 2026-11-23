/* g6threads.dll: forwards thread notifications to the loader. See g6threads.h.
 *
 * Built without the C runtime (/NODEFAULTLIB, own entry point): the DLL needs nothing but a few
 * kernel32 functions, and staying out of the CRT keeps its DllMain trivially safe under the
 * loader lock. */

#include "g6threads.h"

#include <windows.h>

static DWORD g_slot = TLS_OUT_OF_INDEXES;

/* Circular list of live thread records, with a sentinel. */
static SRWLOCK g_list_lock = SRWLOCK_INIT;
static G6ThreadRecord g_threads = {&g_threads, &g_threads, NULL, 0, {0}};

static SRWLOCK g_callback_lock = SRWLOCK_INIT;
static G6ThreadCallback g_callback = NULL;
static void* g_context = NULL;

static void notify(G6ThreadRecord* record, unsigned long reason) {
    AcquireSRWLockShared(&g_callback_lock);
    if (g_callback != NULL) {
        g_callback(record, reason, g_context);
    }
    ReleaseSRWLockShared(&g_callback_lock);
}

static void thread_attach(void) {
    G6ThreadRecord* record =
        (G6ThreadRecord*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(G6ThreadRecord));
    if (record == NULL) {
        return;
    }
    record->teb = NtCurrentTeb();
    record->thread_id = GetCurrentThreadId();
    TlsSetValue(g_slot, record);

    AcquireSRWLockExclusive(&g_list_lock);
    record->prev = g_threads.prev;
    record->next = &g_threads;
    g_threads.prev->next = record;
    g_threads.prev = record;
    ReleaseSRWLockExclusive(&g_list_lock);

    notify(record, DLL_THREAD_ATTACH);
}

static void thread_detach(void) {
    G6ThreadRecord* record = (G6ThreadRecord*)TlsGetValue(g_slot);
    if (record == NULL) {
        return;
    }
    notify(record, DLL_THREAD_DETACH);

    AcquireSRWLockExclusive(&g_list_lock);
    record->prev->next = record->next;
    record->next->prev = record->prev;
    ReleaseSRWLockExclusive(&g_list_lock);

    TlsSetValue(g_slot, NULL);
    HeapFree(GetProcessHeap(), 0, record);
}

BOOL WINAPI g6_threads_entry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_slot = TlsAlloc();
        if (g_slot == TLS_OUT_OF_INDEXES) {
            return FALSE;
        }
        thread_attach();
        break;
    case DLL_THREAD_ATTACH: thread_attach(); break;
    case DLL_THREAD_DETACH: thread_detach(); break;
    case DLL_PROCESS_DETACH:
        /* At process exit (reserved != NULL) other threads are already gone; leave it all. */
        if (reserved == NULL) {
            thread_detach();
            TlsFree(g_slot);
        }
        break;
    default: break;
    }
    return TRUE;
}

__declspec(dllexport) void g6_threads_set_callback(G6ThreadCallback callback, void* context) {
    AcquireSRWLockExclusive(&g_callback_lock);
    g_callback = callback;
    g_context = context;
    ReleaseSRWLockExclusive(&g_callback_lock);
}

__declspec(dllexport) void g6_threads_for_each(G6ThreadVisitor visitor, void* context) {
    AcquireSRWLockShared(&g_list_lock);
    for (G6ThreadRecord* record = g_threads.next; record != &g_threads; record = record->next) {
        visitor(record, context);
    }
    ReleaseSRWLockShared(&g_list_lock);
}

__declspec(dllexport) G6ThreadRecord* g6_threads_current(void) {
    return (G6ThreadRecord*)TlsGetValue(g_slot);
}
