#pragma once

// How the loader binds each import of a guest module: the policy table from
// docs/execution-model.md ("Import policy").
//
// Rules are keyed by a DLL pattern: a file name ("kernel32.dll") or a prefix ending in '*'
// ("api-ms-win-core-*"). A function rule beats a DLL rule; among rules of one kind an exact
// name beats a pattern and a longer pattern beats a shorter one. A DLL that no rule matches is
// stubbed: deny by default.
//
// Networking DLLs are never forwarded (hard rule 5). A forward rule that names one is rejected
// when it is added, a pattern that happens to match one doesn't apply to it, and a forwarded
// function whose host implementation turns out to live in one is bound offline instead.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace g6::loader {

enum class Binding { Forward, Hle, Offline, Stub };

const char* binding_name(Binding binding);

// What a stub returns, and what an offline function without an implementation returns.
struct StubResult {
    uint64_t value = 0;
    std::optional<uint32_t> last_error;

    // Zero, with the last error set to ERROR_CALL_NOT_IMPLEMENTED: the default for Win32 APIs.
    static StubResult win32();
    // E_NOTIMPL, for HRESULT APIs.
    static StubResult hresult();
};

// An imported symbol: by name, or by ordinal when `name` is empty.
struct Symbol {
    std::string name;
    uint16_t ordinal = 0;

    static Symbol by_name(std::string_view name);
    static Symbol by_ordinal(uint16_t ordinal);
    bool is_ordinal() const { return name.empty(); }
};

struct Resolution {
    Binding binding = Binding::Stub;
    // The function to bind: the host function (Forward), our implementation (Hle, or Offline
    // with an implementation), or nullptr for a stub or an offline function without one.
    const void* address = nullptr;
    StubResult stub;
    // Why the import ended up as a stub or offline, for the log.
    std::string note;
};

class ImportRegistry {
public:
    ImportRegistry();
    ~ImportRegistry();
    ImportRegistry(ImportRegistry&& other) noexcept;
    ImportRegistry& operator=(ImportRegistry&& other) noexcept;
    ImportRegistry(const ImportRegistry&) = delete;
    ImportRegistry& operator=(const ImportRegistry&) = delete;

    // ---- DLL rules ----

    // Binds every function of the matching DLLs to the host function of the same name in
    // `host_dll`, which defaults to the imported DLL itself (that works for API sets too).
    // Ordinal imports aren't forwarded by a DLL rule, because ordinals differ between the
    // console's DLLs and the host's. Throws std::invalid_argument when the pattern or
    // `host_dll` names a networking DLL.
    void forward_dll(std::string_view pattern, std::string_view host_dll = {});
    // Binds the matching DLLs offline: registered implementations as given, every other
    // function to an offline stub that returns `result`.
    void offline_dll(std::string_view pattern, StubResult result = StubResult::win32());
    // Stubs the matching DLLs, returning `result`. Without this rule an unknown DLL's stubs
    // return StubResult::win32(), so COM-style DLLs need StubResult::hresult() here.
    void stub_dll(std::string_view pattern, StubResult result);
    // Marks the matching DLLs as replaced: the loader never maps them, even when the file is in
    // its search paths, because our own code does their job (docs/side-dlls.md). Imports from
    // them, and LoadLibrary on them, go through the other rules like any DLL that isn't a guest
    // module: with no rule they are stubbed, and LoadLibrary fails. Doesn't count for knows().
    void replace_dll(std::string_view pattern);

    // ---- Function rules ----

    void hle(std::string_view pattern, const Symbol& symbol, const void* function);
    void offline(std::string_view pattern, const Symbol& symbol, const void* function);
    void stub(std::string_view pattern, const Symbol& symbol, StubResult result);
    // Forwards one function, optionally under another name or from another host DLL. Throws
    // std::invalid_argument for a networking DLL, like forward_dll.
    void forward(std::string_view pattern, const Symbol& symbol, std::string_view host_dll = {},
                 std::string_view host_symbol = {});

    // ---- Queries ----

    // Whether any rule matches the DLL.
    bool knows(std::string_view dll) const;
    // The binding of the DLL rule that matches (Stub when none does, Offline for networking).
    Binding dll_binding(std::string_view dll) const;
    // Whether the host can load the DLL a forward rule sends `dll` to.
    bool host_provides(std::string_view dll);
    // Whether a replace_dll rule matches the DLL.
    bool is_replaced(std::string_view dll) const;

    // Resolves one import. Forwarding loads the host DLL on first use; host DLLs stay loaded
    // until the registry is destroyed. Safe to call from several threads.
    Resolution resolve(std::string_view dll, const Symbol& symbol);

private:
    struct DllRule {
        Binding binding = Binding::Stub;
        std::string host_dll;
        StubResult result;
    };
    struct FunctionRule {
        Binding binding = Binding::Stub;
        const void* function = nullptr;
        std::string host_dll;
        std::string host_symbol;
        StubResult result;
    };
    struct HostModules;

    const DllRule* find_dll_rule(std::string_view dll) const;
    const FunctionRule* find_function_rule(std::string_view dll, const Symbol& symbol) const;
    void add_function(std::string_view pattern, const Symbol& symbol, FunctionRule rule);
    Resolution forward_to_host(std::string_view dll, std::string_view host_dll,
                               const Symbol& symbol, StubResult result);

    // Key: normalized pattern.
    std::map<std::string, DllRule, std::less<>> dll_rules_;
    // Key: symbol key ("name" or "#ordinal"), then normalized pattern.
    std::map<std::string, std::map<std::string, FunctionRule, std::less<>>, std::less<>>
        function_rules_;
    // Normalized patterns of replaced DLLs.
    std::set<std::string, std::less<>> replaced_;
    std::unique_ptr<HostModules> host_;
};

// The key for DLL names: lowercase file name, with ".dll" added when there is no extension.
std::string normalize_dll_name(std::string_view name);

// Whether the DLL is a networking DLL, which is never forwarded and never mapped.
bool is_networking_dll(std::string_view dll);

// How the log names an import: the symbol name, or <dll stem>_ord<N> for an ordinal import.
std::string symbol_display_name(std::string_view dll, const Symbol& symbol);

} // namespace g6::loader
