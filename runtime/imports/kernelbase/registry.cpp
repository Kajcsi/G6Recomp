// The registry functions (g6/imports/registry.h): the console-mode key the GDK thunk reads, and
// honest failure for everything else.

#include "g6/imports/registry.h"

#include <cstring>
#include <cwctype>
#include <intrin.h>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "g6/io/text.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"

namespace g6::imports::kernelbase {
namespace {

constexpr std::wstring_view kOemPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
constexpr std::wstring_view kConsoleMode = L"ConsoleMode";

// The handle we give out for the OEM key: the address of an object of ours, so it can't be a
// handle the host registry issued. Nothing else is ever opened, so it is the only open key.
const char g_oem_key_tag = 0;
HKEY oem_key() {
    return reinterpret_cast<HKEY>(const_cast<char*>(&g_oem_key_tag));
}

bool equal_ignoring_case(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

bool is_oem_path(HKEY key, LPCWSTR sub_key) {
    if (key != HKEY_LOCAL_MACHINE || sub_key == nullptr) {
        return false;
    }
    std::wstring_view path = sub_key;
    while (!path.empty() && path.front() == L'\\') {
        path.remove_prefix(1);
    }
    while (!path.empty() && path.back() == L'\\') {
        path.remove_suffix(1);
    }
    return equal_ignoring_case(path, kOemPath);
}

// Each refused request is logged once, by what it asked for.
void refuse_once(const char* function, const std::string& what, const void* caller) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    const std::scoped_lock lock(mutex);
    if (reported.insert(std::string(function) + ' ' + what).second) {
        LOG_WARN(imports,
                 "{}({}) from {}: not implemented; returning ERROR_CALL_NOT_IMPLEMENTED (reported "
                 "once)",
                 function, what, caller);
    }
}

std::string key_name(HKEY key, LPCWSTR sub_key) {
    std::string name = fmt::format("{}", static_cast<const void*>(key));
    if (sub_key != nullptr) {
        name += ", \"" + io::utf8(sub_key) + '"';
    }
    return name;
}

LSTATUS open_key(HKEY key, LPCWSTR sub_key, PHKEY result, const char* function,
                 const void* caller) {
    if (result == nullptr) {
        return ERROR_INVALID_PARAMETER;
    }
    *result = nullptr;
    if (is_oem_path(key, sub_key)) {
        *result = oem_key();
        return ERROR_SUCCESS;
    }
    refuse_once(function, key_name(key, sub_key), caller);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

} // namespace

LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR sub_key, DWORD /*options*/, REGSAM /*access*/,
                             PHKEY result) {
    return open_key(key, sub_key, result, "RegOpenKeyExW", _ReturnAddress());
}

LSTATUS WINAPI RegOpenKeyW(HKEY key, LPCWSTR sub_key, PHKEY result) {
    return open_key(key, sub_key, result, "RegOpenKeyW", _ReturnAddress());
}

LSTATUS WINAPI RegQueryValueExW(HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type,
                                LPBYTE data, LPDWORD data_size) {
    if (key != oem_key()) {
        refuse_once("RegQueryValueExW",
                    key_name(key, nullptr) + ", \"" +
                        (value_name != nullptr ? io::utf8(value_name) : std::string()) + '"',
                    _ReturnAddress());
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    if (reserved != nullptr || (data != nullptr && data_size == nullptr)) {
        return ERROR_INVALID_PARAMETER;
    }
    // The key holds one value, and no default value.
    if (value_name == nullptr || !equal_ignoring_case(value_name, kConsoleMode)) {
        return ERROR_FILE_NOT_FOUND;
    }
    constexpr DWORD kSize = sizeof(DWORD);
    if (type != nullptr) {
        *type = REG_DWORD;
    }
    if (data == nullptr) {
        // A size query, or none at all.
        if (data_size != nullptr) {
            *data_size = kSize;
        }
        return ERROR_SUCCESS;
    }
    const DWORD capacity = *data_size;
    *data_size = kSize;
    if (capacity < kSize) {
        return ERROR_MORE_DATA;
    }
    const DWORD mode = kConsoleModeSeriesX;
    std::memcpy(data, &mode, kSize);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI RegCloseKey(HKEY key) {
    // Only the OEM key is ever open.
    return key == oem_key() ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

void register_registry(loader::ImportRegistry& imports) {
    for (const char* dll : {"api-ms-win-core-registry-*", "advapi32.dll"}) {
        imports.hle(dll, loader::Symbol::by_name("RegOpenKeyExW"),
                    reinterpret_cast<const void*>(&RegOpenKeyExW));
        imports.hle(dll, loader::Symbol::by_name("RegOpenKeyW"),
                    reinterpret_cast<const void*>(&RegOpenKeyW));
        imports.hle(dll, loader::Symbol::by_name("RegQueryValueExW"),
                    reinterpret_cast<const void*>(&RegQueryValueExW));
        imports.hle(dll, loader::Symbol::by_name("RegCloseKey"),
                    reinterpret_cast<const void*>(&RegCloseKey));
    }
}

} // namespace g6::imports::kernelbase
