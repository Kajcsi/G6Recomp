// The device proxy (g6/gpu/device.h) over a WARP device: the base slots reach the PC device, the
// slots that return structures take both argument orders, the byte at +0x18 is set, the slots
// not known yet fail, and the proxy's references hold the PC device.

#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <windows.h>

#include "g6/gpu/device.h"
#include "g6test.h"

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
    // 43 returns a structure in a form not seen yet; 48, 73 and 133 are console methods; the
    // last slot is the end of the table.
    for (const unsigned index : {43U, 48U, 73U, 133U, kDeviceSlots - 1}) {
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
