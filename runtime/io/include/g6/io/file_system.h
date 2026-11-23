#pragma once

// The console's file system on the host (docs/io.md, "The model"): drive letters mounted on host
// directories, read-only or read-write, and the guest's current directory. The HLE file functions
// in runtime/imports reach files only through the active FileSystem, so the guest sees its
// mounted drives and nothing else.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "g6/io/path.h"

namespace g6::io {

enum class Access : uint8_t { ReadOnly, ReadWrite };

struct Resolved {
    ConsolePath console;
    std::wstring host; // the host path to open; empty on failure
    Access access = Access::ReadOnly;
    bool trailing_separator = false;
    uint32_t error = 0; // a Win32 error code; 0 on success

    bool ok() const { return error == 0; }
    bool writable() const { return access == Access::ReadWrite; }
};

class FileSystem {
public:
    // Mounts `host_root`, an existing directory, at `drive` ('A' to 'Z', either case), replacing
    // an earlier mount there. The first drive mounted becomes the current directory's. Throws
    // std::invalid_argument for a drive that isn't a letter or a root that isn't a directory.
    void mount(wchar_t drive, const std::filesystem::path& host_root, Access access);
    void unmount(wchar_t drive);
    // Removes every mount and the current directory.
    void reset();
    bool is_mounted(wchar_t drive) const;

    // Resolves a console path against the current directory to a host path. A drive that isn't
    // mounted gives ERROR_PATH_NOT_FOUND, and is logged the first time it is asked for.
    Resolved resolve(std::wstring_view path, Wildcards wildcards = Wildcards::Refuse) const;
    // The absolute console path, without looking at mounts: what GetFullPathName returns.
    ParsedPath absolute(std::wstring_view path, Wildcards wildcards = Wildcards::Refuse) const;
    // The console path of a host path inside a mount, or nothing when it is in none.
    std::optional<std::wstring> console_path_of(std::wstring_view host_path) const;

    // The current directory ("G:\..."), or empty before anything is mounted.
    std::wstring current_directory() const;
    // Changes the current directory to an existing directory. Returns a Win32 error, 0 on
    // success.
    uint32_t set_current_directory(std::wstring_view path);

private:
    struct Mount {
        std::wstring host_root; // absolute, without a trailing separator
        Access access = Access::ReadOnly;
        bool mounted = false;
    };

    static size_t index(wchar_t drive);
    std::wstring host_path(const Mount& mount, const ConsolePath& path,
                           bool trailing_separator) const;

    mutable std::shared_mutex mutex_;
    std::array<Mount, 26> mounts_;
    ConsolePath current_;
    mutable std::atomic<uint32_t> reported_drives_{0};
};

// The FileSystem the HLE functions use. It starts with no mounts, so every path fails with
// ERROR_PATH_NOT_FOUND until the runtime mounts the standard drives.
FileSystem& active_file_system();

// The per-user data directory for a build: %LOCALAPPDATA%\G6Recomp\<build ID>.
std::filesystem::path user_data_directory(std::wstring_view build_id);

// Mounts the standard drives (docs/io.md): the install read-only at G:, and <data>\temp,
// created when missing, read-write at T:. The current directory becomes G:\. Throws
// std::filesystem::filesystem_error when the temp directory can't be created, and
// std::invalid_argument when the install isn't a directory.
void mount_standard_drives(FileSystem& file_system, const std::filesystem::path& install,
                           const std::filesystem::path& data);

} // namespace g6::io
