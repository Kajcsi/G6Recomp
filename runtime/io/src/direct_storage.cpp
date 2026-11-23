// The console's DirectStorage (g6/io/direct_storage.h).

#include "g6/io/direct_storage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "g6/com_stand_in.h"
#include "g6/io/file_system.h"
#include "g6/io/text.h"
#include "g6/log.h"

namespace g6::io {
namespace {

using namespace dstorage_slot;

bool same(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

std::string guid_text(const GUID& guid) {
    return fmt::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                       guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
                       guid.Data4[7]);
}

template <typename Function>
const void* slot(Function function) noexcept {
    return reinterpret_cast<const void*>(function);
}

// ---- Objects we handed out ----

// The files and status arrays alive now. A request or status names one by pointer; it is
// checked here before anything reads through it, so a stale or foreign pointer fails the
// request instead of crashing.
struct Live {
    std::mutex mutex;
    std::unordered_set<const com::StandIn*> objects;
};

Live& live() {
    static Live instance;
    return instance;
}

void add_live(const com::StandIn* object) {
    const std::scoped_lock lock(live().mutex);
    live().objects.insert(object);
}

void remove_live(const com::StandIn* object) {
    const std::scoped_lock lock(live().mutex);
    live().objects.erase(object);
}

// Takes a reference on `object` when it is alive and has `table`, so it stays alive until the
// queue is done with it.
com::StandIn* reference_if_live(const void* object, const com::StandInTable& table) {
    const std::scoped_lock lock(live().mutex);
    const auto found = live().objects.find(static_cast<const com::StandIn*>(object));
    if (found == live().objects.end() || (*found)->vtable != table.data()) {
        return nullptr;
    }
    auto* standin = const_cast<com::StandIn*>(*found);
    com::add_ref(*standin);
    return standin;
}

void release(com::StandIn* object) {
    if (object != nullptr) {
        using Release = ULONG(STDMETHODCALLTYPE*)(com::StandIn*);
        reinterpret_cast<Release>(const_cast<void*>(object->vtable[2]))(object);
    }
}

// ---- Status arrays ----

struct StatusArray {
    com::StandIn header;
    uint32_t capacity;
    std::unique_ptr<std::atomic<HRESULT>[]> entries;
};

bool STDMETHODCALLTYPE status_is_complete(com::StandIn* self, uint32_t index) {
    auto* array = reinterpret_cast<StatusArray*>(self);
    return index < array->capacity &&
           array->entries[index].load(std::memory_order_acquire) != E_PENDING;
}

HRESULT STDMETHODCALLTYPE status_get_hresult(com::StandIn* self, uint32_t index) {
    auto* array = reinterpret_cast<StatusArray*>(self);
    if (index >= array->capacity) {
        return E_INVALIDARG;
    }
    return array->entries[index].load(std::memory_order_acquire);
}

const com::StandInTable g_status_table = []() noexcept {
    com::StandInTable table = com::stand_in_table();
    table[kStatusIsComplete] = slot(&status_is_complete);
    table[kStatusGetHResult] = slot(&status_get_hresult);
    return table;
}();

// Locking a std::mutex throws only on a system error, which ends the run anyway.
void destroy_status_array(com::StandIn* object) noexcept { // NOLINT(bugprone-exception-escape)
    remove_live(object);
    delete reinterpret_cast<StatusArray*>(object);
}

// ---- Files ----

struct File {
    com::StandIn header;
    std::mutex mutex;
    HANDLE handle = INVALID_HANDLE_VALUE; // INVALID_HANDLE_VALUE once closed
    std::wstring path;                    // the console path, for the log

    // Reads `size` bytes at `offset` into `destination`, all of them or fails.
    HRESULT read(uint64_t offset, void* destination, uint32_t size) {
        const std::scoped_lock lock(mutex);
        if (handle == INVALID_HANDLE_VALUE) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        }
        auto* bytes = static_cast<uint8_t*>(destination);
        uint32_t done = 0;
        while (done < size) {
            OVERLAPPED at{};
            at.Offset = static_cast<DWORD>(offset + done);
            at.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
            DWORD got = 0;
            if (ReadFile(handle, bytes + done, size - done, &got, &at) == 0) {
                const DWORD error = GetLastError();
                return HRESULT_FROM_WIN32(error == ERROR_HANDLE_EOF ? ERROR_HANDLE_EOF : error);
            }
            if (got == 0) {
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            }
            done += got;
        }
        return S_OK;
    }
};

void STDMETHODCALLTYPE file_close(com::StandIn* self) {
    auto* file = reinterpret_cast<File*>(self);
    const std::scoped_lock lock(file->mutex);
    if (file->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file->handle);
        file->handle = INVALID_HANDLE_VALUE;
    }
}

HANDLE STDMETHODCALLTYPE file_get_handle(com::StandIn* self) {
    auto* file = reinterpret_cast<File*>(self);
    const std::scoped_lock lock(file->mutex);
    HANDLE copy = nullptr;
    if (file->handle == INVALID_HANDLE_VALUE ||
        DuplicateHandle(GetCurrentProcess(), file->handle, GetCurrentProcess(), &copy, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == 0) {
        return nullptr;
    }
    return copy;
}

const com::StandInTable g_file_table = []() noexcept {
    com::StandInTable table = com::stand_in_table();
    table[kFileClose] = slot(&file_close);
    table[kFileGetHandle] = slot(&file_get_handle);
    return table;
}();

// As destroy_status_array.
void destroy_file(com::StandIn* object) noexcept { // NOLINT(bugprone-exception-escape)
    remove_live(object);
    file_close(object);
    delete reinterpret_cast<File*>(object);
}

// ---- Queues ----

constexpr uint32_t kCommandRequest = 0;
constexpr uint32_t kCommandStatus = 1;
constexpr uint32_t kCommandSignal = 2;
constexpr uint32_t kNoCommand = 0xFFFFFFFF;

struct Entry {
    uint32_t command = kCommandRequest;
    RequestX request{};
    com::StandIn* object = nullptr; // the request's file, or the status array, with a reference
    uint32_t index = 0;             // the status entry
    uint64_t value = 0;             // the fence value
};

// Options words already reported as not implemented, so each is logged once.
struct Reported {
    std::mutex mutex;
    std::vector<uint64_t> options;
    bool first(uint64_t value) {
        const std::scoped_lock lock(mutex);
        if (std::ranges::find(options, value) != options.end() || options.size() >= 64) {
            return false;
        }
        options.push_back(value);
        return true;
    }
};

Reported& reported() {
    static Reported instance;
    return instance;
}

// Whether `size` bytes at `address` are committed memory the guest can write.
bool writable(const void* address, size_t size) {
    constexpr DWORD kWritable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    auto at = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = at + size;
    if (address == nullptr || end < at) {
        return false;
    }
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) == 0 ||
            region.State != MEM_COMMIT || (region.Protect & kWritable) == 0 ||
            (region.Protect & PAGE_GUARD) != 0) {
            return false;
        }
        at = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

bool readable(const void* address, size_t size) {
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    auto at = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = at + size;
    if (address == nullptr || end < at) {
        return false;
    }
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) == 0 ||
            region.State != MEM_COMMIT || (region.Protect & kReadable) == 0 ||
            (region.Protect & PAGE_GUARD) != 0) {
            return false;
        }
        at = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

struct Queue {
    com::StandIn header;
    QueueDescX desc{};
    std::string name;
    HANDLE error_event = nullptr; // manual reset, set by the first failure

    std::mutex mutex; // guards everything below
    std::vector<Entry> pending;
    bool closed = false;
    uint32_t failures = 0;
    HRESULT first_failure = S_OK;
    uint32_t first_command = kNoCommand;
    HRESULT since_status = S_OK; // the first failure since the last status entry ran

    std::mutex running; // keeps submits in order

    uint32_t capacity() const { return std::max<uint32_t>(desc.capacity, 1); }

    void fail(HRESULT hresult, uint32_t command) {
        const std::scoped_lock lock(mutex);
        if (failures++ == 0) {
            first_failure = hresult;
            first_command = command;
            SetEvent(error_event);
        }
        if (since_status == S_OK) {
            since_status = hresult;
        }
    }

    HRESULT run_request(const Entry& entry) {
        const RequestX& request = entry.request;
        if (request.options != 0) {
            if (reported().first(request.options)) {
                std::array<uint8_t, 4> head{};
                if (entry.object != nullptr && request.source_size >= head.size()) {
                    reinterpret_cast<File*>(entry.object)
                        ->read(request.offset, head.data(), head.size());
                }
                LOG_WARN(io,
                         "DirectStorage queue \"{}\": options {:#x} aren't implemented (to {:#x} "
                         "bytes at {}, from {:#x} bytes at {:#x}, intermediate {:#x}, first "
                         "bytes {:02x} {:02x} {:02x} {:02x}); E_NOTIMPL, reported once",
                         name, request.options, request.destination_size, request.destination,
                         request.source_size, request.offset, request.intermediate_size, head[0],
                         head[1], head[2], head[3]);
            }
            return E_NOTIMPL;
        }
        if (request.source_size > request.destination_size) {
            return E_INVALIDARG;
        }
        if (!writable(request.destination, request.source_size)) {
            return E_INVALIDARG;
        }
        if (desc.source_type == 1) {
            if (!readable(request.source, request.source_size)) {
                return E_INVALIDARG;
            }
            std::memmove(request.destination, request.source, request.source_size);
            return S_OK;
        }
        if (entry.object == nullptr) {
            return E_INVALIDARG; // not a file we opened, or one released already
        }
        return reinterpret_cast<File*>(entry.object)
            ->read(request.offset, request.destination, request.source_size);
    }

    void run(const Entry& entry) {
        switch (entry.command) {
        case kCommandRequest: {
            const HRESULT hresult = run_request(entry);
            if (FAILED(hresult)) {
                LOG_DEBUG(io, "DirectStorage queue \"{}\": request failed ({:#x})", name,
                          static_cast<uint32_t>(hresult));
                fail(hresult, kCommandRequest);
            }
            break;
        }
        case kCommandStatus: {
            HRESULT result = S_OK;
            {
                const std::scoped_lock lock(mutex);
                result = since_status;
                since_status = S_OK;
            }
            auto* array = reinterpret_cast<StatusArray*>(entry.object);
            array->entries[entry.index].store(result, std::memory_order_release);
            break;
        }
        case kCommandSignal:
            // The fence comes from the guest's D3D12.X device, which runtime/gpu wraps at M3.
            if (!header.reported[kQueueEnqueueSignal].exchange(true)) {
                LOG_WARN(io,
                         "DirectStorage queue \"{}\": fence signals aren't implemented until "
                         "runtime/gpu wraps fences; the signal fails",
                         name);
            }
            fail(E_NOTIMPL, kCommandSignal);
            break;
        default: break;
        }
    }

    void submit() {
        const std::scoped_lock order(running);
        std::vector<Entry> batch;
        {
            const std::scoped_lock lock(mutex);
            batch.swap(pending);
        }
        for (const Entry& entry : batch) {
            run(entry);
            if (entry.command != kCommandSignal) {
                release(entry.object);
            }
        }
    }

    // Queues an entry, submitting first when the queue is full, as PC DirectStorage does.
    void enqueue(Entry entry) {
        bool full = false;
        {
            const std::scoped_lock lock(mutex);
            if (closed) {
                if (entry.command != kCommandSignal) {
                    release(entry.object);
                }
                return;
            }
            pending.push_back(entry);
            full = pending.size() >= capacity();
        }
        if (full) {
            submit();
        }
    }
};

void STDMETHODCALLTYPE queue_enqueue_request(com::StandIn* self, const RequestX* request) {
    auto* queue = reinterpret_cast<Queue*>(self);
    if (request == nullptr) {
        queue->fail(E_INVALIDARG, kCommandRequest);
        return;
    }
    Entry entry;
    entry.command = kCommandRequest;
    std::memcpy(&entry.request, request, sizeof(RequestX));
    if (queue->desc.source_type != 1) {
        entry.object = reference_if_live(request->source, g_file_table);
    }
    queue->enqueue(entry);
}

void STDMETHODCALLTYPE queue_enqueue_status(com::StandIn* self, const void* array, uint32_t index) {
    auto* queue = reinterpret_cast<Queue*>(self);
    com::StandIn* object = reference_if_live(array, g_status_table);
    if (object == nullptr || index >= reinterpret_cast<StatusArray*>(object)->capacity) {
        release(object);
        LOG_WARN(io, "DirectStorage queue \"{}\": status entry {} of {} isn't ours; ignored",
                 queue->name, index, array);
        return;
    }
    reinterpret_cast<StatusArray*>(object)->entries[index].store(E_PENDING,
                                                                 std::memory_order_release);
    Entry entry;
    entry.command = kCommandStatus;
    entry.object = object;
    entry.index = index;
    queue->enqueue(entry);
}

void STDMETHODCALLTYPE queue_enqueue_signal(com::StandIn* self, void* fence, uint64_t value) {
    Entry entry;
    entry.command = kCommandSignal;
    entry.object = static_cast<com::StandIn*>(fence); // not ours: never released or read
    entry.value = value;
    reinterpret_cast<Queue*>(self)->enqueue(entry);
}

void STDMETHODCALLTYPE queue_submit(com::StandIn* self) {
    reinterpret_cast<Queue*>(self)->submit();
}

void STDMETHODCALLTYPE queue_cancel_requests_with_tag(com::StandIn* self, uint64_t mask,
                                                      uint64_t value) {
    auto* queue = reinterpret_cast<Queue*>(self);
    std::vector<Entry> cancelled;
    {
        const std::scoped_lock lock(queue->mutex);
        const auto cancel = std::ranges::stable_partition(queue->pending, [&](const Entry& entry) {
            return entry.command != kCommandRequest || (entry.request.tag & mask) != value;
        });
        cancelled.assign(cancel.begin(), cancel.end());
        queue->pending.erase(cancel.begin(), cancel.end());
    }
    for (const Entry& entry : cancelled) {
        release(entry.object);
    }
}

void STDMETHODCALLTYPE queue_close(com::StandIn* self) {
    auto* queue = reinterpret_cast<Queue*>(self);
    std::vector<Entry> dropped;
    {
        const std::scoped_lock lock(queue->mutex);
        queue->closed = true;
        dropped.swap(queue->pending);
    }
    for (const Entry& entry : dropped) {
        if (entry.command == kCommandStatus) {
            reinterpret_cast<StatusArray*>(entry.object)
                ->entries[entry.index]
                .store(HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED), std::memory_order_release);
        }
        if (entry.command != kCommandSignal) {
            release(entry.object);
        }
    }
}

HANDLE STDMETHODCALLTYPE queue_get_error_event(com::StandIn* self) {
    auto* queue = reinterpret_cast<Queue*>(self);
    HANDLE copy = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), queue->error_event, GetCurrentProcess(), &copy, 0,
                        FALSE, DUPLICATE_SAME_ACCESS) == 0) {
        return nullptr;
    }
    return copy;
}

void STDMETHODCALLTYPE queue_retrieve_error_record(com::StandIn* self, ErrorRecordX* record) {
    if (record == nullptr) {
        return;
    }
    auto* queue = reinterpret_cast<Queue*>(self);
    const std::scoped_lock lock(queue->mutex);
    std::memset(record, 0, sizeof(ErrorRecordX));
    record->failure_count = queue->failures;
    record->hresult = queue->first_failure;
    record->command_type = queue->first_command;
}

void STDMETHODCALLTYPE queue_query(com::StandIn* self, QueueInfoX* info) {
    if (info == nullptr) {
        return;
    }
    auto* queue = reinterpret_cast<Queue*>(self);
    const std::scoped_lock lock(queue->mutex);
    std::memcpy(info->desc, &queue->desc, kQueueDescSize);
    const auto used = static_cast<uint32_t>(queue->pending.size());
    const uint32_t empty = queue->capacity() > used ? queue->capacity() - used : 0;
    info->empty_slots = static_cast<uint16_t>(std::min<uint32_t>(empty, UINT16_MAX));
    info->requests_until_submit = info->empty_slots;
}

const com::StandInTable g_queue_table = []() noexcept {
    com::StandInTable table = com::stand_in_table();
    table[kQueueEnqueueRequest] = slot(&queue_enqueue_request);
    table[kQueueEnqueueStatus] = slot(&queue_enqueue_status);
    table[kQueueEnqueueSignal] = slot(&queue_enqueue_signal);
    table[kQueueSubmit] = slot(&queue_submit);
    table[kQueueCancelRequestsWithTag] = slot(&queue_cancel_requests_with_tag);
    table[kQueueClose] = slot(&queue_close);
    table[kQueueGetErrorEvent] = slot(&queue_get_error_event);
    table[kQueueRetrieveErrorRecord] = slot(&queue_retrieve_error_record);
    table[kQueueQuery] = slot(&queue_query);
    return table;
}();

// As destroy_status_array.
void destroy_queue(com::StandIn* object) noexcept { // NOLINT(bugprone-exception-escape)
    auto* queue = reinterpret_cast<Queue*>(object);
    queue_close(object);
    CloseHandle(queue->error_event);
    delete queue;
}

// ---- The factory ----

HRESULT create_queue(const QueueDescX* desc, size_t desc_size, const GUID* riid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (desc == nullptr || riid == nullptr) {
        return E_INVALIDARG;
    }
    if (!same(*riid, kDStorageQueueX)) {
        LOG_WARN(io, "DirectStorage CreateQueue({}): not the queue interface we serve",
                 guid_text(*riid));
        return E_NOINTERFACE;
    }
    if (desc->source_type > 1) {
        return E_INVALIDARG;
    }
    auto* queue = new (std::nothrow)
        Queue{com::StandIn{g_queue_table, "IDStorageQueueX", kDStorageQueueX, &destroy_queue}};
    if (queue == nullptr) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(&queue->desc, desc, desc_size);
    if (desc->name != nullptr) {
        queue->name = desc->name;
    }
    queue->error_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (queue->error_event == nullptr) {
        const DWORD error = GetLastError();
        delete queue;
        return HRESULT_FROM_WIN32(error);
    }
    LOG_INFO(io, "DirectStorage queue \"{}\": {} source, capacity {}, priority {}", queue->name,
             desc->source_type == 0 ? "file" : "memory", desc->capacity,
             static_cast<int>(desc->priority));
    com::add_ref(queue->header);
    *out = queue;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_create_queue(com::StandIn* /*self*/, const QueueDescX* desc,
                                               const GUID* riid, void** out) {
    return create_queue(desc, kQueueDescSize, riid, out);
}

HRESULT STDMETHODCALLTYPE factory_create_queue_x(com::StandIn* /*self*/, const QueueDescX* desc,
                                                 const GUID* riid, void** out) {
    return create_queue(desc, sizeof(QueueDescX), riid, out);
}

HRESULT STDMETHODCALLTYPE factory_open_file(com::StandIn* /*self*/, const wchar_t* path,
                                            const GUID* riid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (path == nullptr || riid == nullptr) {
        return E_INVALIDARG;
    }
    if (!same(*riid, kDStorageFileX)) {
        LOG_WARN(io, "DirectStorage OpenFile({}): not the file interface we serve",
                 guid_text(*riid));
        return E_NOINTERFACE;
    }
    const Resolved resolved = active_file_system().resolve(path);
    if (!resolved.ok()) {
        LOG_DEBUG(io, "DirectStorage OpenFile(\"{}\"): error {}", utf8(path), resolved.error);
        return HRESULT_FROM_WIN32(resolved.error);
    }
    // Read-only even on a read-write mount: DirectStorage only reads.
    HANDLE handle =
        CreateFileW(resolved.host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        LOG_DEBUG(io, "DirectStorage OpenFile(\"{}\"): error {}", utf8(path), error);
        return HRESULT_FROM_WIN32(error);
    }
    auto* file = new (std::nothrow)
        File{com::StandIn{g_file_table, "IDStorageFileX", kDStorageFileX, &destroy_file}};
    if (file == nullptr) {
        CloseHandle(handle);
        return E_OUTOFMEMORY;
    }
    file->handle = handle;
    file->path = path;
    LOG_DEBUG(io, "DirectStorage OpenFile(\"{}\")", utf8(path));
    add_live(&file->header);
    com::add_ref(file->header);
    *out = file;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_create_status_array(com::StandIn* /*self*/, uint32_t capacity,
                                                      const char* name, const GUID* riid,
                                                      void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (riid == nullptr || capacity == 0) {
        return E_INVALIDARG;
    }
    // Which IID the executable passes isn't known yet: whatever it asks for, it gets a status
    // array, and the log shows the IID so it can be checked.
    auto* array = new (std::nothrow) StatusArray{
        com::StandIn{g_status_table, "IDStorageStatusArrayX", *riid, &destroy_status_array},
        capacity, nullptr};
    if (array == nullptr) {
        return E_OUTOFMEMORY;
    }
    array->entries.reset(new (std::nothrow) std::atomic<HRESULT>[capacity]);
    if (!array->entries) {
        delete array;
        return E_OUTOFMEMORY;
    }
    for (uint32_t index = 0; index < capacity; ++index) {
        array->entries[index].store(E_PENDING, std::memory_order_relaxed);
    }
    LOG_INFO(io, "DirectStorage status array \"{}\": {} entries, interface {}",
             name != nullptr ? name : "", capacity, guid_text(*riid));
    add_live(&array->header);
    com::add_ref(array->header);
    *out = array;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_set_cpu_affinity(com::StandIn* /*self*/, uint64_t mask) {
    LOG_INFO(io, "DirectStorage SetCpuAffinity({:#x}): requests run on the submitting thread",
             mask);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_set_debug_flags(com::StandIn* /*self*/, uint32_t flags) {
    LOG_INFO(io, "DirectStorage SetDebugFlags({:#x})", flags);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_set_staging_buffer_size(com::StandIn* /*self*/, uint32_t bytes) {
    // Requests read straight into their destination, so there is no staging buffer to size.
    LOG_INFO(io, "DirectStorage SetStagingBufferSize({:#x})", bytes);
    return S_OK;
}

const com::StandInTable g_factory_table = []() noexcept {
    com::StandInTable table = com::stand_in_table();
    table[kFactoryCreateQueue] = slot(&factory_create_queue);
    table[kFactoryOpenFile] = slot(&factory_open_file);
    table[kFactoryCreateStatusArray] = slot(&factory_create_status_array);
    table[kFactorySetCpuAffinity] = slot(&factory_set_cpu_affinity);
    table[kFactorySetDebugFlags] = slot(&factory_set_debug_flags);
    table[kFactorySetStagingBufferSize] = slot(&factory_set_staging_buffer_size);
    table[kFactoryCreateQueueX] = slot(&factory_create_queue_x);
    return table;
}();

com::StandIn g_factory{g_factory_table, "IDStorageFactoryX", kDStorageFactoryX};

} // namespace

HRESULT DStorageGetFactory(const GUID* riid, void** factory) {
    if (factory == nullptr) {
        return E_POINTER;
    }
    *factory = nullptr;
    if (riid == nullptr) {
        return E_INVALIDARG;
    }
    if (!same(*riid, kDStorageFactoryX)) {
        LOG_WARN(io, "DStorageGetFactory({}): not the console factory we serve", guid_text(*riid));
        return E_NOINTERFACE;
    }
    LOG_INFO(io, "DStorageGetFactory: uncompressed reads through runtime/io");
    com::add_ref(g_factory);
    *factory = &g_factory;
    return S_OK;
}

} // namespace g6::io
