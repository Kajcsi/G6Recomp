/* What test_runner and its guest executable, runner_guest.exe, agree on. The test puts the
   executable in a folder that stands in for an install, runs it with g6recomp.exe, and reads the
   exit code. */

#pragma once

/* The guest's arguments after its program name, as the test passes them. */
#define G6_RUNNER_ARGUMENT_1 "one"
#define G6_RUNNER_ARGUMENT_2 "two words"

/* The guest's own path, as its GetModuleFileName and argv[0] give it. */
#define G6_RUNNER_PROGRAM "G:\\runner_guest.exe"

/* A file the test writes into the install, and the guest copies to T:. */
#define G6_RUNNER_DATA_FILE "runner_data.txt"
#define G6_RUNNER_DATA "data from the install"
#define G6_RUNNER_COPY_FILE "runner_copy.txt"

/* A third argument that makes the guest run an int3 under an __except block and end at once with
   G6_RUNNER_INT3_SKIPPED, when g6recomp's --skip-int3 stepped over it, or G6_RUNNER_INT3_CAUGHT,
   when the block caught it. */
#define G6_RUNNER_INT3_ARGUMENT "int3"

/* The stack reserve g6recomp.exe is linked with (app/CMakeLists.txt). The guest's main thread and
   a thread it starts with no stack size both get it. */
enum { G6_RUNNER_STACK_RESERVE = 8 << 20 };

/* The guest's exit codes: G6_RUNNER_OK when everything was as the test arranged, otherwise the
   first check that failed. */
enum {
    G6_RUNNER_OK = 42,
    G6_RUNNER_BAD_ARGUMENTS = 10,
    G6_RUNNER_BAD_DIRECTORY = 11,
    G6_RUNNER_NO_DATA = 12,
    G6_RUNNER_BAD_DATA = 13,
    G6_RUNNER_WROTE_INSTALL = 14,
    G6_RUNNER_NO_TEMP = 15,
    G6_RUNNER_BAD_MODULE_PATH = 16,
    G6_RUNNER_BAD_PROGRAM = 17,
    G6_RUNNER_NO_PROGRAM_FILE = 18,
    G6_RUNNER_SMALL_STACK = 19,
    G6_RUNNER_SMALL_THREAD_STACK = 20,
    G6_RUNNER_INT3_SKIPPED = 21,
    G6_RUNNER_INT3_CAUGHT = 22,
};
