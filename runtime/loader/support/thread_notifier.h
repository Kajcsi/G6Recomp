#pragma once

/* Interface of the loader's thread notifier (thread_notifier.c).
 *
 * Windows tells every loaded module about each thread that starts and exits, but not modules the
 * runtime maps itself. The notifier is a TLS callback in the host module, which Windows calls on
 * every thread attach and detach. It passes those calls on to the loader, so guest modules get
 * their static TLS initialized and their TLS callbacks and DllMain called on every thread. It
 * also keeps a list of the threads it has seen, which the loader walks to initialize static TLS
 * on threads that already exist when a guest module loads.
 *
 * In an executable the callback runs from the start of the process, so the list holds every
 * thread that takes part in DLL notifications, however early it started. */

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

/* Sets the callback, or clears it with NULL. Returns once no call to the previous callback is
 * still running. */
void g6_threads_set_callback(G6ThreadCallback callback, void* context);
/* Calls `visitor` for every live thread the notifier has seen. Threads can't register or exit
 * while it runs, so the visitor must not wait on another thread. */
void g6_threads_for_each(G6ThreadVisitor visitor, void* context);
/* The calling thread's record, or NULL for a thread the notifier never saw: one created to skip
 * DLL notifications, or, in a DLL host, one that started before the DLL loaded. */
G6ThreadRecord* g6_threads_current(void);
/* Nonzero once Windows has called the notifier for the process (or its DLL) attaching. Zero means
 * the host's TLS directory doesn't list the notifier, and the loader can't work. */
int g6_threads_started(void);

/* NOLINTEND(modernize-use-using, modernize-redundant-void-arg) */

#ifdef __cplusplus
}
#endif
