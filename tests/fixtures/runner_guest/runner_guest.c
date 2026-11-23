/* An original executable for test_runner (docs/execution-model.md, phase 5, "The runner"). Its
   universal CRT is DLL imports, as the game's is, so its startup code gets argc and argv from the
   CRT's argument functions; its C++ runtime is linked in (tests/CMakeLists.txt says why).
   g6recomp.exe runs it from a folder that stands in for an install. It checks what the runner
   set up, through the guest's own imports: its arguments, its own path, its current directory,
   a file in the install, that the install can't be written, and that T: can. Its exit code says
   which check failed, if one did. */

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "runner_guest_api.h"

/* The size of the calling thread's stack reservation. */
static ULONG_PTR stack_reserve(void) {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return high - low;
}

/* An int3 in a function of its own: clang's __try covers calls, not a fault in the block's own
   instructions. */
static __declspec(noinline) void soft_assert(void) {
    __debugbreak();
}

static DWORD WINAPI report_stack(void* reserve) {
    *(ULONG_PTR*)reserve = stack_reserve();
    return 0;
}

int main(int argc, char** argv) {
    char text[64] = {0};
    char header[2] = {0};
    char module_path[MAX_PATH] = {0};
    wchar_t directory[MAX_PATH] = {0};
    FILE* file = NULL;
    ULONG_PTR thread_reserve = 0;
    HANDLE thread = NULL;

    /* The entry point runs on the runner's main thread, and a thread started with no stack size
       gets the runner's reserve, not this executable's. */
    if (stack_reserve() < G6_RUNNER_STACK_RESERVE) {
        return G6_RUNNER_SMALL_STACK;
    }
    thread = CreateThread(NULL, 0, report_stack, &thread_reserve, 0, NULL);
    if (thread == NULL) {
        return G6_RUNNER_SMALL_THREAD_STACK;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (thread_reserve < G6_RUNNER_STACK_RESERVE) {
        return G6_RUNNER_SMALL_THREAD_STACK;
    }

    if (argc == 4 && strcmp(argv[3], G6_RUNNER_INT3_ARGUMENT) == 0) {
        int result = G6_RUNNER_INT3_SKIPPED;
        __try {
            soft_assert();
        } __except (GetExceptionCode() == EXCEPTION_BREAKPOINT ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
            result = G6_RUNNER_INT3_CAUGHT;
        }
        return result;
    }
    if (argc != 3 || strcmp(argv[1], G6_RUNNER_ARGUMENT_1) != 0 ||
        strcmp(argv[2], G6_RUNNER_ARGUMENT_2) != 0) {
        return G6_RUNNER_BAD_ARGUMENTS;
    }

    /* The guest's own path is a console path, which its file functions open. */
    if (GetModuleFileNameA(NULL, module_path, MAX_PATH) == 0 ||
        strcmp(module_path, G6_RUNNER_PROGRAM) != 0) {
        return G6_RUNNER_BAD_MODULE_PATH;
    }
    if (strcmp(argv[0], G6_RUNNER_PROGRAM) != 0) {
        return G6_RUNNER_BAD_PROGRAM;
    }
    if (fopen_s(&file, argv[0], "rb") != 0) {
        return G6_RUNNER_NO_PROGRAM_FILE;
    }
    fread(header, 1, sizeof header, file);
    fclose(file);
    if (header[0] != 'M' || header[1] != 'Z') {
        return G6_RUNNER_NO_PROGRAM_FILE;
    }

    if (GetCurrentDirectoryW(MAX_PATH, directory) != 3 || wcscmp(directory, L"G:\\") != 0) {
        return G6_RUNNER_BAD_DIRECTORY;
    }

    /* A relative path opens a file in the install. */
    if (fopen_s(&file, G6_RUNNER_DATA_FILE, "rb") != 0) {
        return G6_RUNNER_NO_DATA;
    }
    fread(text, 1, sizeof text - 1, file);
    fclose(file);
    if (strcmp(text, G6_RUNNER_DATA) != 0) {
        return G6_RUNNER_BAD_DATA;
    }

    if (fopen_s(&file, "G:\\" G6_RUNNER_COPY_FILE, "wb") == 0) {
        fclose(file);
        return G6_RUNNER_WROTE_INSTALL;
    }
    if (fopen_s(&file, "T:\\" G6_RUNNER_COPY_FILE, "wb") != 0) {
        return G6_RUNNER_NO_TEMP;
    }
    fputs(text, file);
    fclose(file);
    return G6_RUNNER_OK;
}
