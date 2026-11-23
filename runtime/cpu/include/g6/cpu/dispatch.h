#pragma once

// Dispatch from guest addresses to recompiled functions, and the stop path for guest code that
// can't continue. Every generated module registers its function table at startup; indirect
// calls and jumps resolve their target here. An unknown target is a fault: nothing interprets
// guest code and the original code is never executed.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "g6/cpu/context.h"

namespace g6::cpu {

using HostFunction = void (*)(GuestContext&);

struct FunctionEntry {
    uint64_t address = 0; // guest address at the module's assigned base
    HostFunction function = nullptr;
};

// One generated module's table, emitted as `g6_module_<name>` in its table.cpp.
struct ModuleFunctions {
    const char* name = nullptr;
    uint64_t base = 0; // assigned load address
    uint64_t size = 0; // size of the mapped image
    const FunctionEntry* entries = nullptr;
    size_t count = 0;
};

// Registration happens before any guest code runs; lookups don't lock.
void register_module(const ModuleFunctions& module);
void unregister_modules();

// The recompiled function starting at `address`, or nullptr.
HostFunction find_function(uint64_t address);

// Like find_function, but a missing function is a fault reported against the guest instruction
// at `from` (0 when the host is the caller).
HostFunction resolve(GuestContext& ctx, uint64_t address, uint64_t from);

// Runs the guest function at `address` from the host. The caller sets up the arguments and a
// valid guest stack in `ctx`; this pushes a return address of 0 the way a guest `call` would,
// so the callee sees a normal frame, and returns when the guest function does.
void invoke(GuestContext& ctx, uint64_t address);

// Guest code that can't continue (a trap, a fault, a transfer to an unknown address, an
// instruction the recompiler doesn't implement yet) ends here: the address and registers are
// logged and the process stops. Guest exceptions get their own model later; see docs/codegen.md.
[[noreturn]] void fault(GuestContext& ctx, uint64_t address, std::string_view what);
[[noreturn]] void unimplemented(GuestContext& ctx, uint64_t address, const char* mnemonic);

} // namespace g6::cpu
