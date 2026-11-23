#pragma once

// Patches the loader applies to a guest module while mapping it, before any of its code runs
// (docs/hooks.md). Four kinds:
//
// * A replacement sends calls of a guest function to a host function with the same signature.
//   The host function can call the original through a trampoline.
// * A mid-function hook runs a host function at an address inside a guest function, with the
//   guest's registers in a HookContext it may read and change, then resumes.
// * A data patch writes bytes.
// * Trapping cpuid and xgetbv makes every such instruction in the module's code answer with
//   what g6::cpu::guest_cpuid and guest_xgetbv report.
//
// A code patch overwrites whole instructions with a 5-byte jump (its "window"). The loader
// refuses a patch whose window breaks a rule in docs/hooks.md, and then the module fails to
// load with a message naming the rule.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace g6::loader {

// A full 256-bit vector register.
struct alignas(32) Ymm {
    std::array<uint64_t, 4> lanes{};
};

// The guest's registers at a mid-function hook. The general registers are in encoding order.
struct alignas(32) HookContext {
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbx;
    uint64_t rsp; // the guest's stack pointer at the hook site
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rflags;
    // The hook site. Leave it to run the instructions the patch covers and carry on; change it
    // to resume elsewhere. rsp may be raised (to return from the function early, for example)
    // but never lowered: the memory below it is in use while the hook runs.
    uint64_t rip;
    std::array<Ymm, 16> ymm;
};

using MidHook = void (*)(HookContext& context);

// Checks the bytes a patch expects at its address in the module's file, before relocation:
// FNV-1a over `size` bytes starting at the patch's address. A mismatch fails the load. It
// catches a wrong address, or a patch written for another build, without the bytes themselves
// appearing in the source (hard rule 3).
struct ExpectedBytes {
    uint32_t size = 0;
    uint64_t hash = 0;
};

// The 64-bit FNV-1a hash ExpectedBytes uses.
uint64_t hash_bytes(std::span<const uint8_t> bytes);

struct Replacement {
    uint32_t rva = 0; // the guest function's entry
    const void* function = nullptr;
    // When set, receives the address of a trampoline that runs the original function. It is set
    // when the module loads, and reset to null if the load fails.
    void** original = nullptr;
    std::optional<ExpectedBytes> expect;
};

struct MidFunctionHook {
    uint32_t rva = 0;
    MidHook hook = nullptr;
    std::optional<ExpectedBytes> expect;
    // The function's exception handler never sees an exception raised while the hook or the
    // moved instructions run, so a mid-function hook is refused where that handler has work to
    // do (docs/hooks.md, rule 4). Under a C++ handler the engine reads the tables and refuses
    // windows in a try block or where objects are live. A handler that only checks the stack
    // cookie has nothing to do, so it never stops a hook. The engine refuses every site in a catch
    // or cleanup funclet, in a function with a catch it can't find the continuation of, or under
    // a handler it doesn't know. Set this when such a site was checked by hand: the hook can't
    // throw and the moved instructions can't fault, or the handler has nothing to do there. It
    // never overrides rule 2, so a catch block still may not resume inside the window, and the
    // window still may not start at a call's return address.
    bool handler_checked = false;
};

struct DataPatch {
    uint32_t rva = 0;
    std::vector<uint8_t> bytes;
    std::optional<ExpectedBytes> expect;
};

struct ModulePatches {
    std::vector<Replacement> replacements;
    std::vector<MidFunctionHook> mid_hooks;
    std::vector<DataPatch> data;
    // Trap every cpuid and xgetbv instruction that function discovery finds in the module.
    bool trap_cpuid = false;

    Replacement& replace(uint32_t rva, const void* function, void** original = nullptr) {
        return replacements.emplace_back(Replacement{rva, function, original, std::nullopt});
    }
    MidFunctionHook& mid(uint32_t rva, MidHook hook) {
        return mid_hooks.emplace_back(MidFunctionHook{rva, hook, std::nullopt, false});
    }
    DataPatch& write(uint32_t rva, std::vector<uint8_t> bytes) {
        return data.emplace_back(DataPatch{rva, std::move(bytes), std::nullopt});
    }
    bool empty() const {
        return replacements.empty() && mid_hooks.empty() && data.empty() && !trap_cpuid;
    }
};

} // namespace g6::loader
