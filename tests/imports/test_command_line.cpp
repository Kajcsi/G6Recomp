// The guest's command line (g6/imports/command_line.h): the C runtime's parsing rules, checked
// against what the host UCRT gave for the same lines (docs/execution-model.md, phase 5, "Command
// line"), and the CRT's argument functions called directly.

#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include <fmt/format.h>
#include <fmt/xchar.h>

#include "g6/imports/command_line.h"
#include "g6/io/text.h"
#include "g6test.h"

using g6::imports::join_command_line;
using g6::imports::set_guest_command_line;
using g6::imports::split_command_line;
using g6::imports::winmain_command_line;
namespace kernelbase = g6::imports::kernelbase;
namespace ucrtbase = g6::imports::ucrtbase;

namespace {

struct Parsed {
    std::wstring_view line;
    std::vector<std::wstring> arguments;
    std::wstring_view winmain;
};

// Each line with the argv and the WinMain command line the host's ucrtbase.dll gave a process
// started with it: Microsoft's documented examples first, then the edges.
const std::vector<Parsed>& measured() {
    static const std::vector<Parsed> lines = {
        {LR"(prog "a b c" d e)", {L"prog", L"a b c", L"d", L"e"}, LR"("a b c" d e)"},
        {LR"(prog "ab\"c" "\\" d)", {L"prog", LR"(ab"c)", LR"(\)", L"d"}, LR"("ab\"c" "\\" d)"},
        {LR"(prog a\\\b d"e f"g h)", {L"prog", LR"(a\\\b)", L"de fg", L"h"}, LR"(a\\\b d"e f"g h)"},
        {LR"(prog a\\\"b c d)", {L"prog", LR"(a\"b)", L"c", L"d"}, LR"(a\\\"b c d)"},
        {LR"(prog a\\\\"b c" d e)", {L"prog", LR"(a\\b c)", L"d", L"e"}, LR"(a\\\\"b c" d e)"},
        {LR"(prog a"b"" c d)", {L"prog", LR"(ab" c d)"}, LR"(a"b"" c d)"},
        // The program name: quotes toggle, backslashes stay, only spaces and tabs end it.
        {LR"("C:\my dir\prog.exe"  x)", {LR"(C:\my dir\prog.exe)", L"x"}, L"x"},
        {LR"(C:\"my dir"\prog.exe a)", {LR"(C:\my dir\prog.exe)", L"a"}, L"a"},
        {LR"("C:\dir\\" a)", {LR"(C:\dir\\)", L"a"}, L"a"},
        {LR"("prog"x y)", {L"progx", L"y"}, L"y"},
        {L"  prog a", {L"", L"prog", L"a"}, L"prog a"},
        {L"prog\x1a b", {L"prog\x1a", L"b"}, L"b"},
        // Separators and quotes.
        {L"prog\ta\t\tb", {L"prog", L"a", L"b"}, L"a\t\tb"},
        {L"prog a  ", {L"prog", L"a"}, L"a  "},
        {LR"(prog "")", {L"prog", L""}, LR"("")"},
        {LR"(prog a""b)", {L"prog", L"ab"}, LR"(a""b)"},
        {LR"(prog "a""b")", {L"prog", LR"(a"b)"}, LR"("a""b")"},
        {LR"(prog """a""")", {L"prog", LR"("a")"}, LR"("""a""")"},
        {LR"(prog "open)", {L"prog", L"open"}, LR"("open)"},
        {LR"(prog a\)", {L"prog", LR"(a\)"}, LR"(a\)"},
        {L"prog \x00a0x", {L"prog", L"\x00a0x"}, L"\x00a0x"},
        {L"", {L""}, L""},
    };
    return lines;
}

bool is_ascii(std::wstring_view text) {
    for (const wchar_t c : text) {
        if (c > 0x7F) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> narrow_all(const std::vector<std::wstring>& texts) {
    std::vector<std::string> result;
    result.reserve(texts.size());
    for (const std::wstring& text : texts) {
        result.push_back(g6::io::narrow(text));
    }
    return result;
}

// The guest's __argc and __argv, checking the null after the last.
std::vector<std::string> narrow_argv() {
    std::vector<std::string> result;
    const char* const* argv = *ucrtbase::__p___argv();
    const int argc = *ucrtbase::__p___argc();
    if (argv == nullptr) {
        return result;
    }
    for (int index = 0; index < argc; ++index) {
        result.emplace_back(argv[index]);
    }
    CHECK(argv[argc] == nullptr);
    return result;
}

std::vector<std::wstring> wide_argv() {
    std::vector<std::wstring> result;
    const wchar_t* const* argv = *ucrtbase::__p___wargv();
    const int argc = *ucrtbase::__p___argc();
    if (argv == nullptr) {
        return result;
    }
    for (int index = 0; index < argc; ++index) {
        result.emplace_back(argv[index]);
    }
    CHECK(argv[argc] == nullptr);
    return result;
}

// The host ucrtbase.dll's invalid parameter handler, which the HLE calls, replaced by one that
// counts for as long as this lives.
class InvalidParameters {
public:
    InvalidParameters() {
        HMODULE crt = LoadLibraryExW(L"ucrtbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        REQUIRE(crt != nullptr);
        set_ = reinterpret_cast<SetHandler>(GetProcAddress(crt, "_set_invalid_parameter_handler"));
        errno_location_ = reinterpret_cast<Location>(GetProcAddress(crt, "_errno"));
        REQUIRE(set_ != nullptr);
        REQUIRE(errno_location_ != nullptr);
        count_ = 0;
        previous_ = set_(&count);
    }
    InvalidParameters(const InvalidParameters&) = delete;
    InvalidParameters& operator=(const InvalidParameters&) = delete;
    ~InvalidParameters() { set_(previous_); }

    static int calls() { return count_; }
    int guest_errno() const { return *errno_location_(); }

private:
    using SetHandler = _invalid_parameter_handler (*)(_invalid_parameter_handler);
    using Location = int* (*)();

    static void count(const wchar_t* /*expression*/, const wchar_t* /*function*/,
                      const wchar_t* /*file*/, unsigned /*line*/, uintptr_t /*reserved*/) {
        ++count_;
    }

    static inline int count_ = 0;
    SetHandler set_ = nullptr;
    Location errno_location_ = nullptr;
    _invalid_parameter_handler previous_ = nullptr;
};

constexpr std::wstring_view kProgram = LR"(C:\games\my game\game.exe)";
constexpr std::wstring_view kLine = LR"("C:\games\my game\game.exe" -x "y z")";

} // namespace

// ---- Parsing ----

TEST_CASE("split_command_line gives the argv the host CRT gave") {
    for (const Parsed& parsed : measured()) {
        INFO(fmt::format("line {}", g6::io::utf8(parsed.line)));
        CHECK(split_command_line(parsed.line) == parsed.arguments);
        if (is_ascii(parsed.line)) {
            CHECK(split_command_line(std::string_view(g6::io::narrow(parsed.line))) ==
                  narrow_all(parsed.arguments));
        }
    }
}

TEST_CASE("winmain_command_line gives the WinMain command line the host CRT gave") {
    for (const Parsed& parsed : measured()) {
        INFO(fmt::format("line {}", g6::io::utf8(parsed.line)));
        CHECK(winmain_command_line(parsed.line) == parsed.winmain);
        if (is_ascii(parsed.line)) {
            const std::string line = g6::io::narrow(parsed.line);
            CHECK(winmain_command_line(std::string_view(line)) == g6::io::narrow(parsed.winmain));
        }
    }
}

TEST_CASE("a narrow byte above 0x7F is no blank") {
    // As the host CRT parsed "progé é" from code page 1252: é ends neither the program name nor
    // an argument. Only meaningful where é isn't a lead byte of the active code page.
    if (IsDBCSLeadByte(0xE9) != FALSE) {
        return;
    }
    const std::string_view line = "prog\xe9 \xe9 x";
    CHECK((split_command_line(line) == std::vector<std::string>{"prog\xe9", "\xe9", "x"}));
    CHECK(winmain_command_line(line) == std::string_view("\xe9 x"));
}

TEST_CASE("join_command_line quotes what split_command_line would change") {
    const std::vector<std::wstring> arguments = {
        L"plain",         L"",         L"two words", L"tab\there",   LR"(quote"inside)",
        LR"(trailing\)",  LR"(two\\)", LR"(a\"b)",   LR"(\\\")",     L"\"",
        LR"(back\slash)", LR"(\)",     LR"(""")",    L"-flag=value",
    };
    for (const std::wstring_view program :
         {std::wstring_view(LR"(C:\games\game.exe)"), std::wstring_view(LR"(C:\my games\game.exe)"),
          std::wstring_view(LR"(C:\my dir\)"), std::wstring_view()}) {
        const std::wstring line = join_command_line(program, arguments);
        INFO(fmt::format("line {}", g6::io::utf8(line)));
        std::vector<std::wstring> expected{std::wstring(program)};
        expected.insert(expected.end(), arguments.begin(), arguments.end());
        CHECK(split_command_line(line) == expected);
    }
    CHECK(join_command_line(L"game.exe", std::vector<std::wstring>{L"-a", L"b"}) ==
          L"game.exe -a b");
}

// ---- The CRT's functions ----

TEST_CASE("GetCommandLine and the CRT's command line are the guest's") {
    set_guest_command_line(kProgram, kLine);
    CHECK(std::wstring_view(kernelbase::GetCommandLineW()) == kLine);
    CHECK(std::string(kernelbase::GetCommandLineA()) == g6::io::narrow(kLine));
    // The CRT's _acmdln is GetCommandLineA's pointer itself, as on the host.
    CHECK(*ucrtbase::__p__acmdln() == kernelbase::GetCommandLineA());
    CHECK(*ucrtbase::__p__wcmdln() == kernelbase::GetCommandLineW());
    CHECK(std::string_view(ucrtbase::_get_narrow_winmain_command_line()) == R"(-x "y z")");
    CHECK(std::wstring_view(ucrtbase::_get_wide_winmain_command_line()) == LR"(-x "y z")");
}

TEST_CASE("each configure call parses the CRT's command line into a new argv") {
    set_guest_command_line(kProgram, kLine);
    // A new guest process has none yet.
    CHECK_EQ(*ucrtbase::__p___argc(), 0);
    CHECK(*ucrtbase::__p___argv() == nullptr);
    CHECK(*ucrtbase::__p___wargv() == nullptr);

    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_unexpanded_arguments) == 0);
    const std::vector<std::string> expected = {R"(C:\games\my game\game.exe)", "-x", "y z"};
    CHECK(narrow_argv() == expected);

    const char* const* first = *ucrtbase::__p___argv();
    const char* const* const* where = ucrtbase::__p___argv();
    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_unexpanded_arguments) == 0);
    CHECK(*ucrtbase::__p___argv() != first);
    CHECK(ucrtbase::__p___argv() == where);
    CHECK(narrow_argv() == expected);
    // The older one stays valid.
    CHECK(std::string_view(first[2]) == "y z");

    // _acmdln is read when the call is made.
    char other[] = "other x y";
    char* saved = *ucrtbase::__p__acmdln();
    *ucrtbase::__p__acmdln() = other;
    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_unexpanded_arguments) == 0);
    CHECK((narrow_argv() == std::vector<std::string>{"other", "x", "y"}));
    CHECK(std::string_view(ucrtbase::_get_narrow_winmain_command_line()) == "x y");
    *ucrtbase::__p__acmdln() = saved;
}

TEST_CASE("the wide argv and the program names come from the configure call of their width") {
    set_guest_command_line(kProgram, kLine);
    const InvalidParameters invalid;
    wchar_t* wide = nullptr;
    char* narrow = nullptr;
    CHECK_EQ(ucrtbase::_get_wpgmptr(&wide), EINVAL);
    CHECK_EQ(ucrtbase::_get_pgmptr(&narrow), EINVAL);
    CHECK_EQ(InvalidParameters::calls(), 2);
    CHECK(wide == nullptr);
    CHECK(narrow == nullptr);

    REQUIRE(ucrtbase::_configure_wide_argv(_crt_argv_unexpanded_arguments) == 0);
    CHECK((wide_argv() == std::vector<std::wstring>{std::wstring(kProgram), L"-x", L"y z"}));
    REQUIRE(ucrtbase::_get_wpgmptr(&wide) == 0);
    CHECK(std::wstring_view(wide) == kProgram);
    CHECK(*ucrtbase::__p__wpgmptr() == wide);
    CHECK_EQ(ucrtbase::_get_pgmptr(&narrow), EINVAL);

    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_unexpanded_arguments) == 0);
    REQUIRE(ucrtbase::_get_pgmptr(&narrow) == 0);
    CHECK(std::string(narrow) == g6::io::narrow(kProgram));
    CHECK(*ucrtbase::__p__pgmptr() == narrow);

    CHECK_EQ(ucrtbase::_get_pgmptr(nullptr), EINVAL);
    CHECK_EQ(InvalidParameters::calls(), 4);
    CHECK_EQ(invalid.guest_errno(), EINVAL);
}

TEST_CASE("an empty _acmdln gives the program path, parsed like a command line") {
    set_guest_command_line(kProgram, kLine);
    char empty[] = "";
    char* saved = *ucrtbase::__p__acmdln();
    *ucrtbase::__p__acmdln() = empty;
    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_unexpanded_arguments) == 0);
    CHECK((narrow_argv() == std::vector<std::string>{R"(C:\games\my)", R"(game\game.exe)"}));
    CHECK(std::string_view(ucrtbase::_get_narrow_winmain_command_line()).empty());
    *ucrtbase::__p__acmdln() = nullptr;
    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_unexpanded_arguments) == 0);
    CHECK_EQ(*ucrtbase::__p___argc(), 2);
    CHECK(std::string_view(ucrtbase::_get_narrow_winmain_command_line()).empty());
    *ucrtbase::__p__acmdln() = saved;
}

TEST_CASE("no arguments changes nothing, an unknown mode is invalid, wildcards stay") {
    set_guest_command_line(kProgram, L"game.exe *.txt a?c");
    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_unexpanded_arguments) == 0);
    CHECK_EQ(*ucrtbase::__p___argc(), 3);

    *ucrtbase::__p___argc() = 77;
    CHECK_EQ(ucrtbase::_configure_narrow_argv(_crt_argv_no_arguments), 0);
    CHECK_EQ(*ucrtbase::__p___argc(), 77);

    const InvalidParameters invalid;
    // A mode the CRT doesn't define, which a guest can still pass.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto unknown = static_cast<_crt_argv_mode>(5);
    CHECK_EQ(ucrtbase::_configure_narrow_argv(unknown), EINVAL);
    CHECK_EQ(ucrtbase::_configure_wide_argv(unknown), EINVAL);
    CHECK_EQ(InvalidParameters::calls(), 2);
    CHECK_EQ(invalid.guest_errno(), EINVAL);
    CHECK_EQ(*ucrtbase::__p___argc(), 77);

    REQUIRE(ucrtbase::_configure_narrow_argv(_crt_argv_expanded_arguments) == 0);
    CHECK((narrow_argv() == std::vector<std::string>{"game.exe", "*.txt", "a?c"}));
}

TEST_CASE("an empty command line becomes the quoted program path, as CreateProcess makes it") {
    set_guest_command_line(kProgram, L"");
    CHECK(std::wstring_view(kernelbase::GetCommandLineW()) == LR"("C:\games\my game\game.exe")");
    REQUIRE(ucrtbase::_configure_wide_argv(_crt_argv_unexpanded_arguments) == 0);
    CHECK((wide_argv() == std::vector<std::wstring>{std::wstring(kProgram)}));
    CHECK(std::wstring_view(ucrtbase::_get_wide_winmain_command_line()).empty());
}
