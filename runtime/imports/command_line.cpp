// The guest process's command line (g6/imports/command_line.h). kernelbase's GetCommandLine and
// ucrtbase's argument functions share one command line, so they live together here. Written from
// the CRT's documented parsing rules and the behaviour measured on the host
// (docs/execution-model.md, phase 5, "Command line").

#include "g6/imports/command_line.h"

#include <atomic>
#include <cerrno>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <windows.h>

#include "g6/io/text.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"
#include "ucrtbase/host_crt.h"

namespace g6::imports {
namespace {

// ---- Parsing ----

bool is_lead_byte(char c) {
    return IsDBCSLeadByte(static_cast<BYTE>(c)) != FALSE;
}
bool is_lead_byte(wchar_t /*c*/) {
    return false;
}

// What separates the program name and the arguments.
template <typename Char>
bool is_separator(Char c) {
    return c == Char{' '} || c == Char{'\t'};
}

// What ends the program name for WinMain. Unsigned, so that the code page's bytes above 0x7F are
// not blanks.
template <typename Char>
bool is_blank(Char c) {
    using Unsigned = std::make_unsigned_t<Char>;
    return c != Char{0} && static_cast<Unsigned>(c) <= static_cast<Unsigned>(' ');
}

// The CRT reads a C string.
template <typename Char>
std::basic_string_view<Char> until_terminator(std::basic_string_view<Char> line) {
    return line.substr(0, line.find(Char{0}));
}

template <typename Char>
std::vector<std::basic_string<Char>> split(std::basic_string_view<Char> line) {
    constexpr Char kQuote{'"'};
    constexpr Char kBackslash{'\\'};
    line = until_terminator(line);
    const size_t end = line.size();
    size_t at = 0;
    std::vector<std::basic_string<Char>> arguments;

    // The program name: quotes only toggle quoting, and backslashes are kept.
    std::basic_string<Char> program;
    bool quoted = false;
    while (at < end) {
        const Char c = line[at];
        if (c == kQuote) {
            quoted = !quoted;
            ++at;
            continue;
        }
        if (!quoted && is_separator(c)) {
            break;
        }
        program.push_back(c);
        ++at;
        if (is_lead_byte(c) && at < end) {
            program.push_back(line[at++]);
        }
    }
    arguments.push_back(std::move(program));

    for (;;) {
        while (at < end && is_separator(line[at])) {
            ++at;
        }
        if (at == end) {
            break;
        }
        std::basic_string<Char> argument;
        quoted = false;
        while (at < end) {
            size_t backslashes = 0;
            while (at < end && line[at] == kBackslash) {
                ++backslashes;
                ++at;
            }
            if (at < end && line[at] == kQuote) {
                argument.append(backslashes / 2, kBackslash);
                if (backslashes % 2 == 1) {
                    argument.push_back(kQuote);
                } else if (quoted && at + 1 < end && line[at + 1] == kQuote) {
                    argument.push_back(kQuote);
                    ++at;
                } else {
                    quoted = !quoted;
                }
                ++at;
                continue;
            }
            argument.append(backslashes, kBackslash);
            if (at == end || (!quoted && is_separator(line[at]))) {
                break;
            }
            const Char c = line[at++];
            argument.push_back(c);
            if (is_lead_byte(c) && at < end) {
                argument.push_back(line[at++]);
            }
        }
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

template <typename Char>
std::basic_string_view<Char> winmain(std::basic_string_view<Char> line) {
    line = until_terminator(line);
    size_t at = 0;
    bool quoted = false;
    while (at < line.size()) {
        const Char c = line[at];
        if (c == Char{'"'}) {
            quoted = !quoted;
        } else if (!quoted && is_blank(c)) {
            break;
        }
        ++at;
        if (is_lead_byte(c) && at < line.size()) {
            ++at;
        }
    }
    while (at < line.size() && is_blank(line[at])) {
        ++at;
    }
    return line.substr(at);
}

// ---- The guest process ----

// One argv the CRT built: the strings, and the null-terminated array of pointers to them.
template <typename Char>
class Argv {
public:
    explicit Argv(std::vector<std::basic_string<Char>> strings) : strings_(std::move(strings)) {
        pointers_.reserve(strings_.size() + 1);
        for (std::basic_string<Char>& text : strings_) {
            pointers_.push_back(text.data());
        }
        pointers_.push_back(nullptr);
    }

    int count() const { return static_cast<int>(strings_.size()); }
    Char** pointers() { return pointers_.data(); }

private:
    std::vector<std::basic_string<Char>> strings_;
    std::vector<Char*> pointers_;
};

// The text and the CRT's globals of one width.
template <typename Char>
struct Width {
    std::basic_string<Char> program;
    std::basic_string<Char> line;
    // _acmdln or _wcmdln, _pgmptr or _wpgmptr, and __argv or __wargv, which the guest reads and
    // may change through the addresses the CRT hands out.
    Char* command_line = nullptr;
    Char* program_name = nullptr;
    Char** argv = nullptr;
    // Every argv built for this guest process; an older one stays valid, as in the CRT.
    std::vector<std::unique_ptr<Argv<Char>>> built;

    void start(std::basic_string<Char> new_program, std::basic_string<Char> new_line) {
        program = std::move(new_program);
        line = std::move(new_line);
        command_line = line.data();
        program_name = nullptr;
        argv = nullptr;
        built.clear();
    }
};

struct GuestCommandLine {
    // Guards the configure calls and set_guest_command_line. The guest reads the globals without
    // it, as it reads the CRT's.
    std::mutex lock;
    bool set = false;
    std::atomic<bool> warned_unset{false};
    std::atomic<bool> warned_wildcards{false};
    int argc = 0;
    Width<char> narrow;
    Width<wchar_t> wide;

    GuestCommandLine() {
        narrow.start({}, {});
        wide.start({}, {});
    }

    template <typename Char>
    Width<Char>& width() {
        if constexpr (std::is_same_v<Char, char>) {
            return narrow;
        } else {
            return wide;
        }
    }
};

GuestCommandLine& guest() {
    static GuestCommandLine instance;
    return instance;
}

// ---- The CRT's errors ----

struct HostErrors {
    int* (*errno_location)() = nullptr;
    void (*invalid_parameter)() = nullptr;

    static const HostErrors& get() {
        static const HostErrors instance;
        return instance;
    }

private:
    HostErrors() {
        ucrtbase::bind_host_crt("_errno", errno_location);
        ucrtbase::bind_host_crt("_invalid_parameter_noinfo", invalid_parameter);
    }
};

int fail(int error) {
    *HostErrors::get().errno_location() = error;
    return error;
}

// What the CRT's parameter validation does: errno, then the guest's invalid parameter handler,
// which by default ends the process.
int invalid_parameter() {
    fail(EINVAL);
    HostErrors::get().invalid_parameter();
    return EINVAL;
}

template <typename Char>
bool has_wildcard(const std::vector<std::basic_string<Char>>& arguments) {
    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::basic_string<Char>& argument = arguments[index];
        if (argument.find(Char{'*'}) != std::basic_string<Char>::npos ||
            argument.find(Char{'?'}) != std::basic_string<Char>::npos) {
            return true;
        }
    }
    return false;
}

template <typename Char>
int configure(_crt_argv_mode mode) {
    if (mode == _crt_argv_no_arguments) {
        return 0;
    }
    if (mode != _crt_argv_unexpanded_arguments && mode != _crt_argv_expanded_arguments) {
        return invalid_parameter();
    }
    GuestCommandLine& process = guest();
    try {
        const std::scoped_lock lock(process.lock);
        if (!process.set && !process.warned_unset.exchange(true)) {
            LOG_WARN(crt, "the guest parses its command line, but the runner never set one");
        }
        Width<Char>& width = process.width<Char>();
        width.program_name = width.program.data();
        // An empty command line gives the program path, parsed like any command line.
        const Char* line = width.command_line;
        const std::basic_string_view<Char> text = line == nullptr || *line == Char{0}
                                                      ? std::basic_string_view<Char>(width.program)
                                                      : std::basic_string_view<Char>(line);
        std::vector<std::basic_string<Char>> arguments = split(text);
        if (mode == _crt_argv_expanded_arguments && has_wildcard(arguments) &&
            !process.warned_wildcards.exchange(true)) {
            LOG_INFO(crt, "the guest's arguments have wildcards; they stay as written");
        }
        auto argv = std::make_unique<Argv<Char>>(std::move(arguments));
        process.argc = argv->count();
        width.argv = argv->pointers();
        width.built.push_back(std::move(argv));
    } catch (const std::bad_alloc&) {
        return fail(ENOMEM);
    }
    return 0;
}

template <typename Char>
errno_t get_program_name(Char** result) {
    if (result == nullptr) {
        return invalid_parameter();
    }
    Char* program = guest().width<Char>().program_name;
    if (program == nullptr) {
        return invalid_parameter();
    }
    *result = program;
    return 0;
}

template <typename Char>
Char* get_winmain_command_line() {
    static Char empty[1] = {Char{0}};
    const Width<Char>& width = guest().width<Char>();
    if (width.command_line == nullptr) {
        return empty;
    }
    const std::basic_string_view<Char> whole(width.command_line);
    return width.command_line + (winmain(whole).data() - whole.data());
}

} // namespace

void set_guest_command_line(std::wstring_view program, std::wstring_view line) {
    std::wstring wide_line(line);
    if (wide_line.empty()) {
        wide_line = L"\"" + std::wstring(program) + L"\"";
    }
    std::string narrow_program = io::narrow(program);
    std::string narrow_line = io::narrow(wide_line);

    GuestCommandLine& process = guest();
    const std::scoped_lock lock(process.lock);
    process.narrow.start(std::move(narrow_program), std::move(narrow_line));
    process.wide.start(std::wstring(program), std::move(wide_line));
    process.argc = 0;
    process.set = true;
}

std::vector<std::string> split_command_line(std::string_view line) {
    return split(line);
}

std::vector<std::wstring> split_command_line(std::wstring_view line) {
    return split(line);
}

std::string_view winmain_command_line(std::string_view line) {
    return winmain(line);
}

std::wstring_view winmain_command_line(std::wstring_view line) {
    return winmain(line);
}

std::wstring join_command_line(std::wstring_view program, std::span<const std::wstring> arguments) {
    std::wstring line;
    if (program.empty() || program.find_first_of(L" \t") != std::wstring_view::npos) {
        line.append(L"\"").append(program).append(L"\"");
    } else {
        line.append(program);
    }
    for (const std::wstring& argument : arguments) {
        line.push_back(L' ');
        if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
            line.append(argument);
            continue;
        }
        line.push_back(L'"');
        size_t backslashes = 0;
        for (const wchar_t c : argument) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            // Backslashes before a quote are doubled, and the quote escaped.
            line.append(c == L'"' ? (2 * backslashes) + 1 : backslashes, L'\\');
            line.push_back(c);
            backslashes = 0;
        }
        // And before the closing quote.
        line.append(2 * backslashes, L'\\');
        line.push_back(L'"');
    }
    return line;
}

namespace kernelbase {

LPSTR WINAPI GetCommandLineA() {
    return guest().narrow.line.data();
}

LPWSTR WINAPI GetCommandLineW() {
    return guest().wide.line.data();
}

} // namespace kernelbase

// NOLINTBEGIN(bugprone-reserved-identifier)
namespace ucrtbase {

int _configure_narrow_argv(_crt_argv_mode mode) {
    return configure<char>(mode);
}

int _configure_wide_argv(_crt_argv_mode mode) {
    return configure<wchar_t>(mode);
}

int* __p___argc() {
    return &guest().argc;
}

char*** __p___argv() {
    return &guest().narrow.argv;
}

wchar_t*** __p___wargv() {
    return &guest().wide.argv;
}

char** __p__acmdln() {
    return &guest().narrow.command_line;
}

wchar_t** __p__wcmdln() {
    return &guest().wide.command_line;
}

char** __p__pgmptr() {
    return &guest().narrow.program_name;
}

wchar_t** __p__wpgmptr() {
    return &guest().wide.program_name;
}

errno_t _get_pgmptr(char** result) {
    return get_program_name(result);
}

errno_t _get_wpgmptr(wchar_t** result) {
    return get_program_name(result);
}

char* _get_narrow_winmain_command_line() {
    return get_winmain_command_line<char>();
}

wchar_t* _get_wide_winmain_command_line() {
    return get_winmain_command_line<wchar_t>();
}

} // namespace ucrtbase
// NOLINTEND(bugprone-reserved-identifier)

// ---- Registration ----

namespace {

struct Entry {
    const char* name;
    const void* function;
};

template <typename Function>
Entry entry(const char* name, Function* function) {
    return {name, reinterpret_cast<const void*>(function)};
}

} // namespace

void register_command_line(loader::ImportRegistry& imports) {
    for (const char* dll :
         {"api-ms-win-core-processenvironment-*", "kernel32.dll", "kernelbase.dll"}) {
        for (const Entry& item : {entry("GetCommandLineA", &kernelbase::GetCommandLineA),
                                  entry("GetCommandLineW", &kernelbase::GetCommandLineW)}) {
            imports.hle(dll, loader::Symbol::by_name(item.name), item.function);
        }
    }
    const Entry crt[] = {
        entry("_configure_narrow_argv", &ucrtbase::_configure_narrow_argv),
        entry("_configure_wide_argv", &ucrtbase::_configure_wide_argv),
        entry("__p___argc", &ucrtbase::__p___argc),
        entry("__p___argv", &ucrtbase::__p___argv),
        entry("__p___wargv", &ucrtbase::__p___wargv),
        entry("__p__acmdln", &ucrtbase::__p__acmdln),
        entry("__p__wcmdln", &ucrtbase::__p__wcmdln),
        entry("__p__pgmptr", &ucrtbase::__p__pgmptr),
        entry("__p__wpgmptr", &ucrtbase::__p__wpgmptr),
        entry("_get_pgmptr", &ucrtbase::_get_pgmptr),
        entry("_get_wpgmptr", &ucrtbase::_get_wpgmptr),
        entry("_get_narrow_winmain_command_line", &ucrtbase::_get_narrow_winmain_command_line),
        entry("_get_wide_winmain_command_line", &ucrtbase::_get_wide_winmain_command_line),
    };
    for (const Entry& item : crt) {
        imports.hle("api-ms-win-crt-*", loader::Symbol::by_name(item.name), item.function);
    }
}

} // namespace g6::imports
