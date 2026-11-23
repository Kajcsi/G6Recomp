// The Win32 and C runtime path functions on runtime/io (docs/io.md, "Functions"), called through
// mounts in a temporary directory: reads from the read-only install, writes to the read-write
// temp drive, refusals, and the console's current directory and temp path.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <share.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <windows.h>

#include "g6/imports/crt_paths.h"
#include "g6/imports/files.h"
#include "g6/io/file_system.h"
#include "g6/io/text.h"
#include "g6test.h"

namespace fs = std::filesystem;
namespace k = g6::imports::kernelbase;
namespace crt = g6::imports::ucrtbase;

namespace {

// install\data.txt ("hello") and install\sub\inner.txt mounted read-only at G:, and data\temp
// mounted read-write at T:, on the active FileSystem. Everything is removed at the end.
class Drives {
public:
    Drives() {
        static std::atomic<int> counter{0};
        root_ =
            fs::temp_directory_path() / ("g6files-test-" + std::to_string(GetCurrentProcessId()) +
                                         "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(root_ / "install" / "sub");
        std::ofstream(root_ / "install" / "data.txt", std::ios::binary) << "hello";
        std::ofstream(root_ / "install" / "sub" / "inner.txt", std::ios::binary) << "inner";
        g6::io::active_file_system().reset();
        g6::io::mount_standard_drives(g6::io::active_file_system(), root_ / "install",
                                      root_ / "data");
    }
    // remove_all with an error code throws only std::bad_alloc, which ends the test run anyway.
    ~Drives() { // NOLINT(bugprone-exception-escape)
        g6::io::active_file_system().reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    Drives(const Drives&) = delete;
    Drives& operator=(const Drives&) = delete;

    fs::path install() const { return root_ / "install"; }
    fs::path temp() const { return root_ / "data" / "temp"; }

private:
    fs::path root_;
};

std::string read_all(HANDLE file) {
    char buffer[64] = {};
    DWORD read = 0;
    if (ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr) == FALSE) {
        return {};
    }
    return {buffer, read};
}

// The size of a host file, or -1. (Not std::filesystem::file_size, whose MSVC implementation
// trips clang-tidy's enum-range analyzer inside the standard header.)
int64_t size_of(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        return -1;
    }
    return (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// The guest's CRT, ucrtbase.dll, which the wrappers' FILE objects, errno, and buffers belong to.
// In a debug build this test's own CRT is ucrtbased, so it can't touch them directly.
struct GuestCrt {
    int (*fclose)(FILE*) = nullptr;
    size_t (*fread)(void*, size_t, size_t, FILE*) = nullptr;
    int (*fputs)(const char*, FILE*) = nullptr;
    int* (*errno_location)() = nullptr;
    void (*free)(void*) = nullptr;

    GuestCrt() {
        HMODULE module = LoadLibraryExW(L"ucrtbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        bind(module, "fclose", fclose);
        bind(module, "fread", fread);
        bind(module, "fputs", fputs);
        bind(module, "_errno", errno_location);
        bind(module, "free", free);
    }

    int& error() const { return *errno_location(); }

    template <typename Function>
    static void bind(HMODULE module, const char* name, Function& function) {
        function = reinterpret_cast<Function>(GetProcAddress(module, name));
        if (function == nullptr) {
            throw std::runtime_error(std::string("ucrtbase.dll has no ") + name);
        }
    }
};

std::string read_all(const GuestCrt& guest, FILE* file) {
    char buffer[64] = {};
    const size_t read = guest.fread(buffer, 1, sizeof(buffer) - 1, file);
    return {buffer, read};
}

// The last error after a call, so a check can't pick up one set later.
template <typename Call>
DWORD last_error_of(Call call) {
    SetLastError(0);
    call();
    return GetLastError();
}

} // namespace

TEST_CASE("CreateFile reads the install and refuses to write it") {
    const Drives drives;
    HANDLE file = k::CreateFileW(L"G:\\data.txt", GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CHECK_EQ(read_all(file), std::string("hello"));
    CloseHandle(file);

    // Relative to the current directory G:\, and through the ANSI version.
    file = k::CreateFileA("sub\\inner.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CHECK_EQ(read_all(file), std::string("inner"));
    CloseHandle(file);

    // Every way of writing is refused on the read-only mount.
    struct Write {
        DWORD access;
        DWORD disposition;
        DWORD flags;
    };
    for (const Write write :
         {Write{GENERIC_WRITE, OPEN_EXISTING, 0}, Write{FILE_APPEND_DATA, OPEN_EXISTING, 0},
          Write{DELETE, OPEN_EXISTING, 0}, Write{GENERIC_READ, CREATE_ALWAYS, 0},
          Write{GENERIC_READ, TRUNCATE_EXISTING, 0}, Write{GENERIC_READ, CREATE_NEW, 0},
          Write{GENERIC_READ, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE}}) {
        INFO("access " + std::to_string(write.access) + ", disposition " +
             std::to_string(write.disposition));
        HANDLE refused = INVALID_HANDLE_VALUE;
        const DWORD error = last_error_of([&] {
            refused = k::CreateFileW(L"G:\\data.txt", write.access, FILE_SHARE_READ, nullptr,
                                     write.disposition, write.flags, nullptr);
        });
        CHECK(refused == INVALID_HANDLE_VALUE);
        CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});
    }
    CHECK(fs::exists(drives.install() / "data.txt"));

    // OPEN_ALWAYS opens an existing file for reading, and can't create one.
    HANDLE existing = INVALID_HANDLE_VALUE;
    DWORD error = last_error_of([&] {
        existing = k::CreateFileW(L"G:\\data.txt", GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_ALWAYS, 0, nullptr);
    });
    REQUIRE(existing != INVALID_HANDLE_VALUE);
    CHECK_EQ(error, DWORD{ERROR_ALREADY_EXISTS});
    CloseHandle(existing);
    error = last_error_of([&] {
        CHECK(k::CreateFileW(L"G:\\new.txt", GENERIC_READ, 0, nullptr, OPEN_ALWAYS, 0, nullptr) ==
              INVALID_HANDLE_VALUE);
    });
    CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});
    CHECK(!fs::exists(drives.install() / "new.txt"));

    // A read-only mount turns MAXIMUM_ALLOWED into reading.
    file = k::CreateFileW(L"G:\\data.txt", MAXIMUM_ALLOWED, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CHECK_EQ(read_all(file), std::string("hello"));
    CloseHandle(file);
}

TEST_CASE("CreateFile writes to the temp drive and refuses paths outside the mounts") {
    const Drives drives;
    HANDLE file = k::CreateFileW(L"T:\\new.txt", GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(file, "abc", 3, &written, nullptr) != FALSE);
    CloseHandle(file);
    CHECK(size_of(drives.temp() / "new.txt") == 3);

    CREATEFILE2_EXTENDED_PARAMETERS parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    file = k::CreateFile2(L"T:\\two.txt", GENERIC_WRITE, 0, CREATE_ALWAYS, &parameters);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    CHECK(fs::exists(drives.temp() / "two.txt"));
    CHECK(k::CreateFile2(L"G:\\two.txt", GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr) ==
          INVALID_HANDLE_VALUE);

    const struct {
        const wchar_t* path;
        DWORD error;
    } refused[] = {
        {L"Q:\\x.txt", ERROR_PATH_NOT_FOUND},
        {L"C:\\Windows\\win.ini", ERROR_PATH_NOT_FOUND},
        {L"\\\\?\\C:\\Windows\\win.ini", ERROR_INVALID_NAME},
        {L"G:\\data.txt:stream", ERROR_INVALID_NAME},
        {L"G:\\NUL", ERROR_INVALID_NAME},
        {L"", ERROR_PATH_NOT_FOUND},
        {nullptr, ERROR_PATH_NOT_FOUND},
    };
    for (const auto& item : refused) {
        INFO(item.path != nullptr ? g6::io::utf8(item.path) : std::string("null"));
        HANDLE handle = INVALID_HANDLE_VALUE;
        const DWORD error = last_error_of([&] {
            handle = k::CreateFileW(item.path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        });
        CHECK(handle == INVALID_HANDLE_VALUE);
        CHECK_EQ(error, item.error);
    }
    // The host refuses a file name with a trailing separator, as Win32 would.
    CHECK(k::CreateFileW(L"G:\\data.txt\\", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) ==
          INVALID_HANDLE_VALUE);
}

TEST_CASE("directory searches list the mount and refuse the root") {
    const Drives drives;
    WIN32_FIND_DATAW data{};
    HANDLE search = k::FindFirstFileW(L"G:\\sub\\*", &data);
    REQUIRE(search != INVALID_HANDLE_VALUE);
    std::set<std::wstring> names;
    do {
        names.insert(data.cFileName);
    } while (FindNextFileW(search, &data) != FALSE);
    FindClose(search);
    CHECK(names.contains(L"inner.txt"));

    WIN32_FIND_DATAA ansi{};
    search = k::FindFirstFileA("G:\\*.txt", &ansi);
    REQUIRE(search != INVALID_HANDLE_VALUE);
    CHECK_EQ(std::string(ansi.cFileName), std::string("data.txt"));
    CHECK_EQ(ansi.nFileSizeLow, DWORD{5});
    FindClose(search);

    search = k::FindFirstFileExW(L"G:\\sub\\inner.txt", FindExInfoBasic, &data,
                                 FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    REQUIRE(search != INVALID_HANDLE_VALUE);
    FindClose(search);

    DWORD error =
        last_error_of([&] { CHECK(k::FindFirstFileW(L"G:\\", &data) == INVALID_HANDLE_VALUE); });
    CHECK_EQ(error, DWORD{ERROR_FILE_NOT_FOUND});
    error = last_error_of(
        [&] { CHECK(k::FindFirstFileW(L"G:\\s*b\\*", &data) == INVALID_HANDLE_VALUE); });
    CHECK_EQ(error, DWORD{ERROR_INVALID_NAME});
}

TEST_CASE("attributes are read anywhere and written only on the temp drive") {
    const Drives drives;
    CHECK((k::GetFileAttributesW(L"G:\\sub") & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK((k::GetFileAttributesW(L"G:\\") & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK(k::GetFileAttributesA("G:\\data.txt") != INVALID_FILE_ATTRIBUTES);
    CHECK(k::GetFileAttributesW(L"G:\\missing.txt") == INVALID_FILE_ATTRIBUTES);

    WIN32_FILE_ATTRIBUTE_DATA info{};
    CHECK(k::GetFileAttributesExW(L"G:\\data.txt", GetFileExInfoStandard, &info) != FALSE);
    CHECK_EQ(info.nFileSizeLow, DWORD{5});
    CHECK(k::GetFileAttributesExA("G:\\sub\\inner.txt", GetFileExInfoStandard, &info) != FALSE);

    const DWORD error = last_error_of(
        [] { CHECK(k::SetFileAttributesW(L"G:\\data.txt", FILE_ATTRIBUTE_HIDDEN) == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});
    std::ofstream(drives.temp() / "attr.txt") << "x";
    CHECK(k::SetFileAttributesW(L"T:\\attr.txt", FILE_ATTRIBUTE_HIDDEN) != FALSE);
    CHECK((GetFileAttributesW((drives.temp() / "attr.txt").c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);
}

TEST_CASE("directories and files are created, moved, and removed on the temp drive only") {
    const Drives drives;
    // On the install, an existing directory is reported as existing; anything else is denied.
    DWORD error = last_error_of([] { CHECK(k::CreateDirectoryW(L"G:\\sub", nullptr) == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_ALREADY_EXISTS});
    error = last_error_of([] { CHECK(k::CreateDirectoryA("G:\\new", nullptr) == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});
    CHECK(!fs::exists(drives.install() / "new"));

    CHECK(k::CreateDirectoryW(L"T:\\dir", nullptr) != FALSE);
    CHECK(fs::is_directory(drives.temp() / "dir"));
    CHECK(k::RemoveDirectoryA("T:\\dir") != FALSE);
    CHECK(!fs::exists(drives.temp() / "dir"));
    error = last_error_of([] { CHECK(k::RemoveDirectoryW(L"T:\\") == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});
    error = last_error_of([] { CHECK(k::RemoveDirectoryW(L"G:\\sub") == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});

    error = last_error_of([] { CHECK(k::DeleteFileW(L"G:\\data.txt") == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});
    CHECK(fs::exists(drives.install() / "data.txt"));

    CHECK(SUCCEEDED(k::CopyFile2(L"G:\\data.txt", L"T:\\copy.txt", nullptr)));
    CHECK(size_of(drives.temp() / "copy.txt") == 5);
    CHECK(k::CopyFile2(L"T:\\copy.txt", L"G:\\copy.txt", nullptr) ==
          HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));

    // Where the host has no CopyFile2, CopyFileExW does the copy on the host paths.
    const std::wstring source = (drives.install() / "data.txt").wstring();
    const std::wstring copy = (drives.temp() / "fallback.txt").wstring();
    COPYFILE2_EXTENDED_PARAMETERS fail_if_exists{};
    fail_if_exists.dwSize = sizeof(fail_if_exists);
    fail_if_exists.dwCopyFlags = COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_NO_OFFLOAD;
    CHECK(SUCCEEDED(k::detail::copy_file2_fallback(source.c_str(), copy.c_str(), nullptr)));
    CHECK(size_of(drives.temp() / "fallback.txt") == 5);
    CHECK(k::detail::copy_file2_fallback(source.c_str(), copy.c_str(), &fail_if_exists) ==
          HRESULT_FROM_WIN32(ERROR_FILE_EXISTS));
    fs::remove(drives.temp() / "fallback.txt");

    CHECK(k::MoveFileExW(L"T:\\copy.txt", L"T:\\moved.txt", 0) != FALSE);
    CHECK(fs::exists(drives.temp() / "moved.txt"));
    error = last_error_of([] { CHECK(k::MoveFileExW(L"G:\\data.txt", L"T:\\x.txt", 0) == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_ACCESS_DENIED});
    error = last_error_of([] {
        CHECK(k::MoveFileExW(L"T:\\moved.txt", L"T:\\x.txt", MOVEFILE_DELAY_UNTIL_REBOOT) == FALSE);
    });
    CHECK_EQ(error, DWORD{ERROR_INVALID_PARAMETER});
    error = last_error_of([] { CHECK(k::MoveFileExW(L"T:\\moved.txt", nullptr, 0) == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_INVALID_PARAMETER});

    CHECK(k::DeleteFileA("T:\\moved.txt") != FALSE);
    CHECK(!fs::exists(drives.temp() / "moved.txt"));
}

TEST_CASE("the current directory, full paths, and the temp path are console paths") {
    const Drives drives;
    wchar_t buffer[MAX_PATH] = {};
    CHECK_EQ(k::GetTempPathW(MAX_PATH, buffer), DWORD{3});
    CHECK(std::wstring(buffer) == L"T:\\");
    CHECK_EQ(k::GetTempPath2W(2, buffer), DWORD{4});

    CHECK_EQ(k::GetCurrentDirectoryW(MAX_PATH, buffer), DWORD{3});
    CHECK(std::wstring(buffer) == L"G:\\");
    CHECK_EQ(k::GetCurrentDirectoryW(0, nullptr), DWORD{4});
    CHECK(k::SetCurrentDirectoryW(L"sub") != FALSE);
    char ansi[MAX_PATH] = {};
    CHECK_EQ(k::GetCurrentDirectoryA(MAX_PATH, ansi), DWORD{6});
    CHECK_EQ(std::string(ansi), std::string("G:\\sub"));
    const DWORD error =
        last_error_of([] { CHECK(k::SetCurrentDirectoryA("G:\\data.txt") == FALSE); });
    CHECK_EQ(error, DWORD{ERROR_DIRECTORY});

    LPWSTR file_part = nullptr;
    CHECK_EQ(k::GetFullPathNameW(L"inner.txt", MAX_PATH, buffer, &file_part), DWORD{16});
    CHECK(std::wstring(buffer) == L"G:\\sub\\inner.txt");
    REQUIRE(file_part != nullptr);
    CHECK(std::wstring(file_part) == L"inner.txt");
    CHECK_EQ(k::GetFullPathNameW(L"inner.txt", 4, buffer, nullptr), DWORD{17});
    CHECK_EQ(k::GetFullPathNameW(L"..\\x\\", MAX_PATH, buffer, &file_part), DWORD{5});
    CHECK(std::wstring(buffer) == L"G:\\x\\");
    CHECK(file_part == nullptr);
    LPSTR ansi_part = nullptr;
    CHECK_EQ(k::GetFullPathNameA("T:/a/b.txt", MAX_PATH, ansi, &ansi_part), DWORD{10});
    CHECK_EQ(std::string(ansi), std::string("T:\\a\\b.txt"));
    REQUIRE(ansi_part != nullptr);
    CHECK_EQ(std::string(ansi_part), std::string("b.txt"));

    ULARGE_INTEGER available{};
    ULARGE_INTEGER total{};
    CHECK(k::GetDiskFreeSpaceExW(nullptr, &available, &total, nullptr) != FALSE);
    CHECK(total.QuadPart > 0);
    CHECK(k::GetDiskFreeSpaceExW(L"T:\\", &available, nullptr, nullptr) != FALSE);
}

TEST_CASE("the C runtime's path functions go through the same mounts") {
    const Drives drives;
    const GuestCrt guest;
    FILE* file = crt::fopen("G:\\data.txt", "rb");
    REQUIRE(file != nullptr);
    CHECK_EQ(read_all(guest, file), std::string("hello"));
    guest.fclose(file);

    guest.error() = 0;
    CHECK(crt::fopen("G:\\data.txt", "r+") == nullptr);
    CHECK_EQ(guest.error(), EACCES);
    guest.error() = 0;
    CHECK(crt::_wfopen(L"G:\\new.txt", L"w") == nullptr);
    CHECK_EQ(guest.error(), EACCES);
    guest.error() = 0;
    CHECK(crt::fopen("Q:\\x.txt", "r") == nullptr);
    CHECK_EQ(guest.error(), ENOENT);
    guest.error() = 0;
    CHECK(crt::fopen("G:\\a|b", "r") == nullptr);
    CHECK_EQ(guest.error(), EINVAL);

    file = nullptr;
    CHECK_EQ(crt::fopen_s(&file, "T:\\out.txt", "wb"), 0);
    REQUIRE(file != nullptr);
    guest.fputs("written", file);
    guest.fclose(file);
    CHECK(size_of(drives.temp() / "out.txt") == 7);
    file = reinterpret_cast<FILE*>(1);
    CHECK_EQ(crt::_wfopen_s(&file, L"G:\\x.txt", L"a"), EACCES);
    CHECK(file == nullptr);
    file = crt::_fsopen("T:\\out.txt", "rb", _SH_DENYWR);
    REQUIRE(file != nullptr);
    CHECK_EQ(read_all(guest, file), std::string("written"));
    guest.fclose(file);

    struct _stat64 status{};
    CHECK_EQ(crt::_stat64("G:\\data.txt", &status), 0);
    CHECK_EQ(status.st_size, 5LL);
    CHECK_EQ(status.st_dev, static_cast<_dev_t>('G' - 'A'));
    CHECK_EQ(crt::_wstat64(L"G:\\missing", &status), -1);

    CHECK_EQ(crt::_access("G:\\data.txt", 0), 0);
    CHECK_EQ(crt::_access("G:\\data.txt", 4), 0);
    guest.error() = 0;
    CHECK_EQ(crt::_access("G:\\data.txt", 2), -1);
    CHECK_EQ(guest.error(), EACCES);
    CHECK_EQ(crt::_waccess(L"T:\\out.txt", 6), 0);
    guest.error() = 0;
    CHECK_EQ(crt::_access("G:\\missing", 0), -1);
    CHECK_EQ(guest.error(), ENOENT);

    guest.error() = 0;
    CHECK_EQ(crt::_mkdir("G:\\sub"), -1);
    CHECK_EQ(guest.error(), EEXIST);
    guest.error() = 0;
    CHECK_EQ(crt::_wmkdir(L"G:\\new"), -1);
    CHECK_EQ(guest.error(), EACCES);
    CHECK_EQ(crt::_mkdir("T:\\made"), 0);
    CHECK_EQ(crt::_wrmdir(L"T:\\made"), 0);
    CHECK(!fs::exists(drives.temp() / "made"));
    CHECK_EQ(crt::_rmdir("T:\\"), -1);

    guest.error() = 0;
    CHECK_EQ(crt::remove("G:\\data.txt"), -1);
    CHECK_EQ(guest.error(), EACCES);
    CHECK_EQ(crt::rename("T:\\out.txt", "T:\\renamed.txt"), 0);
    CHECK(fs::exists(drives.temp() / "renamed.txt"));
    guest.error() = 0;
    CHECK_EQ(crt::_wrename(L"T:\\renamed.txt", L"G:\\renamed.txt"), -1);
    CHECK_EQ(guest.error(), EACCES);
    CHECK_EQ(crt::_unlink("T:\\renamed.txt"), 0);
    CHECK(!fs::exists(drives.temp() / "renamed.txt"));

    char* current = crt::_getcwd(nullptr, 0);
    REQUIRE(current != nullptr);
    CHECK_EQ(std::string(current), std::string("G:\\"));
    guest.free(current);
    char small[2] = {};
    guest.error() = 0;
    CHECK(crt::_getcwd(small, 2) == nullptr);
    CHECK_EQ(guest.error(), ERANGE);
    CHECK_EQ(crt::_chdir("sub"), 0);
    wchar_t wide[MAX_PATH] = {};
    REQUIRE(crt::_wgetcwd(wide, MAX_PATH) != nullptr);
    CHECK(std::wstring(wide) == L"G:\\sub");
    guest.error() = 0;
    CHECK_EQ(crt::_wchdir(L"Q:\\"), -1);
    CHECK_EQ(guest.error(), ENOENT);
}
