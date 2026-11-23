// HLE versions of the loader functions guest code imports (docs/execution-model.md, "Loader",
// step 7). They see guest modules, and DLLs the import registry provides, but never hand the
// guest a host module or a host function outside the policy:
//
// * A guest module's handle is its base address.
// * Any other DLL the registry provides gets a pseudo handle, a reserved address that stands for
//   the DLL name. GetProcAddress on it resolves through the registry under that name, so an API
//   set the policy forwards stays forwarded and one it doesn't stays out of reach.
// * Networking DLLs are never present (hard rule 5).
//
// Each function keeps the caller's last error on success and sets the Win32 error on failure.

#include <filesystem>
#include <string>
#include <windows.h>

#include "g6/log.h"
#include "loader_impl.h"
#include "text.h"

namespace g6::loader {
namespace {

// Restores the caller's last error on the way out, unless the function failed with its own.
class LastError {
public:
    LastError() : value_(::GetLastError()) {}
    ~LastError() { ::SetLastError(value_); }
    LastError(const LastError&) = delete;
    LastError& operator=(const LastError&) = delete;
    void fail(DWORD error) { value_ = error; }

private:
    DWORD value_;
};

uint64_t address_of(const void* pointer) {
    return reinterpret_cast<uint64_t>(pointer);
}

HMODULE as_handle(uint64_t base) {
    return reinterpret_cast<HMODULE>(base);
}

// The guest module whose handle (base address) this is.
Module::State* guest_module(const Loader::Impl& impl, HMODULE handle) {
    Module::State* module = impl.containing(address_of(handle));
    return module != nullptr && module->base() == address_of(handle) ? module : nullptr;
}

bool is_known_handle(const Loader::Impl& impl, HMODULE handle) {
    return guest_module(impl, handle) != nullptr || impl.pseudo_name(handle).has_value();
}

std::wstring widen_ansi(LPCSTR text) {
    return widen(text, CP_ACP);
}

// Copies `text` the way GetModuleFileName does: truncated and terminated when it doesn't fit.
template <typename Char>
DWORD copy_truncated(std::basic_string_view<Char> text, Char* buffer, DWORD size,
                     LastError& error) {
    if (buffer == nullptr || size == 0) {
        error.fail(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (text.size() < size) {
        text.copy(buffer, text.size());
        buffer[text.size()] = Char{0};
        return static_cast<DWORD>(text.size());
    }
    text.copy(buffer, size - 1);
    buffer[size - 1] = Char{0};
    error.fail(ERROR_INSUFFICIENT_BUFFER);
    return size;
}

// The path GetModuleFileName reports for a guest module or a pseudo handle, if it is one.
std::optional<std::wstring> known_module_path(const Loader::Impl& impl, HMODULE module) {
    if (module == nullptr) {
        if (const Module::State* executable = impl.executable()) {
            return executable->path.wstring();
        }
        return std::nullopt;
    }
    if (const Module::State* guest = guest_module(impl, module)) {
        return guest->path.wstring();
    }
    if (const auto name = impl.pseudo_name(module)) {
        return widen(*name);
    }
    return std::nullopt;
}

// Named like the Win32 functions they stand in for; only register_loader_hle hands them out.
namespace hle {

HMODULE WINAPI GetModuleHandleW(LPCWSTR name) {
    LastError error;
    try {
        Loader::Impl* impl = active_loader();
        if (impl == nullptr) {
            HMODULE host = ::GetModuleHandleW(name);
            error.fail(::GetLastError());
            return host;
        }
        if (name == nullptr) {
            if (const Module::State* executable = impl->executable()) {
                return as_handle(executable->base());
            }
            return ::GetModuleHandleW(nullptr);
        }
        const std::string key = normalize_dll_name(narrow(name));
        if (const Module::State* module = impl->find_published(key)) {
            return as_handle(module->base());
        }
        if (impl->host_side_available(key)) {
            if (const void* handle = impl->pseudo_handle(key)) {
                return static_cast<HMODULE>(const_cast<void*>(handle));
            }
        }
        error.fail(ERROR_MOD_NOT_FOUND);
    } catch (...) {
        error.fail(ERROR_NOT_ENOUGH_MEMORY);
    }
    return nullptr;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR name) {
    if (name == nullptr) {
        return hle::GetModuleHandleW(nullptr);
    }
    try {
        return hle::GetModuleHandleW(widen_ansi(name).c_str());
    } catch (...) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

BOOL WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module) {
    if (module == nullptr) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *module = nullptr;
    const Loader::Impl* impl = active_loader();
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0) {
        if (impl != nullptr) {
            if (const Module::State* guest = impl->containing(address_of(name))) {
                *module = as_handle(guest->base());
                return TRUE;
            }
        }
        // Host code: the host's answer, a real module the guest can do little with (see
        // GetProcAddress).
        return ::GetModuleHandleExW(flags, name, module);
    }
    // Guest modules stay loaded until the Loader goes, so the reference-count flags change
    // nothing.
    *module = hle::GetModuleHandleW(name);
    return *module != nullptr ? TRUE : FALSE;
}

BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module) {
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0 || name == nullptr) {
        return hle::GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(name), module);
    }
    try {
        return hle::GetModuleHandleExW(flags, widen_ansi(name).c_str(), module);
    } catch (...) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
}

FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name) {
    LastError error;
    try {
        Loader::Impl* impl = active_loader();
        if (impl == nullptr) {
            error.fail(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
        if (name == nullptr || (IS_INTRESOURCE(name) && LOWORD(name) == 0)) {
            error.fail(ERROR_PROC_NOT_FOUND);
            return nullptr;
        }
        const Symbol symbol = IS_INTRESOURCE(name)
                                  ? Symbol::by_ordinal(LOWORD(reinterpret_cast<uintptr_t>(name)))
                                  : Symbol::by_name(name);

        Loader::Impl::Target target;
        const Module::State* guest =
            module == nullptr ? impl->executable() : guest_module(*impl, module);
        if (guest != nullptr) {
            target = impl->resolve_export(*guest, symbol, false);
        } else if (const auto key = impl->pseudo_name(module)) {
            target = impl->resolve(*key, symbol, false);
        } else {
            // A real host module, which the guest can only have from host code. It still gets
            // nothing the policy doesn't give its DLL name.
            wchar_t path[MAX_PATH];
            const DWORD length =
                module != nullptr ? ::GetModuleFileNameW(module, path, MAX_PATH) : 0;
            if (length == 0 || length == MAX_PATH) {
                error.fail(ERROR_MOD_NOT_FOUND);
                return nullptr;
            }
            const std::string host_key = normalize_dll_name(narrow({path, length}));
            const Resolution resolution = impl->imports.resolve(host_key, symbol);
            target.address = resolution.address;
            target.binding = resolution.binding;
        }
        if (target.address == nullptr) {
            error.fail(ERROR_PROC_NOT_FOUND);
            return nullptr;
        }
        return reinterpret_cast<FARPROC>(const_cast<void*>(target.address));
    } catch (...) {
        error.fail(ERROR_NOT_ENOUGH_MEMORY);
    }
    return nullptr;
}

HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    LastError error;
    try {
        Loader::Impl* impl = active_loader();
        if (impl == nullptr || name == nullptr || file != nullptr) {
            error.fail(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const std::string requested = narrow(name);
        const std::string key = normalize_dll_name(requested);
        if (const Module::State* module = impl->find_published(key)) {
            return as_handle(module->base());
        }
        if (is_networking_dll(key)) {
            LOG_DEBUG(loader, "guest LoadLibrary({}): networking DLL, reported missing", requested);
            error.fail(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
        if (impl->find_guest_file(key)) {
            constexpr DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE |
                                        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                                        LOAD_LIBRARY_AS_IMAGE_RESOURCE;
            if ((flags & kDataOnly) != 0) {
                LOG_WARN(loader, "guest LoadLibrary({}) as data only isn't supported", requested);
                error.fail(ERROR_NOT_SUPPORTED);
                return nullptr;
            }
            try {
                return as_handle(impl->load(std::filesystem::path(widen(key)), 0)->base());
            } catch (const kernel::LoadError& failure) {
                LOG_ERROR(loader, "guest LoadLibrary({}) failed: {}", requested, failure.what());
                error.fail(ERROR_DLL_INIT_FAILED);
                return nullptr;
            }
        }
        if (impl->host_side_available(key)) {
            if (const void* handle = impl->pseudo_handle(key)) {
                return static_cast<HMODULE>(const_cast<void*>(handle));
            }
        }
        if (impl->imports.is_replaced(key)) {
            LOG_INFO(loader, "guest LoadLibrary({}): replaced DLL without an implementation yet",
                     requested);
            error.fail(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
        LOG_DEBUG(loader, "guest LoadLibrary({}): no such module", requested);
        error.fail(ERROR_MOD_NOT_FOUND);
    } catch (...) {
        error.fail(ERROR_NOT_ENOUGH_MEMORY);
    }
    return nullptr;
}

HMODULE WINAPI LoadLibraryW(LPCWSTR name) {
    return hle::LoadLibraryExW(name, nullptr, 0);
}

HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    if (name == nullptr) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    try {
        return hle::LoadLibraryExW(widen_ansi(name).c_str(), file, flags);
    } catch (...) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

HMODULE WINAPI LoadLibraryA(LPCSTR name) {
    return hle::LoadLibraryExA(name, nullptr, 0);
}

BOOL WINAPI FreeLibrary(HMODULE module) {
    // Guest modules stay mapped until the Loader goes, and pseudo handles hold nothing.
    const Loader::Impl* impl = active_loader();
    if (impl != nullptr && is_known_handle(*impl, module)) {
        return TRUE;
    }
    return ::FreeLibrary(module);
}

DECLSPEC_NORETURN void WINAPI FreeLibraryAndExitThread(HMODULE module, DWORD code) {
    const Loader::Impl* impl = active_loader();
    if (impl != nullptr && is_known_handle(*impl, module)) {
        ::ExitThread(code);
    }
    ::FreeLibraryAndExitThread(module, code);
}

// A DLL that runs threadpool callbacks takes a reference to itself and has the pool drop it
// when the callback returns (the console's msvcp140 does this for its task scheduler). Windows'
// pool would hand a guest handle to its own loader, which ignores it today only because the
// failed unload goes unchecked.
void WINAPI FreeLibraryWhenCallbackReturns(PTP_CALLBACK_INSTANCE instance, HMODULE module) {
    const Loader::Impl* impl = active_loader();
    if (impl != nullptr && is_known_handle(*impl, module)) {
        return;
    }
    ::FreeLibraryWhenCallbackReturns(instance, module);
}

BOOL WINAPI DisableThreadLibraryCalls(HMODULE module) {
    const Loader::Impl* impl = active_loader();
    if (impl != nullptr) {
        if (Module::State* guest = guest_module(*impl, module)) {
            // Windows accepts the call from a DLL with static TLS but keeps notifying it (seen on
            // Windows 11 24H2): its TLS callbacks set up thread_local variables on new threads.
            if (guest->tls.slot < 0) {
                guest->thread_calls.store(false, std::memory_order_relaxed);
            }
            return TRUE;
        }
        if (impl->pseudo_name(module)) {
            return TRUE;
        }
    }
    return ::DisableThreadLibraryCalls(module);
}

DWORD WINAPI GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) {
    LastError error;
    try {
        const Loader::Impl* impl = active_loader();
        if (impl != nullptr) {
            if (const auto path = known_module_path(*impl, module)) {
                return copy_truncated(std::wstring_view(*path), buffer, size, error);
            }
        }
    } catch (...) {
        error.fail(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const DWORD length = ::GetModuleFileNameW(module, buffer, size);
    error.fail(::GetLastError());
    return length;
}

DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size) {
    LastError error;
    try {
        const Loader::Impl* impl = active_loader();
        if (impl != nullptr) {
            if (const auto path = known_module_path(*impl, module)) {
                const std::string ansi = narrow(*path, CP_ACP);
                return copy_truncated(std::string_view(ansi), buffer, size, error);
            }
        }
    } catch (...) {
        error.fail(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const DWORD length = ::GetModuleFileNameA(module, buffer, size);
    error.fail(::GetLastError());
    return length;
}

// The C++ runtime's throw finds the image base of its type information through this, and
// Windows only knows the modules it loaded itself.
PVOID NTAPI RtlPcToFileHeader(PVOID pc, PVOID* base) {
    const Loader::Impl* impl = active_loader();
    if (impl != nullptr) {
        if (const Module::State* guest = impl->containing(address_of(pc))) {
            *base = reinterpret_cast<PVOID>(guest->base());
            return *base;
        }
    }
    return ::RtlPcToFileHeader(pc, base);
}

} // namespace hle
} // namespace

void register_loader_hle(ImportRegistry& imports) {
    struct Entry {
        const char* name;
        const void* function;
    };
    const Entry loader_functions[] = {
        {"GetModuleHandleA", reinterpret_cast<const void*>(&hle::GetModuleHandleA)},
        {"GetModuleHandleW", reinterpret_cast<const void*>(&hle::GetModuleHandleW)},
        {"GetModuleHandleExA", reinterpret_cast<const void*>(&hle::GetModuleHandleExA)},
        {"GetModuleHandleExW", reinterpret_cast<const void*>(&hle::GetModuleHandleExW)},
        {"GetProcAddress", reinterpret_cast<const void*>(&hle::GetProcAddress)},
        {"LoadLibraryA", reinterpret_cast<const void*>(&hle::LoadLibraryA)},
        {"LoadLibraryW", reinterpret_cast<const void*>(&hle::LoadLibraryW)},
        {"LoadLibraryExA", reinterpret_cast<const void*>(&hle::LoadLibraryExA)},
        {"LoadLibraryExW", reinterpret_cast<const void*>(&hle::LoadLibraryExW)},
        {"FreeLibrary", reinterpret_cast<const void*>(&hle::FreeLibrary)},
        {"FreeLibraryAndExitThread", reinterpret_cast<const void*>(&hle::FreeLibraryAndExitThread)},
        {"DisableThreadLibraryCalls",
         reinterpret_cast<const void*>(&hle::DisableThreadLibraryCalls)},
        {"GetModuleFileNameA", reinterpret_cast<const void*>(&hle::GetModuleFileNameA)},
        {"GetModuleFileNameW", reinterpret_cast<const void*>(&hle::GetModuleFileNameW)},
    };
    for (const char* dll : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-libraryloader-*"}) {
        for (const Entry& entry : loader_functions) {
            imports.hle(dll, Symbol::by_name(entry.name), entry.function);
        }
    }
    for (const char* dll : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-threadpool-*"}) {
        imports.hle(dll, Symbol::by_name("FreeLibraryWhenCallbackReturns"),
                    reinterpret_cast<const void*>(&hle::FreeLibraryWhenCallbackReturns));
    }
    for (const char* dll :
         {"kernel32.dll", "kernelbase.dll", "ntdll.dll", "api-ms-win-core-rtlsupport-*"}) {
        imports.hle(dll, Symbol::by_name("RtlPcToFileHeader"),
                    reinterpret_cast<const void*>(&hle::RtlPcToFileHeader));
    }
}

} // namespace g6::loader
