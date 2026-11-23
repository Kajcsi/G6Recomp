/* What the hook engine tests (tests/loader/test_hooks.cpp) and their original guest module
 * share. Plain C, so the C parts of the guest can include it too. */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The SEH exception code the tests raise (customer bit set). */
#define G6H_SEH_CODE 0xE0068001u

/* Addresses inside the hand-written functions, which the export table can't give: g6h_sites
 * holds their RVAs in this order. */
enum G6HSite {
    G6H_SITE_REGS,           /* g6h_mid_regs, after its first instruction */
    G6H_SITE_FLAGS,          /* g6h_mid_flags, after the compare */
    G6H_SITE_DF,             /* g6h_mid_df, after it sets the direction flag */
    G6H_SITE_REDIRECT,       /* g6h_mid_redirect, after its first instruction */
    G6H_SITE_REDIRECT_ALT,   /* g6h_mid_redirect: code that runs only when a hook sends rip there */
    G6H_SITE_YMM,            /* g6h_mid_ymm, between loading and storing the vector registers */
    G6H_SITE_FRAMED,         /* g6h_mid_framed, in the body after its prologue */
    G6H_SITE_LOAD,           /* g6h_mid_load, at the load through its argument */
    G6H_SITE_EPILOGUE,       /* g6h_framed, at its epilogue */
    G6H_SITE_BEFORE_CPUID,   /* g6h_cpuid, two instructions before its cpuid */
    G6H_SITE_LOOKALIKE,      /* bytes after g6h_xgetbv that read as cpuid and xgetbv, never run */
    G6H_SITE_RETURN_ADDRESS, /* g6h_after_call, where its call returns */
    G6H_SITE_AFTER_RETURN,   /* g6h_after_call, one instruction after that */
    G6H_SITE_COUNT
};

/* Hand-written functions (hook_guest_asm.c). The comments say what each returns. */

int g6h_rip_prologue(int x); /* x + a value in .data, loaded RIP-relative first thing */
int g6h_jcc_entry(int x);    /* -1 for 0, otherwise 2 * x; a short jcc at the entry */
int g6h_tiny(void);          /* 0, in three bytes followed by int3 padding */
int g6h_loop(int n);         /* n for n >= 1, from a loop back to its second instruction */
int g6h_calls(int x);        /* g6h_jcc_entry(x) + 1, with the call near the entry */
int g6h_framed(int (*callback)(int), int x); /* callback(x) + x, with a frame */
uint64_t g6h_mid_regs(uint64_t a, uint64_t b, uint64_t c, uint64_t d); /* (a + b) * c + d */
int g6h_mid_flags(int a, int b); /* a < b, with the compare before the hook site */
uint64_t g6h_mid_df(void);       /* rflags after the hook site, with the direction flag set */
int g6h_mid_redirect(int x);     /* x + 2 */
void g6h_mid_ymm(const void* in, void* out); /* copies 512 bytes through ymm0 to ymm15 */
int g6h_mid_framed(int x);                   /* 3 * x, with a frame */
int g6h_mid_load(const int* p);              /* *p + 1, with a frame */
int g6h_after_call(int x);                   /* g6h_mid_framed(x) + 7 */
int g6h_handled(int x); /* x + 5, with a frame under a handler the engine doesn't read */
void g6h_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]); /* eax, ebx, ecx, edx */
uint64_t g6h_xgetbv(uint32_t index);

/* C and C++ callers (hook_guest.c, hook_guest_cpp.cpp). */

/* g6h_mid_framed(x) under a __try/__finally, inside a __try/__except for G6H_SEH_CODE: its
 * result, or the exception code. */
uint32_t g6h_seh_catch_mid(int x);
/* g6h_framed with a callback that raises G6H_SEH_CODE: the code it catches, or 0. */
uint32_t g6h_seh_through_framed(int x);
/* g6h_mid_load(p), or -1 when it faults with an access violation. */
int g6h_load_or_fault(const int* p);
/* 0 when g6h_xgetbv(index) returns, otherwise the code of the exception it raised. */
uint32_t g6h_xgetbv_fault(uint32_t index);
/* g6h_mid_framed(x) + 1 with a local whose destructor counts a cleanup, inside a catch (int):
 * its result, or the int caught. */
int g6h_cpp_catch_mid(int x);
/* g6h_framed with a callback that throws x when x is negative: its result, or the int caught. */
int g6h_cpp_through_framed(int x);
/* __finally blocks and destructors run so far. */
int g6h_cleanups(void);
int g6h_get_data_value(void);

/* The value the data patch test changes, exported as data. */
extern int g6h_data_value;
/* Counts one cleanup; used by both halves of the guest, not exported. */
void g6h_note_cleanup(void);

#ifdef __cplusplus
}
#endif
