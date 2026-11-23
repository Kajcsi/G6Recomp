#include "g6/loader/import_registry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <windows.h>

#include <fmt/format.h>

#include "text.h"

namespace g6::loader {
namespace {

// DLLs whose job is networking or online services, by file name. Hard rule 5: none of them is
// ever forwarded to the host or mapped from the game's files. XCurl and libHttpClient are the
// game's own networking libraries. gameplatformservices.dll is the PC's Gaming Services
// runtime, which XCurl can load itself to reach the Game Runtime (docs/side-dlls.md).
constexpr std::array kNetworkingDlls = {
    "cryptnet.dll",
    "dnsapi.dll",
    "fwpuclnt.dll",
    "gameplatformservices.dll",
    "httpapi.dll",
    "iphlpapi.dll",
    "libhttpclient.gdk.dll",
    "mpr.dll",
    "mswsock.dll",
    "netapi32.dll",
    "nsi.dll",
    "rasapi32.dll",
    "urlmon.dll",
    "webio.dll",
    "websocket.dll",
    "winhttp.dll",
    "wininet.dll",
    "winnsi.dll",
    "wlanapi.dll",
    "ws2_32.dll",
    "wsock32.dll",
    "xcurl.dll",
};

// API set families that resolve to networking DLLs.
constexpr std::array kNetworkingPrefixes = {
    "api-ms-win-net-",
    "ext-ms-win-net-",
    "ext-ms-win-networking-",
    "ext-ms-win-winhttp-",
};

constexpr uint32_t kHresultNotImplemented = 0x80004001; // E_NOTIMPL

std::string normalize_pattern(std::string_view pattern) {
    if (!pattern.empty() && pattern.back() == '*') {
        return ascii_lower(pattern);
    }
    return normalize_dll_name(pattern);
}

// How well `pattern` matches `dll`: negative for no match, larger for a more specific match.
int64_t match_score(std::string_view pattern, std::string_view dll) {
    if (!pattern.empty() && pattern.back() == '*') {
        const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
        return dll.starts_with(prefix) ? static_cast<int64_t>(prefix.size()) : -1;
    }
    return pattern == dll ? std::numeric_limits<int64_t>::max() : -1;
}

std::string symbol_key(const Symbol& symbol) {
    return symbol.is_ordinal() ? fmt::format("#{}", symbol.ordinal) : symbol.name;
}

void reject_networking(std::string_view what, std::string_view name) {
    std::string_view stem = name;
    if (!stem.empty() && stem.back() == '*') {
        stem.remove_suffix(1);
    }
    if (!stem.empty() && is_networking_dll(stem)) {
        throw std::invalid_argument(fmt::format(
            "{} '{}' names a networking DLL, which is never forwarded (hard rule 5)", what, name));
    }
}

// File name of the host module that contains `address`, or "" when none does.
std::string host_module_of(const void* address) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) == 0) {
        return {};
    }
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return normalize_dll_name(narrow(path));
}

} // namespace

const char* binding_name(Binding binding) {
    switch (binding) {
    case Binding::Forward: return "forwarded";
    case Binding::Hle: return "HLE";
    case Binding::Offline: return "offline";
    case Binding::Stub: return "stubbed";
    }
    return "?";
}

StubResult StubResult::win32() {
    return StubResult{.value = 0, .last_error = ERROR_CALL_NOT_IMPLEMENTED};
}

StubResult StubResult::hresult() {
    return StubResult{.value = kHresultNotImplemented, .last_error = std::nullopt};
}

Symbol Symbol::by_name(std::string_view name) {
    return Symbol{.name = std::string(name), .ordinal = 0};
}

Symbol Symbol::by_ordinal(uint16_t ordinal) {
    return Symbol{.name = {}, .ordinal = ordinal};
}

std::string normalize_dll_name(std::string_view name) {
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::string result = ascii_lower(name);
    if (!result.empty() && result.back() == '.') {
        // A trailing dot means "no extension" to LoadLibrary.
        result.pop_back();
    } else if (result.find('.') == std::string::npos) {
        result += ".dll";
    }
    return result;
}

bool is_networking_dll(std::string_view dll) {
    const std::string name = normalize_dll_name(dll);
    for (const char* networking : kNetworkingDlls) {
        if (name == networking) {
            return true;
        }
    }
    for (const char* prefix : kNetworkingPrefixes) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

std::string symbol_display_name(std::string_view dll, const Symbol& symbol) {
    if (!symbol.is_ordinal()) {
        return symbol.name;
    }
    std::string stem = normalize_dll_name(dll);
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) {
        stem.resize(dot);
    }
    return fmt::format("{}_ord{}", stem, symbol.ordinal);
}

// ---- Host modules ----

struct ImportRegistry::HostModules {
    std::mutex mutex;
    std::map<std::string, HMODULE, std::less<>> modules;

    HostModules() = default;
    HostModules(const HostModules&) = delete;
    HostModules& operator=(const HostModules&) = delete;
    ~HostModules() {
        for (auto& [name, module] : modules) {
            if (module != nullptr) {
                FreeLibrary(module);
            }
        }
    }

    // Loads a host DLL, caching failures too. LoadLibrary runs without the mutex held: the
    // Windows loader lock may be held by a thread that is waiting for this mutex.
    HMODULE load(const std::string& name) {
        {
            const std::scoped_lock lock(mutex);
            if (const auto it = modules.find(name); it != modules.end()) {
                return it->second;
            }
        }
        HMODULE module =
            LoadLibraryExW(widen(name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const std::scoped_lock lock(mutex);
        const auto [it, inserted] = modules.emplace(name, module);
        if (!inserted && module != nullptr) {
            FreeLibrary(module);
        }
        return it->second;
    }
};

// ---- Registry ----

ImportRegistry::ImportRegistry() : host_(std::make_unique<HostModules>()) {}
ImportRegistry::~ImportRegistry() = default;
// Moving a std::map allocates a sentinel node in the Microsoft STL; running out of memory while
// moving the registry ends the process, which is fine at startup.
// NOLINTNEXTLINE(bugprone-exception-escape)
ImportRegistry::ImportRegistry(ImportRegistry&& other) noexcept = default;
ImportRegistry& ImportRegistry::operator=(ImportRegistry&& other) noexcept = default;

void ImportRegistry::forward_dll(std::string_view pattern, std::string_view host_dll) {
    reject_networking("forward rule", pattern);
    if (!host_dll.empty()) {
        reject_networking("forward target", host_dll);
    }
    dll_rules_[normalize_pattern(pattern)] =
        DllRule{.binding = Binding::Forward,
                .host_dll = host_dll.empty() ? std::string() : normalize_dll_name(host_dll),
                .result = StubResult::win32()};
}

void ImportRegistry::offline_dll(std::string_view pattern, StubResult result) {
    dll_rules_[normalize_pattern(pattern)] =
        DllRule{.binding = Binding::Offline, .host_dll = {}, .result = result};
}

void ImportRegistry::stub_dll(std::string_view pattern, StubResult result) {
    dll_rules_[normalize_pattern(pattern)] =
        DllRule{.binding = Binding::Stub, .host_dll = {}, .result = result};
}

void ImportRegistry::replace_dll(std::string_view pattern) {
    replaced_.insert(normalize_pattern(pattern));
}

void ImportRegistry::add_function(std::string_view pattern, const Symbol& symbol,
                                  FunctionRule rule) {
    function_rules_[symbol_key(symbol)][normalize_pattern(pattern)] = std::move(rule);
}

void ImportRegistry::hle(std::string_view pattern, const Symbol& symbol, const void* function) {
    if (function == nullptr) {
        throw std::invalid_argument("an HLE rule needs a function");
    }
    add_function(pattern, symbol, FunctionRule{.binding = Binding::Hle, .function = function});
}

void ImportRegistry::offline(std::string_view pattern, const Symbol& symbol, const void* function) {
    if (function == nullptr) {
        throw std::invalid_argument("an offline rule needs a function");
    }
    add_function(pattern, symbol, FunctionRule{.binding = Binding::Offline, .function = function});
}

void ImportRegistry::stub(std::string_view pattern, const Symbol& symbol, StubResult result) {
    add_function(pattern, symbol, FunctionRule{.binding = Binding::Stub, .result = result});
}

void ImportRegistry::forward(std::string_view pattern, const Symbol& symbol,
                             std::string_view host_dll, std::string_view host_symbol) {
    reject_networking("forward rule", pattern);
    if (!host_dll.empty()) {
        reject_networking("forward target", host_dll);
    }
    add_function(
        pattern, symbol,
        FunctionRule{.binding = Binding::Forward,
                     .host_dll = host_dll.empty() ? std::string() : normalize_dll_name(host_dll),
                     .host_symbol = std::string(host_symbol)});
}

const ImportRegistry::DllRule* ImportRegistry::find_dll_rule(std::string_view dll) const {
    const DllRule* best = nullptr;
    int64_t best_score = -1;
    for (const auto& [pattern, rule] : dll_rules_) {
        const int64_t score = match_score(pattern, dll);
        if (score > best_score) {
            best = &rule;
            best_score = score;
        }
    }
    return best;
}

const ImportRegistry::FunctionRule* ImportRegistry::find_function_rule(std::string_view dll,
                                                                       const Symbol& symbol) const {
    const auto rules = function_rules_.find(symbol_key(symbol));
    if (rules == function_rules_.end()) {
        return nullptr;
    }
    const FunctionRule* best = nullptr;
    int64_t best_score = -1;
    for (const auto& [pattern, rule] : rules->second) {
        const int64_t score = match_score(pattern, dll);
        if (score > best_score) {
            best = &rule;
            best_score = score;
        }
    }
    return best;
}

bool ImportRegistry::knows(std::string_view dll_name) const {
    const std::string dll = normalize_dll_name(dll_name);
    if (is_networking_dll(dll) || find_dll_rule(dll) != nullptr) {
        return true;
    }
    for (const auto& [key, rules] : function_rules_) {
        for (const auto& [pattern, rule] : rules) {
            if (match_score(pattern, dll) >= 0) {
                return true;
            }
        }
    }
    return false;
}

Binding ImportRegistry::dll_binding(std::string_view dll_name) const {
    const std::string dll = normalize_dll_name(dll_name);
    if (is_networking_dll(dll)) {
        return Binding::Offline;
    }
    const DllRule* rule = find_dll_rule(dll);
    return rule != nullptr ? rule->binding : Binding::Stub;
}

bool ImportRegistry::host_provides(std::string_view dll_name) {
    const std::string dll = normalize_dll_name(dll_name);
    const DllRule* rule = find_dll_rule(dll);
    if (is_networking_dll(dll) || rule == nullptr || rule->binding != Binding::Forward) {
        return false;
    }
    return host_->load(rule->host_dll.empty() ? dll : rule->host_dll) != nullptr;
}

bool ImportRegistry::is_replaced(std::string_view dll_name) const {
    const std::string dll = normalize_dll_name(dll_name);
    return std::ranges::any_of(
        replaced_, [&](const std::string& pattern) { return match_score(pattern, dll) >= 0; });
}

Resolution ImportRegistry::forward_to_host(std::string_view dll, std::string_view host_dll,
                                           const Symbol& symbol, StubResult result) {
    const std::string host = host_dll.empty() ? std::string(dll) : std::string(host_dll);
    if (is_networking_dll(host)) {
        return Resolution{
            .binding = Binding::Offline, .stub = result, .note = "networking is never forwarded"};
    }
    HMODULE module = host_->load(host);
    if (module == nullptr) {
        return Resolution{.binding = Binding::Stub,
                          .stub = result,
                          .note = fmt::format("the host has no {}", host)};
    }
    const void* function = reinterpret_cast<const void*>(GetProcAddress(
        module, symbol.is_ordinal()
                    ? MAKEINTRESOURCEA(symbol.ordinal) // NOLINT(performance-no-int-to-ptr)
                    : symbol.name.c_str()));
    if (function == nullptr) {
        return Resolution{.binding = Binding::Stub,
                          .stub = result,
                          .note = fmt::format("the host's {} has no {}", host,
                                              symbol_display_name(host, symbol))};
    }
    // An API set or a forwarder can lead somewhere the DLL's name doesn't say.
    const std::string owner = host_module_of(function);
    if (is_networking_dll(owner)) {
        return Resolution{.binding = Binding::Offline,
                          .stub = result,
                          .note = fmt::format("the host implementation is in {}", owner)};
    }
    return Resolution{.binding = Binding::Forward, .address = function};
}

Resolution ImportRegistry::resolve(std::string_view dll_name, const Symbol& symbol) {
    const std::string dll = normalize_dll_name(dll_name);
    const bool networking = is_networking_dll(dll);
    const DllRule* dll_rule = find_dll_rule(dll);
    const StubResult dll_result = dll_rule != nullptr && dll_rule->binding != Binding::Forward
                                      ? dll_rule->result
                                      : StubResult::win32();

    if (const FunctionRule* rule = find_function_rule(dll, symbol)) {
        switch (rule->binding) {
        case Binding::Hle:
            return Resolution{.binding = networking ? Binding::Offline : Binding::Hle,
                              .address = rule->function};
        case Binding::Offline:
            return Resolution{.binding = Binding::Offline, .address = rule->function};
        case Binding::Stub:
            return Resolution{.binding = Binding::Stub,
                              .stub = rule->result,
                              .note = "stubbed by a function rule"};
        case Binding::Forward:
            if (networking) {
                return Resolution{.binding = Binding::Offline,
                                  .stub = dll_result,
                                  .note = "networking is never forwarded"};
            }
            return forward_to_host(dll, rule->host_dll,
                                   rule->host_symbol.empty() ? symbol
                                                             : Symbol::by_name(rule->host_symbol),
                                   dll_result);
        }
    }

    if (networking) {
        return Resolution{
            .binding = Binding::Offline, .stub = dll_result, .note = "networking DLL"};
    }
    if (dll_rule == nullptr) {
        return Resolution{.binding = Binding::Stub,
                          .stub = StubResult::win32(),
                          .note = is_replaced(dll) ? "replaced DLL without an implementation yet"
                                                   : "no rule for this DLL"};
    }
    switch (dll_rule->binding) {
    case Binding::Forward:
        if (symbol.is_ordinal()) {
            return Resolution{.binding = Binding::Stub,
                              .stub = StubResult::win32(),
                              .note = "ordinal imports aren't forwarded"};
        }
        return forward_to_host(dll, dll_rule->host_dll, symbol, StubResult::win32());
    case Binding::Offline:
        return Resolution{.binding = Binding::Offline, .stub = dll_result, .note = "offline DLL"};
    case Binding::Hle:
    case Binding::Stub: break;
    }
    return Resolution{.binding = Binding::Stub, .stub = dll_result, .note = "stubbed DLL"};
}

} // namespace g6::loader
