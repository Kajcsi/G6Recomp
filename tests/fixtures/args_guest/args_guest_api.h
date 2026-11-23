/* What test_crt_args and its guest executable and DLL, args_guest.exe and args_guest_dll.dll,
   agree on. */

#pragma once

/* The size of each buffer they record into, in characters. */
enum { G6_ARGS_TEXT_SIZE = 512 };

/* Follows each argument in g6_args_argv. */
enum { G6_ARGS_SEPARATOR = '|' };
