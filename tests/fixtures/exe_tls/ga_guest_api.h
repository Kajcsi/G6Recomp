/* What test_exe_tls and its guest executable, ga_guest.exe, agree on. */

#pragma once

enum {
    G6_GA_INITIAL = 0x5A5A1234,
    G6_GA_NAME_SIZE = 16,
    /* All the bits the guest's entry point sets. */
    G6_GA_ENTRY_CHECKS = 0xF,
    /* A thread's value when its detach should be counted. */
    G6_GA_DETACH_MARK = 0x0D7AC4ED,
};

#define G6_GA_NAME "slot zero"
