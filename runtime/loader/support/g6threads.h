#pragma once

/* Interface of g6threads.dll, the loader's thread notifier.
 *
 * Windows tells every loaded DLL about each thread that starts and exits, but not modules the
 * runtime maps itself. This tiny DLL, which Windows loads normally, passes those notifications
 * on to the loader, so guest modules get their static TLS initialized and their TLS callbacks
 * and DllMain called on every thread. It also keeps a list of the threads it has seen, which
 * the loader walks to initialize static TLS on threads that already exist when a guest module
 * loads.
 *
 * The loader loads the DLL with LoadLibrary and looks up the functions by name. */

#ifdef __cplusplus
extern "C" {
#endif

/* A C header: typedefs and (void) parameter lists are C's spelling. */
/* NOLINTBEGIN(modernize-use-using, modernize-redundant-void-arg) */

#define G6_THREADS_MAX_TLS_SLOTS 8

typedef struct G6ThreadRecord {
    struct G6ThreadRecord* prev;
    struct G6ThreadRecord* next;
    /* The thread's TEB, which stays put for the thread's lifetime. */
    void* teb;
    unsigned long thread_id;
    /* Owned by the loader. Per TLS donor slot: whether this thread's block holds the guest's
     * template yet (0 no, 1 being copied, 2 yes). */
    volatile long tls_state[G6_THREADS_MAX_TLS_SLOTS];
} G6ThreadRecord;

/* `reason` is DLL_THREAD_ATTACH or DLL_THREAD_DETACH. Called on the thread itself, with the
 * Windows loader lock held, like DllMain. */
typedef void (*G6ThreadCallback)(G6ThreadRecord* record, unsigned long reason, void* context);
typedef void (*G6ThreadVisitor)(G6ThreadRecord* record, void* context);

/* g6_threads_set_callback: sets the callback, or clears it with NULL. Returns once no call to
 * the previous callback is still running. */
typedef void (*G6ThreadsSetCallback)(G6ThreadCallback callback, void* context);
/* g6_threads_for_each: calls `visitor` for every live thread the DLL has seen. Threads can't
 * register or exit while it runs, so the visitor must not wait on another thread. */
typedef void (*G6ThreadsForEach)(G6ThreadVisitor visitor, void* context);
/* g6_threads_current: the calling thread's record, or NULL for a thread that started before
 * the DLL loaded. */
typedef G6ThreadRecord* (*G6ThreadsCurrent)(void);

/* NOLINTEND(modernize-use-using, modernize-redundant-void-arg) */

#ifdef __cplusplus
}
#endif
