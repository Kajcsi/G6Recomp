#pragma once

// Small string helpers for the loader: module names are ASCII in practice, Win32 wants UTF-16.

#include <string>
#include <string_view>
#include <windows.h>

namespace g6::loader {

inline std::string ascii_lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

inline std::wstring widen(std::string_view text, UINT code_page = CP_UTF8) {
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

inline std::string narrow(std::wstring_view text, UINT code_page = CP_UTF8) {
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

} // namespace g6::loader
