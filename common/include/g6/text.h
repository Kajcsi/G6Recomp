#pragma once

// Converting UTF-16 to UTF-8 for messages, and to a narrow code page when the result has to fit a
// fixed buffer. Windows' ANSI functions cut a result that doesn't fit after a whole character,
// never in the middle of a double-byte or UTF-8 sequence, so a caller never sees a lead byte
// without its trail.

#include <cstddef>
#include <string>
#include <string_view>
#include <windows.h>

namespace g6 {

// `text` as UTF-8, for logs and error messages. std::filesystem::path::string() converts to the
// ANSI code page instead, and throws for a character outside it, so an error about a file in a
// folder with such a name would become a different error.
inline std::string utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

// `text` in `code_page`, cut after the last whole character that fits in `max_bytes`. A surrogate
// pair counts as one character.
inline std::string narrow_prefix(std::wstring_view text, size_t max_bytes, UINT code_page) {
    std::string result;
    size_t at = 0;
    while (at < text.size()) {
        const size_t units =
            IS_HIGH_SURROGATE(text[at]) && at + 1 < text.size() && IS_LOW_SURROGATE(text[at + 1])
                ? 2
                : 1;
        char bytes[8];
        const int length =
            WideCharToMultiByte(code_page, 0, text.data() + at, static_cast<int>(units), bytes,
                                static_cast<int>(sizeof(bytes)), nullptr, nullptr);
        if (length <= 0 || result.size() + static_cast<size_t>(length) > max_bytes) {
            break;
        }
        result.append(bytes, static_cast<size_t>(length));
        at += units;
    }
    return result;
}

} // namespace g6
