#pragma once

// The C runtime functions that take a path (docs/io.md, "Functions"), for imports from
// api-ms-win-crt-*. Each resolves its path through the active g6::io::FileSystem and calls the
// host CRT's wide function on the host path: ucrtbase.dll's, which the guest's forwarded CRT
// imports bind to. FILE objects, errno, and malloc'd buffers therefore belong to the guest's CRT,
// also in a debug build of the runtime, whose own CRT is ucrtbased.
//
// Failures set errno as the CRT does: ENOENT for a path that doesn't resolve, EINVAL for a
// refused name, EACCES for a write to a read-only mount.

#include <cstdio>
#include <sys/stat.h>

namespace g6::loader {
class ImportRegistry;
}

// The names are the CRT's own, as CLAUDE.md asks; they live in this namespace only.
// NOLINTBEGIN(bugprone-reserved-identifier)
namespace g6::imports::ucrtbase {

FILE* fopen(const char* name, const char* mode);
FILE* _wfopen(const wchar_t* name, const wchar_t* mode);
errno_t fopen_s(FILE** stream, const char* name, const char* mode);
errno_t _wfopen_s(FILE** stream, const wchar_t* name, const wchar_t* mode);
FILE* _fsopen(const char* name, const char* mode, int share);
FILE* _wfsopen(const wchar_t* name, const wchar_t* mode, int share);

int _stat64(const char* name, struct ::_stat64* buffer);
int _wstat64(const wchar_t* name, struct ::_stat64* buffer);
int _access(const char* name, int mode);
int _waccess(const wchar_t* name, int mode);

int _mkdir(const char* name);
int _wmkdir(const wchar_t* name);
int _rmdir(const char* name);
int _wrmdir(const wchar_t* name);
int remove(const char* name);
int _wremove(const wchar_t* name);
int _unlink(const char* name);
int _wunlink(const wchar_t* name);
int rename(const char* existing, const char* destination);
int _wrename(const wchar_t* existing, const wchar_t* destination);

char* _getcwd(char* buffer, int length);
wchar_t* _wgetcwd(wchar_t* buffer, int length);
int _chdir(const char* name);
int _wchdir(const wchar_t* name);

// Binds the functions above for the CRT API sets.
void register_paths(loader::ImportRegistry& imports);

} // namespace g6::imports::ucrtbase
// NOLINTEND(bugprone-reserved-identifier)
