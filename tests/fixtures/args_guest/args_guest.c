// An original executable for test_crt_args, built /MD like the game's, so its startup code gets
// argc and argv from the CRT's argument functions through its imports. main records what it got
// and what the command-line functions return; the test reads the records through the exports.

#include <corecrt_startup.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "args_guest_api.h"

__declspec(dllimport) const wchar_t* g6_args_at_attach(void);

__declspec(dllexport) int g6_args_argc = -1;
// Each argument followed by G6_ARGS_SEPARATOR.
__declspec(dllexport) char g6_args_argv[G6_ARGS_TEXT_SIZE];
__declspec(dllexport) wchar_t g6_args_command_line[G6_ARGS_TEXT_SIZE];
__declspec(dllexport) char g6_args_command_line_a[G6_ARGS_TEXT_SIZE];
__declspec(dllexport) char g6_args_winmain[G6_ARGS_TEXT_SIZE];
__declspec(dllexport) char g6_args_program[G6_ARGS_TEXT_SIZE];
__declspec(dllexport) wchar_t g6_args_at_attach_copy[G6_ARGS_TEXT_SIZE];

int main(int argc, char** argv) {
    static const char separator[] = {G6_ARGS_SEPARATOR, 0};
    char* program = NULL;

    g6_args_argc = argc;
    for (int index = 0; index < argc; ++index) {
        strcat_s(g6_args_argv, G6_ARGS_TEXT_SIZE, argv[index]);
        strcat_s(g6_args_argv, G6_ARGS_TEXT_SIZE, separator);
    }
    wcsncpy_s(g6_args_command_line, G6_ARGS_TEXT_SIZE, GetCommandLineW(), _TRUNCATE);
    strncpy_s(g6_args_command_line_a, G6_ARGS_TEXT_SIZE, GetCommandLineA(), _TRUNCATE);
    strncpy_s(g6_args_winmain, G6_ARGS_TEXT_SIZE, _get_narrow_winmain_command_line(), _TRUNCATE);
    if (_get_pgmptr(&program) == 0) {
        strncpy_s(g6_args_program, G6_ARGS_TEXT_SIZE, program, _TRUNCATE);
    }
    wcsncpy_s(g6_args_at_attach_copy, G6_ARGS_TEXT_SIZE, g6_args_at_attach(), _TRUNCATE);
    return 0;
}
