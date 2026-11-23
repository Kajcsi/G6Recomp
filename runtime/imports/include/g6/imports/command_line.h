#pragma once

// The guest process's command line (docs/execution-model.md, phase 5, "Command line"). The guest
// sees the line the runner sets here, never the host's: GetCommandLineA and GetCommandLineW
// return it, and the C runtime's argument functions parse it. The runner sets it before it loads
// the guest's modules, whose process-attach code can already read it.
//
// The C runtime's functions follow the host UCRT, as measured on Windows 11 26200:
//
//   * _configure_narrow_argv and _configure_wide_argv parse the CRT's _acmdln or _wcmdln when
//     they are called, into a new argv each time; __p___argv and the others always return the
//     same address. A DLL built with the CRT calls them again from its own startup code.
//   * The program name is set by the configure call of its width. Before that, _get_pgmptr and
//     _get_wpgmptr fail with EINVAL.
//   * With an empty or null _acmdln, the program path itself is parsed as the command line.
//   * _crt_argv_no_arguments changes nothing. A mode the CRT doesn't define sets errno to EINVAL,
//     calls the invalid parameter handler, and returns EINVAL, as do the getters given null.
//   * _crt_argv_expanded_arguments keeps arguments with wildcards as written, which is what the
//     UCRT does when nothing matches: the guest gets no listing of host directories.
//
// The CRT's errno and invalid parameter handler are those of the host's ucrtbase.dll, which the
// guest's forwarded CRT imports bind to.

#include <corecrt_startup.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace g6::loader {
class ImportRegistry;
}

namespace g6::imports {

// Starts the guest process's command line. `program` is the executable's path as the guest's
// GetModuleFileName reports it, and `line` the whole command line, program name first; an empty
// line becomes the quoted program path, as CreateProcess makes it. The narrow forms use the
// active code page. Everything the guest got from the previous command line is released, so the
// runner calls this between guest processes.
void set_guest_command_line(std::wstring_view program, std::wstring_view line);

// Splits a command line by the C runtime's rules. The program name ends at the first space or tab
// outside quotes, and a quote in it only toggles quoting. Each argument after it ends at a space
// or tab outside quotes. A run of 2n backslashes before a quote gives n backslashes and the quote
// toggles quoting, except that inside quotes a doubled quote gives one literal quote; 2n+1
// backslashes give n backslashes and a literal quote; backslashes before anything else are kept.
// In the narrow form, a lead byte of the active code page takes its trail byte with it.
std::vector<std::string> split_command_line(std::string_view line);
std::vector<std::wstring> split_command_line(std::wstring_view line);

// The part of a command line that WinMain gets. Here the program name ends at the first character
// outside quotes whose code is a space or below, control characters included, and every such
// character after it is skipped.
std::string_view winmain_command_line(std::string_view line);
std::wstring_view winmain_command_line(std::wstring_view line);

// Joins a program and its arguments into a command line that split_command_line reads back
// unchanged. A program path has no quotes in it.
std::wstring join_command_line(std::wstring_view program, std::span<const std::wstring> arguments);

// Binds GetCommandLineA and GetCommandLineW for api-ms-win-core-processenvironment-*, kernel32,
// and kernelbase, and the functions in ucrtbase below for api-ms-win-crt-*.
void register_command_line(loader::ImportRegistry& imports);

namespace kernelbase {

LPSTR WINAPI GetCommandLineA();
LPWSTR WINAPI GetCommandLineW();

} // namespace kernelbase

// The names are the CRT's own, as CLAUDE.md asks; they live in this namespace only.
// NOLINTBEGIN(bugprone-reserved-identifier)
namespace ucrtbase {

int _configure_narrow_argv(_crt_argv_mode mode);
int _configure_wide_argv(_crt_argv_mode mode);
int* __p___argc();
char*** __p___argv();
wchar_t*** __p___wargv();
char** __p__acmdln();
wchar_t** __p__wcmdln();
char** __p__pgmptr();
wchar_t** __p__wpgmptr();
errno_t _get_pgmptr(char** result);
errno_t _get_wpgmptr(wchar_t** result);
char* _get_narrow_winmain_command_line();
wchar_t* _get_wide_winmain_command_line();

} // namespace ucrtbase
// NOLINTEND(bugprone-reserved-identifier)

} // namespace g6::imports
