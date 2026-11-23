/* An original guest DLL for test_crash_report (docs/execution-model.md, "Crash reports"): guest
   frames for the stack walk to get through, and a fault in guest code. Optimized, so the function
   that faults is a leaf without unwind data, as the compiler makes them. No C runtime and no
   entry point. */

#include "stack_guest_api.h"

/* Read after each call, so no call is a tail call and every caller keeps its frame. */
static volatile int g_after = 1;

/* A leaf: it calls nothing and needs no stack, so it gets no unwind data. */
__declspec(dllexport, noinline) void g6_stack_leaf_store(volatile int* address) {
    *address = G6_STACK_STORED;
}

static __declspec(noinline) int stack_store_below(volatile int* address) {
    g6_stack_leaf_store(address);
    return g_after + 1;
}

/* Stores through `address` two guest frames down. */
__declspec(dllexport, noinline) int g6_stack_store(volatile int* address) {
    return stack_store_below(address) + g_after;
}

static __declspec(noinline) int stack_call_below(G6StackCallback callback, void* context) {
    return callback(context) + g_after;
}

/* Calls the host two guest frames down. */
__declspec(dllexport, noinline) int g6_stack_call(G6StackCallback callback, void* context) {
    return stack_call_below(callback, context) + g_after;
}
