#pragma once

// The host's ucrtbase.dll: the C runtime that the guest's forwarded api-ms-win-crt-* imports bind
// to. The runtime's own CRT can be another one (ucrtbased in debug builds), so every FILE object,
// errno value, malloc'd buffer, and invalid parameter handler the guest has belongs to this one.

#include <cstdlib>
#include <windows.h>

#include "g6/log.h"

namespace g6::imports::ucrtbase {

// Loaded once; the process ends if it can't be.
inline HMODULE host_crt() {
    static const HMODULE module = [] {
        HMODULE loaded = LoadLibraryExW(L"ucrtbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (loaded == nullptr) {
            LOG_ERROR(crt, "cannot load the host's ucrtbase.dll (error {})", GetLastError());
            std::abort();
        }
        return loaded;
    }();
    return module;
}

// Sets `function` to its export `name`; the process ends if there is none.
template <typename Function>
void bind_host_crt(const char* name, Function& function) {
    function = reinterpret_cast<Function>(GetProcAddress(host_crt(), name));
    if (function == nullptr) {
        LOG_ERROR(crt, "the host's ucrtbase.dll has no {}", name);
        std::abort();
    }
}

} // namespace g6::imports::ucrtbase
