// XGameSave on a folder (docs/gdk.md, "XGameSave"): providers for the offline user, containers
// that appear once an update is submitted, atomic updates, reads into one buffer, names checked
// by the public rules and matched without case, deletes, the limits, handles that expire, the
// time-sensitive rule, files a crash leaves behind, and the slots called through QueryApiImpl.

#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

#include "g6/gdk/async.h"
#include "g6/gdk/game_save.h"
#include "g6/gdk/runtime.h"
#include "g6/gdk/task_queue.h"
#include "g6/gdk/thread.h"
#include "g6/gdk/user.h"
#include "g6test.h"

using namespace g6::gdk;
namespace fs = std::filesystem;

namespace {

using Mode = XTaskQueueDispatchMode;
using Port = XTaskQueuePort;

constexpr const char* kConfiguration = "Test-Config";
constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80070490);    // ERROR_NOT_FOUND
constexpr HRESULT kFileCorrupt = static_cast<HRESULT>(0x80070570); // ERROR_FILE_CORRUPT

// A manual queue for one test.
struct Queue {
    XTaskQueueHandle handle = nullptr;
    Queue() { REQUIRE(XTaskQueueCreate(Mode::Manual, Mode::Manual, &handle) == S_OK); }
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;
    ~Queue() { XTaskQueueCloseHandle(handle); }

    void finish() const {
        while (XTaskQueueDispatch(handle, Port::Work, 0) ||
               XTaskQueueDispatch(handle, Port::Completion, 0)) {
        }
    }
};

XAsyncBlock block_on(const Queue& queue) {
    XAsyncBlock async{};
    async.queue = queue.handle;
    return async;
}

XUserHandle add_user() {
    const Queue queue;
    XAsyncBlock async = block_on(queue);
    REQUIRE(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &async) == S_OK);
    queue.finish();
    XUserHandle user = nullptr;
    REQUIRE(XUserAddResult(&async, &user) == S_OK);
    return user;
}

// A save directory for one test under the host's temp directory, removed at the end.
class SaveRoot {
public:
    SaveRoot() {
        static std::atomic<int> counter{0};
        root_ =
            fs::temp_directory_path() / ("g6saves-test-" + std::to_string(GetCurrentProcessId()) +
                                         "-" + std::to_string(counter.fetch_add(1)));
        set_game_save_directory(root_);
    }
    // remove_all with an error code throws only std::bad_alloc, which ends the test run anyway.
    ~SaveRoot() { // NOLINT(bugprone-exception-escape)
        set_game_save_directory({});
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    SaveRoot(const SaveRoot&) = delete;
    SaveRoot& operator=(const SaveRoot&) = delete;
    SaveRoot(SaveRoot&&) = delete;
    SaveRoot& operator=(SaveRoot&&) = delete;

    const fs::path& path() const { return root_; }
    // The folder of kConfiguration.
    fs::path configuration() const { return root_ / "s-test-config"; }

private:
    fs::path root_;
};

// The offline user with a provider for kConfiguration.
struct Session {
    XUserHandle user = add_user();
    XGameSaveProviderHandle provider = nullptr;

    Session() {
        REQUIRE(XGameSaveInitializeProvider(user, kConfiguration, false, &provider) == S_OK);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() {
        XGameSaveCloseProvider(provider);
        XUserCloseHandle(user);
    }
};

// A container handle, closed at the end.
struct Container {
    XGameSaveContainerHandle handle = nullptr;

    Container(XGameSaveProviderHandle provider, const char* name) {
        REQUIRE(XGameSaveCreateContainer(provider, name, &handle) == S_OK);
    }
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) = delete;
    Container& operator=(Container&&) = delete;
    ~Container() { XGameSaveCloseContainer(handle); }
};

const uint8_t* bytes_of(std::string_view text) {
    return reinterpret_cast<const uint8_t*>(text.data());
}

using Blobs = std::initializer_list<std::pair<const char*, std::string_view>>;

// Writes blobs to a container in one update.
HRESULT write(XGameSaveProviderHandle provider, const char* container, Blobs blobs,
              const char* display_name = nullptr) {
    XGameSaveContainerHandle handle = nullptr;
    HRESULT result = XGameSaveCreateContainer(provider, container, &handle);
    XGameSaveUpdateHandle update = nullptr;
    if (SUCCEEDED(result)) {
        result = XGameSaveCreateUpdate(handle, display_name, &update);
    }
    for (const auto& [name, data] : blobs) {
        if (SUCCEEDED(result)) {
            result = XGameSaveSubmitBlobWrite(update, name, bytes_of(data), data.size());
        }
    }
    if (SUCCEEDED(result)) {
        result = XGameSaveSubmitUpdate(update);
    }
    XGameSaveCloseUpdate(update);
    XGameSaveCloseContainer(handle);
    return result;
}

using Contents = std::vector<std::pair<std::string, std::string>>;

// The blobs as names and data, from records laid out as XGameSaveReadBlobData leaves them.
Contents contents_of(const XGameSaveBlob* blobs, uint32_t count) {
    Contents found;
    for (uint32_t i = 0; i < count; ++i) {
        found.emplace_back(
            blobs[i].info.name,
            std::string(reinterpret_cast<const char*>(blobs[i].data), blobs[i].info.size));
    }
    return found;
}

// Reads synchronously into a large buffer; all blobs when `names` is empty.
HRESULT read(XGameSaveContainerHandle container, std::vector<const char*> names, Contents& found) {
    std::vector<XGameSaveBlob> buffer(1024);
    auto count = static_cast<uint32_t>(names.size());
    const HRESULT result =
        XGameSaveReadBlobData(container, names.empty() ? nullptr : names.data(), &count,
                              buffer.size() * sizeof(XGameSaveBlob), buffer.data());
    if (SUCCEEDED(result)) {
        found = contents_of(buffer.data(), count);
    }
    return result;
}

// A container's info, with its strings copied: they are valid only during the callback.
struct Info {
    std::string name;
    std::string display_name;
    XGameSaveContainerInfo values;
};

bool CALLBACK collect_info(const XGameSaveContainerInfo* info, void* context) {
    static_cast<std::vector<Info>*>(context)->push_back(
        {.name = info->name, .display_name = info->display_name, .values = *info});
    return true;
}

bool CALLBACK collect_name(const XGameSaveContainerInfo* info, void* context) {
    static_cast<std::vector<std::string>*>(context)->emplace_back(info->name);
    return true;
}

bool CALLBACK first_only(const XGameSaveContainerInfo* info, void* context) {
    static_cast<std::vector<std::string>*>(context)->emplace_back(info->name);
    return false;
}

bool CALLBACK collect_blob(const XGameSaveBlobInfo* info, void* context) {
    static_cast<std::vector<std::pair<std::string, uint32_t>>*>(context)->emplace_back(info->name,
                                                                                       info->size);
    return true;
}

std::vector<std::string> containers(XGameSaveProviderHandle provider,
                                    const char* prefix = nullptr) {
    std::vector<std::string> names;
    const HRESULT result =
        prefix != nullptr
            ? XGameSaveEnumerateContainerInfoByName(provider, prefix, &names, &collect_name)
            : XGameSaveEnumerateContainerInfo(provider, &names, &collect_name);
    CHECK(result == S_OK);
    return names;
}

HRESULT info_of(XGameSaveProviderHandle provider, const char* name, Info& info) {
    std::vector<Info> found;
    const HRESULT result = XGameSaveGetContainerInfo(provider, name, &found, &collect_info);
    if (SUCCEEDED(result)) {
        REQUIRE(found.size() == 1);
        info = found[0];
    }
    return result;
}

bool exists(XGameSaveProviderHandle provider, const char* name) {
    Info info{};
    return info_of(provider, name, info) == S_OK;
}

std::vector<std::string> files_in(const fs::path& folder) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

void write_text(const fs::path& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
}

// Calls a slot as a thunk does: the object first, then the function's arguments.
template <typename Result, typename... Args>
Result call(void* object, int slot, Args... args) {
    const void* const* vtable = *static_cast<const void* const* const*>(object);
    using Function = Result (*)(void*, Args...);
    return reinterpret_cast<Function>(const_cast<void*>(vtable[slot]))(object, args...);
}

} // namespace

TEST_CASE("providers are for the offline user and a configuration") {
    const SaveRoot root;
    XUser* const user = add_user();
    XGameSaveProviderHandle provider = nullptr;
    CHECK(XGameSaveInitializeProvider(nullptr, kConfiguration, false, &provider) == E_INVALIDARG);
    CHECK(XGameSaveInitializeProvider(user, nullptr, false, &provider) ==
          kGameSaveNoServiceConfiguration);
    CHECK(XGameSaveInitializeProvider(user, "", false, &provider) ==
          kGameSaveNoServiceConfiguration);
    CHECK(XGameSaveInitializeProvider(user, kConfiguration, false, nullptr) == E_INVALIDARG);
    CHECK(provider == nullptr);

    REQUIRE(XGameSaveInitializeProvider(user, kConfiguration, true, &provider) == S_OK);
    CHECK(provider != nullptr);
    CHECK(fs::is_directory(root.configuration()));
    XGameSaveCloseProvider(provider);
    XGameSaveCloseProvider(nullptr);

    // Without a save directory there is nowhere to save.
    set_game_save_directory({});
    provider = nullptr;
    CHECK(XGameSaveInitializeProvider(user, kConfiguration, false, &provider) == kGameSaveNoAccess);
    CHECK(provider == nullptr);
    set_game_save_directory(root.path());
    XUserCloseHandle(user);
}

TEST_CASE("the asynchronous provider and the quota") {
    const SaveRoot root;
    XUser* const user = add_user();
    const Queue queue;
    XAsyncBlock async = block_on(queue);
    CHECK(XGameSaveInitializeProviderAsync(nullptr, kConfiguration, false, &async) == E_INVALIDARG);
    CHECK(XGameSaveInitializeProviderAsync(user, "", false, &async) ==
          kGameSaveNoServiceConfiguration);
    REQUIRE(XGameSaveInitializeProviderAsync(user, kConfiguration, false, &async) == S_OK);
    CHECK(XAsyncGetStatus(&async, false) == E_PENDING);
    queue.finish();
    // A null out pointer leaves the result to fetch.
    CHECK(XGameSaveInitializeProviderResult(&async, nullptr) == E_INVALIDARG);
    XGameSaveProviderHandle provider = nullptr;
    REQUIRE(XGameSaveInitializeProviderResult(&async, &provider) == S_OK);
    REQUIRE(provider != nullptr);

    int64_t quota = 0;
    CHECK(XGameSaveGetRemainingQuota(provider, &quota) == S_OK);
    CHECK_EQ(quota, kGameSaveQuota);
    CHECK(XGameSaveGetRemainingQuota(provider, nullptr) == E_INVALIDARG);
    REQUIRE(write(provider, "slot", {{"data", std::string(100, 'x')}}) == S_OK);
    CHECK(XGameSaveGetRemainingQuota(provider, &quota) == S_OK);
    CHECK_EQ(quota, kGameSaveQuota - 100);

    async = block_on(queue);
    REQUIRE(XGameSaveGetRemainingQuotaAsync(provider, &async) == S_OK);
    queue.finish();
    quota = 0;
    CHECK(XGameSaveGetRemainingQuotaResult(&async, &quota) == S_OK);
    CHECK_EQ(quota, kGameSaveQuota - 100);

    XGameSaveCloseProvider(provider);
    XUserCloseHandle(user);
}

TEST_CASE("a container appears once an update is submitted") {
    const SaveRoot root;
    const Session session;
    const Container container(session.provider, "slot1");
    Info info{};
    CHECK(info_of(session.provider, "slot1", info) == kNotFound);
    CHECK(containers(session.provider).empty());

    XGameSaveUpdateHandle update = nullptr;
    REQUIRE(XGameSaveCreateUpdate(container.handle, "Slot One", &update) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "a", bytes_of("alpha"), 5) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "b", bytes_of("beta"), 4) == S_OK);
    CHECK(!exists(session.provider, "slot1"));
    const auto before = static_cast<int64_t>(std::time(nullptr));
    REQUIRE(XGameSaveSubmitUpdate(update) == S_OK);

    REQUIRE(info_of(session.provider, "slot1", info) == S_OK);
    CHECK_EQ(info.name, std::string("slot1"));
    CHECK_EQ(info.display_name, std::string("Slot One"));
    CHECK_EQ(info.values.blob_count, 2U);
    CHECK_EQ(info.values.total_size, uint64_t{9});
    CHECK(!info.values.needs_sync);
    CHECK(info.values.last_modified_time >= before - 1);
    CHECK(info.values.last_modified_time <= static_cast<int64_t>(std::time(nullptr)) + 1);
    std::vector<Info> listed;
    CHECK(XGameSaveEnumerateContainerInfo(session.provider, &listed, &collect_info) == S_OK);
    REQUIRE(listed.size() == 1);
    CHECK_EQ(listed[0].display_name, std::string("Slot One"));

    std::vector<std::pair<std::string, uint32_t>> blobs;
    CHECK(XGameSaveEnumerateBlobInfo(container.handle, &blobs, &collect_blob) == S_OK);
    CHECK(blobs == (std::vector<std::pair<std::string, uint32_t>>{{"a", 5}, {"b", 4}}));

    // An update is submitted once.
    CHECK(XGameSaveSubmitUpdate(update) == kGameSaveHandleExpired);
    CHECK(XGameSaveSubmitBlobWrite(update, "c", bytes_of("c"), 1) == kGameSaveHandleExpired);
    XGameSaveCloseUpdate(update);

    // An empty update makes an empty container.
    REQUIRE(write(session.provider, "empty", {}) == S_OK);
    REQUIRE(info_of(session.provider, "empty", info) == S_OK);
    CHECK_EQ(info.values.blob_count, 0U);
}

TEST_CASE("reading blobs into one buffer") {
    const SaveRoot root;
    const Session session;
    REQUIRE(write(session.provider, "c", {{"one", "1"}, {"Two", "22"}, {"three", ""}}) == S_OK);
    const Container container(session.provider, "c");

    // Null names read every blob, in name order without case: the records, then the data.
    std::vector<XGameSaveBlob> buffer(16);
    uint32_t count = 0;
    REQUIRE(XGameSaveReadBlobData(container.handle, nullptr, &count,
                                  buffer.size() * sizeof(XGameSaveBlob), buffer.data()) == S_OK);
    REQUIRE(count == 3);
    CHECK(buffer[0].data == reinterpret_cast<uint8_t*>(buffer.data() + 3));
    CHECK(contents_of(buffer.data(), count) ==
          (Contents{{"one", "1"}, {"three", ""}, {"Two", "22"}}));

    Contents found;
    CHECK(read(container.handle, {"two"}, found) == S_OK);
    CHECK(found == (Contents{{"Two", "22"}}));
    CHECK(read(container.handle, {"one", "missing"}, found) == kGameSaveBlobNotFound);

    // The buffer must hold every record and all the data.
    count = 0;
    CHECK(XGameSaveReadBlobData(container.handle, nullptr, &count, 3 * sizeof(XGameSaveBlob) + 2,
                                buffer.data()) == kGameSaveProvidedBufferTooSmall);
    CHECK(XGameSaveReadBlobData(container.handle, nullptr, nullptr, 1024, buffer.data()) ==
          E_INVALIDARG);

    // Asynchronously, with the size from XAsyncGetResultSize.
    const Queue queue;
    XAsyncBlock async = block_on(queue);
    REQUIRE(XGameSaveReadBlobDataAsync(container.handle, nullptr, 0, &async) == S_OK);
    queue.finish();
    size_t size = 0;
    REQUIRE(XAsyncGetResultSize(&async, &size) == S_OK);
    CHECK_EQ(size, 3 * sizeof(XGameSaveBlob) + 3);
    std::vector<XGameSaveBlob> fetched(4);
    count = 0;
    REQUIRE(XGameSaveReadBlobDataResult(&async, size, fetched.data(), &count) == S_OK);
    CHECK(contents_of(fetched.data(), count) ==
          (Contents{{"one", "1"}, {"three", ""}, {"Two", "22"}}));

    const char* names[] = {"missing"};
    async = block_on(queue);
    REQUIRE(XGameSaveReadBlobDataAsync(container.handle, names, 1, &async) == S_OK);
    queue.finish();
    CHECK(XAsyncGetStatus(&async, false) == kGameSaveBlobNotFound);

    // A container with no update yet has no blobs.
    const Container fresh(session.provider, "fresh");
    CHECK(read(fresh.handle, {}, found) == S_OK);
    CHECK(found.empty());
    CHECK(read(fresh.handle, {"x"}, found) == kGameSaveBlobNotFound);
}

TEST_CASE("an update writes, replaces, and deletes together") {
    const SaveRoot root;
    const Session session;
    REQUIRE(write(session.provider, "c", {{"a", "1"}, {"b", "2"}}, "Shown") == S_OK);
    const Container container(session.provider, "c");

    XGameSaveUpdateHandle update = nullptr;
    REQUIRE(XGameSaveCreateUpdate(container.handle, nullptr, &update) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "B", bytes_of("3"), 1) == S_OK);
    CHECK(XGameSaveSubmitBlobDelete(update, "a") == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "c", bytes_of("4"), 1) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "d", bytes_of("5"), 1) == S_OK);
    CHECK(XGameSaveSubmitBlobDelete(update, "d") == S_OK); // the last change wins
    CHECK(XGameSaveSubmitBlobDelete(update, "never") == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "", bytes_of("x"), 1) == E_INVALIDARG);
    CHECK(XGameSaveSubmitBlobWrite(update, nullptr, bytes_of("x"), 1) == E_INVALIDARG);
    CHECK(XGameSaveSubmitBlobWrite(update, "x", nullptr, 1) == E_INVALIDARG);
    REQUIRE(XGameSaveSubmitUpdate(update) == S_OK);
    XGameSaveCloseUpdate(update);

    Contents found;
    CHECK(read(container.handle, {}, found) == S_OK);
    CHECK(found == (Contents{{"B", "3"}, {"c", "4"}}));
    Info info{};
    REQUIRE(info_of(session.provider, "c", info) == S_OK);
    CHECK_EQ(info.display_name, std::string("Shown")); // kept by a null name

    // Replaced and deleted blobs leave no files behind.
    CHECK_EQ(files_in(root.configuration() / "c-c").size(), size_t{3});
}

TEST_CASE("container names follow the public rules and match without case") {
    const SaveRoot root;
    const Session session;
    const std::string longest(256, 'n');
    const std::string too_long(257, 'n');
    for (const char* name :
         {"", "a..b", "a.", "dir/", "bad name", "a\\b", "x/y.z/w", "\xc3\xa9", too_long.c_str()}) {
        INFO(std::string("name: ") + name);
        XGameSaveContainerHandle container = nullptr;
        CHECK(XGameSaveCreateContainer(session.provider, name, &container) ==
              kGameSaveInvalidContainerName);
        CHECK(XGameSaveDeleteContainer(session.provider, name) == kGameSaveInvalidContainerName);
    }
    CHECK(XGameSaveDeleteContainer(session.provider, nullptr) == kGameSaveInvalidContainerName);

    // Names Windows treats specially, long ones, and paths are kept as given.
    for (const char* name :
         {"CON", "nul.txt", "dir/sub/file.name-1", "_", "a.b", longest.c_str()}) {
        INFO(std::string("name: ") + name);
        CHECK(write(session.provider, name, {{"blob", "data"}}) == S_OK);
        CHECK(exists(session.provider, name));
    }
    CHECK(containers(session.provider) ==
          (std::vector<std::string>{"_", "a.b", "CON", "dir/sub/file.name-1", longest, "nul.txt"}));

    // Without case: the same container, under the name it was first saved with.
    REQUIRE(write(session.provider, "Profile", {{"v", "1"}}) == S_OK);
    REQUIRE(write(session.provider, "PROFILE", {{"v", "2"}}) == S_OK);
    Info info{};
    REQUIRE(info_of(session.provider, "profile", info) == S_OK);
    CHECK_EQ(info.name, std::string("Profile"));
    const Container profile(session.provider, "pRoFiLe");
    Contents found;
    CHECK(read(profile.handle, {"V"}, found) == S_OK);
    CHECK(found == (Contents{{"v", "2"}}));
}

TEST_CASE("enumerating by prefix") {
    const SaveRoot root;
    const Session session;
    for (const char* name : {"save/2", "Save/1", "settings"}) {
        REQUIRE(write(session.provider, name, {{"Slot", "x"}, {"shot", "yy"}, {"meta", "z"}}) ==
                S_OK);
    }
    CHECK(containers(session.provider, "SAVE/") == (std::vector<std::string>{"Save/1", "save/2"}));
    CHECK(containers(session.provider, "s").size() == 3);
    CHECK(containers(session.provider, "x").empty());
    CHECK(containers(session.provider, "") == containers(session.provider));

    std::vector<std::string> first;
    CHECK(XGameSaveEnumerateContainerInfo(session.provider, &first, &first_only) == S_OK);
    CHECK(first == (std::vector<std::string>{"Save/1"}));
    CHECK(XGameSaveEnumerateContainerInfo(session.provider, nullptr, nullptr) == E_INVALIDARG);

    const Container container(session.provider, "settings");
    std::vector<std::pair<std::string, uint32_t>> blobs;
    CHECK(XGameSaveEnumerateBlobInfoByName(container.handle, "S", &blobs, &collect_blob) == S_OK);
    CHECK(blobs == (std::vector<std::pair<std::string, uint32_t>>{{"shot", 2}, {"Slot", 1}}));
}

TEST_CASE("deleting containers") {
    const SaveRoot root;
    const Session session;
    REQUIRE(write(session.provider, "keep", {{"b", "1"}}) == S_OK);
    REQUIRE(write(session.provider, "gone", {{"b", "2"}}) == S_OK);
    CHECK(XGameSaveDeleteContainer(session.provider, "GONE") == S_OK);
    CHECK(!exists(session.provider, "gone"));
    CHECK(exists(session.provider, "keep"));
    CHECK(XGameSaveDeleteContainer(session.provider, "gone") == S_OK); // deleted already

    const Queue queue;
    REQUIRE(write(session.provider, "later", {{"b", "3"}}) == S_OK);
    XAsyncBlock async = block_on(queue);
    REQUIRE(XGameSaveDeleteContainerAsync(session.provider, "later", &async) == S_OK);
    CHECK(exists(session.provider, "later")); // in queue order
    queue.finish();
    CHECK(XGameSaveDeleteContainerResult(&async) == S_OK);
    CHECK(!exists(session.provider, "later"));
    CHECK(containers(session.provider) == (std::vector<std::string>{"keep"}));
    CHECK(files_in(root.configuration()) == (std::vector<std::string>{"c-keep"}));

    // On a queue that is shutting down nothing is deleted.
    const Queue closing;
    REQUIRE(XTaskQueueTerminate(closing.handle, false, nullptr, nullptr) == S_OK);
    async = block_on(closing);
    CHECK(XGameSaveDeleteContainerAsync(session.provider, "keep", &async) == E_ABORT);
    CHECK(exists(session.provider, "keep"));
}

TEST_CASE("the update and quota limits") {
    const SaveRoot root;
    const Session session;
    const Container container(session.provider, "big");
    XGameSaveUpdateHandle update = nullptr;
    REQUIRE(XGameSaveCreateUpdate(container.handle, nullptr, &update) == S_OK);
    const std::vector<uint8_t> over(kGameSaveMaxUpdateSize + 1);
    CHECK(XGameSaveSubmitBlobWrite(update, "a", over.data(), over.size()) == kGameSaveUpdateTooBig);
    const std::vector<uint8_t> ten(size_t{10} * 1024 * 1024);
    CHECK(XGameSaveSubmitBlobWrite(update, "a", ten.data(), ten.size()) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "b", ten.data(), ten.size()) == kGameSaveUpdateTooBig);
    XGameSaveCloseUpdate(update);

    // A container that already holds nearly the whole quota, as its manifest says.
    const fs::path full = root.configuration() / "c-full";
    fs::create_directories(full);
    const int64_t held = kGameSaveQuota - 456;
    write_text(full / "manifest.txt", "g6recomp container 1\nname full\ndisplay \nmodified 0\n"
                                      "blob 1.blob " +
                                          std::to_string(held) + " data\n");
    int64_t quota = 0;
    CHECK(XGameSaveGetRemainingQuota(session.provider, &quota) == S_OK);
    CHECK_EQ(quota, int64_t{456});
    CHECK(write(session.provider, "more", {{"x", std::string(457, 'x')}}) ==
          kGameSaveQuotaExceeded);
    CHECK(!exists(session.provider, "more"));
    CHECK(write(session.provider, "more", {{"x", std::string(456, 'x')}}) == S_OK);
    CHECK(XGameSaveGetRemainingQuota(session.provider, &quota) == S_OK);
    CHECK_EQ(quota, int64_t{0});
    // Replacing a blob counts only the difference.
    CHECK(write(session.provider, "more", {{"x", std::string(400, 'y')}}) == S_OK);
    CHECK(XGameSaveGetRemainingQuota(session.provider, &quota) == S_OK);
    CHECK_EQ(quota, int64_t{56});
}

TEST_CASE("saves last across providers and stay with their configuration") {
    const SaveRoot root;
    {
        const Session session;
        REQUIRE(write(session.provider, "slot", {{"progress", "chapter 3"}}, "Chapter 3") == S_OK);
    }
    const Session session;
    const Container container(session.provider, "slot");
    Contents found;
    CHECK(read(container.handle, {}, found) == S_OK);
    CHECK(found == (Contents{{"progress", "chapter 3"}}));

    XUser* const user = add_user();
    XGameSaveProviderHandle other = nullptr;
    REQUIRE(XGameSaveInitializeProvider(user, "another", false, &other) == S_OK);
    CHECK(containers(other).empty());
    XGameSaveCloseProvider(other);
    XUserCloseHandle(user);
}

TEST_CASE("files a crash leaves behind") {
    const SaveRoot root;
    const Session session;
    REQUIRE(write(session.provider, "slot", {{"a", "1"}}) == S_OK);
    const fs::path folder = root.configuration() / "c-slot";
    // A blob and a manifest an update wrote before it was cut off, and a delete that never
    // finished.
    write_text(folder / "77.blob", "stray");
    write_text(folder / "manifest.tmp", "partial");
    fs::create_directories(root.configuration() / "!deleted-1-1" / "c-old");
    write_text(root.configuration() / "!deleted-1-1" / "c-old" / "1.blob", "old");

    Contents found;
    const Container container(session.provider, "slot");
    CHECK(read(container.handle, {}, found) == S_OK);
    CHECK(found == (Contents{{"a", "1"}}));
    CHECK(containers(session.provider) == (std::vector<std::string>{"slot"}));

    // The next update clears the stray files; the next provider, the half-deleted folder.
    REQUIRE(write(session.provider, "slot", {{"b", "2"}}) == S_OK);
    CHECK(!fs::exists(folder / "77.blob"));
    CHECK(!fs::exists(folder / "manifest.tmp"));
    XGameSaveProviderHandle again = nullptr;
    REQUIRE(XGameSaveInitializeProvider(session.user, kConfiguration, false, &again) == S_OK);
    CHECK(!fs::exists(root.configuration() / "!deleted-1-1"));
    XGameSaveCloseProvider(again);

    // A damaged manifest is left alone and its container reported as corrupt.
    fs::create_directories(root.configuration() / "c-broken");
    write_text(root.configuration() / "c-broken" / "manifest.txt", "not a manifest");
    Info info{};
    CHECK(info_of(session.provider, "broken", info) == kFileCorrupt);
    CHECK(write(session.provider, "broken", {{"x", "1"}}) == kFileCorrupt);
    CHECK(containers(session.provider) == (std::vector<std::string>{"slot"}));
    CHECK(fs::exists(root.configuration() / "c-broken" / "manifest.txt"));
}

TEST_CASE("handles expire with their provider") {
    const SaveRoot root;
    XUser* const user = add_user();
    XGameSaveProviderHandle provider = nullptr;
    REQUIRE(XGameSaveInitializeProvider(user, kConfiguration, false, &provider) == S_OK);
    REQUIRE(write(provider, "slot", {{"a", "1"}}) == S_OK);
    XGameSaveContainerHandle container = nullptr;
    REQUIRE(XGameSaveCreateContainer(provider, "slot", &container) == S_OK);
    XGameSaveUpdateHandle update = nullptr;
    REQUIRE(XGameSaveCreateUpdate(container, nullptr, &update) == S_OK);
    XGameSaveCloseProvider(provider);

    Contents found;
    CHECK(read(container, {}, found) == kGameSaveHandleExpired);
    XGameSaveUpdateHandle another = nullptr;
    CHECK(XGameSaveCreateUpdate(container, nullptr, &another) == kGameSaveHandleExpired);
    CHECK(XGameSaveSubmitBlobWrite(update, "b", bytes_of("2"), 1) == kGameSaveHandleExpired);
    CHECK(XGameSaveSubmitUpdate(update) == kGameSaveHandleExpired);
    XGameSaveCloseUpdate(update);
    XGameSaveCloseContainer(container);

    CHECK(read(nullptr, {}, found) == E_INVALIDARG);
    CHECK(XGameSaveCreateContainer(nullptr, "slot", &container) == E_INVALIDARG);
    int64_t quota = 0;
    CHECK(XGameSaveGetRemainingQuota(nullptr, &quota) == E_INVALIDARG);
    XUserCloseHandle(user);
}

// Leonida 2cc83a80: a closed handle stays expired when a new object takes its memory.
TEST_CASE("closed handles stay closed") {
    const SaveRoot root;
    const Session session;
    REQUIRE(write(session.provider, "slot", {{"a", "1"}}) == S_OK);
    XGameSaveContainerHandle closed = nullptr;
    REQUIRE(XGameSaveCreateContainer(session.provider, "slot", &closed) == S_OK);
    XGameSaveUpdateHandle closed_update = nullptr;
    REQUIRE(XGameSaveCreateUpdate(closed, nullptr, &closed_update) == S_OK);
    XGameSaveCloseUpdate(closed_update);
    XGameSaveCloseContainer(closed);

    // Opened and closed often, so new objects get freed memory back.
    std::set<const void*> seen;
    for (int i = 0; i < 64; ++i) {
        XGameSaveContainerHandle container = nullptr;
        REQUIRE(XGameSaveCreateContainer(session.provider, "slot", &container) == S_OK);
        XGameSaveUpdateHandle update = nullptr;
        REQUIRE(XGameSaveCreateUpdate(container, nullptr, &update) == S_OK);
        CHECK(container != closed);
        CHECK(update != closed_update);
        CHECK(seen.insert(container).second);
        CHECK(seen.insert(update).second);
        XGameSaveCloseUpdate(update);
        XGameSaveCloseContainer(container);
    }
    const Container live(session.provider, "slot");
    Contents found;
    CHECK(read(closed, {}, found) == kGameSaveHandleExpired);
    CHECK(XGameSaveSubmitBlobWrite(closed_update, "b", bytes_of("2"), 1) == kGameSaveHandleExpired);
    CHECK(read(live.handle, {}, found) == S_OK);
    CHECK(found == (Contents{{"a", "1"}}));

    // A handle of one kind isn't one of another, and closing it twice does nothing.
    CHECK(XGameSaveSubmitUpdate(reinterpret_cast<XGameSaveUpdateHandle>(live.handle)) ==
          kGameSaveHandleExpired);
    XGameSaveCloseContainer(closed);
    CHECK(read(live.handle, {}, found) == S_OK);
}

TEST_CASE("asynchronous updates") {
    const SaveRoot root;
    const Session session;
    const Container container(session.provider, "slot");
    XGameSaveUpdateHandle update = nullptr;
    REQUIRE(XGameSaveCreateUpdate(container.handle, nullptr, &update) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "a", bytes_of("1"), 1) == S_OK);

    // Never begun on a queue that is shutting down, so it can still be submitted.
    {
        const Queue closing;
        REQUIRE(XTaskQueueTerminate(closing.handle, false, nullptr, nullptr) == S_OK);
        XAsyncBlock async = block_on(closing);
        CHECK(XGameSaveSubmitUpdateAsync(update, &async) == E_ABORT);
    }
    const Queue queue;
    XAsyncBlock async = block_on(queue);
    REQUIRE(XGameSaveSubmitUpdateAsync(update, &async) == S_OK);
    CHECK(!exists(session.provider, "slot"));
    XAsyncBlock twice = block_on(queue);
    CHECK(XGameSaveSubmitUpdateAsync(update, &twice) == kGameSaveHandleExpired);
    queue.finish();
    CHECK(XGameSaveSubmitUpdateResult(&async) == S_OK);
    Contents found;
    CHECK(read(container.handle, {}, found) == S_OK);
    CHECK(found == (Contents{{"a", "1"}}));
    XGameSaveCloseUpdate(update);
}

TEST_CASE("time-sensitive threads need the asynchronous forms") {
    const SaveRoot root;
    const Session session;
    REQUIRE(write(session.provider, "slot", {{"a", "1"}}) == S_OK);
    const Container container(session.provider, "slot");
    XGameSaveUpdateHandle update = nullptr;
    REQUIRE(XGameSaveCreateUpdate(container.handle, nullptr, &update) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(update, "b", bytes_of("2"), 1) == S_OK);

    REQUIRE(XThreadSetTimeSensitive(true) == S_OK);
    XGameSaveProviderHandle provider = nullptr;
    CHECK(XGameSaveInitializeProvider(session.user, kConfiguration, false, &provider) ==
          kGameSaveAsyncFunctionRequired);
    int64_t quota = 0;
    CHECK(XGameSaveGetRemainingQuota(session.provider, &quota) == kGameSaveAsyncFunctionRequired);
    Contents found;
    CHECK(read(container.handle, {}, found) == kGameSaveAsyncFunctionRequired);
    CHECK(XGameSaveSubmitUpdate(update) == kGameSaveAsyncFunctionRequired);
    CHECK(XGameSaveDeleteContainer(session.provider, "slot") == kGameSaveAsyncFunctionRequired);
    REQUIRE(XThreadSetTimeSensitive(false) == S_OK);

    // The refused submit left the update as it was.
    CHECK(XGameSaveSubmitUpdate(update) == S_OK);
    XGameSaveCloseUpdate(update);
    CHECK(read(container.handle, {}, found) == S_OK);
    CHECK(found == (Contents{{"a", "1"}, {"b", "2"}}));
}

TEST_CASE("XGameSave through its slots") {
    const SaveRoot root;
    XUser* const user = add_user();
    void* save = nullptr;
    REQUIRE(QueryApiImpl(&kGameSaveClassId, &kGameSaveInterfaceId, &save) == S_OK);
    void* second = nullptr;
    REQUIRE(QueryApiImpl(&kGameSaveClassId, &kGameSave2InterfaceId, &second) == S_OK);
    CHECK(second != save);
    void* found = nullptr;
    CHECK(call<HRESULT>(second, 0, &kGameSaveInterfaceId, &found) == S_OK);
    CHECK(found == save);

    XGameSaveProviderHandle provider = nullptr;
    REQUIRE(call<HRESULT>(save, 3, user, kConfiguration, false, &provider) == S_OK);
    XGameSaveContainerHandle container = nullptr;
    REQUIRE(call<HRESULT>(save, 16, provider, "slot", &container) == S_OK);
    XGameSaveUpdateHandle update = nullptr;
    REQUIRE(call<HRESULT>(save, 23, container, "Shown", &update) == S_OK);
    CHECK(call<HRESULT>(save, 25, update, "a", bytes_of("12"), size_t{2}) == S_OK);
    CHECK(call<HRESULT>(save, 27, update) == S_OK);
    call<void>(save, 24, update);

    std::vector<XGameSaveBlob> buffer(4);
    uint32_t count = 0;
    CHECK(call<HRESULT>(save, 20, container, static_cast<const char**>(nullptr), &count,
                        buffer.size() * sizeof(XGameSaveBlob), buffer.data()) == S_OK);
    CHECK(contents_of(buffer.data(), count) == (Contents{{"a", "12"}}));
    std::vector<Info> infos;
    CHECK(call<HRESULT>(save, 13, provider, "slot", static_cast<void*>(&infos), &collect_info) ==
          S_OK);
    CHECK(infos.size() == 1);
    int64_t quota = 0;
    CHECK(call<HRESULT>(save, 7, provider, &quota) == S_OK);
    CHECK_EQ(quota, kGameSaveQuota - 2);
    call<void>(save, 17, container);
    CHECK(call<HRESULT>(save, 10, provider, "slot") == S_OK);
    call<void>(save, 6, provider);

    // XGameSaveFiles and the second interface's slots aren't provided.
    char folder[MAX_PATH] = {};
    CHECK(call<HRESULT>(save, 31, static_cast<XAsyncBlock*>(nullptr), sizeof(folder), folder) ==
          E_NOTIMPL);
    CHECK(call<HRESULT>(second, 40) == E_NOTIMPL);
    XUserCloseHandle(user);
}
