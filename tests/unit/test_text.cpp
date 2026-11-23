// g6::narrow_prefix: a UTF-16 string in a narrow code page, cut after a whole character. Uses
// code pages every Windows install converts to (932, 1252, UTF-8), whatever the system's own is.

#include <string>

#include "g6/text.h"
#include "g6test.h"

using g6::narrow_prefix;

TEST_CASE("text that fits converts whole") {
    CHECK(narrow_prefix(L"G:\\game.exe", 64, 1252) == "G:\\game.exe");
    CHECK(narrow_prefix(L"G:\\game.exe", 11, 1252) == "G:\\game.exe");
    CHECK(narrow_prefix(L"", 4, 1252).empty());
    CHECK(narrow_prefix(L"abc", 0, 1252).empty());
}

TEST_CASE("single-byte text is cut at the limit") {
    CHECK(narrow_prefix(L"G:\\game.exe", 3, 1252) == "G:\\");
}

TEST_CASE("a double-byte character is never split") {
    // In Shift-JIS (932), U+65E5 is two bytes and 'a' is one.
    const std::wstring text = L"a\u65E5\u65E5";
    CHECK(narrow_prefix(text, 5, 932).size() == 5);
    CHECK(narrow_prefix(text, 4, 932).size() == 3);
    CHECK(narrow_prefix(text, 2, 932) == "a");
    CHECK(narrow_prefix(text, 1, 932) == "a");
}

TEST_CASE("a UTF-8 sequence and a surrogate pair stay whole") {
    // U+00E9 is two bytes in UTF-8, U+1F600 (a surrogate pair in UTF-16) is four.
    const std::wstring text = L"\u00E9\U0001F600x";
    CHECK(narrow_prefix(text, 7, CP_UTF8) == "\xC3\xA9\xF0\x9F\x98\x80x");
    CHECK(narrow_prefix(text, 6, CP_UTF8) == "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(narrow_prefix(text, 5, CP_UTF8) == "\xC3\xA9");
    CHECK(narrow_prefix(text, 1, CP_UTF8).empty());
}
