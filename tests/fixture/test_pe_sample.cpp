// PE readers and function discovery against pe_sample.dll, an original fixture built by this
// project. The DLL is also loaded with LoadLibrary so the tests can compare what the readers
// report against what the Windows loader and the fixture's own functions say.

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <windows.h>

#include "function_discovery.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6/pe/unwind.h"
#include "g6test.h"

namespace pe = g6::pe;
namespace recomp = g6::recomp;

namespace {

struct Loaded {
    pe::Image image;
    HMODULE module;

    uint32_t rva_of(const void* address) const {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address) -
                                     reinterpret_cast<uintptr_t>(module));
    }
    uint32_t export_rva(const char* name) const {
        auto* const address = GetProcAddress(module, name);
        if (address == nullptr) {
            throw std::runtime_error(std::string("missing export ") + name);
        }
        return rva_of(reinterpret_cast<const void*>(address));
    }
    template <typename Result>
    Result call(const char* name) const {
        using Function = Result (*)();
        return reinterpret_cast<Function>(GetProcAddress(module, name))();
    }
};

const Loaded& sample() {
    static const Loaded loaded{pe::Image::load_file(G6_PE_SAMPLE_PATH),
                               LoadLibraryA(G6_PE_SAMPLE_PATH)};
    if (loaded.module == nullptr) {
        throw std::runtime_error("LoadLibrary failed for " G6_PE_SAMPLE_PATH);
    }
    return loaded;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

const pe::ImportedModule* find_module(const std::vector<pe::ImportedModule>& modules,
                                      const std::string& dll) {
    for (const pe::ImportedModule& module : modules) {
        if (iequals(module.dll, dll)) {
            return &module;
        }
    }
    return nullptr;
}

bool has_symbol(const pe::ImportedModule& module, const std::string& name) {
    return std::ranges::any_of(
        module.symbols, [&](const pe::ImportedSymbol& symbol) { return symbol.name == name; });
}

} // namespace

TEST_CASE("fixture: headers match the loaded module") {
    const Loaded& s = sample();
    CHECK(s.image.is_dll());
    CHECK_EQ(s.image.machine(), pe::kMachineAmd64);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(s.module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(s.module) + dos->e_lfanew);
    CHECK_EQ(s.image.size_of_image(), uint32_t{nt->OptionalHeader.SizeOfImage});
    CHECK_EQ(s.image.entry_point(), uint32_t{nt->OptionalHeader.AddressOfEntryPoint});
    // The loader rewrites ImageBase in the mapped header when it relocates the module, so the
    // file's preferred base can only be checked for plausibility here.
    CHECK(s.image.image_base() != 0);
    CHECK_EQ(s.image.image_base() & 0xFFFF, uint64_t{0});
}

TEST_CASE("fixture: the SEH-guarded call catches an access violation") {
    using Guarded = int (*)(volatile int*);
    const auto guarded = reinterpret_cast<Guarded>(GetProcAddress(sample().module, "fixture_seh"));
    REQUIRE(guarded != nullptr);
    volatile int value = 42;
    CHECK_EQ(guarded(&value), 42);
    CHECK_EQ(guarded(nullptr), -1);
}

TEST_CASE("fixture: exports match GetProcAddress") {
    const Loaded& s = sample();
    const pe::ExportTable table = pe::read_exports(s.image);
    CHECK(iequals(table.dll_name, "pe_sample.dll"));
    size_t checked = 0;
    for (const pe::Export& entry : table.exports) {
        REQUIRE(!entry.name.empty());
        CHECK(entry.forwarder.empty());
        CHECK_EQ(entry.rva, s.export_rva(entry.name.c_str()));
        ++checked;
    }
    CHECK(checked >= 10);
}

TEST_CASE("fixture: regular and delay-load imports") {
    const Loaded& s = sample();
    const std::vector<pe::ImportedModule> modules = pe::read_imports(s.image);
    const pe::ImportedModule* kernel32 = find_module(modules, "KERNEL32.dll");
    REQUIRE(kernel32 != nullptr);
    CHECK(!kernel32->delay_load);
    CHECK(has_symbol(*kernel32, "GetTickCount64"));
    const pe::ImportedModule* user32 = find_module(modules, "USER32.dll");
    REQUIRE(user32 != nullptr);
    CHECK(user32->delay_load);
    CHECK(has_symbol(*user32, "GetSystemMetrics"));
    for (const pe::ImportedModule& module : modules) {
        for (const pe::ImportedSymbol& symbol : module.symbols) {
            CHECK(s.image.contains(symbol.iat_rva, 8));
        }
    }
    // The delay-load call still works through the real loader.
    CHECK(s.call<int>("fixture_metrics") >= 0);
}

TEST_CASE("fixture: TLS directory lists the callback") {
    const Loaded& s = sample();
    const auto tls = pe::read_tls(s.image);
    REQUIRE(tls.has_value());
    const uint32_t callback = s.rva_of(s.call<void*>("fixture_tls_callback_address"));
    CHECK(std::find(tls->callback_rvas.begin(), tls->callback_rvas.end(), callback) !=
          tls->callback_rvas.end());
    CHECK_EQ(s.call<int>("fixture_tls_read"), 7);
}

TEST_CASE("fixture: CFG table holds address-taken functions") {
    const Loaded& s = sample();
    const auto config = pe::read_load_config(s.image);
    REQUIRE(config.has_value());
    CHECK((config->guard_flags & 0x100) != 0); // IMAGE_GUARD_CF_INSTRUMENTED
    const uint32_t leaf = s.rva_of(s.call<void*>("fixture_asm_leaf_address"));
    const auto& targets = config->guard_cf_functions;
    CHECK(std::ranges::is_sorted(targets));
    CHECK(std::ranges::binary_search(targets, leaf));
    CHECK(config->security_cookie_va != 0);
}

TEST_CASE("fixture: relocations cover the code-pointer table") {
    const Loaded& s = sample();
    const uint32_t table = s.export_rva("fixture_table");
    const auto relocations = pe::read_relocations(s.image);
    const auto relocated = [&](uint32_t rva) {
        return std::ranges::any_of(relocations, [&](const pe::Relocation& r) {
            return r.rva == rva && r.type == pe::kRelocDir64;
        });
    };
    CHECK(relocated(table));
    CHECK(relocated(table + 8));
    const uint32_t after = s.rva_of(s.call<void*>("fixture_asm_after_address"));
    CHECK_EQ(s.image.va_to_rva(s.image.read<uint64_t>(table + 8)), after);
}

TEST_CASE("fixture: .pdata covers C functions but not the assembly leaves") {
    const Loaded& s = sample();
    const auto entries = pe::read_runtime_functions(s.image);
    const pe::RuntimeFunction* seh =
        pe::find_runtime_function(entries, s.export_rva("fixture_seh"));
    REQUIRE(seh != nullptr);
    const pe::RuntimeFunction primary = pe::primary_function(s.image, *seh);
    CHECK(pe::read_unwind_info(s.image, primary.unwind_info).has_handler());
    CHECK(pe::find_runtime_function(entries, s.export_rva("fixture_calls_leaf")) != nullptr);
    const uint32_t leaf = s.rva_of(s.call<void*>("fixture_asm_leaf_address"));
    CHECK(pe::find_runtime_function(entries, leaf) == nullptr);
    for (const pe::RuntimeFunction& entry : entries) {
        if (!entry.indirect()) {
            (void)pe::read_unwind_info(s.image, entry.unwind_info); // must not throw
        }
    }
}

TEST_CASE("fixture: discovery finds every exported function and the assembly leaves") {
    const Loaded& s = sample();
    const recomp::DiscoveryResult result = recomp::discover_functions(s.image);
    CHECK_EQ(result.stats.unwind_errors, size_t{0});
    CHECK_EQ(result.stats.directory_errors, size_t{0});

    for (const pe::Export& entry : pe::read_exports(s.image).exports) {
        if (s.image.is_executable(entry.rva)) {
            const recomp::DiscoveredFunction* function = result.find(entry.rva);
            CHECK(function != nullptr);
            if (function != nullptr) {
                CHECK((function->sources & recomp::kSourceExport) != 0);
            }
        }
    }
    CHECK(result.find(s.image.entry_point()) != nullptr);

    const uint32_t leaf = s.rva_of(s.call<void*>("fixture_asm_leaf_address"));
    const recomp::DiscoveredFunction* leaf_function = result.find(leaf);
    REQUIRE(leaf_function != nullptr);
    CHECK(!leaf_function->has_unwind_info);
    CHECK((leaf_function->sources & recomp::kSourceDirectCall) != 0);
    CHECK_EQ(leaf_function->end, leaf + 4); // lea + ret

    const uint32_t fall = s.export_rva("fixture_asm_fall");
    const uint32_t after = s.rva_of(s.call<void*>("fixture_asm_after_address"));
    const recomp::DiscoveredFunction* fall_function = result.find(fall);
    const recomp::DiscoveredFunction* after_function = result.find(after);
    REQUIRE(fall_function != nullptr);
    REQUIRE(after_function != nullptr);
    CHECK_EQ(fall_function->end, after);
    CHECK((after_function->sources & recomp::kSourceRelocation) != 0);
    CHECK(result.stats.leaf_splits >= 1);

    const uint32_t tls_callback = s.rva_of(s.call<void*>("fixture_tls_callback_address"));
    const recomp::DiscoveredFunction* callback = result.find(tls_callback);
    REQUIRE(callback != nullptr);
    CHECK((callback->sources & recomp::kSourceTlsCallback) != 0);
}

TEST_CASE("fixture: switch jump tables resolve to the code the switch really runs") {
    const Loaded& s = sample();
    const recomp::DiscoveryResult result = recomp::discover_functions(s.image);
    const auto check_tables = [&](const char* name, size_t min_cases) {
        const recomp::DiscoveredFunction* function = result.find(s.export_rva(name));
        REQUIRE(function != nullptr);
        REQUIRE(function->jump_tables.size() == 1);
        const recomp::JumpTable& table = function->jump_tables[0];
        CHECK(table.bounded);
        CHECK(table.targets.size() >= min_cases);
        for (const uint32_t target : table.targets) {
            CHECK(function->contains(target));
        }
    };
    // The assembly dispatch in MSVC's image-relative form: exactly its four cases.
    check_tables("fixture_asm_switch", 4);
    const recomp::DiscoveredFunction* asm_switch = result.find(s.export_rva("fixture_asm_switch"));
    REQUIRE(asm_switch != nullptr);
    CHECK_EQ(asm_switch->jump_tables[0].targets.size(), size_t{4});

    // The compiler-generated table; its exact shape is the compiler's choice.
    check_tables("fixture_switch", 8);

    // Both still behave as written when run natively.
    using AsmSwitch = int (*)(unsigned);
    const auto run_asm =
        reinterpret_cast<AsmSwitch>(GetProcAddress(s.module, "fixture_asm_switch"));
    REQUIRE(run_asm != nullptr);
    CHECK_EQ(run_asm(0), 10);
    CHECK_EQ(run_asm(3), 40);
    CHECK_EQ(run_asm(4), -1);
    using CSwitch = int (*)(int, int);
    const auto run_c = reinterpret_cast<CSwitch>(GetProcAddress(s.module, "fixture_switch"));
    REQUIRE(run_c != nullptr);
    CHECK_EQ(run_c(1, 5), 16);
    CHECK_EQ(run_c(6, 0), -1);
    CHECK_EQ(run_c(9, 5), 0);
}
