/* The loader's thread notifier: a TLS callback in the host module. See thread_notifier.h and
 * docs/execution-model.md, "TLS and the TEB".
 *
 * In an executable, Windows calls the callback for the process attaching before the C runtime
 * starts, and for thread attach and detach with the loader lock held. So it uses nothing but a
 * few kernel32 functions, and data that needs no constructor. */

#include "thread_notifier.h"

#include <windows.h>

static DWORD g_slot = TLS_OUT_OF_INDEXES;
static volatile LONG g_started = 0;

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

    /* Windows frees the thread's TLS blocks only after this returns. Until the record is out of
     * the list, a walk holding the list lock can still fill them. */
    AcquireSRWLockExclusive(&g_list_lock);
    record->prev->next = record->next;
    record->next->prev = record->prev;
    ReleaseSRWLockExclusive(&g_list_lock);

    TlsSetValue(g_slot, NULL);
    HeapFree(GetProcessHeap(), 0, record);
}

static void NTAPI on_tls_callback(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_slot = TlsAlloc();
        if (g_slot == TLS_OUT_OF_INDEXES) {
            return; /* g6_threads_started stays 0, and the loader refuses to start */
        }
        thread_attach();
        InterlockedExchange(&g_started, 1);
        break;
    case DLL_THREAD_ATTACH:
        if (g_slot != TLS_OUT_OF_INDEXES) {
            thread_attach();
        }
        break;
    case DLL_THREAD_DETACH:
        if (g_slot != TLS_OUT_OF_INDEXES) {
            thread_detach();
        }
        break;
    case DLL_PROCESS_DETACH:
        /* At process exit (reserved != NULL) other threads are already gone; leave it all. Only a
         * DLL host that is unloaded gets here with NULL. */
        if (reserved == NULL && g_slot != TLS_OUT_OF_INDEXES) {
            thread_detach();
            TlsFree(g_slot);
            g_slot = TLS_OUT_OF_INDEXES;
        }
        break;
    default: break;
    }
}

/* The callback's entry in the host's TLS callback array, which runs from ".CRT$XLA" to
 * ".CRT$XLZ" in name order. Windows calls the entries in array order on attach and on detach
 * alike (measured on Windows 11), so ".CRT$XLB" puts this one before the C runtime's callbacks
 * for the host's own thread_local variables: on detach, guest code runs before their
 * destructors. Nothing refers to the entry or to the TLS directory by name, so the linker is told
 * to keep both. */
#pragma section(".CRT$XLB", read)
/* External linkage on purpose: the /INCLUDE below names it. */
/* NOLINTNEXTLINE(misc-use-internal-linkage) */
__declspec(allocate(".CRT$XLB")) void(NTAPI* const g6_thread_notifier_callback)(
    PVOID, DWORD, PVOID) = on_tls_callback;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g6_thread_notifier_callback")

void g6_threads_set_callback(G6ThreadCallback callback, void* context) {
    AcquireSRWLockExclusive(&g_callback_lock);
    g_callback = callback;
    g_context = context;
    ReleaseSRWLockExclusive(&g_callback_lock);
}

void g6_threads_for_each(G6ThreadVisitor visitor, void* context) {
    AcquireSRWLockShared(&g_list_lock);
    for (G6ThreadRecord* record = g_threads.next; record != &g_threads; record = record->next) {
        visitor(record, context);
    }
    ReleaseSRWLockShared(&g_list_lock);
}

G6ThreadRecord* g6_threads_current(void) {
    return g_slot != TLS_OUT_OF_INDEXES ? (G6ThreadRecord*)TlsGetValue(g_slot) : NULL;
}

int g6_threads_started(void) {
    return InterlockedCompareExchange(&g_started, 0, 0) != 0;
}
