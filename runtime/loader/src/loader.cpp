#include "g6/loader/loader.h"

#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <random>
#include <stdexcept>
#include <windows.h>

#include <fmt/format.h>

#include "g6/log.h"
#include "loader_impl.h"
#include "text.h"

namespace g6::loader {
namespace {

std::atomic<Loader::Impl*> g_active{nullptr};

// The PEB, from the TEB (gs:[0x60]), which the entry point of an executable receives.
constexpr size_t kTebPeb = 0x60;

// The default cookie the linker writes, which the loader must replace (the PE format's
// description of the load configuration's SecurityCookie).
constexpr uint64_t kDefaultSecurityCookie = 0x00002B992DDFA232;

// Pseudo handles: one 64 KiB step per DLL name in a reserved, inaccessible range, so they look
// like module handles and are never valid memory.
constexpr size_t kPseudoStep = size_t{64} << 10;
constexpr size_t kPseudoCount = 256;

using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);

template <typename T>
T read_at(uint64_t address) {
    T value;
    std::memcpy(static_cast<void*>(&value), reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
void write_at(uint64_t address, const T& value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

std::string describe_thunk(uint64_t address, void* context) {
    return static_cast<const Loader::Impl*>(context)->describe(address);
}

void call_tls_callbacks(const Module::State& module, DWORD reason) {
    if (module.tls.callbacks == 0) {
        return;
    }
    // Read the array each time, as Windows does: a module may add callbacks at run time.
    for (uint64_t at = module.tls.callbacks;; at += sizeof(uint64_t)) {
        const auto callback = read_at<PIMAGE_TLS_CALLBACK>(at);
        if (callback == nullptr) {
            break;
        }
        callback(reinterpret_cast<PVOID>(module.base()), reason, nullptr);
    }
}

DllMainFn dll_main(const Module::State& module) {
    if (module.executable || module.image.entry_point() == 0) {
        return nullptr;
    }
    return reinterpret_cast<DllMainFn>(module.base() + module.image.entry_point());
}

void fill_tls_block(void** array, const TlsInfo& tls) {
    auto* block = array != nullptr ? static_cast<uint8_t*>(array[tls.index]) : nullptr;
    if (block == nullptr) {
        return; // a thread Windows gave no block; nothing we can do for it
    }
    std::memcpy(block, tls.raw_data, tls.raw_size);
    std::memset(block + tls.raw_size, 0, tls.zero_fill);
}

// Copies the template into a thread's block exactly once, whichever of the thread itself (on
// THREAD_ATTACH) and the loader (walking existing threads) gets there first.
void claim_and_fill(G6ThreadRecord* record, void** array, const TlsInfo& tls) {
    volatile long* state = &record->tls_state[tls.slot];
    if (InterlockedCompareExchange(state, 1, 0) == 0) {
        fill_tls_block(array, tls);
        InterlockedExchange(state, 2);
        return;
    }
    while (InterlockedCompareExchange(state, 2, 2) != 2) {
        YieldProcessor();
    }
}

uint64_t random_cookie() {
    std::random_device device;
    for (;;) {
        const uint64_t value = ((uint64_t{device()} << 32) | device()) & 0x0000FFFFFFFFFFFFull;
        if (value != 0 && value != kDefaultSecurityCookie) {
            return value;
        }
    }
}

// Splits an export forwarder, "dll.symbol" or "dll.#ordinal", at its last dot.
std::optional<std::pair<std::string, Symbol>> parse_forwarder(std::string_view forwarder) {
    const size_t dot = forwarder.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == forwarder.size()) {
        return std::nullopt;
    }
    const std::string dll = normalize_dll_name(forwarder.substr(0, dot));
    const std::string_view symbol = forwarder.substr(dot + 1);
    if (symbol.front() == '#') {
        uint32_t ordinal = 0;
        for (const char c : symbol.substr(1)) {
            if (c < '0' || c > '9' || ordinal > 0xFFFF) {
                return std::nullopt;
            }
            ordinal = ordinal * 10 + static_cast<uint32_t>(c - '0');
        }
        if (ordinal == 0 || ordinal > 0xFFFF) {
            return std::nullopt;
        }
        return std::pair{dll, Symbol::by_ordinal(static_cast<uint16_t>(ordinal))};
    }
    return std::pair{dll, Symbol::by_name(symbol)};
}

} // namespace

Loader::Impl* active_loader() {
    return g_active.load(std::memory_order_acquire);
}

namespace {

// Answers the cpuid and xgetbv instructions the hook engine turned into ud2 (docs/hooks.md,
// rule 6), and passes every other fault on untouched.
LONG CALLBACK on_cpu_trap(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode != STATUS_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const Loader::Impl* loader = active_loader();
    CONTEXT& context = *pointers->ContextRecord;
    const Module::State* module = loader != nullptr ? loader->containing(context.Rip) : nullptr;
    if (module == nullptr || !module->patches ||
        !module->patches->answer_trap(static_cast<uint32_t>(context.Rip - module->base()),
                                      context)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

// ---- Module::State ----

Module::State::State(Loader::Impl& owner, std::string module_name, std::filesystem::path file,
                     pe::Image pe_image)
    : loader(&owner), name(std::move(module_name)), key(normalize_dll_name(name)),
      path(std::move(file)), image(std::move(pe_image)), executable(!image.is_dll()) {}

const pe::Export* Module::State::export_named(std::string_view export_name) const {
    const auto it = exports_by_name.find(export_name);
    return it != exports_by_name.end() ? &exports.exports[it->second] : nullptr;
}

const pe::Export* Module::State::export_numbered(uint32_t ordinal) const {
    const auto it = exports_by_ordinal.find(ordinal);
    return it != exports_by_ordinal.end() ? &exports.exports[it->second] : nullptr;
}

// ---- Thread notifications ----

void on_thread_notification(G6ThreadRecord* record, unsigned long reason, void* context) {
    const auto* impl = static_cast<const Loader::Impl*>(context);
    const ModuleList* list = impl->published();
    if (list == nullptr) {
        return;
    }
    if (reason == DLL_THREAD_ATTACH) {
        // Static TLS first, for every published module: a module whose PROCESS_ATTACH is
        // still running may already use TLS on threads it starts.
        void** const array = current_tls_array();
        for (const Module::State* module : list->modules) {
            if (module->tls.slot >= 0) {
                claim_and_fill(record, array, module->tls);
            }
        }
        for (const Module::State* module : list->modules) {
            if (!module->attached.load(std::memory_order_acquire) ||
                !module->thread_calls.load(std::memory_order_relaxed)) {
                continue;
            }
            call_tls_callbacks(*module, DLL_THREAD_ATTACH);
            if (DllMainFn main = dll_main(*module)) {
                main(reinterpret_cast<HINSTANCE>(module->base()), DLL_THREAD_ATTACH, nullptr);
            }
        }
    } else if (reason == DLL_THREAD_DETACH) {
        for (auto it = list->modules.rbegin(); it != list->modules.rend(); ++it) {
            const Module::State* module = *it;
            if (!module->attached.load(std::memory_order_acquire) ||
                !module->thread_calls.load(std::memory_order_relaxed)) {
                continue;
            }
            call_tls_callbacks(*module, DLL_THREAD_DETACH);
            if (DllMainFn main = dll_main(*module)) {
                main(reinterpret_cast<HINSTANCE>(module->base()), DLL_THREAD_DETACH, nullptr);
            }
        }
    }
}

// ---- Loader::Impl ----

Loader::Impl::Impl(LoaderOptions options)
    : search_paths(std::move(options.search_paths)), imports(std::move(options.imports)),
      guest_path(std::move(options.guest_path)), threads(ThreadSupport::get()),
      stubs(&describe_thunk, this) {
    Impl* expected = nullptr;
    if (!g_active.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        throw std::logic_error("only one Loader may exist at a time");
    }
    try {
        for (auto& [name, module_patches] : options.patches) {
            if (!patches.emplace(normalize_dll_name(name), std::move(module_patches)).second) {
                throw std::invalid_argument(fmt::format(
                    "the patches for {} are given twice, under different spellings", name));
            }
        }
        register_loader_hle(imports);
        pseudo_region = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kPseudoStep * kPseudoCount, MEM_RESERVE, PAGE_NOACCESS));
        if (pseudo_region == nullptr) {
            throw kernel::LoadError(
                fmt::format("cannot reserve room for module handles (error {})", GetLastError()));
        }
        ThreadSupport::set_callback(&on_thread_notification, this);
    } catch (...) {
        if (pseudo_region != nullptr) {
            VirtualFree(pseudo_region, 0, MEM_RELEASE);
        }
        g_active.store(nullptr, std::memory_order_release);
        throw;
    }
}

Loader::Impl::~Impl() {
    // Returns once no notification is running, and none starts afterwards.
    ThreadSupport::set_callback(nullptr, nullptr);

    if (const ModuleList* list = published()) {
        for (auto it = list->modules.rbegin(); it != list->modules.rend(); ++it) {
            Module::State& module = **it;
            if (!module.attached.exchange(false)) {
                continue;
            }
            call_tls_callbacks(module, DLL_PROCESS_DETACH);
            if (DllMainFn main = dll_main(module)) {
                main(reinterpret_cast<HINSTANCE>(module.base()), DLL_PROCESS_DETACH, nullptr);
            }
        }
    }
    // Guest code is done running; the HLE loader functions have nothing to work on any more.
    g_active.store(nullptr, std::memory_order_release);

    for (const auto& state : states) {
        release(*state);
    }
    VirtualFree(pseudo_region, 0, MEM_RELEASE);
    if (trap_handler != nullptr) {
        RemoveVectoredExceptionHandler(trap_handler);
    }
}

Module::State* Loader::Impl::find_published(std::string_view key) const {
    if (const ModuleList* list = published()) {
        for (Module::State* module : list->modules) {
            if (module->key == key) {
                return module;
            }
        }
    }
    return nullptr;
}

Module::State* Loader::Impl::find_any(std::string_view key) const {
    if (Module::State* module = find_published(key)) {
        return module;
    }
    for (Module::State* module : pending) {
        if (module->key == key) {
            return module;
        }
    }
    return nullptr;
}

Module::State* Loader::Impl::containing(uint64_t address) const {
    if (const ModuleList* list = published()) {
        for (Module::State* module : list->modules) {
            if (module->contains(address)) {
                return module;
            }
        }
    }
    return nullptr;
}

Module::State* Loader::Impl::executable() const {
    if (const ModuleList* list = published()) {
        for (Module::State* module : list->modules) {
            if (module->executable) {
                return module;
            }
        }
    }
    return nullptr;
}

std::optional<std::filesystem::path> Loader::Impl::find_guest_file(std::string_view key) const {
    if (key.empty() || is_networking_dll(key) || imports.is_replaced(key)) {
        return std::nullopt;
    }
    for (const std::filesystem::path& folder : search_paths) {
        std::error_code error;
        std::filesystem::path candidate = folder / std::filesystem::path(widen(key));
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool Loader::Impl::host_side_available(std::string_view key) {
    if (is_networking_dll(key) || !imports.knows(key)) {
        return false;
    }
    switch (imports.dll_binding(key)) {
    case Binding::Forward: return imports.host_provides(key);
    case Binding::Offline: return false;
    case Binding::Hle:
    case Binding::Stub:
        // Function rules only, or a stub rule: present, though GetProcAddress may find little.
        return true;
    }
    return false;
}

const void* Loader::Impl::pseudo_handle(std::string_view key) {
    const std::scoped_lock lock(pseudo_mutex);
    const auto it = std::ranges::find(pseudo_names, key);
    const size_t index = static_cast<size_t>(it - pseudo_names.begin());
    if (it == pseudo_names.end()) {
        if (pseudo_names.size() == kPseudoCount) {
            return nullptr;
        }
        pseudo_names.emplace_back(key);
    }
    return pseudo_region + index * kPseudoStep;
}

const void* Loader::Impl::loaded_pseudo_handle(std::string_view key) {
    {
        const std::scoped_lock lock(pseudo_mutex);
        if (const auto it = std::ranges::find(pseudo_names, key); it != pseudo_names.end()) {
            return pseudo_region + static_cast<size_t>(it - pseudo_names.begin()) * kPseudoStep;
        }
    }
    const bool api_set = key.starts_with("api-ms-") || key.starts_with("ext-ms-");
    if (api_set && ::GetModuleHandleW(widen(key).c_str()) != nullptr) {
        return pseudo_handle(key);
    }
    return nullptr;
}

std::optional<std::string> Loader::Impl::pseudo_name(const void* handle) const {
    const auto address = reinterpret_cast<uintptr_t>(handle);
    const auto start = reinterpret_cast<uintptr_t>(pseudo_region);
    if (address < start || (address - start) % kPseudoStep != 0) {
        return std::nullopt;
    }
    const size_t index = (address - start) / kPseudoStep;
    const std::scoped_lock lock(pseudo_mutex);
    if (index >= pseudo_names.size()) {
        return std::nullopt;
    }
    return pseudo_names[index];
}

Loader::Impl::Target Loader::Impl::resolve(std::string_view key, const Symbol& symbol, bool locked,
                                           int depth) {
    const Module::State* module = locked ? find_any(key) : find_published(key);
    if (module != nullptr) {
        return resolve_export(*module, symbol, locked, depth);
    }
    Resolution resolution = imports.resolve(key, symbol);
    return Target{resolution.address, resolution.binding, false, resolution.stub,
                  std::move(resolution.note)};
}

Loader::Impl::Target Loader::Impl::resolve_export(const Module::State& module, const Symbol& symbol,
                                                  bool locked, int depth) {
    const pe::Export* entry = symbol.is_ordinal() ? module.export_numbered(symbol.ordinal)
                                                  : module.export_named(symbol.name);
    if (entry == nullptr) {
        return Target{nullptr, Binding::Stub, true, StubResult::win32(),
                      fmt::format("{} doesn't export it", module.name)};
    }
    if (entry->forwarder.empty()) {
        return Target{reinterpret_cast<const void*>(module.base() + entry->rva),
                      Binding::Forward,
                      true,
                      {},
                      {}};
    }
    constexpr int kMaxForwarderDepth = 16;
    const auto forwarder = parse_forwarder(entry->forwarder);
    if (!forwarder || depth >= kMaxForwarderDepth) {
        return Target{nullptr, Binding::Stub, true, StubResult::win32(),
                      fmt::format("{} forwards it to {}, which can't be resolved", module.name,
                                  entry->forwarder)};
    }
    return resolve(forwarder->first, forwarder->second, locked, depth + 1);
}

std::string Loader::Impl::describe(uint64_t address) const {
    if (const Module::State* module = containing(address)) {
        return fmt::format("{}+{:#x}", module->name, address - module->base());
    }
    return fmt::format("{:#x}", address);
}

const void* Loader::Impl::stub_for(std::string_view key, const Symbol& symbol,
                                   const Target& target) {
    const std::string display = symbol_display_name(key, symbol);
    std::string cache_key = fmt::format("{}!{}", key, display);
    if (const auto it = stub_cache.find(cache_key); it != stub_cache.end()) {
        return it->second;
    }
    const void* thunk =
        stubs.create(std::string(key), display, target.stub, target.binding == Binding::Offline);
    stub_cache.emplace(std::move(cache_key), thunk);
    return thunk;
}

Module::State* Loader::Impl::map_module(const std::filesystem::path& path, std::string name,
                                        uint64_t base, std::vector<Module::State*>& batch) {
    pe::Image image = pe::Image::load_file(path);
    if (image.machine() != pe::kMachineAmd64) {
        throw kernel::LoadError(fmt::format("{} is not an x86-64 image", path.string()));
    }

    auto owned = std::make_unique<Module::State>(*this, std::move(name), path, std::move(image));
    Module::State& module = *owned;
    if (base != 0) {
        module.mapped = kernel::map_image_writable(module.image, base);
    } else {
        try {
            module.mapped = kernel::map_image_writable(module.image, module.image.image_base());
        } catch (const kernel::LoadError&) {
            module.mapped = kernel::map_image_writable(module.image, 0);
        }
    }
    states.push_back(std::move(owned));
    pending.push_back(&module);

    module.exports = pe::read_exports(module.image);
    for (size_t i = 0; i < module.exports.exports.size(); ++i) {
        const pe::Export& entry = module.exports.exports[i];
        module.exports_by_ordinal.emplace(entry.ordinal, i);
        if (!entry.name.empty()) {
            module.exports_by_name.emplace(entry.name, i);
        }
    }

    // Everything this module needs that is a guest module comes along: imported DLLs,
    // delay-loaded ones included, and DLLs its exports forward to.
    std::vector<std::string> needed;
    for (const pe::ImportedModule& imported : pe::read_imports(module.image)) {
        needed.push_back(imported.dll);
    }
    for (const pe::Export& entry : module.exports.exports) {
        if (const auto forwarder = parse_forwarder(entry.forwarder)) {
            needed.push_back(forwarder->first);
        }
    }
    for (const std::string& dll : needed) {
        const std::string key = normalize_dll_name(dll);
        if (find_any(key) != nullptr) {
            continue;
        }
        if (const auto file = find_guest_file(key)) {
            map_module(*file, dll, 0, batch);
        }
    }

    batch.push_back(&module); // after its dependencies: initialization order
    return &module;
}

void Loader::Impl::bind_imports(Module::State& module, BindStats& stats) {
    const uint64_t base = module.base();
    for (const pe::ImportedModule& imported : pe::read_imports(module.image)) {
        const std::string key = normalize_dll_name(imported.dll);
        for (const pe::ImportedSymbol& entry : imported.symbols) {
            const Symbol symbol = entry.name.empty() ? Symbol::by_ordinal(entry.ordinal.value_or(0))
                                                     : Symbol::by_name(entry.name);
            const Target target = resolve(key, symbol, true);
            const void* address = target.address;
            if (target.guest && address != nullptr) {
                ++stats.guest;
            } else if (target.binding == Binding::Offline) {
                ++stats.offline;
            } else if (address == nullptr || target.binding == Binding::Stub) {
                ++stats.stubbed;
            } else if (target.binding == Binding::Forward) {
                ++stats.forwarded;
            } else {
                ++stats.hle;
            }
            if (address == nullptr) {
                address = stub_for(key, symbol, target);
                LOG_DEBUG(loader, "{}: {}!{} is {} ({})", module.name, key,
                          symbol_display_name(key, symbol),
                          target.binding == Binding::Offline ? "offline" : "a stub", target.note);
            }
            if (!module.image.contains(entry.iat_rva, sizeof(uint64_t))) {
                throw kernel::LoadError(
                    fmt::format("{} has an import slot outside the image", module.name));
            }
            write_at(base + entry.iat_rva, reinterpret_cast<uint64_t>(address));
        }
        // A DLL the policy provides counts as loaded once a guest module imports it, so the
        // guest's GetModuleHandle finds it from then on.
        if (find_any(key) == nullptr && host_side_available(key)) {
            pseudo_handle(key);
        }
        // Delay-load imports are bound already, so the delay-load helper never runs. The module
        // handle slot still gets a handle, for code that reads it.
        if (imported.delay_load && imported.module_handle_rva != 0 &&
            module.image.contains(imported.module_handle_rva, sizeof(uint64_t))) {
            const Module::State* target = find_any(key);
            const void* handle = target != nullptr ? reinterpret_cast<const void*>(target->base())
                                                   : pseudo_handle(key);
            write_at(base + imported.module_handle_rva, reinterpret_cast<uint64_t>(handle));
        }
    }
}

void Loader::Impl::set_up(Module::State& module) {
    const uint64_t base = module.base();
    const pe::Image& image = module.image;

    // Security cookie.
    if (const auto config = pe::read_load_config(image); config && config->security_cookie_va) {
        const auto rva = image.try_va_to_rva(config->security_cookie_va);
        if (!rva || !image.contains(*rva, sizeof(uint64_t))) {
            throw kernel::LoadError(
                fmt::format("{} has its security cookie outside the image", module.name));
        }
        write_at(base + *rva, random_cookie());
    }

    // Exception table.
    if (const pe::DataDirectory table = image.directory(pe::Directory::Exception);
        table.present()) {
        if (!image.contains(table.rva, table.size)) {
            throw kernel::LoadError(
                fmt::format("{} has its exception table outside the image", module.name));
        }
        const auto count = static_cast<DWORD>(table.size / sizeof(RUNTIME_FUNCTION));
        if (!RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(base + table.rva), count,
                                 base)) {
            throw kernel::LoadError(
                fmt::format("cannot register the exception table of {}", module.name));
        }
        module.function_table = true;
    }

    // Static TLS. Read from the mapped image, where relocations have made the addresses ours.
    if (const pe::DataDirectory tls = image.directory(pe::Directory::Tls); tls.present()) {
        if (!image.contains(tls.rva, sizeof(IMAGE_TLS_DIRECTORY64))) {
            throw kernel::LoadError(
                fmt::format("{} has its TLS directory outside the image", module.name));
        }
        const auto directory = read_at<IMAGE_TLS_DIRECTORY64>(base + tls.rva);
        const auto inside = [&](uint64_t address, uint64_t size) {
            return address >= base && address - base <= module.mapped.size &&
                   size <= module.mapped.size - (address - base);
        };
        const uint64_t start = directory.StartAddressOfRawData;
        const uint64_t end = directory.EndAddressOfRawData;
        if (end < start || (end != start && !inside(start, end - start)) ||
            !inside(directory.AddressOfIndex, sizeof(ULONG)) ||
            (directory.AddressOfCallBacks != 0 &&
             !inside(directory.AddressOfCallBacks, sizeof(uint64_t)))) {
            throw kernel::LoadError(fmt::format("{} has a malformed TLS directory", module.name));
        }
        TlsInfo info;
        info.raw_data = reinterpret_cast<const uint8_t*>(start);
        info.raw_size = static_cast<size_t>(end - start);
        info.zero_fill = directory.SizeOfZeroFill;
        info.callbacks = directory.AddressOfCallBacks;

        // Bits 20-23 of Characteristics hold the alignment like a section's: 2^(n-1) bytes.
        // Windows allocates TLS blocks from the heap, which aligns to 16 bytes.
        const uint32_t alignment_code = (directory.Characteristics >> 20) & 0xF;
        if (alignment_code > 5) {
            throw kernel::LoadError(fmt::format(
                "{} needs its TLS data aligned to {} bytes; TLS blocks are aligned to 16",
                module.name, uint64_t{1} << (alignment_code - 1)));
        }
        const size_t needed = info.raw_size + info.zero_fill;
        if (needed > ThreadSupport::kDonorCapacity) {
            throw kernel::LoadError(fmt::format(
                "{} needs {:#x} bytes of static TLS, but a TLS donor holds {:#x}; raise "
                "G6_TLS_DONOR_CAPACITY",
                module.name, needed, ThreadSupport::kDonorCapacity));
        }
        if (needed != 0) {
            // A guest executable takes TLS index 0 from the host when the host lends it, since
            // code built with /GA reads slot 0 of the TLS array instead of its index variable
            // (docs/execution-model.md, "TLS and the TEB").
            info.slot = threads.acquire_donor(module.executable);
            info.index = threads.donor_index(info.slot);
            if (module.executable && info.index != 0) {
                LOG_INFO(loader,
                         "{} is an executable with static TLS, but gets TLS index {}: the host "
                         "executable doesn't lend index 0, so code built with /GA would read the "
                         "wrong block",
                         module.name, info.index);
            }
        }
        write_at(directory.AddressOfIndex, static_cast<ULONG>(info.index));
        module.tls = info;
    }

    // Hooks and patches, while the code is still writable and before any of it runs.
    if (const auto it = patches.find(module.key); it != patches.end() && !it->second.empty()) {
        module.patches = apply_patches(module.name, image, module.mapped, it->second);
        if (module.patches->has_traps() && trap_handler == nullptr) {
            // First in line, so it answers before any handler the guest installs.
            trap_handler = AddVectoredExceptionHandler(1, &on_cpu_trap);
            if (trap_handler == nullptr) {
                throw kernel::LoadError("cannot install the handler for trapped cpuid");
            }
        }
    }

    kernel::protect_image(image, module.mapped, kernel::CodeAccess::Execute);
}

void Loader::Impl::publish(const std::vector<Module::State*>& add,
                           const std::vector<Module::State*>& remove) {
    auto list = std::make_unique<ModuleList>();
    if (const ModuleList* old = published()) {
        std::ranges::copy_if(old->modules, std::back_inserter(list->modules),
                             [&](const Module::State* module) {
                                 return std::ranges::find(remove, module) == remove.end();
                             });
    }
    list->modules.insert(list->modules.end(), add.begin(), add.end());
    current.store(list.get(), std::memory_order_release);
    lists.push_back(std::move(list));
    std::erase_if(pending, [&](Module::State* module) {
        return std::ranges::find(add, module) != add.end();
    });
}

void Loader::Impl::fill_tls_on_existing_threads(const std::vector<Module::State*>& batch) {
    std::vector<const TlsInfo*> blocks;
    for (const Module::State* module : batch) {
        if (module->tls.slot >= 0) {
            blocks.push_back(&module->tls);
        }
    }
    if (blocks.empty()) {
        return;
    }
    ThreadSupport::for_each_thread(
        [](G6ThreadRecord* record, void* context) {
            void** const array = tls_array_of(record->teb);
            for (const TlsInfo* tls : *static_cast<std::vector<const TlsInfo*>*>(context)) {
                claim_and_fill(record, array, *tls);
            }
        },
        &blocks);
    // A thread the notifier never saw has no record: one created to skip DLL notifications, or,
    // with the loader in a DLL host, one that started before that DLL loaded. If that is this
    // one, fill its blocks directly; other such threads keep the donor's contents (see
    // docs/execution-model.md).
    if (ThreadSupport::current_thread() == nullptr) {
        void** const array = current_tls_array();
        for (const TlsInfo* tls : blocks) {
            fill_tls_block(array, *tls);
        }
    }
}

void Loader::Impl::initialize(const std::vector<Module::State*>& batch) {
    for (size_t i = 0; i < batch.size(); ++i) {
        Module::State& module = *batch[i];
        call_tls_callbacks(module, DLL_PROCESS_ATTACH);
        if (DllMainFn main = dll_main(module)) {
            if (!main(reinterpret_cast<HINSTANCE>(module.base()), DLL_PROCESS_ATTACH, nullptr)) {
                // As with LoadLibrary: the failed DLL gets a full PROCESS_DETACH, its TLS
                // callbacks first and then its entry point, as on an unload (measured on Windows
                // 11; test_loader compares). So do the modules this load already initialized, in
                // reverse order.
                call_tls_callbacks(module, DLL_PROCESS_DETACH);
                main(reinterpret_cast<HINSTANCE>(module.base()), DLL_PROCESS_DETACH, nullptr);
                for (size_t j = i; j-- > 0;) {
                    Module::State& done = *batch[j];
                    if (!done.attached.exchange(false)) {
                        continue;
                    }
                    call_tls_callbacks(done, DLL_PROCESS_DETACH);
                    if (DllMainFn done_main = dll_main(done)) {
                        done_main(reinterpret_cast<HINSTANCE>(done.base()), DLL_PROCESS_DETACH,
                                  nullptr);
                    }
                }
                throw kernel::LoadError(
                    fmt::format("DllMain of {} failed to initialize it", module.name));
            }
        }
        module.attached.store(true, std::memory_order_release);
    }
}

void Loader::Impl::release(Module::State& module) {
    module.patches.reset();
    if (module.function_table) {
        RtlDeleteFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(
            module.base() + module.image.directory(pe::Directory::Exception).rva));
        module.function_table = false;
    }
    if (module.tls.slot >= 0) {
        threads.release_donor(module.tls.slot);
        module.tls.slot = -1;
    }
    if (module.mapped.base != 0) {
        kernel::unmap_image(module.mapped);
        module.mapped = {};
    }
}

void Loader::Impl::discard(const std::vector<Module::State*>& batch, bool was_published) {
    if (was_published) {
        publish({}, batch);
        // Setting the callback again waits for notifications already running, which may still
        // be looking at the discarded modules.
        ThreadSupport::set_callback(&on_thread_notification, this);
    }
    for (Module::State* module : batch) {
        release(*module);
    }
    std::erase_if(pending, [&](Module::State* module) {
        return std::ranges::find(batch, module) != batch.end();
    });
}

Module::State* Loader::Impl::load(const std::filesystem::path& path, uint64_t base) {
    const std::scoped_lock lock(mutex);

    const std::string name = narrow(path.filename().wstring());
    const std::string key = normalize_dll_name(name);
    if (is_networking_dll(key)) {
        throw kernel::LoadError(
            fmt::format("{} is a networking DLL, which is never loaded (hard rule 5)", name));
    }
    if (imports.is_replaced(key)) {
        throw kernel::LoadError(
            fmt::format("{} is replaced by the runtime's own code and never loaded", name));
    }
    if (Module::State* module = find_any(key)) {
        return module;
    }
    std::filesystem::path file = path;
    if (!path.has_parent_path()) {
        const auto found = find_guest_file(key);
        if (!found) {
            throw kernel::LoadError(fmt::format("cannot find {} in the search paths", name));
        }
        file = *found;
    }

    std::vector<Module::State*> batch;
    try {
        try {
            map_module(file, name, base, batch);
            for (Module::State* module : batch) {
                BindStats stats;
                bind_imports(*module, stats);
                set_up(*module);
                const bool preferred = module->base() == module->image.image_base();
                LOG_INFO(loader,
                         "mapped {} at {:#x}{}: {} from guest modules, {} forwarded, {} HLE, {} "
                         "offline, {} stubbed",
                         module->name, module->base(), preferred ? "" : " (relocated)", stats.guest,
                         stats.forwarded, stats.hle, stats.offline, stats.stubbed);
            }
            stubs.seal();
        } catch (const kernel::LoadError&) {
            throw;
        } catch (const std::runtime_error& error) { // pe::FormatError, and failures to allocate
            throw kernel::LoadError(fmt::format("cannot load {}: {}", name, error.what()));
        }
    } catch (...) {
        // `batch` misses a module whose own mapping threw part-way; `pending` has them all.
        const std::vector<Module::State*> mapped = pending;
        discard(mapped, false);
        throw;
    }

    // From here on, threads see the new modules: TLS first, then initialization.
    publish(batch, {});
    fill_tls_on_existing_threads(batch);
    try {
        initialize(batch);
    } catch (...) {
        discard(batch, true);
        throw;
    }
    return batch.back();
}

uint32_t Loader::Impl::run_entry_point(Module::State& module) {
    if (!module.executable) {
        throw std::invalid_argument(fmt::format("{} is not an executable", module.name));
    }
    const auto entry =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(module.base() + module.image.entry_point());
    auto* peb = reinterpret_cast<void*>(__readgsqword(kTebPeb));
    HANDLE thread = CreateThread(nullptr, static_cast<SIZE_T>(module.image.stack_reserve()), entry,
                                 peb, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) {
        throw std::runtime_error(fmt::format("cannot start the entry point of {} (error {})",
                                             module.name, GetLastError()));
    }
    WaitForSingleObject(thread, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    return code;
}

// ---- Module ----

const std::string& Module::name() const {
    return state_->name;
}

const std::filesystem::path& Module::path() const {
    return state_->path;
}

uint64_t Module::base() const {
    return state_->base();
}

uint64_t Module::size() const {
    return state_->mapped.size;
}

bool Module::is_executable() const {
    return state_->executable;
}

const pe::Image& Module::image() const {
    return state_->image;
}

void* Module::find_export(std::string_view export_name) const {
    return const_cast<void*>(
        state_->loader->resolve_export(*state_, Symbol::by_name(export_name), false).address);
}

void* Module::find_export(uint16_t ordinal) const {
    return const_cast<void*>(
        state_->loader->resolve_export(*state_, Symbol::by_ordinal(ordinal), false).address);
}

// ---- Loader ----

Loader::Loader(LoaderOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Loader::~Loader() = default;

Module Loader::load(const std::filesystem::path& path, uint64_t base) {
    return Module(impl_->load(path, base));
}

uint32_t Loader::run_entry_point(Module executable) {
    return impl_->run_entry_point(*executable.state_);
}

std::optional<Module> Loader::find(std::string_view name) const {
    if (Module::State* module = impl_->find_published(normalize_dll_name(name))) {
        return Module(module);
    }
    return std::nullopt;
}

std::optional<Module> Loader::module_at(uint64_t address) const {
    if (Module::State* module = impl_->containing(address)) {
        return Module(module);
    }
    return std::nullopt;
}

std::vector<Module> Loader::modules() const {
    std::vector<Module> result;
    if (const ModuleList* list = impl_->published()) {
        for (Module::State* module : list->modules) {
            result.push_back(Module(module));
        }
    }
    return result;
}

} // namespace g6::loader
