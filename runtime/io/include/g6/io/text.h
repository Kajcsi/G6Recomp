#pragma once

// Conversions between the guest's narrow strings and the UTF-16 paths Win32 takes. The ANSI file
// functions use the active code page, as on desktop Windows (docs/io.md, "Functions").

#include <string>
#include <string_view>
#include <windows.h>

namespace g6::io {

inline std::wstring widen(std::string_view text, UINT code_page = CP_ACP) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

inline std::string narrow(std::wstring_view text, UINT code_page = CP_ACP) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(code_page, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(code_page, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

inline std::string utf8(std::wstring_view text) {
    return narrow(text, CP_UTF8);
}

} // namespace g6::io
