/* An original executable for test_runner (docs/execution-model.md, phase 5, "The runner"). Its
   universal CRT is DLL imports, as the game's is, so its startup code gets argc and argv from the
   CRT's argument functions; its C++ runtime is linked in (tests/CMakeLists.txt says why).
   g6recomp.exe runs it from a folder that stands in for an install. It checks what the runner
   set up, through the guest's own imports: its arguments, its current directory, a file in the
   install, that the install can't be written, and that T: can. Its exit code says which check
   failed, if one did. */

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "runner_guest_api.h"

int main(int argc, char** argv) {
    char text[64] = {0};
    wchar_t directory[MAX_PATH] = {0};
    FILE* file = NULL;

    if (argc != 3 || strcmp(argv[1], G6_RUNNER_ARGUMENT_1) != 0 ||
        strcmp(argv[2], G6_RUNNER_ARGUMENT_2) != 0) {
        return G6_RUNNER_BAD_ARGUMENTS;
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
