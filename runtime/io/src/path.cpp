// Console path parsing (docs/io.md, "Path rules"). Written from Win32's documented path
// behaviour: separators, the four path forms, "." and "..", trailing dots and spaces, and the
// names and characters Win32 gives a meaning of its own.

#include "g6/io/path.h"

#include <array>
#include <cstddef>
#include <windows.h>

namespace g6::io {
namespace {

constexpr size_t kMaxComponent = 255;
constexpr size_t kMaxPath = 32767;

bool is_separator(wchar_t c) {
    return c == L'\\' || c == L'/';
}

bool is_ascii_letter(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

wchar_t to_upper_ascii(wchar_t c) {
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

bool equals_ascii_nocase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (to_upper_ascii(a[i]) != to_upper_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

// Characters a component may not hold. '?' and '*' are wildcards, and the DOS wildcards '<',
// '>', and '"' are refused everywhere. ':' would name a stream or a second drive.
bool is_refused_char(wchar_t c, bool allow_wildcards) {
    if (c < 0x20) {
        return true;
    }
    switch (c) {
    case L'<':
    case L'>':
    case L'"':
    case L'|':
    case L':': return true;
    case L'?':
    case L'*': return !allow_wildcards;
    default: return false;
    }
}

// Win32 drops trailing dots and spaces from the last component, and no name can be created
// with them through Win32, so they are dropped from every component.
std::wstring_view trim_trailing(std::wstring_view component) {
    while (!component.empty() && (component.back() == L'.' || component.back() == L' ')) {
        component.remove_suffix(1);
    }
    return component;
}

ParsedPath failure(uint32_t error) {
    ParsedPath parsed;
    parsed.error = error;
    return parsed;
}

} // namespace

std::wstring ConsolePath::str() const {
    if (drive == L'\0') {
        return {};
    }
    std::wstring text{drive, L':', L'\\'};
    for (size_t i = 0; i < components.size(); ++i) {
        if (i != 0) {
            text += L'\\';
        }
        text += components[i];
    }
    return text;
}

bool is_device_name(std::wstring_view component) {
    // The base name: up to the first dot, without trailing spaces.
    std::wstring_view base = component.substr(0, component.find(L'.'));
    while (!base.empty() && base.back() == L' ') {
        base.remove_suffix(1);
    }
    static constexpr std::array<std::wstring_view, 6> kNames = {L"CON", L"PRN",    L"AUX",
                                                                L"NUL", L"CONIN$", L"CONOUT$"};
    for (const std::wstring_view name : kNames) {
        if (equals_ascii_nocase(base, name)) {
            return true;
        }
    }
    // COM0-COM9 and LPT0-LPT9, and the superscript digits Win32 also accepts.
    if (base.size() == 4 && (equals_ascii_nocase(base.substr(0, 3), L"COM") ||
                             equals_ascii_nocase(base.substr(0, 3), L"LPT"))) {
        const wchar_t digit = base[3];
        return (digit >= L'0' && digit <= L'9') || digit == L'\u00B9' || digit == L'\u00B2' ||
               digit == L'\u00B3';
    }
    return false;
}

ParsedPath parse_console_path(std::wstring_view path, const ConsolePath& current,
                              Wildcards wildcards) {
    if (path.empty()) {
        return failure(ERROR_PATH_NOT_FOUND);
    }
    if (path.size() > kMaxPath) {
        return failure(ERROR_FILENAME_EXCED_RANGE);
    }
    // UNC paths and the \\?\ and \\.\ device forms.
    if (path.size() >= 2 && is_separator(path[0]) && is_separator(path[1])) {
        return failure(ERROR_INVALID_NAME);
    }

    ParsedPath parsed;
    std::wstring_view rest = path;
    if (path.size() >= 2 && path[1] == L':') {
        if (!is_ascii_letter(path[0])) {
            return failure(ERROR_INVALID_NAME);
        }
        parsed.path.drive = to_upper_ascii(path[0]);
        rest = path.substr(2);
        if (rest.empty() || !is_separator(rest.front())) {
            // Drive-relative: against the current directory on its drive, else the root.
            if (current.drive == parsed.path.drive) {
                parsed.path.components = current.components;
            }
        }
    } else if (is_separator(path[0])) {
        // Rooted: on the current drive.
        if (current.drive == L'\0') {
            return failure(ERROR_PATH_NOT_FOUND);
        }
        parsed.path.drive = current.drive;
    } else {
        if (current.drive == L'\0') {
            return failure(ERROR_PATH_NOT_FOUND);
        }
        parsed.path = current;
    }
    parsed.trailing_separator = !rest.empty() && is_separator(rest.back());

    std::vector<std::wstring>& components = parsed.path.components;
    size_t start = 0;
    while (start <= rest.size()) {
        size_t end = start;
        while (end < rest.size() && !is_separator(rest[end])) {
            ++end;
        }
        const std::wstring_view raw = rest.substr(start, end - start);
        const bool last = end >= rest.size();
        start = end + 1;
        if (raw.empty() || raw == L".") {
            continue;
        }
        if (raw == L"..") {
            // Stops at the root, as in Win32.
            if (!components.empty()) {
                components.pop_back();
            }
            continue;
        }
        const std::wstring_view name = trim_trailing(raw);
        if (name.empty()) {
            // Only dots and spaces: Win32 ends the path there, and in the middle nothing by
            // that name can exist.
            if (last) {
                continue;
            }
            return failure(ERROR_PATH_NOT_FOUND);
        }
        const bool allow_wildcards = last && wildcards == Wildcards::LastComponent;
        for (const wchar_t c : name) {
            if (is_refused_char(c, allow_wildcards)) {
                return failure(ERROR_INVALID_NAME);
            }
        }
        if (is_device_name(name)) {
            return failure(ERROR_INVALID_NAME);
        }
        if (name.size() > kMaxComponent) {
            return failure(ERROR_FILENAME_EXCED_RANGE);
        }
        components.emplace_back(name);
    }
    return parsed;
}

} // namespace g6::io
