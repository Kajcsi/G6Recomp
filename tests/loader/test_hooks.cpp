// The hook engine (docs/hooks.md) on an original guest module (tests/fixtures/hook_guest):
// replacements with and without the original, the window rules, mid-function hooks that read
// and change registers, flags, and vector registers, exceptions through replaced functions and
// hooks, trapped cpuid and xgetbv, and data patches.

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <intrin.h>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "g6/cpu/cpuid.h"
#include "g6/kernel/image_loader.h"
#include "g6/loader/loader.h"
#include "g6/loader/patches.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6test.h"
#include "hook_guest_api.h"

using g6::kernel::LoadError;
using g6::loader::ExpectedBytes;
using g6::loader::HookContext;
using g6::loader::Loader;
using g6::loader::LoaderOptions;
using g6::loader::Module;
using g6::loader::ModulePatches;

namespace {

constexpr std::string_view kGuest = "hook_guest.dll";
constexpr uint64_t kCarryFlag = 0x1;
constexpr uint64_t kSignFlag = 0x80;
constexpr uint64_t kDirectionFlag = 0x400;

// ---- The fixture's layout, read from the file before any loader patches it ----

// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's map move constructor allocates.
struct Layout {
    g6::pe::Image image;
    std::map<std::string, uint32_t, std::less<>> exports;
    std::array<uint32_t, G6H_SITE_COUNT> sites{};
};

const Layout& layout() {
    static const Layout value = [] {
        Layout result{
            g6::pe::Image::load_file(std::string(G6_GUEST_DIR) + "/hook_guest.dll"), {}, {}};
        for (const g6::pe::Export& entry : g6::pe::read_exports(result.image).exports) {
            if (!entry.name.empty()) {
                result.exports.emplace(entry.name, entry.rva);
            }
        }
        const uint32_t table = result.exports.at("g6h_sites");
        for (uint32_t i = 0; i < G6H_SITE_COUNT; ++i) {
            result.sites[i] = result.image.read<uint32_t>(table + (i * 4));
        }
        return result;
    }();
    return value;
}

uint32_t rva_of(std::string_view name) {
    const auto it = layout().exports.find(name);
    if (it == layout().exports.end()) {
        throw std::runtime_error(fmt::format("hook_guest.dll doesn't export {}", name));
    }
    return it->second;
}

uint32_t site(G6HSite which) {
    return layout().sites[which];
}

template <typename F>
const void* host(F* function) {
    return reinterpret_cast<const void*>(function);
}

LoaderOptions make_options(ModulePatches patches) {
    LoaderOptions options;
    options.search_paths.emplace_back(G6_GUEST_DIR);
    options.imports.forward_dll("kernel32.dll");
    options.imports.forward_dll("api-ms-win-core-*");
    options.imports.forward_dll("ntdll.dll");
    options.patches.emplace(std::string(kGuest), std::move(patches));
    return options;
}

// An export of the guest as a function pointer; fails the test case when it is missing.
template <typename T>
T guest(const Module& module, std::string_view name) {
    void* address = module.find_export(name);
    if (address == nullptr) {
        g6::test::report_failure(__FILE__, __LINE__,
                                 fmt::format("hook_guest.dll doesn't export {}", name));
        throw g6::test::RequireFailed{};
    }
    return reinterpret_cast<T>(address);
}

// Loads the guest with `patches` and checks that the load fails with a message containing
// `reason`, leaving nothing of the guest loaded.
void check_refused(ModulePatches patches, std::string_view reason, int line) {
    Loader loader(make_options(std::move(patches)));
    try {
        loader.load(std::string(kGuest));
    } catch (const LoadError& error) {
        const std::string_view message = error.what();
        if (message.find(reason) == std::string_view::npos) {
            g6::test::report_failure(
                __FILE__, line,
                fmt::format(R"(refused for another reason: "{}", expected "{}")", message, reason));
        }
        if (loader.find(kGuest)) {
            g6::test::report_failure(__FILE__, line, "the refused module stayed loaded");
        }
        return;
    }
    g6::test::report_failure(__FILE__, line,
                             fmt::format(R"(loaded, but expected a refusal: "{}")", reason));
}

#define CHECK_REFUSED(patches, reason) check_refused((patches), (reason), __LINE__)

// ---- Host functions the replacements send calls to ----

using IntFn = int (*)(int);
using FramedFn = int (*)(int (*)(int), int);

void* g_original_rip = nullptr;
void* g_original_jcc = nullptr;
void* g_original_framed = nullptr;
void* g_original_unused = nullptr;

int host_rip_prologue(int x) {
    return reinterpret_cast<IntFn>(g_original_rip)(x) * 10;
}

int host_jcc_entry(int x) {
    return reinterpret_cast<IntFn>(g_original_jcc)(x) + 1000;
}

int host_framed(int (*callback)(int), int x) {
    return reinterpret_cast<FramedFn>(g_original_framed)(callback, x) + 100;
}

int host_tiny() {
    return 7;
}

int host_negate(int x) {
    return -x;
}

int host_square(int x) {
    return x * x;
}

// ---- Mid-function hooks ----

enum class Mode { Observe, Change, Redirect, ReturnEarly, ThrowCpp, RaiseSeh };

struct HookRecord {
    HookContext seen{};
    uint64_t redirect_to = 0;
    Mode mode = Mode::Observe;
    int calls = 0;
    bool direction_clear = false; // the direction flag was clear while the hook ran
};

HookRecord g_hook;

void reset_hook(Mode mode, uint64_t redirect_to = 0) {
    g_hook = HookRecord{};
    g_hook.mode = mode;
    g_hook.redirect_to = redirect_to;
}

void record(const HookContext& context) {
    ++g_hook.calls;
    g_hook.seen = context;
    g_hook.direction_clear = (__readeflags() & kDirectionFlag) == 0;
}

void hook_observe(HookContext& context) {
    record(context);
}

void hook_regs(HookContext& context) {
    record(context);
    if (g_hook.mode == Mode::Change) {
        context.rdx += 10;
        context.r9 += 1000;
    }
}

void hook_flags(HookContext& context) {
    record(context);
    if (g_hook.mode == Mode::Change) {
        context.rflags ^= kSignFlag;
    }
}

void hook_redirect(HookContext& context) {
    record(context);
    if (g_hook.mode == Mode::Redirect) {
        context.rip = g_hook.redirect_to;
    } else if (g_hook.mode == Mode::ReturnEarly) {
        // At the site the guest has no frame, so rsp points at its return address.
        context.rax = 77;
        std::memcpy(&context.rip, reinterpret_cast<const void*>(context.rsp), sizeof(uint64_t));
        context.rsp += 8;
    }
}

// Sets every bit of ymm0 to ymm15. C++ code in a hook may change any vector register: the
// calling convention preserves only the low halves of xmm6 to xmm15.
__declspec(noinline) void clobber_vector_registers() {
    __asm__ volatile("vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n\t"
                     "vpcmpeqd %%ymm1, %%ymm1, %%ymm1\n\t"
                     "vpcmpeqd %%ymm2, %%ymm2, %%ymm2\n\t"
                     "vpcmpeqd %%ymm3, %%ymm3, %%ymm3\n\t"
                     "vpcmpeqd %%ymm4, %%ymm4, %%ymm4\n\t"
                     "vpcmpeqd %%ymm5, %%ymm5, %%ymm5\n\t"
                     "vpcmpeqd %%ymm6, %%ymm6, %%ymm6\n\t"
                     "vpcmpeqd %%ymm7, %%ymm7, %%ymm7\n\t"
                     "vpcmpeqd %%ymm8, %%ymm8, %%ymm8\n\t"
                     "vpcmpeqd %%ymm9, %%ymm9, %%ymm9\n\t"
                     "vpcmpeqd %%ymm10, %%ymm10, %%ymm10\n\t"
                     "vpcmpeqd %%ymm11, %%ymm11, %%ymm11\n\t"
                     "vpcmpeqd %%ymm12, %%ymm12, %%ymm12\n\t"
                     "vpcmpeqd %%ymm13, %%ymm13, %%ymm13\n\t"
                     "vpcmpeqd %%ymm14, %%ymm14, %%ymm14\n\t"
                     "vpcmpeqd %%ymm15, %%ymm15, %%ymm15\n\t" ::
                         : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
                           "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

constexpr std::array<uint64_t, 4> kReplacedLanes = {1, 2, 3, 4};

void hook_ymm(HookContext& context) {
    record(context);
    context.ymm[3].lanes = kReplacedLanes;
    clobber_vector_registers();
}

void hook_throw(HookContext& context) {
    record(context);
    if (g_hook.mode == Mode::ThrowCpp) {
        throw 42; // NOLINT(hicpp-exception-baseclass): the guest catches an int
    }
    if (g_hook.mode == Mode::RaiseSeh) {
        RaiseException(G6H_SEH_CODE, 0, 0, nullptr);
    }
}

std::vector<uint8_t> bytes_at(uint64_t address, size_t size) {
    const auto* begin = reinterpret_cast<const uint8_t*>(address);
    return {begin, begin + size};
}

} // namespace

TEST_CASE("replacements send calls to host functions, which can call the original") {
    ModulePatches patches;
    patches.replace(rva_of("g6h_rip_prologue"), host(&host_rip_prologue), &g_original_rip);
    patches.replace(rva_of("g6h_jcc_entry"), host(&host_jcc_entry), &g_original_jcc);
    patches.replace(rva_of("g6h_framed"), host(&host_framed), &g_original_framed);
    patches.replace(rva_of("g6h_tiny"), host(&host_tiny)); // its window runs into padding
    {
        Loader loader(make_options(patches));
        const Module module = loader.load(std::string(kGuest));
        REQUIRE(g_original_rip != nullptr);
        REQUIRE(g_original_jcc != nullptr);
        REQUIRE(g_original_framed != nullptr);

        // The moved RIP-relative load still reads the guest's value.
        CHECK_EQ(guest<IntFn>(module, "g6h_rip_prologue")(5), 1050);
        // The moved short jump still reaches its target, taken or not.
        const auto jcc = guest<IntFn>(module, "g6h_jcc_entry");
        CHECK_EQ(jcc(0), 999);
        CHECK_EQ(jcc(21), 1042);
        // Guest code calling a replaced function reaches the replacement too.
        CHECK_EQ(guest<IntFn>(module, "g6h_calls")(4), 1009);
        // A trampoline with a frame: push rbx and sub rsp moved together.
        CHECK_EQ(guest<FramedFn>(module, "g6h_framed")(&host_square, 5), 130);
        CHECK_EQ(guest<int (*)()>(module, "g6h_tiny")(), 7);
    }
    // Unloading resets the original pointers.
    CHECK(g_original_rip == nullptr);
    CHECK(g_original_jcc == nullptr);
    CHECK(g_original_framed == nullptr);
}

TEST_CASE("windows that break a rule fail the load") {
    const auto one = [](auto&& add) {
        ModulePatches patches;
        add(patches);
        return patches;
    };
    CHECK_REFUSED(one([](ModulePatches& p) { p.replace(site(G6H_SITE_REGS), host(&host_tiny)); }),
                  "a replacement goes at a function's entry");
    CHECK_REFUSED(one([](ModulePatches& p) { p.mid(site(G6H_SITE_REGS) + 1, &hook_observe); }),
                  "isn't the start of an instruction");
    CHECK_REFUSED(one([](ModulePatches& p) {
                      p.replace(rva_of("g6h_loop"), host(&host_negate), &g_original_unused);
                  }),
                  "a jump inside the function lands in the window");
    CHECK_REFUSED(one([](ModulePatches& p) {
                      p.replace(rva_of("g6h_calls"), host(&host_negate), &g_original_unused);
                  }),
                  "holds a call");
    CHECK_REFUSED(one([](ModulePatches& p) {
                      p.replace(rva_of("g6h_tiny"), host(&host_tiny), &g_original_unused);
                  }),
                  "runs past the end of the function");
    CHECK_REFUSED(one([](ModulePatches& p) {
                      p.mid(site(G6H_SITE_REGS), &hook_observe);
                      p.mid(site(G6H_SITE_REGS), &hook_observe);
                  }),
                  "overlaps the mid-function hook");
    CHECK_REFUSED(one([](ModulePatches& p) {
                      p.write(rva_of("g6h_tiny") + 1, {0x90});
                      p.replace(rva_of("g6h_tiny"), host(&host_tiny));
                  }),
                  "overlaps the data patch");
    CHECK_REFUSED(
        one([](ModulePatches& p) {
            p.replace(rva_of("g6h_jcc_entry"), host(&host_jcc_entry), &g_original_unused).expect =
                ExpectedBytes{7, 0x1234};
        }),
        "don't match the expected hash");
    CHECK_REFUSED(one([](ModulePatches& p) { p.mid(site(G6H_SITE_EPILOGUE), &hook_observe); }),
                  "epilogue");
    CHECK_REFUSED(
        one([](ModulePatches& p) { p.write(layout().image.size_of_image() - 2, {1, 2, 3, 4}); }),
        "outside the image");
    CHECK_REFUSED(one([](ModulePatches& p) {
                      p.trap_cpuid = true;
                      p.mid(site(G6H_SITE_BEFORE_CPUID), &hook_observe);
                  }),
                  "which the loader traps");
    // A refused load never hands out the original.
    CHECK(g_original_unused == nullptr);
}

TEST_CASE("a matching hash, a window that never runs, and hooks in SEH and C++ functions load") {
    const uint32_t jcc = rva_of("g6h_jcc_entry");
    ModulePatches patches;
    patches.replace(jcc, host(&host_jcc_entry), &g_original_jcc).expect =
        ExpectedBytes{7, g6::loader::hash_bytes(layout().image.bytes(jcc, 7))};
    // Without the original, the call in the window never runs.
    patches.replace(rva_of("g6h_calls"), host(&host_negate));
    // A C function with __try: its handler is a C scope table, and the entry is outside it.
    patches.mid(rva_of("g6h_seh_catch_mid"), &hook_observe);
    // A C++ function with a try block, under the static C runtime's __CxxFrameHandler3: the
    // entry is outside every state (tests/loader/test_cxx_hooks.cpp has the rest).
    patches.mid(rva_of("g6h_cpp_catch_mid"), &hook_observe);

    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));
    CHECK_EQ(guest<IntFn>(module, "g6h_jcc_entry")(3), 1006);
    CHECK_EQ(guest<IntFn>(module, "g6h_calls")(3), -3);
    reset_hook(Mode::Observe);
    CHECK_EQ(guest<uint32_t (*)(int)>(module, "g6h_seh_catch_mid")(4), 12u);
    CHECK_EQ(g_hook.calls, 1);
    CHECK_EQ(guest<IntFn>(module, "g6h_cpp_catch_mid")(4), 13);
    CHECK_EQ(g_hook.calls, 2);
}

TEST_CASE("a hook at a call's return address is refused, and one an instruction later loads") {
    // Refused whatever the handler, and even after a hand check: unwinding out of the call would
    // read the patch jump as an epilogue (docs/hooks.md, "Windows at return addresses").
    ModulePatches at_return;
    at_return.mid(site(G6H_SITE_RETURN_ADDRESS), &hook_observe).handler_checked = true;
    CHECK_REFUSED(std::move(at_return), "return address of the call");

    ModulePatches patches;
    patches.mid(site(G6H_SITE_AFTER_RETURN), &hook_observe);
    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));
    reset_hook(Mode::Observe);
    CHECK_EQ(guest<IntFn>(module, "g6h_after_call")(2), 13);
    CHECK_EQ(g_hook.calls, 1);
}

TEST_CASE("a hook under a handler the engine doesn't read needs a hand check") {
    ModulePatches unchecked;
    unchecked.mid(rva_of("g6h_handled"), &hook_observe);
    CHECK_REFUSED(std::move(unchecked), "exception handler is none the engine reads");

    ModulePatches patches;
    patches.mid(rva_of("g6h_handled"), &hook_observe).handler_checked = true;
    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));
    reset_hook(Mode::Observe);
    CHECK_EQ(guest<IntFn>(module, "g6h_handled")(2), 7);
    CHECK_EQ(g_hook.calls, 1);
}

TEST_CASE("mid-function hooks see the guest's registers and flags and can change them") {
    ModulePatches patches;
    patches.mid(site(G6H_SITE_REGS), &hook_regs);
    patches.mid(site(G6H_SITE_FLAGS), &hook_flags);
    patches.mid(site(G6H_SITE_DF), &hook_observe);
    patches.mid(site(G6H_SITE_REDIRECT), &hook_redirect);
    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));
    const uint64_t base = module.base();

    using RegsFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
    const auto regs = guest<RegsFn>(module, "g6h_mid_regs");
    reset_hook(Mode::Observe);
    CHECK_EQ(regs(3, 4, 5, 6), 41u);
    CHECK_EQ(g_hook.calls, 1);
    CHECK_EQ(g_hook.seen.rip, base + site(G6H_SITE_REGS));
    CHECK_EQ(g_hook.seen.rax, 3u);
    CHECK_EQ(g_hook.seen.rcx, 3u);
    CHECK_EQ(g_hook.seen.rdx, 4u);
    CHECK_EQ(g_hook.seen.r8, 5u);
    CHECK_EQ(g_hook.seen.r9, 6u);
    CHECK(g_hook.direction_clear);
    reset_hook(Mode::Change);
    CHECK_EQ(regs(3, 4, 5, 6), ((3u + 14u) * 5u) + 1006u);

    using FlagsFn = int (*)(int, int);
    const auto flags = guest<FlagsFn>(module, "g6h_mid_flags");
    reset_hook(Mode::Observe);
    CHECK_EQ(flags(1, 2), 1);
    CHECK((g_hook.seen.rflags & kSignFlag) != 0);
    CHECK((g_hook.seen.rflags & kCarryFlag) != 0);
    CHECK_EQ(flags(5, 2), 0);
    reset_hook(Mode::Change);
    CHECK_EQ(flags(1, 2), 0);
    CHECK_EQ(flags(5, 2), 1);

    // The guest sets the direction flag before the site: the hook runs with it clear, and the
    // guest gets it back.
    reset_hook(Mode::Observe);
    const uint64_t after = guest<uint64_t (*)()>(module, "g6h_mid_df")();
    CHECK((after & kDirectionFlag) != 0);
    CHECK((g_hook.seen.rflags & kDirectionFlag) != 0);
    CHECK(g_hook.direction_clear);

    const auto redirect = guest<IntFn>(module, "g6h_mid_redirect");
    reset_hook(Mode::Observe);
    CHECK_EQ(redirect(5), 7);
    reset_hook(Mode::Redirect, base + site(G6H_SITE_REDIRECT_ALT));
    CHECK_EQ(redirect(5), 1005);
    reset_hook(Mode::ReturnEarly);
    CHECK_EQ(redirect(5), 77);
    CHECK_EQ(g_hook.calls, 1);
}

TEST_CASE("vector registers survive a hook at full width, and a hook can change them") {
    ModulePatches patches;
    patches.mid(site(G6H_SITE_YMM), &hook_ymm);
    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));

    std::array<uint64_t, 64> in{};
    for (size_t i = 0; i < in.size(); ++i) {
        in[i] = 0x0123456789ABCDEFull * (i + 1);
    }
    std::array<uint64_t, 64> out{};
    reset_hook(Mode::Observe);
    guest<void (*)(const void*, void*)>(module, "g6h_mid_ymm")(in.data(), out.data());
    REQUIRE(g_hook.calls == 1);
    for (size_t reg = 0; reg < 16; ++reg) {
        for (size_t lane = 0; lane < 4; ++lane) {
            CHECK_EQ(g_hook.seen.ymm[reg].lanes[lane], in[(reg * 4) + lane]);
            const uint64_t expected = reg == 3 ? kReplacedLanes[lane] : in[(reg * 4) + lane];
            CHECK_EQ(out[(reg * 4) + lane], expected);
        }
    }
}

TEST_CASE("exceptions pass through replaced functions and mid-function hooks") {
    ModulePatches patches;
    patches.replace(rva_of("g6h_framed"), host(&host_framed), &g_original_framed);
    patches.mid(site(G6H_SITE_FRAMED), &hook_throw);
    patches.mid(site(G6H_SITE_LOAD), &hook_observe);
    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));
    const auto cleanups = guest<int (*)()>(module, "g6h_cleanups");

    // Through the original and the host replacement, to a guest catch.
    const auto cpp_through = guest<IntFn>(module, "g6h_cpp_through_framed");
    CHECK_EQ(cpp_through(5), 110);
    CHECK_EQ(cpp_through(-3), -3);
    CHECK_EQ(guest<uint32_t (*)(int)>(module, "g6h_seh_through_framed")(1), G6H_SEH_CODE);

    // Out of a hook body, through the stub and the moved window, to the guest function's
    // caller, with the cleanups in between run. They run on a normal return too.
    const auto cpp_catch = guest<IntFn>(module, "g6h_cpp_catch_mid");
    int before = cleanups();
    reset_hook(Mode::Observe);
    CHECK_EQ(cpp_catch(4), 13);
    reset_hook(Mode::ThrowCpp);
    CHECK_EQ(cpp_catch(4), 42);
    CHECK_EQ(g_hook.calls, 1);
    CHECK_EQ(cleanups(), before + 2);

    const auto seh_catch = guest<uint32_t (*)(int)>(module, "g6h_seh_catch_mid");
    before = cleanups();
    reset_hook(Mode::Observe);
    CHECK_EQ(seh_catch(4), 12u);
    reset_hook(Mode::RaiseSeh);
    CHECK_EQ(seh_catch(4), G6H_SEH_CODE);
    CHECK_EQ(cleanups(), before + 2);

    // A fault in the moved copy of a window instruction reaches the guest caller's handler.
    const auto load = guest<int (*)(const int*)>(module, "g6h_load_or_fault");
    const int value = 41;
    reset_hook(Mode::Observe);
    CHECK_EQ(load(&value), 42);
    CHECK_EQ(load(nullptr), -1);
    CHECK_EQ(g_hook.calls, 2);
}

TEST_CASE("trapped cpuid and xgetbv answer with the guest's view") {
    ModulePatches patches;
    patches.trap_cpuid = true;
    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));
    const uint64_t base = module.base();

    const auto cpuid = guest<void (*)(uint32_t, uint32_t, uint32_t*)>(module, "g6h_cpuid");
    const std::array<std::pair<uint32_t, uint32_t>, 6> leaves = {
        {{0, 0}, {1, 0}, {7, 0}, {0xD, 0}, {0x80000000, 0}, {0x80000001, 0}}};
    for (const auto& [leaf, subleaf] : leaves) {
        std::array<uint32_t, 4> seen{};
        cpuid(leaf, subleaf, seen.data());
        const g6::cpu::CpuidResult expected = g6::cpu::guest_cpuid(leaf, subleaf);
        // Leaf 1 reports the APIC ID of whichever core the thread runs on.
        const uint32_t ebx_mask = leaf == 1 ? 0x00FFFFFFu : 0xFFFFFFFFu;
        CHECK_EQ(seen[0], expected.eax);
        CHECK_EQ(seen[1] & ebx_mask, expected.ebx & ebx_mask);
        CHECK_EQ(seen[2], expected.ecx);
        CHECK_EQ(seen[3], expected.edx);
    }

    CHECK_EQ(guest<uint64_t (*)(uint32_t)>(module, "g6h_xgetbv")(0), g6::cpu::guest_xgetbv(0));
    const auto xgetbv_fault = guest<uint32_t (*)(uint32_t)>(module, "g6h_xgetbv_fault");
    CHECK_EQ(xgetbv_fault(0), 0u);
    // An index the host rejects faults, at the trap.
    CHECK_EQ(xgetbv_fault(0x7FFFFFFF), static_cast<uint32_t>(STATUS_ILLEGAL_INSTRUCTION));

    // The sites hold ud2; bytes that only look like cpuid and xgetbv are untouched.
    const std::vector<uint8_t> ud2 = {0x0F, 0x0B};
    CHECK(bytes_at(base + site(G6H_SITE_BEFORE_CPUID) + 4, 2) == ud2);
    CHECK(bytes_at(base + rva_of("g6h_xgetbv"), 2) == ud2);
    const std::vector<uint8_t> lookalike = {0x0F, 0xA2, 0x0F, 0x01, 0xD0};
    CHECK(bytes_at(base + site(G6H_SITE_LOOKALIKE), 5) == lookalike);
}

TEST_CASE("a data patch writes its bytes") {
    ModulePatches patches;
    patches.write(rva_of("g6h_data_value"), {0x39, 0x30, 0x00, 0x00});
    Loader loader(make_options(patches));
    const Module module = loader.load(std::string(kGuest));
    CHECK_EQ(guest<int (*)()>(module, "g6h_get_data_value")(), 12345);
}
