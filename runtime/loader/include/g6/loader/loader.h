#pragma once

// The native loader (docs/execution-model.md, "Loader"). It maps guest modules itself instead of
// handing them to the Windows loader, so that it decides what every import binds to:
//
// 1. Maps each module at its preferred base, or wherever there is room, with relocations.
// 2. Binds imports, delay-load imports included, through guest modules and the ImportRegistry.
//    Delay-load imports are bound when the module loads, not when they are first called.
// 3. Registers each module's exception table with Windows, so SEH and C++ exceptions cross
//    guest and host frames.
// 4. Gives each module with a TLS directory static TLS borrowed from a donor DLL.
// 5. Sets each module's security cookie, and applies its hooks and patches (docs/hooks.md).
// 6. Initializes the new modules in dependency order: TLS callbacks, then DllMain. The
//    executable gets its TLS callbacks; run_entry_point starts it.
// 7. Binds the guest's loader functions (GetModuleHandle, GetProcAddress, LoadLibrary, and a
//    few others) to HLE versions that see only guest modules and the registry.
//
// One Loader may exist at a time, because the HLE loader functions are plain Win32 functions
// with no room for a context. Destroying the Loader detaches and unmaps every guest module, so
// no guest code may be running then.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "g6/loader/import_registry.h"
#include "g6/loader/patches.h"
#include "g6/pe/image.h"

namespace g6::loader {

// A guest module the loader mapped. A cheap handle, valid until the Loader is destroyed.
class Module {
public:
    // The file name, as the importing module (or the caller of Loader::load) spelled it.
    const std::string& name() const;
    const std::filesystem::path& path() const;
    uint64_t base() const;
    uint64_t size() const;
    bool is_executable() const;
    const pe::Image& image() const;

    // The address of an export, with forwarders followed, or nullptr when there is none.
    void* find_export(std::string_view name) const;
    void* find_export(uint16_t ordinal) const;

    template <typename T>
    T export_as(std::string_view name) const {
        return reinterpret_cast<T>(find_export(name));
    }

    bool operator==(const Module& other) const { return state_ == other.state_; }

    struct State;

private:
    friend class Loader;
    explicit Module(State* state) : state_(state) {}
    State* state_;
};

// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's map move constructor allocates.
struct LoaderOptions {
    // Folders searched for guest modules, in order. An import whose DLL is found here is mapped
    // as a guest module, unless it is a networking DLL or the registry marks it replaced; every
    // other import goes to the registry.
    std::vector<std::filesystem::path> search_paths;
    // The policy for everything that isn't a guest module. The loader adds its own HLE rules for
    // the loader functions, which take precedence over the DLL rules given here.
    ImportRegistry imports;
    // Hooks and patches for guest modules, keyed by file name, which is matched like a DLL name
    // in an import. Each module's patches are applied while it loads, before any of its code
    // runs; a patch that breaks a rule in docs/hooks.md fails the load.
    std::map<std::string, ModulePatches> patches;
    // The path the guest's GetModuleFileName reports for a guest module mapped from `file`.
    // Unset, it reports `file` itself. The runner gives console paths (docs/io.md), so the guest
    // can open its own files by those paths. Called on guest threads, so it must be thread-safe.
    std::function<std::wstring(const std::filesystem::path& file)> guest_path;
    // The least stack reserve run_entry_point gives the entry point's thread, when the
    // executable's header asks for less. The runner gives the reserve it is linked with.
    uint64_t min_entry_stack_reserve = 0;
};

class Loader {
public:
    // Throws kernel::LoadError when the support DLLs can't be loaded, and std::logic_error when
    // another Loader exists.
    explicit Loader(LoaderOptions options);
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // Loads a guest module and the guest modules it needs, binds their imports, and initializes
    // the new modules. `path` is a file name to look up in the search paths, or a path. A
    // module that is already loaded is returned as it is. `base` 0 maps the module at its
    // preferred base, or anywhere when that range is taken; any other value maps it exactly
    // there. Throws kernel::LoadError, and then nothing of the failed load stays mapped.
    Module load(const std::filesystem::path& path, uint64_t base = 0);

    // Runs the executable's entry point on a new thread with the stack reserve from its PE
    // header (at least LoaderOptions::min_entry_stack_reserve), waits for it, and returns its
    // exit code. The entry point gets the PEB, as on
    // Windows. It must return rather than exit the process.
    uint32_t run_entry_point(Module executable);

    std::optional<Module> find(std::string_view name) const;
    std::optional<Module> module_at(uint64_t address) const;
    std::vector<Module> modules() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace g6::loader
