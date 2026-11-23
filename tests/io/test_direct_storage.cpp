// The console's DirectStorage (g6/io/direct_storage.h): the factory, files opened through the
// active FileSystem, queues that read in order on Submit, status arrays, failures and the error
// record, cancelling by tag, and memory queues.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include <windows.h>

#include "g6/io/direct_storage.h"
#include "g6/io/file_system.h"
#include "g6test.h"

namespace fs = std::filesystem;
using namespace g6::io;
using namespace g6::io::dstorage_slot;

namespace {

template <typename Function>
Function method(void* object, size_t index) {
    const auto* const* table = *static_cast<const void* const* const*>(object);
    return reinterpret_cast<Function>(const_cast<void*>(table[index]));
}

ULONG release(void* object) {
    return method<ULONG(STDMETHODCALLTYPE*)(void*)>(object, 2)(object);
}

using OpenFileMethod = HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, const GUID*, void**);
using CreateQueue = HRESULT(STDMETHODCALLTYPE*)(void*, const QueueDescX*, const GUID*, void**);
using CreateStatusArray = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, const char*, const GUID*,
                                                      void**);
using SetStagingBufferSize = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t);
using GetHandle = HANDLE(STDMETHODCALLTYPE*)(void*);
using EnqueueRequest = void(STDMETHODCALLTYPE*)(void*, const RequestX*);
using EnqueueStatus = void(STDMETHODCALLTYPE*)(void*, void*, uint32_t);
using Submit = void(STDMETHODCALLTYPE*)(void*);
using CancelRequestsWithTag = void(STDMETHODCALLTYPE*)(void*, uint64_t, uint64_t);
using GetErrorEvent = HANDLE(STDMETHODCALLTYPE*)(void*);
using RetrieveErrorRecord = void(STDMETHODCALLTYPE*)(void*, ErrorRecordX*);
using IsComplete = bool(STDMETHODCALLTYPE*)(void*, uint32_t);
using GetHResult = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t);

constexpr GUID kSomeStatusArray = {
    0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};

// A file of 4096 known bytes on a drive of the active FileSystem, unmounted again at the end.
class Drive {
public:
    Drive() {
        root_ = fs::temp_directory_path() /
                ("g6dstorage-test-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root_);
        bytes_.resize(4096);
        for (size_t index = 0; index < bytes_.size(); ++index) {
            bytes_[index] = static_cast<uint8_t>(index * 7 + 3);
        }
        std::ofstream(root_ / "pack.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes_.data()),
                   static_cast<std::streamsize>(bytes_.size()));
        active_file_system().mount(L'Q', root_, Access::ReadOnly);
    }
    ~Drive() { // NOLINT(bugprone-exception-escape)
        active_file_system().unmount(L'Q');
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    Drive(const Drive&) = delete;
    Drive& operator=(const Drive&) = delete;

    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    fs::path root_;
    std::vector<uint8_t> bytes_;
};

void* factory() {
    void* result = nullptr;
    REQUIRE(DStorageGetFactory(&kDStorageFactoryX, &result) == S_OK);
    return result;
}

void* open_pack(void* factory) {
    void* file = nullptr;
    REQUIRE(method<OpenFileMethod>(factory, kFactoryOpenFile)(factory, L"Q:\\pack.bin",
                                                              &kDStorageFileX, &file) == S_OK);
    return file;
}

void* file_queue(void* factory, uint16_t capacity = 128) {
    QueueDescX desc{};
    desc.source_type = 0;
    desc.capacity = capacity;
    desc.name = "test";
    desc.extra = 1;
    void* queue = nullptr;
    REQUIRE(method<CreateQueue>(factory, kFactoryCreateQueueX)(factory, &desc, &kDStorageQueueX,
                                                               &queue) == S_OK);
    return queue;
}

void* status_array(void* factory, uint32_t capacity) {
    void* array = nullptr;
    REQUIRE(method<CreateStatusArray>(factory, kFactoryCreateStatusArray)(
                factory, capacity, "status", &kSomeStatusArray, &array) == S_OK);
    return array;
}

RequestX read_request(void* file, uint64_t offset, void* destination, uint32_t size) {
    RequestX request{};
    request.destination = destination;
    request.destination_size = size;
    request.source = file;
    request.offset = offset;
    request.source_size = size;
    return request;
}

} // namespace

TEST_CASE("the factory answers only the console's interface and takes its settings") {
    void* result = nullptr;
    const GUID other = {0x11111111, 0x2222, 0x3333, {4, 4, 5, 5, 6, 6, 7, 7}};
    CHECK_EQ(DStorageGetFactory(&other, &result), E_NOINTERFACE);
    CHECK(result == nullptr);
    CHECK_EQ(DStorageGetFactory(&kDStorageFactoryX, nullptr), E_POINTER);
    void* object = factory();
    // 32 MiB, as the executable asks for.
    CHECK_EQ(method<SetStagingBufferSize>(object, kFactorySetStagingBufferSize)(object, 0x2000000),
             S_OK);
    release(object);
}

TEST_CASE("files open read-only through the mounted drives") {
    const Drive drive;
    void* object = factory();
    void* file = nullptr;
    const auto open = method<OpenFileMethod>(object, kFactoryOpenFile);
    CHECK_EQ(open(object, L"Q:\\missing.bin", &kDStorageFileX, &file),
             HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    CHECK(file == nullptr);
    CHECK_EQ(open(object, L"Z:\\pack.bin", &kDStorageFileX, &file),
             HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
    CHECK_EQ(open(object, L"Q:\\pack.bin", &kDStorageQueueX, &file), E_NOINTERFACE);

    file = open_pack(object);
    HANDLE handle = method<GetHandle>(file, kFileGetHandle)(file);
    REQUIRE(handle != nullptr);
    LARGE_INTEGER size{};
    CHECK(GetFileSizeEx(handle, &size) != 0);
    CHECK_EQ(size.QuadPart, LONGLONG{4096});
    CloseHandle(handle);
    CHECK_EQ(release(file), ULONG{0});
    release(object);
}

TEST_CASE("a submitted request reads into its destination and completes its status") {
    const Drive drive;
    void* object = factory();
    void* file = open_pack(object);
    void* queue = file_queue(object);
    void* array = status_array(object, 4);
    const auto is_complete = method<IsComplete>(array, kStatusIsComplete);
    const auto get_hresult = method<GetHResult>(array, kStatusGetHResult);
    CHECK(!is_complete(array, 0));
    CHECK_EQ(get_hresult(array, 0), E_PENDING);

    std::vector<uint8_t> destination(1000, 0xEE);
    const RequestX request = read_request(file, 100, destination.data(), 1000);
    method<EnqueueRequest>(queue, kQueueEnqueueRequest)(queue, &request);
    method<EnqueueStatus>(queue, kQueueEnqueueStatus)(queue, array, 0);
    // Nothing runs before Submit.
    CHECK(!is_complete(array, 0));
    CHECK_EQ(destination[0], uint8_t{0xEE});
    // The file may be released while its request is queued.
    release(file);

    method<Submit>(queue, kQueueSubmit)(queue);
    CHECK(is_complete(array, 0));
    CHECK_EQ(get_hresult(array, 0), S_OK);
    CHECK(std::memcmp(destination.data(), drive.bytes().data() + 100, 1000) == 0);
    CHECK_EQ(get_hresult(array, 4), E_INVALIDARG); // past the end

    release(array);
    release(queue);
    release(object);
}

TEST_CASE("a failed request fails the next status, signals the event, and is recorded") {
    const Drive drive;
    void* object = factory();
    void* file = open_pack(object);
    void* queue = file_queue(object);
    void* array = status_array(object, 3);
    const auto enqueue = method<EnqueueRequest>(queue, kQueueEnqueueRequest);
    const auto status = method<EnqueueStatus>(queue, kQueueEnqueueStatus);
    const auto submit = method<Submit>(queue, kQueueSubmit);
    const auto get_hresult = method<GetHResult>(array, kStatusGetHResult);
    HANDLE event = method<GetErrorEvent>(queue, kQueueGetErrorEvent)(queue);
    REQUIRE(event != nullptr);
    CHECK_EQ(WaitForSingleObject(event, 0), DWORD{WAIT_TIMEOUT});

    std::array<uint8_t, 64> destination{};
    // Options this runtime doesn't implement yet fail honestly.
    RequestX compressed = read_request(file, 0, destination.data(), 64);
    compressed.options = 1;
    enqueue(queue, &compressed);
    status(queue, array, 0);
    // A status after it covers only what came after the previous status.
    const RequestX fine = read_request(file, 0, destination.data(), 64);
    enqueue(queue, &fine);
    status(queue, array, 1);
    // Reading past the end of the file.
    const RequestX past_end = read_request(file, 4090, destination.data(), 64);
    enqueue(queue, &past_end);
    status(queue, array, 2);
    submit(queue);

    CHECK_EQ(get_hresult(array, 0), E_NOTIMPL);
    CHECK_EQ(get_hresult(array, 1), S_OK);
    CHECK_EQ(get_hresult(array, 2), HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
    CHECK_EQ(WaitForSingleObject(event, 0), DWORD{WAIT_OBJECT_0});
    CloseHandle(event);

    ErrorRecordX record{};
    method<RetrieveErrorRecord>(queue, kQueueRetrieveErrorRecord)(queue, &record);
    CHECK_EQ(record.failure_count, 2U);
    CHECK_EQ(record.hresult, E_NOTIMPL);
    CHECK_EQ(record.command_type, 0U);

    release(file);
    release(array);
    release(queue);
    release(object);
}

TEST_CASE("cancelled requests never run") {
    const Drive drive;
    void* object = factory();
    void* file = open_pack(object);
    void* queue = file_queue(object);
    void* array = status_array(object, 1);
    std::array<uint8_t, 16> kept{};
    std::array<uint8_t, 16> cancelled{};
    RequestX first = read_request(file, 0, kept.data(), 16);
    first.tag = 0x104;
    RequestX second = read_request(file, 0, cancelled.data(), 16);
    second.tag = 0x205;
    method<EnqueueRequest>(queue, kQueueEnqueueRequest)(queue, &first);
    method<EnqueueRequest>(queue, kQueueEnqueueRequest)(queue, &second);
    method<CancelRequestsWithTag>(queue, kQueueCancelRequestsWithTag)(queue, 0xFF, 0x05);
    method<EnqueueStatus>(queue, kQueueEnqueueStatus)(queue, array, 0);
    method<Submit>(queue, kQueueSubmit)(queue);
    CHECK_EQ(method<GetHResult>(array, kStatusGetHResult)(array, 0), S_OK);
    CHECK(std::memcmp(kept.data(), drive.bytes().data(), 16) == 0);
    CHECK_EQ(cancelled[0], uint8_t{0});
    release(file);
    release(array);
    release(queue);
    release(object);
}

TEST_CASE("a full queue submits by itself, and a memory queue copies") {
    const Drive drive;
    void* object = factory();
    void* file = open_pack(object);
    void* queue = file_queue(object, 2);
    std::array<uint8_t, 8> first{};
    std::array<uint8_t, 8> second{};
    const RequestX a = read_request(file, 8, first.data(), 8);
    const RequestX b = read_request(file, 16, second.data(), 8);
    method<EnqueueRequest>(queue, kQueueEnqueueRequest)(queue, &a);
    CHECK_EQ(first[0], uint8_t{0});
    method<EnqueueRequest>(queue, kQueueEnqueueRequest)(queue, &b);
    CHECK(std::memcmp(first.data(), drive.bytes().data() + 8, 8) == 0);
    CHECK(std::memcmp(second.data(), drive.bytes().data() + 16, 8) == 0);
    release(queue);

    QueueDescX desc{};
    desc.source_type = 1;
    desc.capacity = 16;
    void* memory = nullptr;
    REQUIRE(method<CreateQueue>(object, kFactoryCreateQueue)(object, &desc, &kDStorageQueueX,
                                                             &memory) == S_OK);
    const std::array<uint8_t, 5> source = {1, 2, 3, 4, 5};
    std::array<uint8_t, 5> copy{};
    RequestX request{};
    request.destination = copy.data();
    request.destination_size = 5;
    request.source = const_cast<uint8_t*>(source.data());
    request.source_size = 5;
    method<EnqueueRequest>(memory, kQueueEnqueueRequest)(memory, &request);
    method<Submit>(memory, kQueueSubmit)(memory);
    CHECK(copy == source);
    release(memory);
    release(file);
    release(object);
}
