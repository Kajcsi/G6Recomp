// The import policy table (docs/execution-model.md, "Import policy"). The first two cases are
// the policy test the plan requires: no networking DLL is ever forwarded, whatever the table
// says (hard rule 5).

#include <stdexcept>
#include <string>
#include <utility>
#include <windows.h>

#include "g6/loader/import_registry.h"
#include "g6test.h"

using g6::loader::Binding;
using g6::loader::ImportRegistry;
using g6::loader::StubResult;
using g6::loader::Symbol;

namespace {

constexpr const char* kNetworkingDlls[] = {
    "ws2_32.dll",
    "WS2_32",
    "wsock32.dll",
    "winhttp.dll",
    "wininet.dll",
    "dnsapi.dll",
    "iphlpapi.dll",
    "websocket.dll",
    "xcurl.dll",
    "urlmon.dll",
    "GamePlatformServices.dll",
    R"(C:\Windows\System32\ws2_32.dll)",
    R"(C:\Windows\System32\gameplatformservices.dll)",
    "api-ms-win-net-isolation-l1-1-0.dll",
};

int host_function() {
    return 1;
}

int other_host_function() {
    return 2;
}

} // namespace

TEST_CASE("forward rules for networking DLLs are rejected") {
    ImportRegistry registry;
    for (const char* dll : kNetworkingDlls) {
        CHECK_THROWS_AS(registry.forward_dll(dll), std::invalid_argument);
        CHECK_THROWS_AS(registry.forward(dll, Symbol::by_name("connect")), std::invalid_argument);
        // Nor may a harmless-looking name forward to one.
        CHECK_THROWS_AS(registry.forward_dll("g6alias.dll", dll), std::invalid_argument);
        CHECK_THROWS_AS(registry.forward("kernel32.dll", Symbol::by_name("Sleep"), dll, "connect"),
                        std::invalid_argument);
    }
    CHECK_THROWS_AS(registry.forward_dll("api-ms-win-net-*"), std::invalid_argument);
}

TEST_CASE("a catch-all forward rule never reaches a networking DLL") {
    const bool loaded_before = GetModuleHandleW(L"ws2_32.dll") != nullptr;

    ImportRegistry registry;
    registry.forward_dll("*");
    for (const char* dll : kNetworkingDlls) {
        CHECK(registry.dll_binding(dll) == Binding::Offline);
        CHECK(!registry.host_provides(dll));
        for (const Symbol& symbol : {Symbol::by_name("connect"), Symbol::by_ordinal(4)}) {
            const auto resolution = registry.resolve(dll, symbol);
            CHECK(resolution.binding == Binding::Offline);
            CHECK(resolution.address == nullptr);
        }
    }
    // Resolving never even loaded the host's DLL.
    if (!loaded_before) {
        CHECK(GetModuleHandleW(L"ws2_32.dll") == nullptr);
    }

    // Everything else still forwards.
    const auto resolution = registry.resolve("kernel32.dll", Symbol::by_name("GetCurrentThreadId"));
    CHECK(resolution.binding == Binding::Forward);
    CHECK(resolution.address == reinterpret_cast<const void*>(GetProcAddress(
                                    GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadId")));
}

TEST_CASE("an HLE rule on a networking DLL is bound offline") {
    ImportRegistry registry;
    registry.hle("winhttp.dll", Symbol::by_name("WinHttpOpen"),
                 reinterpret_cast<const void*>(&host_function));
    const auto resolution = registry.resolve("winhttp.dll", Symbol::by_name("WinHttpOpen"));
    CHECK(resolution.binding == Binding::Offline);
    CHECK(resolution.address == reinterpret_cast<const void*>(&host_function));
    CHECK(registry.resolve("winhttp.dll", Symbol::by_name("WinHttpConnect")).binding ==
          Binding::Offline);
}

TEST_CASE("rules apply by specificity") {
    ImportRegistry registry;
    registry.forward_dll("api-ms-win-core-*");
    registry.stub_dll("api-ms-win-core-com-*", StubResult::hresult());
    registry.hle("api-ms-win-core-com-l1-1-0.dll", Symbol::by_name("CoInitializeEx"),
                 reinterpret_cast<const void*>(&host_function));
    registry.hle("api-ms-win-core-com-*", Symbol::by_name("CoInitializeEx"),
                 reinterpret_cast<const void*>(&other_host_function));

    // A function rule beats a DLL rule, and an exact name beats a pattern.
    auto resolution =
        registry.resolve("API-MS-WIN-CORE-COM-L1-1-0", Symbol::by_name("CoInitializeEx"));
    CHECK(resolution.binding == Binding::Hle);
    CHECK(resolution.address == reinterpret_cast<const void*>(&host_function));
    resolution =
        registry.resolve("api-ms-win-core-com-l1-1-1.dll", Symbol::by_name("CoInitializeEx"));
    CHECK(resolution.address == reinterpret_cast<const void*>(&other_host_function));

    // A longer pattern beats a shorter one.
    resolution =
        registry.resolve("api-ms-win-core-com-l1-1-0.dll", Symbol::by_name("CoCreateGuid"));
    CHECK(resolution.binding == Binding::Stub);
    CHECK(resolution.address == nullptr);
    CHECK_EQ(resolution.stub.value, uint64_t{0x80004001});
    CHECK(registry.resolve("api-ms-win-core-synch-l1-2-0.dll", Symbol::by_name("Sleep")).binding ==
          Binding::Forward);
}

TEST_CASE("unknown DLLs and ordinal imports are stubbed") {
    ImportRegistry registry;
    registry.forward_dll("kernel32.dll");

    // Deny by default, with the Win32 error.
    auto resolution = registry.resolve("g6console.dll", Symbol::by_name("XMemAlloc"));
    CHECK(resolution.binding == Binding::Stub);
    CHECK_EQ(resolution.stub.value, uint64_t{0});
    CHECK(resolution.stub.last_error == std::optional<uint32_t>(ERROR_CALL_NOT_IMPLEMENTED));
    CHECK(!registry.knows("g6console.dll"));

    // A DLL rule doesn't forward ordinals: the console's ordinals aren't the host's.
    resolution = registry.resolve("kernel32.dll", Symbol::by_ordinal(1));
    CHECK(resolution.binding == Binding::Stub);

    // A function the host lacks is a stub too.
    resolution = registry.resolve("kernel32.dll", Symbol::by_name("G6NoSuchFunction"));
    CHECK(resolution.binding == Binding::Stub);
    CHECK(resolution.address == nullptr);
}

TEST_CASE("offline DLLs never forward") {
    ImportRegistry registry;
    registry.offline_dll("g6fakenet.dll", StubResult::hresult());
    registry.offline("g6fakenet.dll", Symbol::by_name("Open"),
                     reinterpret_cast<const void*>(&host_function));
    CHECK(registry.dll_binding("g6fakenet.dll") == Binding::Offline);
    auto resolution = registry.resolve("g6fakenet.dll", Symbol::by_name("Open"));
    CHECK(resolution.binding == Binding::Offline);
    CHECK(resolution.address == reinterpret_cast<const void*>(&host_function));
    resolution = registry.resolve("g6fakenet.dll", Symbol::by_name("Send"));
    CHECK(resolution.binding == Binding::Offline);
    CHECK(resolution.address == nullptr);
    CHECK_EQ(resolution.stub.value, uint64_t{0x80004001});
}

TEST_CASE("replaced DLLs") {
    ImportRegistry registry;
    registry.replace_dll("G6Replaced");
    registry.replace_dll("g6family-*");
    CHECK(registry.is_replaced("g6replaced.dll"));
    CHECK(registry.is_replaced(R"(C:\Game\G6REPLACED.DLL)"));
    CHECK(registry.is_replaced("g6family-one.dll"));
    CHECK(!registry.is_replaced("g6other.dll"));
    CHECK(!registry.is_replaced("kernel32.dll"));

    // Replacing alone gives the guest nothing: unknown, stubbed, and the log says why.
    CHECK(!registry.knows("g6replaced.dll"));
    auto resolution = registry.resolve("g6replaced.dll", Symbol::by_name("Build"));
    CHECK(resolution.binding == Binding::Stub);
    CHECK(resolution.address == nullptr);
    CHECK(resolution.note.find("replaced") != std::string::npos);

    // Its implementation comes from ordinary rules.
    registry.hle("g6replaced.dll", Symbol::by_name("Build"),
                 reinterpret_cast<const void*>(&host_function));
    CHECK(registry.knows("g6replaced.dll"));
    CHECK(registry.is_replaced("g6replaced.dll"));
    resolution = registry.resolve("g6replaced.dll", Symbol::by_name("Build"));
    CHECK(resolution.binding == Binding::Hle);
    CHECK(resolution.address == reinterpret_cast<const void*>(&host_function));

    // The rule moves with the registry.
    const ImportRegistry moved = std::move(registry);
    CHECK(moved.is_replaced("g6replaced.dll"));
}

TEST_CASE("DLL and symbol names") {
    using g6::loader::is_networking_dll;
    using g6::loader::normalize_dll_name;
    using g6::loader::symbol_display_name;

    CHECK_EQ(normalize_dll_name("C:\\Windows\\System32\\KERNEL32.DLL"),
             std::string("kernel32.dll"));
    CHECK_EQ(normalize_dll_name("api-ms-win-core-synch-l1-2-0"),
             std::string("api-ms-win-core-synch-l1-2-0.dll"));
    CHECK_EQ(normalize_dll_name("noext."), std::string("noext"));
    CHECK_EQ(normalize_dll_name("game.exe"), std::string("game.exe"));

    CHECK(is_networking_dll("WinHttp.dll"));
    CHECK(is_networking_dll("ext-ms-win-networking-wlanapi-l1-1-0.dll"));
    CHECK(!is_networking_dll("kernel32.dll"));
    CHECK(!is_networking_dll("api-ms-win-core-synch-l1-2-0.dll"));

    CHECK_EQ(symbol_display_name("XMem.dll", Symbol::by_ordinal(12)), std::string("xmem_ord12"));
    CHECK_EQ(symbol_display_name("xmem.dll", Symbol::by_name("XMemAlloc")),
             std::string("XMemAlloc"));
}
