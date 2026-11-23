#pragma once

// Console paths (docs/io.md, "Path rules"): a path the guest passes, resolved the way Win32
// resolves it, minus every form that could reach outside the console's drives. Parsing is pure
// string work; which drives exist is FileSystem's business.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace g6::io {

// An absolute console path with "." and ".." resolved: a drive letter and its components.
struct ConsolePath {
    wchar_t drive = L'\0'; // 'A' to 'Z'; '\0' for none
    std::vector<std::wstring> components;

    // "G:\a\b", or "G:\" for the root; empty when there is no drive.
    std::wstring str() const;
    bool operator==(const ConsolePath&) const = default;
};

enum class Wildcards : uint8_t {
    Refuse,        // '?' and '*' are invalid everywhere
    LastComponent, // allowed in the last component, as in a directory search pattern
};

struct ParsedPath {
    ConsolePath path;
    bool trailing_separator = false; // the path ended in '\' or '/'
    uint32_t error = 0;              // a Win32 error code; 0 on success
};

// Resolves `path` against `current`, the current directory (which may have no drive). Fails
// with ERROR_PATH_NOT_FOUND for an empty path, or a relative one when there is no current
// directory; ERROR_INVALID_NAME for a refused form; and ERROR_FILENAME_EXCED_RANGE for a
// component over 255 characters or a path over 32,767.
ParsedPath parse_console_path(std::wstring_view path, const ConsolePath& current,
                              Wildcards wildcards = Wildcards::Refuse);

// Whether a component names a Win32 device (CON, NUL, COM1, ...), with or without an extension.
bool is_device_name(std::wstring_view component);

} // namespace g6::io
