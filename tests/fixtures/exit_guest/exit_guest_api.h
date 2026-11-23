/* What test_crt_exit and its guest executable, exit_guest.exe, agree on. */

#pragma once

/* What main does, in g6_exit_mode, set by the test before the entry point runs. Each also gives
   the exit code. */
enum {
    G6_EXIT_RETURN = 'r', /* returns 3 from main; the CRT's startup code calls exit(3) */
    G6_EXIT_EXIT = 'e',   /* exit(4) */
    G6_EXIT_QUICK = 'q',  /* quick_exit(5) */
    G6_EXIT_PLAIN = 'x',  /* _exit(6) */
    G6_EXIT_CEXIT = 'c',  /* _cexit(), then exit(7) */
};

/* The events g6_exit_log records, in order, one character each. */
enum {
    G6_EXIT_EVENT_MAIN = 'M',         /* main started */
    G6_EXIT_EVENT_THREAD_LOCAL = 'T', /* main's thread_local object was destroyed: by the exit
                                         callback, or when the thread itself ended */
    G6_EXIT_EVENT_FIRST = '1',        /* the first atexit handler */
    G6_EXIT_EVENT_ADDER = 'A',        /* the second, which registers another */
    G6_EXIT_EVENT_LATE = 'L',         /* the one the second registered */
    G6_EXIT_EVENT_THIRD = '3',        /* the third */
    G6_EXIT_EVENT_STATIC = 'D',       /* the static object was destroyed */
    G6_EXIT_EVENT_QUICK = 'Q',        /* the at_quick_exit handler */
    G6_EXIT_EVENT_AFTER_CEXIT = 'C',  /* _cexit returned */
    G6_EXIT_EVENT_END = '|',          /* written by the test's end function */
};

enum { G6_EXIT_LOG_SIZE = 64 };
