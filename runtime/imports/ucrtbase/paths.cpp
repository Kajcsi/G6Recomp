// The C runtime functions that take a path (docs/io.md, "Functions"). Each resolves its path
// through the active g6::io::FileSystem and calls the host CRT's wide function on the host path.
//
// The runtime's own errno is never touched here: in a debug build it belongs to another CRT than
// the guest's.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <windows.h>

#include "g6/imports/crt_paths.h"
#include "g6/io/file_system.h"
#include "g6/io/text.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"
#include "ucrtbase/host_crt.h"

// NOLINTBEGIN(bugprone-reserved-identifier)
namespace g6::imports::ucrtbase {
namespace {

using io::Resolved;

// The functions of the host CRT (ucrtbase/host_crt.h) that the path functions call.
class HostCrt {
public:
    using Open = FILE* (*)(const wchar_t*, const wchar_t*);
    using OpenSecure = errno_t (*)(FILE**, const wchar_t*, const wchar_t*);
    using OpenShared = FILE* (*)(const wchar_t*, const wchar_t*, int);
    using Stat = int (*)(const wchar_t*, struct ::_stat64*);
    using Access = int (*)(const wchar_t*, int);
    using Path = int (*)(const wchar_t*);
    using Rename = int (*)(const wchar_t*, const wchar_t*);
    using Location = int* (*)();
    using DosLocation = unsigned long* (*)();
    using Allocate = void* (*)(size_t);

    Open wfopen = nullptr;
    OpenSecure wfopen_s = nullptr;
    OpenShared wfsopen = nullptr;
    Stat wstat64 = nullptr;
    Access waccess = nullptr;
    Path wmkdir = nullptr;
    Path wrmdir = nullptr;
    Path wremove = nullptr;
    Rename wrename = nullptr;
    Location errno_location = nullptr;
    DosLocation doserrno_location = nullptr;
    Allocate malloc = nullptr;

    static const HostCrt& get() {
        static const HostCrt instance;
        return instance;
    }

private:
    HostCrt() {
        bind_host_crt("_wfopen", wfopen);
        bind_host_crt("_wfopen_s", wfopen_s);
        bind_host_crt("_wfsopen", wfsopen);
        bind_host_crt("_wstat64", wstat64);
        bind_host_crt("_waccess", waccess);
        bind_host_crt("_wmkdir", wmkdir);
        bind_host_crt("_wrmdir", wrmdir);
        bind_host_crt("_wremove", wremove);
        bind_host_crt("_wrename", wrename);
        bind_host_crt("_errno", errno_location);
        bind_host_crt("__doserrno", doserrno_location);
        bind_host_crt("malloc", malloc);
    }
};

void set_errno_value(int value) {
    *HostCrt::get().errno_location() = value;
}

// The CRT's errno for the Win32 errors a path can fail with, as its own mapping gives them.
int errno_for(uint32_t error) {
    switch (error) {
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_DIRECTORY: return ENOENT;
    default: return EINVAL;
    }
}

// Sets errno and _doserrno for a Win32 error, and returns the errno.
int set_errno(uint32_t error) {
    *HostCrt::get().doserrno_location() = static_cast<unsigned long>(error);
    const int value = errno_for(error);
    set_errno_value(value);
    return value;
}

int fail(uint32_t error) {
    set_errno(error);
    return -1;
}

// A narrow argument widened with the active code page. A null pointer stays null.
class Wide {
public:
    explicit Wide(const char* text) {
        if (text != nullptr) {
            text_ = io::widen(text);
            pointer_ = text_.c_str();
        }
    }
    Wide(const Wide&) = delete;
    Wide& operator=(const Wide&) = delete;

    const wchar_t* get() const { return pointer_; }

private:
    std::wstring text_;
    const wchar_t* pointer_ = nullptr;
};

// Resolves a path; `write` refuses a read-only mount. On failure errno is set.
Resolved resolve_path(const wchar_t* name, bool write) {
    Resolved resolved;
    if (name == nullptr) {
        resolved.error = ERROR_INVALID_PARAMETER;
    } else {
        resolved = io::active_file_system().resolve(name);
        if (resolved.ok() && write && !resolved.writable()) {
            resolved.error = ERROR_ACCESS_DENIED;
        }
    }
    if (!resolved.ok()) {
        set_errno(resolved.error);
    }
    return resolved;
}

bool exists(const Resolved& resolved) {
    return GetFileAttributesW(resolved.host.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Whether an fopen mode writes: "w", "a", or "+" before any ",ccs=" part.
bool mode_writes(std::wstring_view mode) {
    return mode.substr(0, mode.find(L',')).find_first_of(L"wa+") != std::wstring_view::npos;
}

// _getcwd's buffer rules: with no buffer, one of at least `length` characters is allocated
// with malloc for the caller to free.
template <typename Char>
Char* copy_directory(std::basic_string_view<Char> text, Char* buffer, int length) {
    if (buffer == nullptr) {
        const size_t size = std::max(text.size() + 1, static_cast<size_t>(std::max(length, 0)));
        // The guest frees it with the host CRT's free.
        buffer = static_cast<Char*>(HostCrt::get().malloc(size * sizeof(Char)));
        if (buffer == nullptr) {
            set_errno_value(ENOMEM);
            return nullptr;
        }
    } else if (length <= 0) {
        set_errno_value(EINVAL);
        return nullptr;
    } else if (static_cast<size_t>(length) <= text.size()) {
        set_errno_value(ERANGE);
        return nullptr;
    }
    std::copy(text.begin(), text.end(), buffer);
    buffer[text.size()] = Char{};
    return buffer;
}

} // namespace

// ---- Opening files ----

FILE* _wfopen(const wchar_t* name, const wchar_t* mode) {
    if (mode == nullptr) {
        set_errno_value(EINVAL);
        return nullptr;
    }
    const Resolved resolved = resolve_path(name, mode_writes(mode));
    if (!resolved.ok()) {
        return nullptr;
    }
    return HostCrt::get().wfopen(resolved.host.c_str(), mode);
}

FILE* fopen(const char* name, const char* mode) {
    const Wide wide_name(name);
    const Wide wide_mode(mode);
    return ucrtbase::_wfopen(wide_name.get(), wide_mode.get());
}

errno_t _wfopen_s(FILE** stream, const wchar_t* name, const wchar_t* mode) {
    if (stream == nullptr) {
        set_errno_value(EINVAL);
        return EINVAL;
    }
    *stream = nullptr;
    if (mode == nullptr) {
        set_errno_value(EINVAL);
        return EINVAL;
    }
    const Resolved resolved = resolve_path(name, mode_writes(mode));
    if (!resolved.ok()) {
        return errno_for(resolved.error);
    }
    return HostCrt::get().wfopen_s(stream, resolved.host.c_str(), mode);
}

errno_t fopen_s(FILE** stream, const char* name, const char* mode) {
    const Wide wide_name(name);
    const Wide wide_mode(mode);
    return ucrtbase::_wfopen_s(stream, wide_name.get(), wide_mode.get());
}

FILE* _wfsopen(const wchar_t* name, const wchar_t* mode, int share) {
    if (mode == nullptr) {
        set_errno_value(EINVAL);
        return nullptr;
    }
    const Resolved resolved = resolve_path(name, mode_writes(mode));
    if (!resolved.ok()) {
        return nullptr;
    }
    return HostCrt::get().wfsopen(resolved.host.c_str(), mode, share);
}

FILE* _fsopen(const char* name, const char* mode, int share) {
    const Wide wide_name(name);
    const Wide wide_mode(mode);
    return ucrtbase::_wfsopen(wide_name.get(), wide_mode.get(), share);
}

// ---- Status ----

int _wstat64(const wchar_t* name, struct ::_stat64* buffer) {
    if (buffer == nullptr) {
        set_errno_value(EINVAL);
        return -1;
    }
    const Resolved resolved = resolve_path(name, false);
    if (!resolved.ok()) {
        return -1;
    }
    const int result = HostCrt::get().wstat64(resolved.host.c_str(), buffer);
    if (result == 0) {
        // The drive number is the console drive's, not the host volume's.
        const auto drive = static_cast<_dev_t>(resolved.console.drive - L'A');
        buffer->st_dev = drive;
        buffer->st_rdev = drive;
    }
    return result;
}

int _stat64(const char* name, struct ::_stat64* buffer) {
    const Wide wide_name(name);
    return ucrtbase::_wstat64(wide_name.get(), buffer);
}

int _waccess(const wchar_t* name, int mode) {
    if ((mode & ~6) != 0) {
        return fail(ERROR_INVALID_PARAMETER);
    }
    const Resolved resolved = resolve_path(name, false);
    if (!resolved.ok()) {
        return -1;
    }
    const int result = HostCrt::get().waccess(resolved.host.c_str(), mode);
    // Nothing on a read-only mount can be written, whatever its attributes say.
    if (result == 0 && (mode & 2) != 0 && !resolved.writable()) {
        return fail(ERROR_ACCESS_DENIED);
    }
    return result;
}

int _access(const char* name, int mode) {
    const Wide wide_name(name);
    return ucrtbase::_waccess(wide_name.get(), mode);
}

// ---- Creating, removing, and renaming ----

int _wmkdir(const wchar_t* name) {
    const Resolved resolved = resolve_path(name, false);
    if (!resolved.ok()) {
        return -1;
    }
    if (!resolved.writable()) {
        // As CreateDirectory: an existing directory is reported as such.
        return fail(exists(resolved) ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED);
    }
    return HostCrt::get().wmkdir(resolved.host.c_str());
}

int _mkdir(const char* name) {
    const Wide wide_name(name);
    return ucrtbase::_wmkdir(wide_name.get());
}

int _wrmdir(const wchar_t* name) {
    const Resolved resolved = resolve_path(name, true);
    if (!resolved.ok()) {
        return -1;
    }
    if (resolved.console.components.empty()) {
        return fail(ERROR_ACCESS_DENIED);
    }
    return HostCrt::get().wrmdir(resolved.host.c_str());
}

int _rmdir(const char* name) {
    const Wide wide_name(name);
    return ucrtbase::_wrmdir(wide_name.get());
}

int _wremove(const wchar_t* name) {
    const Resolved resolved = resolve_path(name, true);
    if (!resolved.ok()) {
        return -1;
    }
    return HostCrt::get().wremove(resolved.host.c_str());
}

int remove(const char* name) {
    const Wide wide_name(name);
    return ucrtbase::_wremove(wide_name.get());
}

int _wunlink(const wchar_t* name) {
    return ucrtbase::_wremove(name);
}

int _unlink(const char* name) {
    return ucrtbase::remove(name);
}

int _wrename(const wchar_t* existing, const wchar_t* destination) {
    const Resolved from = resolve_path(existing, true);
    if (!from.ok()) {
        return -1;
    }
    const Resolved to = resolve_path(destination, true);
    if (!to.ok()) {
        return -1;
    }
    if (from.console.components.empty() || to.console.components.empty()) {
        return fail(ERROR_ACCESS_DENIED);
    }
    return HostCrt::get().wrename(from.host.c_str(), to.host.c_str());
}

int rename(const char* existing, const char* destination) {
    const Wide wide_existing(existing);
    const Wide wide_destination(destination);
    return ucrtbase::_wrename(wide_existing.get(), wide_destination.get());
}

// ---- The current directory ----

wchar_t* _wgetcwd(wchar_t* buffer, int length) {
    const std::wstring current = io::active_file_system().current_directory();
    if (current.empty()) {
        set_errno_value(ENOENT);
        return nullptr;
    }
    return copy_directory(std::wstring_view{current}, buffer, length);
}

char* _getcwd(char* buffer, int length) {
    const std::wstring current = io::active_file_system().current_directory();
    if (current.empty()) {
        set_errno_value(ENOENT);
        return nullptr;
    }
    const std::string ansi = io::narrow(current);
    return copy_directory(std::string_view{ansi}, buffer, length);
}

int _wchdir(const wchar_t* name) {
    if (name == nullptr) {
        return fail(ERROR_INVALID_PARAMETER);
    }
    const uint32_t error = io::active_file_system().set_current_directory(name);
    return error == 0 ? 0 : fail(error);
}

int _chdir(const char* name) {
    const Wide wide_name(name);
    return ucrtbase::_wchdir(wide_name.get());
}

// ---- Registration ----

namespace {

struct Entry {
    const char* name;
    const void* function;
};

template <typename Function>
Entry entry(const char* name, Function* function) {
    return {name, reinterpret_cast<const void*>(function)};
}

} // namespace

void register_paths(loader::ImportRegistry& imports) {
    const Entry functions[] = {
        entry("fopen", &fopen),         entry("_wfopen", &_wfopen),   entry("fopen_s", &fopen_s),
        entry("_wfopen_s", &_wfopen_s), entry("_fsopen", &_fsopen),   entry("_wfsopen", &_wfsopen),
        entry("_stat64", &_stat64),     entry("_wstat64", &_wstat64), entry("_access", &_access),
        entry("_waccess", &_waccess),   entry("_mkdir", &_mkdir),     entry("_wmkdir", &_wmkdir),
        entry("_rmdir", &_rmdir),       entry("_wrmdir", &_wrmdir),   entry("remove", &remove),
        entry("_wremove", &_wremove),   entry("_unlink", &_unlink),   entry("_wunlink", &_wunlink),
        entry("rename", &rename),       entry("_wrename", &_wrename), entry("_getcwd", &_getcwd),
        entry("_wgetcwd", &_wgetcwd),   entry("_chdir", &_chdir),     entry("_wchdir", &_wchdir),
    };
    for (const Entry& item : functions) {
        imports.hle("api-ms-win-crt-*", loader::Symbol::by_name(item.name), item.function);
    }
}

} // namespace g6::imports::ucrtbase
// NOLINTEND(bugprone-reserved-identifier)
