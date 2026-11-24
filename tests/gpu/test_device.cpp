// The device proxy (g6/gpu/device.h) over a WARP device: the base slots reach the PC device, the
// slots that return structures take both argument orders, the byte at +0x18 is set, the slots
// not known yet fail, and the proxy's references hold the PC device. Then the command queues and
// lists it makes, through the PC's slots and the console's.

#include <array>
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <windows.h>

#include "g6/gpu/device.h"
#include "g6test.h"

using g6::gpu::command_list_proxy_real;
using g6::gpu::command_queue_proxy_real;
using g6::gpu::create_device_proxy;
using g6::gpu::device_proxy_real;
using g6::gpu::kDeviceSlots;

namespace {

// A WARP device, or null where the host has none.
ID3D12Device* warp_device() {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(
            CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    IDXGIAdapter* adapter = nullptr;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(
            factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter)))) {
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                     reinterpret_cast<void**>(&device)))) {
            device = nullptr;
        }
        adapter->Release();
    }
    factory->Release();
    return device;
}

// The reference count of a COM object, read by an AddRef and Release pair.
ULONG references(IUnknown* object) {
    object->AddRef();
    return object->Release();
}

const void* const* table_of(const void* object) {
    return *static_cast<const void* const* const*>(object);
}

// A slot of the table as a function of the given type.
template <typename Function>
Function slot(const void* const* table, unsigned index) {
    return reinterpret_cast<Function>(const_cast<void*>(table[index]));
}

// The device and its proxy, for one test. The proxy owns the reference the device was made with;
// the test keeps one of its own to look at the device after the proxy is gone.
struct Fixture {
    Fixture() : real(warp_device()) {
        if (real != nullptr) {
            real->AddRef();
            if (FAILED(create_device_proxy(real, __uuidof(ID3D12Device),
                                           reinterpret_cast<void**>(&proxy)))) {
                real->Release();
                proxy = nullptr;
            }
        }
    }
    ~Fixture() {
        if (proxy != nullptr) {
            proxy->Release();
        }
        if (real != nullptr) {
            real->Release();
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    ID3D12Device* real = nullptr;
    ID3D12Device* proxy = nullptr;
};

} // namespace

TEST_CASE("the proxy forwards the base device methods to the PC device") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    CHECK(static_cast<void*>(fixture.proxy) != static_cast<void*>(fixture.real));
    CHECK(device_proxy_real(fixture.proxy) == fixture.real);
    CHECK(device_proxy_real(fixture.real) == nullptr);

    // Slots 7, 13, 15 and 36, through the thunks.
    CHECK_EQ(fixture.proxy->GetNodeCount(), fixture.real->GetNodeCount());
    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
    CHECK(SUCCEEDED(fixture.proxy->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                                       sizeof(architecture))));
    for (const auto type : {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER}) {
        CHECK_EQ(fixture.proxy->GetDescriptorHandleIncrementSize(type),
                 fixture.real->GetDescriptorHandleIncrementSize(type));
    }
    ID3D12Fence* fence = nullptr;
    REQUIRE(SUCCEEDED(fixture.proxy->CreateFence(7, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                                 reinterpret_cast<void**>(&fence))));
    CHECK_EQ(fence->GetCompletedValue(), uint64_t{7});
    fence->Release();

    // Slot 38 takes stack arguments, which pass through the thunk untouched.
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 65536;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total = 0;
    fixture.proxy->GetCopyableFootprints(&buffer, 0, 1, 0, &footprint, &rows, &row_size, &total);
    CHECK_EQ(total, uint64_t{65536});
    CHECK_EQ(rows, 1U);
}

TEST_CASE("the byte at +0x18 sends descriptor copies through the device") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    CHECK(reinterpret_cast<const uint8_t*>(fixture.proxy)[0x18] != 0);
}

TEST_CASE("slots 25 and 26 take the PC's argument order and the title's") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    const void* const* table = table_of(fixture.proxy);

    // Slot 26, GetCustomHeapProperties.
    const D3D12_HEAP_PROPERTIES expected =
        fixture.real->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES pc_form =
        fixture.proxy->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_UPLOAD);
    CHECK(std::memcmp(&pc_form, &expected, sizeof(expected)) == 0);

    using HeapProperties = D3D12_HEAP_PROPERTIES* (*)(void* buffer, void* device, UINT node_mask,
                                                      D3D12_HEAP_TYPE type);
    D3D12_HEAP_PROPERTIES title_form{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    const auto* returned =
        slot<HeapProperties>(table, 26)(&title_form, fixture.proxy, 0, D3D12_HEAP_TYPE_UPLOAD);
    CHECK(returned == &title_form);
    CHECK(std::memcmp(&title_form, &expected, sizeof(expected)) == 0);

    // Slot 25, GetResourceAllocationInfo.
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 100000;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        fixture.real->GetResourceAllocationInfo(0, 1, &buffer);
    const D3D12_RESOURCE_ALLOCATION_INFO pc_info =
        fixture.proxy->GetResourceAllocationInfo(0, 1, &buffer);
    CHECK_EQ(pc_info.SizeInBytes, info.SizeInBytes);
    CHECK_EQ(pc_info.Alignment, info.Alignment);

    using AllocationInfo =
        D3D12_RESOURCE_ALLOCATION_INFO* (*)(void* buffer, void* device, UINT visible_mask,
                                            UINT count, const D3D12_RESOURCE_DESC* descs);
    D3D12_RESOURCE_ALLOCATION_INFO title_info{};
    slot<AllocationInfo>(table, 25)(&title_info, fixture.proxy, 0, 1, &buffer);
    CHECK_EQ(title_info.SizeInBytes, info.SizeInBytes);
    CHECK_EQ(title_info.Alignment, info.Alignment);
}

TEST_CASE("slots not known yet fail with E_NOTIMPL") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    const void* const* table = table_of(fixture.proxy);
    using Unknown = HRESULT (*)(void* self, uint64_t a, uint64_t b, uint64_t c);
    // 43 returns a structure in a form not seen yet; 49 and 133 are console methods; the last
    // slot is the end of the table.
    for (const unsigned index : {43U, 49U, 133U, kDeviceSlots - 1}) {
        INFO("slot " + std::to_string(index));
        CHECK_EQ(slot<Unknown>(table, index)(fixture.proxy, 1, 2, 3), E_NOTIMPL);
    }
    // The table ends at a page with no access.
    MEMORY_BASIC_INFORMATION after{};
    REQUIRE(VirtualQuery(static_cast<const void*>(table + kDeviceSlots), &after, sizeof(after)) !=
            0);
    CHECK_EQ(after.Protect, DWORD{0});
    CHECK_EQ(after.State, DWORD{MEM_RESERVE});
}

TEST_CASE("QueryInterface answers the device interfaces with the proxy") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    void* answer = nullptr;
    REQUIRE(SUCCEEDED(fixture.proxy->QueryInterface(__uuidof(ID3D12Device1), &answer)));
    CHECK(answer == fixture.proxy);
    static_cast<IUnknown*>(answer)->Release();
    REQUIRE(SUCCEEDED(fixture.proxy->QueryInterface(__uuidof(IUnknown), &answer)));
    CHECK(answer == fixture.proxy);
    static_cast<IUnknown*>(answer)->Release();
    CHECK_EQ(fixture.proxy->QueryInterface(__uuidof(IDXGIFactory), &answer), E_NOINTERFACE);
    CHECK(answer == nullptr);

    // A proxy asked for an interface that isn't a device's is refused, and the caller keeps its
    // reference.
    void* refused = nullptr;
    fixture.real->AddRef();
    CHECK_EQ(create_device_proxy(fixture.real, __uuidof(ID3D12Fence), &refused), E_NOINTERFACE);
    CHECK(refused == nullptr);
    fixture.real->Release();
}

TEST_CASE("the proxy holds the PC device until its last reference goes") {
    ID3D12Device* real = warp_device();
    if (real == nullptr) {
        SKIP("no WARP device on this host");
    }
    real->AddRef(); // the test's own
    const ULONG before = references(real);
    ID3D12Device* proxy = nullptr;
    real->AddRef(); // the proxy's
    REQUIRE(SUCCEEDED(
        create_device_proxy(real, __uuidof(ID3D12Device), reinterpret_cast<void**>(&proxy))));
    CHECK_EQ(references(real), before + 1);
    CHECK_EQ(proxy->AddRef(), 2U);
    CHECK_EQ(proxy->Release(), 1U);
    CHECK(device_proxy_real(proxy) == real);
    CHECK_EQ(proxy->Release(), 0U);
    CHECK_EQ(references(real), before);
    CHECK(device_proxy_real(proxy) == nullptr);
    real->Release();
    real->Release();
}

namespace {

// An interface the PC SDK doesn't have, as the title asks for its lists by one.
constexpr GUID kTitleListInterface = {
    0x6e5a1f0c, 0x2b7d, 0x4c1e, {0x9a, 0x31, 0x5d, 0x08, 0x7f, 0x22, 0xc4, 0x61}};

using CreateQueueX = HRESULT (*)(void* device, const void* desc, const GUID* riid, void** out);
using CreateListX = HRESULT (*)(void* device, const void* desc, ID3D12CommandAllocator* allocator,
                                ID3D12PipelineState* initial, const GUID* riid, void** out);
using Unknown = HRESULT (*)(void* self, uint64_t a, uint64_t b, uint64_t c);

// The console's copy engine type, for queues, allocators and lists.
constexpr uint32_t kCopyEngine = 0x10;

// A queue through slot 48: the PC's description, then four words of the console's. The default
// is a direct queue.
ID3D12CommandQueue* console_queue(const Fixture& fixture, uint32_t type = 0, uint32_t flags = 0) {
    const std::array<uint32_t, 8> desc = {type, 0, flags, 1, 0x1, 0x0, 0x40000000, 0x1};
    const GUID riid = __uuidof(ID3D12CommandQueue);
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(slot<CreateQueueX>(table_of(fixture.proxy), 48)(fixture.proxy, desc.data(), &riid,
                                                               reinterpret_cast<void**>(&queue)))) {
        return nullptr;
    }
    return queue;
}

// An open direct list through slot 73, as the title's interface.
void* console_list(const Fixture& fixture, ID3D12CommandAllocator* allocator) {
    const std::array<uint32_t, 4> desc = {static_cast<uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0,
                                          0, 0};
    void* list = nullptr;
    if (FAILED(slot<CreateListX>(table_of(fixture.proxy), 73)(
            fixture.proxy, desc.data(), allocator, nullptr, &kTitleListInterface, &list))) {
        return nullptr;
    }
    return list;
}

ID3D12CommandAllocator* direct_allocator(const Fixture& fixture) {
    ID3D12CommandAllocator* allocator = nullptr;
    if (FAILED(fixture.proxy->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     __uuidof(ID3D12CommandAllocator),
                                                     reinterpret_cast<void**>(&allocator)))) {
        return nullptr;
    }
    return allocator;
}

} // namespace

TEST_CASE("slots 8 and 48 make command queue proxies") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pc = nullptr;
    REQUIRE(SUCCEEDED(fixture.proxy->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                                                        reinterpret_cast<void**>(&pc))));
    ID3D12CommandQueue* console = console_queue(fixture);
    REQUIRE(console != nullptr);

    for (ID3D12CommandQueue* queue : {pc, console}) {
        const ID3D12CommandQueue* real = command_queue_proxy_real(queue);
        REQUIRE(real != nullptr);
        CHECK(static_cast<const void*>(real) != static_cast<const void*>(queue));
        // GetDevice hands out the device proxy, never the PC device.
        ID3D12Device* device = nullptr;
        REQUIRE(
            SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))));
        CHECK(device == fixture.proxy);
        device->Release();
        void* answer = nullptr;
        REQUIRE(SUCCEEDED(queue->QueryInterface(__uuidof(ID3D12Pageable), &answer)));
        CHECK(answer == queue);
        static_cast<IUnknown*>(answer)->Release();
        // Slot 14 waits for a call that shows which of the console's queue layouts the title has.
        CHECK_EQ(slot<Unknown>(table_of(queue), 14)(queue, 1, 2, 3), E_NOTIMPL);
    }

    // Each queue holds a reference to the device proxy until its last reference goes.
    const ULONG with_queues = references(fixture.proxy);
    CHECK_EQ(pc->Release(), 0U);
    CHECK_EQ(references(fixture.proxy), with_queues - 1);
    CHECK(command_queue_proxy_real(pc) == nullptr);
    CHECK_EQ(console->Release(), 0U);
    CHECK_EQ(references(fixture.proxy), with_queues - 2);
}

TEST_CASE("the console's queue flags and copy engine type become the PC's") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    // Compute queues with the console's flags 2 and 3, as the title makes them.
    for (const uint32_t flags : {1U, 2U, 3U}) {
        INFO("flags " + std::to_string(flags));
        ID3D12CommandQueue* queue =
            console_queue(fixture, static_cast<uint32_t>(D3D12_COMMAND_LIST_TYPE_COMPUTE), flags);
        REQUIRE(queue != nullptr);
        const D3D12_COMMAND_QUEUE_DESC desc = command_queue_proxy_real(queue)->GetDesc();
        CHECK(desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE);
        CHECK_EQ(static_cast<uint32_t>(desc.Flags), flags & 1U);
        queue->Release();
    }

    // The copy engine: a queue, an allocator through slot 9, and a list through slot 73.
    ID3D12CommandQueue* queue = console_queue(fixture, kCopyEngine);
    REQUIRE(queue != nullptr);
    CHECK(command_queue_proxy_real(queue)->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY);
    using CreateAllocator = HRESULT (*)(void* device, uint32_t type, const GUID* riid, void** out);
    const GUID allocator_interface = __uuidof(ID3D12CommandAllocator);
    ID3D12CommandAllocator* allocator = nullptr;
    REQUIRE(SUCCEEDED(slot<CreateAllocator>(table_of(fixture.proxy), 9)(
        fixture.proxy, kCopyEngine, &allocator_interface, reinterpret_cast<void**>(&allocator))));
    const std::array<uint32_t, 4> list_desc = {kCopyEngine, 0, 1, 0};
    void* list = nullptr;
    REQUIRE(SUCCEEDED(slot<CreateListX>(table_of(fixture.proxy), 73)(
        fixture.proxy, list_desc.data(), allocator, nullptr, &kTitleListInterface, &list)));
    CHECK(static_cast<ID3D12GraphicsCommandList*>(list)->GetType() == D3D12_COMMAND_LIST_TYPE_COPY);
    static_cast<IUnknown*>(list)->Release();
    allocator->Release();
    queue->Release();
}

TEST_CASE("slot 73 makes a list proxy with room for the title's fields") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    ID3D12CommandAllocator* allocator = direct_allocator(fixture);
    REQUIRE(allocator != nullptr);
    void* list = console_list(fixture, allocator);
    REQUIRE(list != nullptr);
    REQUIRE(command_list_proxy_real(list) != nullptr);

    // The write pointer and the ends start at zero, so every record goes to a method.
    auto* bytes = static_cast<uint8_t*>(list);
    for (const size_t offset : {size_t{0x18}, size_t{0x28}, size_t{0x30}}) {
        uint64_t value = 1;
        std::memcpy(&value, bytes + offset, sizeof(value));
        CHECK_EQ(value, uint64_t{0});
    }
    // The title writes its own fields up to +0x564; the proxy keeps working after.
    std::memset(bytes + 0x10, 0x5A, 0x564 - 0x10);

    void* answer = nullptr;
    REQUIRE(SUCCEEDED(static_cast<IUnknown*>(list)->QueryInterface(kTitleListInterface, &answer)));
    CHECK(answer == list);
    static_cast<IUnknown*>(answer)->Release();
    REQUIRE(SUCCEEDED(static_cast<IUnknown*>(list)->QueryInterface(
        __uuidof(ID3D12GraphicsCommandList), &answer)));
    CHECK(answer == list);
    static_cast<IUnknown*>(answer)->Release();

    auto* pc = static_cast<ID3D12GraphicsCommandList*>(list);
    CHECK(pc->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT);
    ID3D12Device* device = nullptr;
    REQUIRE(SUCCEEDED(pc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))));
    CHECK(device == fixture.proxy);
    device->Release();

    // The console's own slots, and those not mapped yet, fail.
    for (const unsigned index : {27U, 31U, 60U, 131U, 132U, 146U}) {
        INFO("list slot " + std::to_string(index));
        CHECK_EQ(slot<Unknown>(table_of(list), index)(list, 1, 2, 3), E_NOTIMPL);
    }

    CHECK(SUCCEEDED(pc->Close()));
    CHECK_EQ(pc->Release(), 0U);
    CHECK(command_list_proxy_real(list) == nullptr);
    allocator->Release();
}

TEST_CASE("a list recorded through the console's slots runs on the queue proxy") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    ID3D12CommandQueue* queue = console_queue(fixture);
    REQUIRE(queue != nullptr);
    ID3D12CommandAllocator* allocator = direct_allocator(fixture);
    REQUIRE(allocator != nullptr);
    void* list = console_list(fixture, allocator);
    REQUIRE(list != nullptr);

    const D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, 0};
    ID3D12QueryHeap* heap = nullptr;
    REQUIRE(SUCCEEDED(fixture.proxy->CreateQueryHeap(&heap_desc, __uuidof(ID3D12QueryHeap),
                                                     reinterpret_cast<void**>(&heap))));
    const D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                         D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = 256;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    REQUIRE(SUCCEEDED(fixture.proxy->CreateCommittedResource(
        &readback, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffer))));

    // Console slots 57 and 58 are the PC's EndQuery and ResolveQueryData, four slots later.
    using EndQuery = void (*)(void* list, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index);
    using ResolveQueryData =
        void (*)(void* list, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count,
                 ID3D12Resource* destination, UINT64 offset);
    const void* const* table = table_of(list);
    slot<EndQuery>(table, 57)(list, heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    slot<ResolveQueryData>(table, 58)(list, heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, buffer, 0);
    REQUIRE(SUCCEEDED(static_cast<ID3D12GraphicsCommandList*>(list)->Close()));

    // Slot 10 hands the PC queue the list behind the proxy.
    const std::array<ID3D12CommandList*, 1> lists = {static_cast<ID3D12CommandList*>(list)};
    queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());

    ID3D12Fence* fence = nullptr;
    REQUIRE(SUCCEEDED(fixture.proxy->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                                 reinterpret_cast<void**>(&fence))));
    REQUIRE(SUCCEEDED(command_queue_proxy_real(queue)->Signal(fence, 1)));
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(done != nullptr);
    REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, done)));
    CHECK_EQ(WaitForSingleObject(done, 10000), DWORD{WAIT_OBJECT_0});
    CloseHandle(done);

    const D3D12_RANGE read{0, sizeof(uint64_t)};
    void* mapped = nullptr;
    REQUIRE(SUCCEEDED(buffer->Map(0, &read, &mapped)));
    uint64_t ticks = 0;
    std::memcpy(&ticks, mapped, sizeof(ticks));
    const D3D12_RANGE written{0, 0};
    buffer->Unmap(0, &written);
    CHECK(ticks != 0);

    fence->Release();
    buffer->Release();
    heap->Release();
    static_cast<IUnknown*>(list)->Release();
    allocator->Release();
    queue->Release();
}

namespace {

using SetFrameInterval = HRESULT (*)(void* device, void* output, UINT length_us, UINT periodicity,
                                     UINT flags);
using ScheduleFrameEvent = HRESULT (*)(void* device, UINT type, UINT offset_us, const void* handles,
                                       UINT flags);
using WaitFrameEvent = HRESULT (*)(void* device, UINT type, UINT timeout_ms, const void* handles,
                                   UINT flags, uint64_t* token);

constexpr HRESULT kStartedClock = 0x10000000;
constexpr UINT kUnschedule = 1;
constexpr UINT kTokenOnTimeout = 2;

// The title's block of handles to signal with a frame event.
struct HandleBlock {
    uint32_t count;
    uint32_t reserved;
    const HANDLE* list;
    int32_t* signaled;
};

// The frame clock's three slots on one device.
struct Clock {
    explicit Clock(const Fixture& fixture)
        : device(fixture.proxy), set_interval(slot<SetFrameInterval>(table_of(device), 78)),
          schedule(slot<ScheduleFrameEvent>(table_of(device), 79)),
          wait(slot<WaitFrameEvent>(table_of(device), 80)) {}

    void* device;
    SetFrameInterval set_interval;
    ScheduleFrameEvent schedule;
    WaitFrameEvent wait;
};

double milliseconds_since(const LARGE_INTEGER& start) {
    LARGE_INTEGER end{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

} // namespace

TEST_CASE("the frame clock signals its events once a frame") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    const Clock clock(fixture);
    uint64_t token = 0;

    // Nothing to schedule or wait for before an interval is set.
    CHECK_EQ(clock.schedule(clock.device, 0, 0, nullptr, 0), E_INVALIDARG);
    CHECK_EQ(clock.wait(clock.device, 0, 10, nullptr, 0, &token), E_INVALIDARG);

    // As the title starts it: 33,333 us, then event 2 at offset 0, which starts the clock.
    CHECK(SUCCEEDED(clock.set_interval(clock.device, nullptr, 16667, 1, 0)));
    CHECK_EQ(clock.schedule(clock.device, 2, 0, nullptr, 0), kStartedClock);
    HANDLE extra = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(extra != nullptr);
    const std::array<HANDLE, 1> handles = {extra};
    const HandleBlock block{1, 0, handles.data(), nullptr};
    CHECK_EQ(clock.schedule(clock.device, 0, 0, &block, 0), S_FALSE);

    // Origins come a frame apart, each with a token of its own.
    REQUIRE(clock.wait(clock.device, 0, 1000, nullptr, 0, &token) == S_OK);
    const uint64_t first = token;
    CHECK(first != 0);
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    token = 0;
    REQUIRE(clock.wait(clock.device, 0, 1000, nullptr, 0, &token) == S_OK);
    const double frame = milliseconds_since(start);
    INFO("a frame took " + std::to_string(frame) + " ms");
    CHECK(frame > 5.0);
    CHECK(frame < 200.0);
    CHECK(token != 0);
    CHECK(token != first);

    // The title's handle is signalled with the origin, and event 2 comes too.
    CHECK_EQ(WaitForSingleObject(extra, 1000), DWORD{WAIT_OBJECT_0});
    CHECK_EQ(clock.wait(clock.device, 2, 1000, nullptr, 0, nullptr), S_OK);
    // An event that isn't scheduled can't be waited for.
    CHECK_EQ(clock.wait(clock.device, 1, 10, nullptr, 0, &token), E_INVALIDARG);

    // Taken off the clock, event 2 can't be waited for; with no interval, nothing can.
    CHECK(SUCCEEDED(clock.schedule(clock.device, 2, 0, nullptr, kUnschedule)));
    CHECK_EQ(clock.wait(clock.device, 2, 10, nullptr, 0, nullptr), E_INVALIDARG);
    CHECK(SUCCEEDED(clock.set_interval(clock.device, nullptr, 0, 1, 0)));
    CHECK_EQ(clock.wait(clock.device, 0, 10, nullptr, 0, &token), E_INVALIDARG);
    CloseHandle(extra);
}

TEST_CASE("a frame clock wait that times out returns S_FALSE") {
    const Fixture fixture;
    if (fixture.real == nullptr) {
        SKIP("no WARP device on this host");
    }
    REQUIRE(fixture.proxy != nullptr);
    const Clock clock(fixture);
    // 15 frames a second: the first origin comes at once, the next one 66.7 ms later.
    CHECK(SUCCEEDED(clock.set_interval(clock.device, nullptr, 66667, 1, 0)));
    CHECK_EQ(clock.schedule(clock.device, 0, 0, nullptr, 0), kStartedClock);
    REQUIRE(clock.wait(clock.device, 0, 1000, nullptr, 0, nullptr) == S_OK);
    uint64_t token = 0;
    CHECK_EQ(clock.wait(clock.device, 0, 1, nullptr, 0, &token), S_FALSE);
    CHECK_EQ(token, uint64_t{0});
    // With the flag, a token comes even so.
    CHECK_EQ(clock.wait(clock.device, 0, 1, nullptr, kTokenOnTimeout, &token), S_FALSE);
    CHECK(token != 0);
}
