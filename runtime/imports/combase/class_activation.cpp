// COM and WinRT class activation; see class_activation.h. Each function keeps the real API's
// signature and contract: a refused class gives REGDB_E_CLASSNOTREG, as for a class that isn't
// registered, with every out pointer cleared.

#include "class_activation.h"

#include <array>
#include <map>
#include <mutex>
#include <objbase.h>
#include <roapi.h>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>
#include <winstring.h>

#include "g6/loader/import_registry.h"
#include "g6/log.h"

namespace g6::imports::combase {
namespace {

// Host COM classes guest code may create. Empty: each one is reviewed against hard rule 5
// before it is added, with a comment saying what uses it.
constexpr std::array<CLSID, 0> kAllowedClasses{};

// Host WinRT classes guest code may activate, by runtime class name. Empty, like the above.
constexpr std::array<std::wstring_view, 0> kAllowedRuntimeClasses{};

std::string utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                            size, nullptr, nullptr);
    }
    return result;
}

std::string describe(REFCLSID clsid) {
    std::array<wchar_t, 40> text{};
    const int length = StringFromGUID2(clsid, text.data(), static_cast<int>(text.size()));
    return length > 1 ? utf8({text.data(), static_cast<size_t>(length - 1)}) : "{?}";
}

std::wstring_view view_of(HSTRING string) {
    UINT32 length = 0;
    const wchar_t* buffer = WindowsGetStringRawBuffer(string, &length);
    return buffer != nullptr ? std::wstring_view(buffer, length) : std::wstring_view();
}

// What has been refused, so each class is logged once, and the WinRT classes the guest
// registered itself.
struct State {
    std::mutex mutex;
    std::set<std::string, std::less<>> refused;
    std::map<RO_REGISTRATION_COOKIE, std::vector<std::wstring>> registered_by_cookie;
    std::multiset<std::wstring, std::less<>> registered;
};

State& state() {
    static State instance;
    return instance;
}

void refuse(std::string_view function, const std::string& what) {
    State& shared = state();
    const std::scoped_lock lock(shared.mutex);
    if (shared.refused.emplace(what).second) {
        LOG_WARN(combase, "{}: refused {}, which no allowlist names (hard rule 5)", function, what);
    }
}

bool allowed(REFCLSID clsid) {
    for (const CLSID& allowed_class : kAllowedClasses) {
        if (IsEqualCLSID(allowed_class, clsid)) {
            return true;
        }
    }
    return false;
}

bool allowed(std::wstring_view runtime_class) {
    for (const std::wstring_view allowed_class : kAllowedRuntimeClasses) {
        if (allowed_class == runtime_class) {
            return true;
        }
    }
    State& shared = state();
    const std::scoped_lock lock(shared.mutex);
    return shared.registered.contains(runtime_class);
}

void clear(MULTI_QI* results, DWORD count) {
    for (DWORD i = 0; results != nullptr && i < count; ++i) {
        results[i].pItf = nullptr;
        results[i].hr = REGDB_E_CLASSNOTREG;
    }
}

// Named like the functions they stand in for; only register_activation hands them out.
namespace hle {

HRESULT WINAPI CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid,
                                LPVOID* object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (!allowed(clsid)) {
        refuse("CoCreateInstance", describe(clsid));
        return REGDB_E_CLASSNOTREG;
    }
    return ::CoCreateInstance(clsid, outer, context, iid, object);
}

HRESULT WINAPI CoCreateInstanceEx(REFCLSID clsid, IUnknown* outer, DWORD context,
                                  COSERVERINFO* server, DWORD count, MULTI_QI* results) {
    // A server name means activation on another machine, which is never allowed.
    if (server != nullptr || !allowed(clsid)) {
        clear(results, count);
        refuse("CoCreateInstanceEx", describe(clsid) + (server != nullptr ? " (remote)" : ""));
        return REGDB_E_CLASSNOTREG;
    }
    return ::CoCreateInstanceEx(clsid, outer, context, server, count, results);
}

HRESULT WINAPI CoCreateInstanceFromApp(REFCLSID clsid, IUnknown* outer, DWORD context,
                                       PVOID reserved, DWORD count, MULTI_QI* results) {
    if (!allowed(clsid)) {
        clear(results, count);
        refuse("CoCreateInstanceFromApp", describe(clsid));
        return REGDB_E_CLASSNOTREG;
    }
    return ::CoCreateInstanceFromApp(clsid, outer, context, reserved, count, results);
}

HRESULT WINAPI CoGetClassObject(REFCLSID clsid, DWORD context, LPVOID server, REFIID iid,
                                LPVOID* object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (server != nullptr || !allowed(clsid)) {
        refuse("CoGetClassObject", describe(clsid) + (server != nullptr ? " (remote)" : ""));
        return REGDB_E_CLASSNOTREG;
    }
    return ::CoGetClassObject(clsid, context, static_cast<COSERVERINFO*>(server), iid, object);
}

HRESULT WINAPI RoGetActivationFactory(HSTRING class_id, REFIID iid, void** factory) {
    if (factory == nullptr) {
        return E_POINTER;
    }
    *factory = nullptr;
    const std::wstring_view name = view_of(class_id);
    if (!allowed(name)) {
        refuse("RoGetActivationFactory", utf8(name));
        return REGDB_E_CLASSNOTREG;
    }
    return ::RoGetActivationFactory(class_id, iid, factory);
}

HRESULT WINAPI RoActivateInstance(HSTRING class_id, IInspectable** instance) {
    if (instance == nullptr) {
        return E_POINTER;
    }
    *instance = nullptr;
    const std::wstring_view name = view_of(class_id);
    if (!allowed(name)) {
        refuse("RoActivateInstance", utf8(name));
        return REGDB_E_CLASSNOTREG;
    }
    return ::RoActivateInstance(class_id, instance);
}

// The guest's own factories are in-process, so they stay reachable through the two functions
// above once registered.
HRESULT WINAPI RoRegisterActivationFactories(HSTRING* class_ids, PFNGETACTIVATIONFACTORY* callbacks,
                                             UINT32 count, RO_REGISTRATION_COOKIE* cookie) {
    const HRESULT result = ::RoRegisterActivationFactories(class_ids, callbacks, count, cookie);
    if (SUCCEEDED(result) && cookie != nullptr) {
        try {
            std::vector<std::wstring> names;
            names.reserve(count);
            for (UINT32 i = 0; i < count; ++i) {
                names.emplace_back(view_of(class_ids[i]));
            }
            State& shared = state();
            const std::scoped_lock lock(shared.mutex);
            for (const std::wstring& name : names) {
                shared.registered.insert(name);
            }
            shared.registered_by_cookie[*cookie] = std::move(names);
        } catch (...) {
            ::RoRevokeActivationFactories(*cookie);
            *cookie = nullptr;
            return E_OUTOFMEMORY;
        }
    }
    return result;
}

void WINAPI RoRevokeActivationFactories(RO_REGISTRATION_COOKIE cookie) {
    ::RoRevokeActivationFactories(cookie);
    State& shared = state();
    const std::scoped_lock lock(shared.mutex);
    const auto it = shared.registered_by_cookie.find(cookie);
    if (it == shared.registered_by_cookie.end()) {
        return;
    }
    for (const std::wstring& name : it->second) {
        if (const auto entry = shared.registered.find(name); entry != shared.registered.end()) {
            shared.registered.erase(entry);
        }
    }
    shared.registered_by_cookie.erase(it);
}

} // namespace hle
} // namespace

void register_activation(loader::ImportRegistry& imports) {
    struct Entry {
        const char* name;
        const void* function;
    };
    const Entry com[] = {
        {"CoCreateInstance", reinterpret_cast<const void*>(&hle::CoCreateInstance)},
        {"CoCreateInstanceEx", reinterpret_cast<const void*>(&hle::CoCreateInstanceEx)},
        {"CoCreateInstanceFromApp", reinterpret_cast<const void*>(&hle::CoCreateInstanceFromApp)},
        {"CoGetClassObject", reinterpret_cast<const void*>(&hle::CoGetClassObject)},
    };
    const Entry winrt[] = {
        {"RoGetActivationFactory", reinterpret_cast<const void*>(&hle::RoGetActivationFactory)},
        {"RoActivateInstance", reinterpret_cast<const void*>(&hle::RoActivateInstance)},
        {"RoRegisterActivationFactories",
         reinterpret_cast<const void*>(&hle::RoRegisterActivationFactories)},
        {"RoRevokeActivationFactories",
         reinterpret_cast<const void*>(&hle::RoRevokeActivationFactories)},
    };
    for (const char* dll : {"api-ms-win-core-com-*", "combase.dll", "ole32.dll"}) {
        for (const Entry& entry : com) {
            imports.hle(dll, loader::Symbol::by_name(entry.name), entry.function);
        }
    }
    for (const char* dll : {"api-ms-win-core-winrt-*", "combase.dll"}) {
        for (const Entry& entry : winrt) {
            imports.hle(dll, loader::Symbol::by_name(entry.name), entry.function);
        }
    }
}

} // namespace g6::imports::combase
