#include "g6/cpu/dispatch.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "g6/log.h"

namespace g6::cpu {
namespace {

// Indirect-call targets are 16-byte aligned under CFG, so most lookups index a dense array;
// the rest (jump targets, hand-written code) go through a hash map.
constexpr uint64_t kSlotShift = 4;

// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's unordered_map move constructor allocates.
struct Module {
    std::string name;
    uint64_t base = 0;
    uint64_t size = 0;
    std::vector<HostFunction> aligned;
    std::unordered_map<uint64_t, HostFunction> unaligned;
};

std::vector<Module>& modules() {
    static std::vector<Module> list;
    return list;
}

const char* const kGprNames[kGprCount] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

} // namespace

void register_module(const ModuleFunctions& table) {
    Module module{.name = table.name,
                  .base = table.base,
                  .size = table.size,
                  .aligned = std::vector<HostFunction>((table.size >> kSlotShift) + 1),
                  .unaligned = {}};
    for (size_t i = 0; i < table.count; ++i) {
        const FunctionEntry& entry = table.entries[i];
        const uint64_t offset = entry.address - table.base;
        if (offset >= table.size) {
            LOG_ERROR(cpu, "{}: function {:#x} lies outside the module", module.name,
                      entry.address);
            continue;
        }
        if ((offset & ((1u << kSlotShift) - 1)) == 0) {
            module.aligned[offset >> kSlotShift] = entry.function;
        } else {
            module.unaligned.emplace(entry.address, entry.function);
        }
    }
    LOG_DEBUG(cpu, "{}: {} functions at {:#x}", module.name, table.count, table.base);
    modules().push_back(std::move(module));
}

void unregister_modules() {
    modules().clear();
}

HostFunction find_function(uint64_t address) {
    for (const Module& module : modules()) {
        const uint64_t offset = address - module.base;
        if (offset >= module.size) {
            continue;
        }
        if ((offset & ((1u << kSlotShift) - 1)) == 0) {
            return module.aligned[offset >> kSlotShift];
        }
        const auto it = module.unaligned.find(address);
        return it != module.unaligned.end() ? it->second : nullptr;
    }
    return nullptr;
}

HostFunction resolve(GuestContext& ctx, uint64_t address, uint64_t from) {
    if (const HostFunction function = find_function(address)) {
        return function;
    }
    fault(ctx, from, fmt::format("transfer to {:#x}, which is not a known function", address));
}

void invoke(GuestContext& ctx, uint64_t address) {
    ctx.gpr[kRsp] -= 8;
    const uint64_t return_address = 0;
    std::memcpy(reinterpret_cast<void*>(ctx.gpr[kRsp]), &return_address, sizeof(return_address));
    resolve(ctx, address, 0)(ctx);
}

void fault(GuestContext& ctx, uint64_t address, std::string_view what) {
    LOG_ERROR(cpu, "guest fault at {:#x}: {}", address, what);
    // Generated code writes registers back before it can fault, but only the ones the faulting
    // function uses are current; the rest still hold their values from its caller.
    for (int i = 0; i < kGprCount; i += 4) {
        LOG_ERROR(cpu, "  {:>3} {:#018x}  {:>3} {:#018x}  {:>3} {:#018x}  {:>3} {:#018x}",
                  kGprNames[i], ctx.gpr[i], kGprNames[i + 1], ctx.gpr[i + 1], kGprNames[i + 2],
                  ctx.gpr[i + 2], kGprNames[i + 3], ctx.gpr[i + 3]);
    }
    std::abort();
}

void unimplemented(GuestContext& ctx, uint64_t address, const char* mnemonic) {
    fault(ctx, address, fmt::format("instruction not implemented yet: {}", mnemonic));
}

} // namespace g6::cpu
