#pragma once

// The loader's internals, shared by loader.cpp and the HLE loader functions in hle_loader.cpp.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "g6/kernel/image_loader.h"
#include "g6/loader/import_registry.h"
#include "g6/loader/loader.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "patcher.h"
#include "stubs.h"
#include "thread_support.h"

namespace g6::loader {

struct TlsInfo {
    int slot = -1;      // TLS donor slot, or -1 when the module has no TLS data
    uint32_t index = 0; // the donor's TLS index
    const uint8_t* raw_data = nullptr;
    size_t raw_size = 0;
    size_t zero_fill = 0;
    uint64_t callbacks = 0; // address of the null-terminated TLS callback array, or 0
};

struct Module::State {
    State(Loader::Impl& owner, std::string module_name, std::filesystem::path file,
          pe::Image pe_image);

    const pe::Export* export_named(std::string_view name) const;
    const pe::Export* export_numbered(uint32_t ordinal) const;
    bool contains(uint64_t address) const {
        return mapped.base != 0 && address - mapped.base < mapped.size;
    }
    uint64_t base() const { return mapped.base; }

    Loader::Impl* loader;
    std::string name; // as the importer or the caller spelled it
    std::string key;  // normalize_dll_name(name)
    std::filesystem::path path;
    pe::Image image;
    bool executable;
    kernel::MappedImage mapped;
    pe::ExportTable exports;
    std::map<std::string, size_t, std::less<>> exports_by_name; // index into exports.exports
    std::map<uint32_t, size_t> exports_by_ordinal;
    TlsInfo tls;
    bool function_table = false;
    std::unique_ptr<AppliedPatches> patches; // null when the module has none
    // Set once PROCESS_ATTACH has run; from then on the module gets thread notifications.
    std::atomic<bool> attached{false};
    // Cleared by DisableThreadLibraryCalls, unless the module has static TLS.
    std::atomic<bool> thread_calls{true};
};

// Modules that are mapped, bound, and visible to lock-free lookups and thread notifications, in
// initialization order. Each list is immutable once published; the loader keeps every list it
// published until it is destroyed, so a reader never sees one freed.
struct ModuleList {
    std::vector<Module::State*> modules;
};

struct Loader::Impl {
    explicit Impl(LoaderOptions options);
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // What an import or a GetProcAddress call resolves to.
    struct Target {
        const void* address = nullptr; // nullptr: needs a stub (or has no address at all)
        Binding binding = Binding::Stub;
        bool guest = false; // exported by a guest module
        StubResult stub;
        std::string note;
    };

    // ---- Lock-free lookups over published modules ----
    const ModuleList* published() const { return current.load(std::memory_order_acquire); }
    Module::State* find_published(std::string_view key) const;
    Module::State* containing(uint64_t address) const;
    Module::State* executable() const;

    // ---- Lookups that also see the load in progress (caller holds `mutex`) ----
    Module::State* find_any(std::string_view key) const;

    std::optional<std::filesystem::path> find_guest_file(std::string_view key) const;
    // Whether a DLL that isn't a guest module counts as present for LoadLibrary and
    // GetModuleHandle: the registry knows it, it isn't networking or offline, and a forwarded
    // DLL exists on the host.
    bool host_side_available(std::string_view key);
    // A stand-in module handle for a DLL that isn't a guest module, so that GetProcAddress on
    // it goes through the registry under the name the guest asked for. Never a real module.
    // Handing one out counts the DLL as loaded.
    const void* pseudo_handle(std::string_view key);
    // The pseudo handle GetModuleHandle gives: only for a DLL that counts as loaded, which is
    // one a guest module imports or has loaded, or an API set whose host DLL is loaded. Windows
    // answers API set names by their host DLL the same way. Null otherwise.
    const void* loaded_pseudo_handle(std::string_view key);
    std::optional<std::string> pseudo_name(const void* handle) const;

    // `locked`: the caller holds `mutex`, so modules of the load in progress count too.
    Target resolve(std::string_view key, const Symbol& symbol, bool locked, int depth = 0);
    Target resolve_export(const Module::State& module, const Symbol& symbol, bool locked,
                          int depth = 0);

    Module::State* load(const std::filesystem::path& path, uint64_t base);
    uint32_t run_entry_point(Module::State& executable);

    std::string describe(uint64_t address) const;

    std::vector<std::filesystem::path> search_paths;
    ImportRegistry imports;
    std::function<std::wstring(const std::filesystem::path&)> guest_path; // may be empty
    std::map<std::string, ModulePatches, std::less<>> patches;            // by normalize_dll_name
    void* trap_handler = nullptr; // the vectored handler for trapped cpuid and xgetbv, once needed
    ThreadSupport& threads;
    StubHeap stubs;

    // Serializes loads. Recursive because a guest DllMain may call LoadLibrary.
    mutable std::recursive_mutex mutex;
    std::vector<std::unique_ptr<Module::State>> states; // every module ever mapped
    std::vector<Module::State*> pending;                // mapped by the load in progress
    std::vector<std::unique_ptr<const ModuleList>> lists;
    std::atomic<const ModuleList*> current{nullptr};
    std::map<std::string, const void*, std::less<>> stub_cache; // "dll!symbol" -> thunk

    mutable std::mutex pseudo_mutex;
    uint8_t* pseudo_region = nullptr;
    std::vector<std::string> pseudo_names;

private:
    struct BindStats {
        size_t guest = 0;
        size_t forwarded = 0;
        size_t hle = 0;
        size_t offline = 0;
        size_t stubbed = 0;
    };

    Module::State* map_module(const std::filesystem::path& path, std::string name, uint64_t base,
                              std::vector<Module::State*>& batch);
    void bind_imports(Module::State& module, BindStats& stats);
    const void* stub_for(std::string_view key, const Symbol& symbol, const Target& target);
    void set_up(Module::State& module);
    void publish(const std::vector<Module::State*>& add, const std::vector<Module::State*>& remove);
    void fill_tls_on_existing_threads(const std::vector<Module::State*>& batch);
    void initialize(const std::vector<Module::State*>& batch);
    void discard(const std::vector<Module::State*>& batch, bool was_published);
    void release(Module::State& module);
};

// The Loader that exists, if any. The HLE loader functions work on it.
Loader::Impl* active_loader();

// Adds the HLE loader functions to the registry (hle_loader.cpp).
void register_loader_hle(ImportRegistry& imports);

// Thread notifications from g6threads.dll (loader.cpp).
void on_thread_notification(G6ThreadRecord* record, unsigned long reason, void* context);

} // namespace g6::loader
