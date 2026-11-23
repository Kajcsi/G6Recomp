// Console path rules (docs/io.md, "Path rules"): the four path forms, "." and "..", trailing
// dots and spaces, every refused form, wildcards, and length limits.

#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include "g6/io/path.h"
#include "g6/io/text.h"
#include "g6test.h"

using g6::io::ConsolePath;
using g6::io::is_device_name;
using g6::io::parse_console_path;
using g6::io::ParsedPath;
using g6::io::Wildcards;

namespace {

const ConsolePath kNoCurrent{};
const ConsolePath kInstallDir{.drive = L'G', .components = {L"dir"}};
const ConsolePath kTempDir{.drive = L'T', .components = {L"x"}};

// The resolved path as text, or the error number when parsing failed.
std::string resolve(std::wstring_view path, const ConsolePath& current = kInstallDir,
                    Wildcards wildcards = Wildcards::Refuse) {
    const ParsedPath parsed = parse_console_path(path, current, wildcards);
    if (parsed.error != 0) {
        return "error " + std::to_string(parsed.error);
    }
    return g6::io::utf8(parsed.path.str());
}

std::string error(uint32_t code) {
    return "error " + std::to_string(code);
}

} // namespace

TEST_CASE("absolute paths keep their drive and drop repeated separators") {
    CHECK_EQ(resolve(L"G:\\a\\b"), std::string("G:\\a\\b"));
    CHECK_EQ(resolve(L"g:/a//b"), std::string("G:\\a\\b"));
    CHECK_EQ(resolve(L"T:\\", kNoCurrent), std::string("T:\\"));
    CHECK_EQ(resolve(L"G:\\a\\b\\", kNoCurrent), std::string("G:\\a\\b"));
    CHECK(parse_console_path(L"G:\\a\\b\\", kNoCurrent).trailing_separator);
    CHECK(parse_console_path(L"G:/a/", kNoCurrent).trailing_separator);
    CHECK(!parse_console_path(L"G:\\a\\b", kNoCurrent).trailing_separator);
}

TEST_CASE("drive-relative, rooted, and relative paths use the current directory") {
    // Drive-relative: the current directory when it is on that drive, else the root.
    CHECK_EQ(resolve(L"G:x"), std::string("G:\\dir\\x"));
    CHECK_EQ(resolve(L"G:"), std::string("G:\\dir"));
    CHECK_EQ(resolve(L"G:x", kTempDir), std::string("G:\\x"));
    CHECK_EQ(resolve(L"G:x", kNoCurrent), std::string("G:\\x"));
    // Rooted: the current drive.
    CHECK_EQ(resolve(L"\\a"), std::string("G:\\a"));
    CHECK_EQ(resolve(L"/a", kTempDir), std::string("T:\\a"));
    // Relative.
    CHECK_EQ(resolve(L"a\\b"), std::string("G:\\dir\\a\\b"));
    CHECK_EQ(resolve(L"a/b", kTempDir), std::string("T:\\x\\a\\b"));
    // Without a current directory, only absolute paths resolve.
    CHECK_EQ(resolve(L"a", kNoCurrent), error(ERROR_PATH_NOT_FOUND));
    CHECK_EQ(resolve(L"\\a", kNoCurrent), error(ERROR_PATH_NOT_FOUND));
    CHECK_EQ(resolve(L""), error(ERROR_PATH_NOT_FOUND));
}

TEST_CASE("dot and dot-dot resolve and stop at the root") {
    CHECK_EQ(resolve(L"G:\\a\\.\\b\\..\\c"), std::string("G:\\a\\c"));
    CHECK_EQ(resolve(L"G:\\..\\..\\a"), std::string("G:\\a"));
    CHECK_EQ(resolve(L"..\\..\\..\\y"), std::string("G:\\y"));
    CHECK_EQ(resolve(L".."), std::string("G:\\"));
    CHECK_EQ(resolve(L"."), std::string("G:\\dir"));
    CHECK_EQ(resolve(L"G:\\a\\b\\..\\..\\..\\..\\Windows"), std::string("G:\\Windows"));
}

TEST_CASE("trailing dots and spaces are dropped from every component") {
    CHECK_EQ(resolve(L"G:\\a.\\b. . "), std::string("G:\\a\\b"));
    CHECK_EQ(resolve(L"G:\\name.txt."), std::string("G:\\name.txt"));
    CHECK_EQ(resolve(L"G:\\a\\..."), std::string("G:\\a"));
    CHECK_EQ(resolve(L"G:\\a\\ . "), std::string("G:\\a"));
    CHECK_EQ(resolve(L"G:\\...\\b"), error(ERROR_PATH_NOT_FOUND));
    // Leading dots and spaces stay.
    CHECK_EQ(resolve(L"G:\\.hidden"), std::string("G:\\.hidden"));
    CHECK_EQ(resolve(L"G:\\ a"), std::string("G:\\ a"));
}

TEST_CASE("forms that could leave the console's drives are refused") {
    const wchar_t* const refused[] = {
        L"\\\\server\\share\\a",
        L"//server/share/a",
        L"\\\\?\\C:\\Windows",
        L"\\\\.\\PhysicalDrive0",
        L"\\??\\C:\\Windows",
        L"G:\\a:b",
        L"G:\\a\\file.txt:stream",
        L"G:\\a\\C:\\Windows",
        L"1:\\a",
        L"G:\\a<b",
        L"G:\\a>b",
        L"G:\\a\"b",
        L"G:\\a|b",
        L"G:\\a\u0001b",
        L"G:\\*.txt",
        L"G:\\a?",
        L"G:\\CON",
        L"G:\\nul.txt",
        L"G:\\dir\\com1",
        L"G:\\dir\\COM0.log",
        L"G:\\Lpt9",
        L"G:\\lpt\u00B9",
        L"G:\\CONOUT$",
        L"G:\\conin$.x",
        L"G:\\AUX .txt",
    };
    for (const wchar_t* path : refused) {
        INFO(g6::io::utf8(path));
        CHECK_EQ(resolve(path), error(ERROR_INVALID_NAME));
    }
}

TEST_CASE("look-alike colons and slashes are ordinary name characters") {
    // Fullwidth colon and solidus, division slash, and fraction slash: Win32 neither separates
    // components nor finds a drive at them, so they stay inside one component.
    for (const wchar_t* path :
         {L"G:\\a\uFF1Ab", L"G:\\..\uFF0F..\uFF0Fx", L"G:\\a\u2215b", L"G:\\a\u2044b"}) {
        INFO(g6::io::utf8(path));
        const ParsedPath parsed = parse_console_path(path, kNoCurrent);
        REQUIRE(parsed.error == 0);
        CHECK(parsed.path.drive == L'G');
        CHECK(parsed.path.components.size() == 1);
    }
    // A fullwidth drive letter and colon start a relative path, not an absolute one.
    CHECK_EQ(resolve(L"\uFF27\uFF1A\\x"), g6::io::utf8(L"G:\\dir\\\uFF27\uFF1A\\x"));
}

TEST_CASE("wildcards are allowed only in a search's last component") {
    CHECK_EQ(resolve(L"G:\\dir\\*.txt", kNoCurrent, Wildcards::LastComponent),
             std::string("G:\\dir\\*.txt"));
    CHECK_EQ(resolve(L"G:\\dir\\file?.*", kNoCurrent, Wildcards::LastComponent),
             std::string("G:\\dir\\file?.*"));
    CHECK_EQ(resolve(L"G:\\d*r\\x.txt", kNoCurrent, Wildcards::LastComponent),
             error(ERROR_INVALID_NAME));
    CHECK_EQ(resolve(L"G:\\dir\\a<b", kNoCurrent, Wildcards::LastComponent),
             error(ERROR_INVALID_NAME));
}

TEST_CASE("components and paths have Win32's length limits") {
    const std::wstring longest(255, L'a');
    CHECK_EQ(resolve(L"G:\\" + longest, kNoCurrent), "G:\\" + std::string(255, 'a'));
    CHECK_EQ(resolve(L"G:\\" + longest + L"a", kNoCurrent), error(ERROR_FILENAME_EXCED_RANGE));
    CHECK_EQ(resolve(L"G:\\" + std::wstring(32766, L'a'), kNoCurrent),
             error(ERROR_FILENAME_EXCED_RANGE));
    // Trailing dots past the limit are dropped before the length counts.
    CHECK_EQ(resolve(L"G:\\" + longest + L"...", kNoCurrent), "G:\\" + std::string(255, 'a'));
}

TEST_CASE("device names are recognized with and without an extension") {
    for (const wchar_t* name :
         {L"CON", L"con", L"con.txt", L"CON .txt", L"PRN", L"aux", L"NUL.tar.gz", L"COM0", L"com9",
          L"LPT1", L"CONIN$", L"conout$", L"COM\u00B2"}) {
        INFO(g6::io::utf8(name));
        CHECK(is_device_name(name));
    }
    for (const wchar_t* name :
         {L"CONSOLE", L"COM10", L"LPT", L"COMA", L"xCON", L"NULL", L"con_", L"data"}) {
        INFO(g6::io::utf8(name));
        CHECK(!is_device_name(name));
    }
}

TEST_CASE("console paths print as Win32 paths") {
    CHECK(ConsolePath{}.str().empty());
    CHECK((ConsolePath{.drive = L'G', .components = {}}.str() == L"G:\\"));
    CHECK((ConsolePath{.drive = L'T', .components = {L"a", L"b.txt"}}.str() == L"T:\\a\\b.txt"));
}
