/* What test_crash_report and its guest DLL, stack_guest.dll, agree on. */

#pragma once

/* A C header: typedefs are C's spelling. */
/* NOLINTBEGIN(modernize-use-using) */

/* The host function g6_stack_call calls two guest frames deep. */
typedef int (*G6StackCallback)(void* context);

/* NOLINTEND(modernize-use-using) */

/* What g6_stack_leaf_store writes. */
enum { G6_STACK_STORED = 7 };
