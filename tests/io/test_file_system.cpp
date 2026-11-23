// The console's drives on the host (docs/io.md, "The model"): mounts in a temporary directory,
// resolving to host paths, read-only and read-write access, the current directory, and mapping
// host paths back to console paths.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <windows.h>

#include "g6/io/file_system.h"
#include "g6/io/text.h"
#include "g6test.h"

namespace fs = std::filesystem;
using g6::io::Access;
using g6::io::FileSystem;
using g6::io::Resolved;

namespace {

// A fresh directory tree under the host's temp directory, removed again at the end:
//   install\data.txt, install\sub\inner.txt, data\.
class TempTree {
public:
    TempTree() {
        static std::atomic<int> counter{0};
        root_ = fs::temp_directory_path() / ("g6io-test-" + std::to_string(GetCurrentProcessId()) +
                                             "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(install() / "sub");
        fs::create_directories(data());
        write(install() / "data.txt", "hello");
        write(install() / "sub" / "inner.txt", "inner");
    }
    // remove_all with an error code throws only std::bad_alloc, which ends the test run anyway.
    ~TempTree() { // NOLINT(bugprone-exception-escape)
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    fs::path root() const { return root_; }
    fs::path install() const { return root_ / "install"; }
    fs::path data() const { return root_ / "data"; }

    static void write(const fs::path& path, const char* text) {
        std::ofstream(path, std::ios::binary) << text;
    }

private:
    fs::path root_;
};

std::string u8(const std::wstring& text) {
    return g6::io::utf8(text);
}

} // namespace

TEST_CASE("mounts need a drive letter and an existing directory") {
    const TempTree tree;
    FileSystem file_system;
    CHECK_THROWS_AS(file_system.mount(L'1', tree.install(), Access::ReadOnly),
                    std::invalid_argument);
    CHECK_THROWS_AS(file_system.mount(L'G', tree.root() / "missing", Access::ReadOnly),
                    std::invalid_argument);
    CHECK_THROWS_AS(file_system.mount(L'G', tree.install() / "data.txt", Access::ReadOnly),
                    std::invalid_argument);
    CHECK(!file_system.is_mounted(L'G'));
    CHECK(file_system.current_directory().empty());

    file_system.mount(L'g', tree.install(), Access::ReadOnly);
    CHECK(file_system.is_mounted(L'G'));
    CHECK_EQ(u8(file_system.current_directory()), std::string("G:\\"));
    // A later mount doesn't move the current directory.
    file_system.mount(L'T', tree.data(), Access::ReadWrite);
    CHECK_EQ(u8(file_system.current_directory()), std::string("G:\\"));

    file_system.unmount(L'G');
    CHECK(!file_system.is_mounted(L'G'));
    CHECK_EQ(file_system.resolve(L"G:\\data.txt").error, uint32_t{ERROR_PATH_NOT_FOUND});
    file_system.reset();
    CHECK(!file_system.is_mounted(L'T'));
    CHECK(file_system.current_directory().empty());
}

TEST_CASE("console paths resolve to host paths inside their mount") {
    const TempTree tree;
    FileSystem file_system;
    file_system.mount(L'G', tree.install(), Access::ReadOnly);
    file_system.mount(L'T', tree.data(), Access::ReadWrite);
    const fs::path install = fs::absolute(tree.install()).lexically_normal();

    const Resolved data = file_system.resolve(L"G:\\data.txt");
    REQUIRE(data.ok());
    CHECK(fs::path(data.host) == install / "data.txt");
    CHECK(!data.writable());
    CHECK(data.console.drive == L'G');

    // Relative to the current directory, and with ".." stopping at the mount's root.
    CHECK(fs::path(file_system.resolve(L"sub/inner.txt").host) == install / "sub" / "inner.txt");
    CHECK(fs::path(file_system.resolve(L"G:\\..\\..\\data.txt").host) == install / "data.txt");
    CHECK(fs::path(file_system.resolve(L"G:\\").host) == install);

    const Resolved temp = file_system.resolve(L"T:\\new.txt");
    REQUIRE(temp.ok());
    CHECK(temp.writable());

    // A trailing separator reaches the host, which refuses it on a file.
    const Resolved trailing = file_system.resolve(L"G:\\data.txt\\");
    REQUIRE(trailing.ok());
    CHECK(trailing.trailing_separator);
    CHECK(trailing.host.ends_with(L"data.txt\\"));

    CHECK_EQ(file_system.resolve(L"Q:\\x").error, uint32_t{ERROR_PATH_NOT_FOUND});
    CHECK_EQ(file_system.resolve(L"G:\\a:b").error, uint32_t{ERROR_INVALID_NAME});
    CHECK_EQ(file_system.resolve(L"G:\\*").error, uint32_t{ERROR_INVALID_NAME});
    CHECK(file_system.resolve(L"G:\\*", g6::io::Wildcards::LastComponent).ok());
}

TEST_CASE("long host paths get the long-path prefix") {
    const TempTree tree;
    FileSystem file_system;
    file_system.mount(L'T', tree.data(), Access::ReadWrite);
    const std::wstring component(100, L'd');
    const Resolved resolved =
        file_system.resolve(L"T:\\" + component + L"\\" + component + L"\\" + component);
    REQUIRE(resolved.ok());
    CHECK(resolved.host.starts_with(L"\\\\?\\"));
    CHECK(!file_system.resolve(L"T:\\short.txt").host.starts_with(L"\\\\?\\"));
}

TEST_CASE("host paths inside a mount map back to console paths") {
    const TempTree tree;
    fs::create_directories(tree.root() / "install2");
    FileSystem file_system;
    file_system.mount(L'G', tree.install(), Access::ReadOnly);
    file_system.mount(L'T', tree.data(), Access::ReadWrite);
    const std::wstring install = fs::absolute(tree.install()).lexically_normal().native();

    CHECK(file_system.console_path_of(install + L"\\sub\\inner.txt") ==
          std::optional<std::wstring>(L"G:\\sub\\inner.txt"));
    CHECK(file_system.console_path_of(L"\\\\?\\" + install + L"\\data.txt") ==
          std::optional<std::wstring>(L"G:\\data.txt"));
    CHECK(file_system.console_path_of(install) == std::optional<std::wstring>(L"G:\\"));
    // A sibling whose name starts like the mount's isn't inside it.
    CHECK(!file_system
               .console_path_of(
                   fs::absolute(tree.root() / "install2" / "x").lexically_normal().native())
               .has_value());
    CHECK(!file_system.console_path_of(L"C:\\Windows\\notepad.exe").has_value());
}

TEST_CASE("the current directory moves only to existing directories") {
    const TempTree tree;
    FileSystem file_system;
    file_system.mount(L'G', tree.install(), Access::ReadOnly);

    CHECK_EQ(file_system.set_current_directory(L"sub"), uint32_t{0});
    CHECK_EQ(u8(file_system.current_directory()), std::string("G:\\sub"));
    CHECK(file_system.resolve(L"inner.txt").ok());
    CHECK_EQ(u8(file_system.resolve(L"inner.txt").console.str()),
             std::string("G:\\sub\\inner.txt"));

    CHECK_EQ(file_system.set_current_directory(L"G:\\data.txt"), uint32_t{ERROR_DIRECTORY});
    CHECK(file_system.set_current_directory(L"G:\\missing") != 0);
    CHECK_EQ(file_system.set_current_directory(L"Q:\\"), uint32_t{ERROR_PATH_NOT_FOUND});
    CHECK_EQ(u8(file_system.current_directory()), std::string("G:\\sub"));

    CHECK_EQ(file_system.set_current_directory(L".."), uint32_t{0});
    CHECK_EQ(u8(file_system.current_directory()), std::string("G:\\"));
}

TEST_CASE("the standard drives are the install and the data directory's temp folder") {
    const TempTree tree;
    FileSystem file_system;
    g6::io::mount_standard_drives(file_system, tree.install(), tree.data());
    CHECK(fs::is_directory(tree.data() / "temp"));
    CHECK(file_system.is_mounted(L'G'));
    CHECK(file_system.is_mounted(L'T'));
    CHECK_EQ(u8(file_system.current_directory()), std::string("G:\\"));
    CHECK(!file_system.resolve(L"G:\\data.txt").writable());
    const Resolved temp = file_system.resolve(L"T:\\x");
    CHECK(temp.writable());
    CHECK(fs::path(temp.host) == fs::absolute(tree.data() / "temp" / "x").lexically_normal());

    const fs::path data = g6::io::user_data_directory(L"1.0.2.1");
    CHECK(data.filename() == L"1.0.2.1");
    CHECK(data.parent_path().filename() == L"G6Recomp");
}
