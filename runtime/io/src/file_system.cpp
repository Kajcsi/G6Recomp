// The console's drives on the host (docs/io.md, "The model").

#include "g6/io/file_system.h"

#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <windows.h>

#include "g6/io/text.h"
#include "g6/log.h"

namespace g6::io {
namespace {

// The longest path Win32 opens without the \\?\ prefix, with room for CreateDirectory's 8.3 name.
constexpr size_t kShortPathLimit = 248;

bool is_drive_root(std::wstring_view host_root) {
    return host_root.size() == 2 && host_root[1] == L':';
}

bool starts_with_nocase(std::wstring_view text, std::wstring_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    // Both lengths are passed, so neither view needs a terminator.
    const int length = static_cast<int>(prefix.size());
    const wchar_t* const head = text.data();   // NOLINT(bugprone-suspicious-stringview-data-usage)
    const wchar_t* const want = prefix.data(); // NOLINT(bugprone-suspicious-stringview-data-usage)
    return CompareStringOrdinal(head, length, want, length, TRUE) == CSTR_EQUAL;
}

std::wstring long_path(std::wstring path) {
    if (path.size() < kShortPathLimit) {
        return path;
    }
    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

} // namespace

size_t FileSystem::index(wchar_t drive) {
    if (drive >= L'a' && drive <= L'z') {
        return static_cast<size_t>(drive - L'a');
    }
    if (drive >= L'A' && drive <= L'Z') {
        return static_cast<size_t>(drive - L'A');
    }
    throw std::invalid_argument("a console drive must be a letter");
}

void FileSystem::mount(wchar_t drive, const std::filesystem::path& host_root, Access access) {
    const size_t slot = index(drive);
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(host_root, error).lexically_normal();
    if (error || !std::filesystem::is_directory(absolute, error)) {
        throw std::invalid_argument("a mount's host root must be an existing directory");
    }
    std::wstring root = absolute.native();
    while (root.size() > 2 && (root.back() == L'\\' || root.back() == L'/')) {
        root.pop_back();
    }
    const auto letter = static_cast<wchar_t>(L'A' + slot);
    const std::unique_lock lock(mutex_);
    mounts_[slot] = Mount{.host_root = std::move(root), .access = access, .mounted = true};
    if (current_.drive == L'\0') {
        current_ = ConsolePath{.drive = letter, .components = {}};
    }
    LOG_INFO(io, "mounted {}: at {} ({})", static_cast<char>(letter), utf8(mounts_[slot].host_root),
             access == Access::ReadOnly ? "read-only" : "read-write");
}

void FileSystem::unmount(wchar_t drive) {
    const size_t slot = index(drive);
    const std::unique_lock lock(mutex_);
    mounts_[slot] = Mount{};
}

void FileSystem::reset() {
    const std::unique_lock lock(mutex_);
    mounts_ = {};
    current_ = ConsolePath{};
    reported_drives_.store(0, std::memory_order_relaxed);
}

bool FileSystem::is_mounted(wchar_t drive) const {
    const size_t slot = index(drive);
    const std::shared_lock lock(mutex_);
    return mounts_[slot].mounted;
}

std::wstring FileSystem::host_path(const Mount& mount, const ConsolePath& path,
                                   bool trailing_separator) const {
    std::wstring host = mount.host_root;
    if (path.components.empty()) {
        if (is_drive_root(host)) {
            host += L'\\';
        }
        return host;
    }
    for (const std::wstring& component : path.components) {
        host += L'\\';
        host += component;
    }
    // Kept, so the host refuses "file.txt\" as Win32 would on the console.
    if (trailing_separator) {
        host += L'\\';
    }
    return long_path(std::move(host));
}

ParsedPath FileSystem::absolute(std::wstring_view path, Wildcards wildcards) const {
    const std::shared_lock lock(mutex_);
    return parse_console_path(path, current_, wildcards);
}

Resolved FileSystem::resolve(std::wstring_view path, Wildcards wildcards) const {
    Resolved resolved;
    ParsedPath parsed = absolute(path, wildcards);
    if (parsed.error != 0) {
        resolved.error = parsed.error;
        return resolved;
    }
    resolved.trailing_separator = parsed.trailing_separator;
    const size_t slot = index(parsed.path.drive);
    const std::shared_lock lock(mutex_);
    const Mount& mount = mounts_[slot];
    if (!mount.mounted) {
        const uint32_t bit = 1U << slot;
        if ((reported_drives_.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
            LOG_WARN(io, "path on drive {}:, which isn't mounted: {}",
                     static_cast<char>(parsed.path.drive), utf8(path));
        }
        resolved.error = ERROR_PATH_NOT_FOUND;
        return resolved;
    }
    resolved.host = host_path(mount, parsed.path, parsed.trailing_separator);
    resolved.access = mount.access;
    resolved.console = std::move(parsed.path);
    return resolved;
}

std::optional<std::wstring> FileSystem::console_path_of(std::wstring_view host_path) const {
    if (host_path.starts_with(L"\\\\?\\UNC\\")) {
        // Only the prefix goes; the share keeps its leading separators.
        return console_path_of(L"\\\\" + std::wstring(host_path.substr(8)));
    }
    if (host_path.starts_with(L"\\\\?\\")) {
        host_path.remove_prefix(4);
    }
    const std::shared_lock lock(mutex_);
    size_t best = mounts_.size();
    for (size_t slot = 0; slot < mounts_.size(); ++slot) {
        const Mount& mount = mounts_[slot];
        if (!mount.mounted || !starts_with_nocase(host_path, mount.host_root)) {
            continue;
        }
        const size_t length = mount.host_root.size();
        if (host_path.size() != length && host_path[length] != L'\\' && host_path[length] != L'/') {
            continue;
        }
        if (best == mounts_.size() || length > mounts_[best].host_root.size()) {
            best = slot;
        }
    }
    if (best == mounts_.size()) {
        return std::nullopt;
    }
    std::wstring_view rest = host_path.substr(mounts_[best].host_root.size());
    while (!rest.empty() && (rest.front() == L'\\' || rest.front() == L'/')) {
        rest.remove_prefix(1);
    }
    std::wstring console{static_cast<wchar_t>(L'A' + best), L':', L'\\'};
    for (const wchar_t c : rest) {
        console += c == L'/' ? L'\\' : c;
    }
    return console;
}

std::wstring FileSystem::current_directory() const {
    const std::shared_lock lock(mutex_);
    return current_.str();
}

uint32_t FileSystem::set_current_directory(std::wstring_view path) {
    Resolved resolved = resolve(path);
    if (!resolved.ok()) {
        return resolved.error;
    }
    const DWORD attributes = GetFileAttributesW(resolved.host.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError();
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return ERROR_DIRECTORY;
    }
    const std::unique_lock lock(mutex_);
    current_ = std::move(resolved.console);
    return 0;
}

FileSystem& active_file_system() {
    static FileSystem instance;
    return instance;
}

std::filesystem::path user_data_directory(std::wstring_view build_id) {
    // %LOCALAPPDATA% rather than the shell's known-folder API, which would bring shell32 and
    // dozens of its dependencies into the guest's process for one path.
    std::filesystem::path base;
    std::wstring local_app_data(MAX_PATH, L'\0');
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(),
                                           static_cast<DWORD>(local_app_data.size()));
    if (length > local_app_data.size()) {
        local_app_data.resize(length);
        length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), length);
    }
    if (length != 0 && length < local_app_data.size()) {
        local_app_data.resize(length);
        base = local_app_data;
    } else {
        base = std::filesystem::temp_directory_path();
    }
    return base / L"G6Recomp" / build_id;
}

void mount_standard_drives(FileSystem& file_system, const std::filesystem::path& install,
                           const std::filesystem::path& data) {
    const std::filesystem::path temp = data / L"temp";
    std::filesystem::create_directories(temp);
    file_system.mount(L'G', install, Access::ReadOnly);
    file_system.mount(L'T', temp, Access::ReadWrite);
    const uint32_t error = file_system.set_current_directory(L"G:\\");
    if (error != 0) {
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "cannot enter G:\\");
    }
}

} // namespace g6::io
