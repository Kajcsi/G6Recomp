// The registry HLE (g6/imports/registry.h): the console-mode key the GDK thunk reads to work out
// the device type, with the Win32 contract for sizes and types, and failure for every other key.

#include <cstdint>
#include <windows.h>

#include "g6/imports/registry.h"
#include "g6test.h"

namespace reg = g6::imports::kernelbase;

namespace {

constexpr const wchar_t* kOem = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";

// What the thunk does with the value: the public XSystemDeviceType it reports, or 1 (PC) when
// the key or value is missing.
uint32_t device_type_from_registry() {
    HKEY key = nullptr;
    if (reg::RegOpenKeyW(HKEY_LOCAL_MACHINE, kOem, &key) != ERROR_SUCCESS) {
        return 1;
    }
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    const LSTATUS status = reg::RegQueryValueExW(key, L"ConsoleMode", nullptr, nullptr,
                                                 reinterpret_cast<LPBYTE>(&mode), &size);
    reg::RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return 1;
    }
    switch (mode) {
    case 0x60: return 6; // Lockhart, Series S
    case 0x64: return 7; // Anaconda, Series X
    case 0x68: return 8; // Series devkit
    default: return 0;
    }
}

} // namespace

TEST_CASE("the console-mode key reads as a Series X") {
    CHECK_EQ(device_type_from_registry(), 7U);

    HKEY key = nullptr;
    // Either open function, any case, with stray separators.
    REQUIRE(reg::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"\\software\\microsoft\\WINDOWS NT\\currentversion\\oem\\", 0,
                               KEY_READ, &key) == ERROR_SUCCESS);
    REQUIRE(key != nullptr);

    DWORD type = 0;
    DWORD size = 0;
    // A size query.
    CHECK_EQ(reg::RegQueryValueExW(key, L"consolemode", nullptr, &type, nullptr, &size),
             LSTATUS{ERROR_SUCCESS});
    CHECK_EQ(type, DWORD{REG_DWORD});
    CHECK_EQ(size, DWORD{4});

    // Too small a buffer gets the size it needs and the data untouched.
    uint8_t small[2] = {0xEE, 0xEE};
    size = sizeof(small);
    CHECK_EQ(reg::RegQueryValueExW(key, L"ConsoleMode", nullptr, nullptr, small, &size),
             LSTATUS{ERROR_MORE_DATA});
    CHECK_EQ(size, DWORD{4});
    CHECK((small[0] == 0xEE && small[1] == 0xEE));

    DWORD mode[2] = {0, 0xEEEEEEEE};
    size = sizeof(mode);
    CHECK_EQ(reg::RegQueryValueExW(key, L"ConsoleMode", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(mode), &size),
             LSTATUS{ERROR_SUCCESS});
    CHECK_EQ(mode[0], reg::kConsoleModeSeriesX);
    CHECK_EQ(mode[1], 0xEEEEEEEEU);
    CHECK_EQ(size, DWORD{4});

    // Other values, the default value, and bad arguments.
    CHECK_EQ(reg::RegQueryValueExW(key, L"OEMName", nullptr, nullptr, nullptr, &size),
             LSTATUS{ERROR_FILE_NOT_FOUND});
    CHECK_EQ(reg::RegQueryValueExW(key, nullptr, nullptr, nullptr, nullptr, &size),
             LSTATUS{ERROR_FILE_NOT_FOUND});
    CHECK_EQ(reg::RegQueryValueExW(key, L"ConsoleMode", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(mode), nullptr),
             LSTATUS{ERROR_INVALID_PARAMETER});
    CHECK_EQ(reg::RegCloseKey(key), LSTATUS{ERROR_SUCCESS});
}

TEST_CASE("every other key fails without touching the host registry") {
    HKEY key = reinterpret_cast<HKEY>(uintptr_t{0x1234});
    CHECK_EQ(reg::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                L"SYSTEM\\CurrentControlSet\\Control\\LeapSecondInformation", 0,
                                KEY_READ, &key),
             LSTATUS{ERROR_CALL_NOT_IMPLEMENTED});
    CHECK(key == nullptr);
    // The OEM path under another root.
    CHECK_EQ(reg::RegOpenKeyW(HKEY_CURRENT_USER, kOem, &key), LSTATUS{ERROR_CALL_NOT_IMPLEMENTED});
    CHECK_EQ(reg::RegOpenKeyW(HKEY_LOCAL_MACHINE, kOem, nullptr), LSTATUS{ERROR_INVALID_PARAMETER});

    // A predefined key, or anything else we never handed out.
    DWORD size = 0;
    CHECK_EQ(
        reg::RegQueryValueExW(HKEY_LOCAL_MACHINE, L"ConsoleMode", nullptr, nullptr, nullptr, &size),
        LSTATUS{ERROR_CALL_NOT_IMPLEMENTED});
    CHECK_EQ(reg::RegCloseKey(reinterpret_cast<HKEY>(uintptr_t{0x1234})),
             LSTATUS{ERROR_INVALID_HANDLE});
}
