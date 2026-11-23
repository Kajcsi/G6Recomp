#include "patcher.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <Zydis/Zydis.h>
#include <fmt/format.h>

#include "code_walker.h"
#include "cxx_exceptions.h"
#include "function_discovery.h"
#include "g6/cpu/cpuid.h"
#include "g6/log.h"
#include "g6/pe/unwind.h"

namespace g6::loader {

uint64_t hash_bytes(std::span<const uint8_t> bytes) {
    constexpr uint64_t kOffsetBasis = 0xCBF29CE484222325ull;
    constexpr uint64_t kPrime = 0x100000001B3ull;
    uint64_t hash = kOffsetBasis;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= kPrime;
    }
    return hash;
}

namespace {

using recomp::DiscoveredFunction;
using recomp::DiscoveryResult;
using recomp::Range;

constexpr uint32_t kJumpSize = 5; // jmp rel32
constexpr uint8_t kInt3 = 0xCC;
constexpr uint64_t kGranularity = 0x10000; // VirtualAlloc's allocation granularity
constexpr uint64_t kReach = INT32_MAX;     // the farthest a rel32 displacement reaches
constexpr size_t kArenaFixed = 0x2000;     // the function table, and slack
constexpr size_t kArenaPerPatch = 0x400;   // a mid-function stub with its window and unwind info
constexpr uint32_t kMaxScopes = 1024;      // more than any real C scope table holds
constexpr int kMaxChainDepth = 32;

// The mid-function stub keeps a HookContext above the shadow space of the call it makes.
constexpr int32_t kContextOffset = 32;
constexpr int32_t kFrameSize = kContextOffset + static_cast<int32_t>(sizeof(HookContext));
constexpr int32_t kFlagsSlot = kContextOffset + static_cast<int32_t>(offsetof(HookContext, rflags));
constexpr int32_t kRipSlot = kContextOffset + static_cast<int32_t>(offsetof(HookContext, rip));
static_assert(offsetof(HookContext, rax) == 0 && offsetof(HookContext, r15) == 15 * 8,
              "the general registers are in encoding order");
static_assert(kFrameSize % 32 == 0, "the stub aligns rsp to 32 before allocating its frame");

constexpr int kRax = 0;
constexpr int kRcx = 1;
constexpr int kRsp = 4;
constexpr int kRbp = 5;
constexpr int kRegisterCount = 16;

constexpr int32_t gpr_slot(int reg) {
    return kContextOffset + (reg * 8);
}

constexpr int32_t ymm_slot(int reg) {
    return kContextOffset + static_cast<int32_t>(offsetof(HookContext, ymm)) + (reg * 32);
}

// Unwind info of the stub after its first instruction: push rbp (ends at 1), mov rbp, rsp (ends
// at 4), so rbp is the frame register. Codes are stored last first.
constexpr std::array<uint8_t, 8> kStubUnwindInfo = {
    0x01, 0x04, 0x02, 0x05, // version 1, prolog 4 bytes, 2 codes, frame register rbp at offset 0
    0x04, 0x03,             // at 4: set frame pointer
    0x01, 0x50,             // at 1: push rbp
};

uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value / alignment * alignment;
}

bool fits_rel32(int64_t displacement) {
    return displacement >= INT32_MIN && displacement <= INT32_MAX;
}

// Reserves and commits `size` bytes within rel32 reach of the whole module: after it if there
// is room, before it otherwise.
uint8_t* reserve_near(uint64_t module_base, uint64_t module_size, size_t size) {
    const uint64_t module_end = module_base + module_size;
    const auto try_at = [&](uint64_t at) {
        return static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    };
    MEMORY_BASIC_INFORMATION info{};
    for (uint64_t at = align_up(module_end, kGranularity); at + size - module_base <= kReach;) {
        if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) {
            break;
        }
        const uint64_t region_end = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_FREE && region_end - at >= size) {
            if (uint8_t* arena = try_at(at)) {
                return arena;
            }
            at += kGranularity;
        } else {
            at = align_up(region_end, kGranularity);
        }
    }
    if (module_base < size + kGranularity) {
        return nullptr;
    }
    for (uint64_t at = align_down(module_base - size, kGranularity);
         at >= kGranularity && module_end - at <= kReach;) {
        if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) {
            break;
        }
        const auto region_begin = reinterpret_cast<uint64_t>(info.BaseAddress);
        const uint64_t region_end = region_begin + info.RegionSize;
        uint64_t next = 0;
        if (info.State == MEM_FREE && region_end - at >= size) {
            if (uint8_t* arena = try_at(at)) {
                return arena;
            }
            next = at - kGranularity;
        } else if (info.State == MEM_FREE) {
            // Free here, but something starts before the arena would end.
            if (region_end < size + kGranularity) {
                break;
            }
            next = align_down(region_end - size, kGranularity);
        } else {
            const auto allocation = reinterpret_cast<uint64_t>(info.AllocationBase);
            if (allocation < size + kGranularity) {
                break;
            }
            next = align_down(allocation - size, kGranularity);
        }
        if (next >= at) {
            break;
        }
        at = next;
    }
    return nullptr;
}

} // namespace

// ---- The patch arena ----

// Memory near a module for its stubs, trampolines, jumps to host code, and their unwind info.
// It is filled front to back while the module is patched, then sealed: made executable and its
// function table registered, so Windows unwinds through the code in it.
class PatchArena {
public:
    PatchArena(uint64_t module_base, uint64_t module_size, size_t size) : size_(size) {
        base_ = reserve_near(module_base, module_size, size);
        if (base_ == nullptr) {
            throw kernel::LoadError(fmt::format(
                "no free memory within 2 GiB of the module at {:#x} for its patch arena",
                module_base));
        }
        std::memset(base_, kInt3, size_);
    }

    ~PatchArena() {
        if (table_ != nullptr) {
            RtlDeleteFunctionTable(table_);
        }
        VirtualFree(base_, 0, MEM_RELEASE);
    }

    PatchArena(const PatchArena&) = delete;
    PatchArena& operator=(const PatchArena&) = delete;

    uint64_t here() const { return reinterpret_cast<uint64_t>(base_ + used_); }

    void align(size_t alignment, uint8_t fill = kInt3) {
        while (used_ % alignment != 0) {
            put(&fill, 1);
        }
    }
    void bytes(std::initializer_list<uint8_t> values) { put(values.begin(), values.size()); }
    void bytes(std::span<const uint8_t> values) { put(values.data(), values.size()); }
    void u32(uint32_t value) { put(&value, sizeof(value)); }
    void u64(uint64_t value) { put(&value, sizeof(value)); }

    // A rel32 field that ends its instruction, reaching `target`.
    void rel32(uint64_t target) {
        const auto displacement = static_cast<int64_t>(target - (here() + 4));
        if (!fits_rel32(displacement)) {
            throw kernel::LoadError(
                fmt::format("{:#x} in the patch arena can't reach {:#x}", here(), target));
        }
        u32(static_cast<uint32_t>(static_cast<int32_t>(displacement)));
    }

    // An 8-byte slot holding `value`. It can be changed with set_slot until seal().
    uint64_t slot(uint64_t value) {
        align(8, 0);
        const uint64_t at = here();
        u64(value);
        return at;
    }
    static void set_slot(uint64_t slot, uint64_t value) {
        std::memcpy(reinterpret_cast<void*>(slot), &value, sizeof(value));
    }

    // Describes [begin, end) to the unwinder with a copy of `unwind_info` (an UNWIND_INFO).
    void add_function(uint64_t begin, uint64_t end, std::span<const uint8_t> unwind_info) {
        align(4, 0);
        const uint64_t info = here();
        bytes(unwind_info);
        RUNTIME_FUNCTION entry{};
        entry.BeginAddress = offset_of(begin);
        entry.EndAddress = offset_of(end);
        entry.UnwindData = offset_of(info);
        functions_.push_back(entry);
    }

    void seal() {
        RUNTIME_FUNCTION* table = nullptr;
        if (!functions_.empty()) {
            std::ranges::sort(functions_, {}, &RUNTIME_FUNCTION::BeginAddress);
            align(4, 0);
            table = reinterpret_cast<RUNTIME_FUNCTION*>(here());
            put(functions_.data(), functions_.size() * sizeof(RUNTIME_FUNCTION));
        }
        DWORD old = 0;
        if (!VirtualProtect(base_, size_, PAGE_EXECUTE_READ, &old)) {
            throw kernel::LoadError(
                fmt::format("cannot make the patch arena executable (error {})", GetLastError()));
        }
        FlushInstructionCache(GetCurrentProcess(), base_, size_);
        if (table != nullptr) {
            if (!RtlAddFunctionTable(table, static_cast<DWORD>(functions_.size()),
                                     reinterpret_cast<DWORD64>(base_))) {
                throw kernel::LoadError("cannot register the patch arena's function table");
            }
            table_ = table;
        }
    }

private:
    void put(const void* data, size_t size) {
        if (size > size_ - used_) {
            throw kernel::LoadError("the patch arena is full");
        }
        std::memcpy(base_ + used_, data, size);
        used_ += size;
    }

    uint32_t offset_of(uint64_t address) const {
        return static_cast<uint32_t>(address - reinterpret_cast<uint64_t>(base_));
    }

    uint8_t* base_ = nullptr;
    size_t size_ = 0;
    size_t used_ = 0;
    std::vector<RUNTIME_FUNCTION> functions_;
    RUNTIME_FUNCTION* table_ = nullptr; // set once registered
};

namespace {

// ---- Mid-function stub ----

// What the stub calls: the hook, then a check that it kept to the rules for rsp.
void run_mid_hook(HookContext* context, MidHook hook) {
    const uint64_t rsp = context->rsp;
    hook(*context);
    if (context->rsp < rsp) {
        LOG_ERROR(hooks,
                  "a mid-function hook lowered rsp from {:#x} to {:#x}; a hook may only raise it",
                  rsp, context->rsp);
        std::terminate();
    }
}

// mov [rsp+disp32], r64 (opcode 0x89) or mov r64, [rsp+disp32] (0x8B).
void gpr_at_rsp(PatchArena& code, uint8_t opcode, int reg, int32_t disp) {
    code.bytes({static_cast<uint8_t>(0x48 | (reg >= 8 ? 0x04 : 0)), opcode,
                static_cast<uint8_t>(0x84 | ((reg & 7) << 3)), 0x24});
    code.u32(static_cast<uint32_t>(disp));
}

// vmovdqu [rsp+disp32], ymm (opcode 0x7F) or vmovdqu ymm, [rsp+disp32] (0x6F).
void ymm_at_rsp(PatchArena& code, uint8_t opcode, int reg, int32_t disp) {
    code.bytes({0xC5, static_cast<uint8_t>(reg >= 8 ? 0x7E : 0xFE), opcode,
                static_cast<uint8_t>(0x84 | ((reg & 7) << 3)), 0x24});
    code.u32(static_cast<uint32_t>(disp));
}

constexpr uint8_t kStore = 0x89;
constexpr uint8_t kLoad = 0x8B;
constexpr uint8_t kVecStore = 0x7F;
constexpr uint8_t kVecLoad = 0x6F;

// The stub's body, entered with the guest's registers and a fake return address on the stack
// (the moved window, so the unwinder sees a function called from the hook site). It builds a
// HookContext, calls run_mid_hook, and resumes from the context: at the moved window when rip
// is still the site, at rip otherwise, with rsp from the context.
void emit_mid_stub(PatchArena& code, uint64_t site_slot, uint64_t hook_slot, uint64_t runner_slot,
                   uint64_t window_slot) {
    code.bytes({0x55});                   // push rbp
    code.bytes({0x48, 0x89, 0xE5});       // mov rbp, rsp
    code.bytes({0x9C});                   // pushfq            [rbp-8]: the guest's flags
    code.bytes({0x48, 0x83, 0xE4, 0xE0}); // and rsp, -32
    code.bytes({0x48, 0x81, 0xEC});       // sub rsp, kFrameSize
    code.u32(static_cast<uint32_t>(kFrameSize));
    for (int reg = 0; reg < kRegisterCount; ++reg) {
        if (reg != kRsp && reg != kRbp) {
            gpr_at_rsp(code, kStore, reg, gpr_slot(reg));
        }
    }
    code.bytes({0x48, 0x8B, 0x45, 0x00}); // mov rax, [rbp]     the guest's rbp
    gpr_at_rsp(code, kStore, kRax, gpr_slot(kRbp));
    code.bytes({0x48, 0x8D, 0x45, 0x10}); // lea rax, [rbp+16]  the guest's rsp
    gpr_at_rsp(code, kStore, kRax, gpr_slot(kRsp));
    code.bytes({0x48, 0x8B, 0x45, 0xF8}); // mov rax, [rbp-8]   the guest's flags
    gpr_at_rsp(code, kStore, kRax, kFlagsSlot);
    code.bytes({0x48, 0x8B, 0x05}); // mov rax, [site]
    code.rel32(site_slot);
    gpr_at_rsp(code, kStore, kRax, kRipSlot);
    for (int reg = 0; reg < kRegisterCount; ++reg) {
        ymm_at_rsp(code, kVecStore, reg, ymm_slot(reg));
    }
    code.bytes({0xFC});                                                         // cld
    code.bytes({0x48, 0x8D, 0x4C, 0x24, static_cast<uint8_t>(kContextOffset)}); // lea rcx, [ctx]
    code.bytes({0x48, 0x8B, 0x15});                                             // mov rdx, [hook]
    code.rel32(hook_slot);
    code.bytes({0xFF, 0x15}); // call [run_mid_hook]
    code.rel32(runner_slot);

    for (int reg = 0; reg < kRegisterCount; ++reg) {
        ymm_at_rsp(code, kVecLoad, reg, ymm_slot(reg));
    }
    gpr_at_rsp(code, kLoad, kRax, kRipSlot);
    code.bytes({0x48, 0x3B, 0x05}); // cmp rax, [site]
    code.rel32(site_slot);
    code.bytes({0x75, 0x07});       // jne over the next instruction
    code.bytes({0x48, 0x8B, 0x05}); // mov rax, [moved window]
    code.rel32(window_slot);
    // Stash where to go and the flags just below the rsp to resume with, which the hook may
    // only have raised, so the stash never overlaps the context still being read.
    gpr_at_rsp(code, kLoad, kRcx, gpr_slot(kRsp));
    code.bytes({0x48, 0x89, 0x41, 0xF8}); // mov [rcx-8], rax
    gpr_at_rsp(code, kLoad, kRax, kFlagsSlot);
    code.bytes({0x48, 0x89, 0x41, 0xF0}); // mov [rcx-16], rax
    code.bytes({0x48, 0x83, 0xE9, 0x10}); // sub rcx, 16
    gpr_at_rsp(code, kStore, kRcx, gpr_slot(kRsp));
    for (int reg = 0; reg < kRegisterCount; ++reg) {
        if (reg != kRsp) {
            gpr_at_rsp(code, kLoad, reg, gpr_slot(reg));
        }
    }
    gpr_at_rsp(code, kLoad, kRsp, gpr_slot(kRsp)); // rsp = the stash
    code.bytes({0x9D});                            // popfq
    code.bytes({0x48, 0x8D, 0x64, 0x24, 0x08});    // lea rsp, [rsp+8]
    code.bytes({0xFF, 0x64, 0x24, 0xF8});          // jmp [rsp-8]
}

// ---- Analysis ----

struct WindowInstruction {
    uint32_t rva = 0;
    ZydisDecodedInstruction insn{};
};

// The whole instructions a patch jump overwrites.
struct Window {
    uint32_t site = 0;
    uint32_t size = 0;
    std::vector<WindowInstruction> instructions; // without int3 padding after the function
};

// Where a call returns to.
struct ReturnSite {
    uint32_t rva = 0;
    uint32_t call = 0;
};

// What the window rules need to know about a function.
struct FunctionFacts {
    std::vector<uint32_t> starts;             // instructions its code reaches, sorted
    std::vector<uint32_t> targets;            // where jumps inside it land, sorted
    std::vector<uint32_t> continuations;      // where catch blocks resume inside it, sorted
    std::vector<ReturnSite> return_sites;     // where its calls return, sorted by rva
    std::vector<Range> try_ranges;            // C __try blocks
    const recomp::CxxFunction* cxx = nullptr; // a C++ handler whose tables the engine checks
    // Set when the function has a handler the engine can't check: why, for the refusal.
    std::string unchecked_handler;
};

struct Occupied {
    uint32_t begin = 0;
    uint32_t end = 0;
    const char* what = "";
};

// For each moved instruction: where the original ends (bytes from the site) and where its copy
// ends (bytes from the start of the copy).
using EndMap = std::vector<std::pair<uint32_t, uint32_t>>;

enum class Kind { Replacement, MidHook, Data };

const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Replacement: return "replacement";
    case Kind::MidHook: return "mid-function hook";
    case Kind::Data: return "data patch";
    }
    return "patch";
}

bool rip_relative(const ZydisDecodedInstruction& insn) {
    return (insn.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0 && insn.raw.modrm.mod == 0 &&
           insn.raw.modrm.rm == 5;
}

bool relative_branch(const ZydisDecodedInstruction& insn) {
    return (insn.meta.category == ZYDIS_CATEGORY_COND_BR ||
            insn.meta.category == ZYDIS_CATEGORY_UNCOND_BR) &&
           insn.raw.imm[0].is_relative != 0;
}

struct ScopeEntry {
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t handler = 0;
    uint32_t target = 0;
};

// The handler data as a C scope table (__C_specific_handler's format), or nullopt when it
// doesn't have that shape. Anything that isn't one counts as some other handler.
std::optional<std::vector<ScopeEntry>> read_scope_table(const pe::Image& image, uint32_t data,
                                                        const DiscoveredFunction& function) {
    if (!image.contains(data, sizeof(uint32_t))) {
        return std::nullopt;
    }
    const auto count = image.read<uint32_t>(data);
    if (count == 0 || count > kMaxScopes ||
        !image.contains(data + 4, uint64_t{count} * sizeof(ScopeEntry))) {
        return std::nullopt;
    }
    std::vector<ScopeEntry> entries(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto entry = image.read<ScopeEntry>(data + 4 + (i * sizeof(ScopeEntry)));
        // The filter is 1 (EXCEPTION_EXECUTE_HANDLER) or code; the target is 0 for __finally.
        if (entry.begin >= entry.end || !function.contains(entry.begin) ||
            !function.contains(entry.end - 1) ||
            (entry.handler > 1 && !image.is_executable(entry.handler)) ||
            (entry.target != 0 && !function.contains(entry.target))) {
            return std::nullopt;
        }
        entries[i] = entry;
    }
    return entries;
}

// UNWIND_INFO bytes (version 1, no handler, no chain) for `codes`.
std::vector<uint8_t> encode_unwind_info(uint8_t prolog_size,
                                        std::optional<std::pair<uint8_t, uint32_t>> frame,
                                        const std::vector<pe::UnwindCode>& codes) {
    std::vector<uint16_t> slots;
    const auto first = [&](const pe::UnwindCode& code, pe::UnwindOp op, uint32_t info) {
        slots.push_back(static_cast<uint16_t>(code.prolog_offset |
                                              (static_cast<uint32_t>(op) << 8) | (info << 12)));
    };
    const auto wide = [&](uint32_t value) {
        slots.push_back(static_cast<uint16_t>(value & 0xFFFF));
        slots.push_back(static_cast<uint16_t>(value >> 16));
    };
    // A save offset in the short form (scaled) when it fits, the far form otherwise.
    const auto save = [&](const pe::UnwindCode& code, pe::UnwindOp short_op, pe::UnwindOp long_op,
                          uint32_t scale) {
        if (code.value % scale == 0 && code.value / scale <= 0xFFFF) {
            first(code, short_op, code.info);
            slots.push_back(static_cast<uint16_t>(code.value / scale));
        } else {
            first(code, long_op, code.info);
            wide(code.value);
        }
    };
    for (const pe::UnwindCode& code : codes) {
        switch (code.op) {
        case pe::UnwindOp::PushNonVolatile: first(code, code.op, code.info); break;
        case pe::UnwindOp::SetFramePointer: first(code, code.op, 0); break;
        case pe::UnwindOp::AllocSmall: first(code, code.op, (code.value - 8) / 8); break;
        case pe::UnwindOp::AllocLarge:
            if (code.value % 8 == 0 && code.value / 8 <= 0xFFFF) {
                first(code, code.op, 0);
                slots.push_back(static_cast<uint16_t>(code.value / 8));
            } else {
                first(code, code.op, 1);
                wide(code.value);
            }
            break;
        case pe::UnwindOp::SaveNonVolatile:
        case pe::UnwindOp::SaveNonVolatileFar:
            save(code, pe::UnwindOp::SaveNonVolatile, pe::UnwindOp::SaveNonVolatileFar, 8);
            break;
        case pe::UnwindOp::SaveXmm128:
        case pe::UnwindOp::SaveXmm128Far:
            save(code, pe::UnwindOp::SaveXmm128, pe::UnwindOp::SaveXmm128Far, 16);
            break;
        default:
            throw kernel::LoadError(fmt::format(
                "unwind op {} can't be carried into the patch arena", static_cast<int>(code.op)));
        }
    }
    if (slots.size() > UINT8_MAX) {
        throw kernel::LoadError("too many unwind codes to carry into the patch arena");
    }
    const uint8_t frame_byte =
        frame ? static_cast<uint8_t>(frame->first | ((frame->second / 16) << 4)) : 0;
    std::vector<uint8_t> bytes{1, prolog_size, static_cast<uint8_t>(slots.size()), frame_byte};
    for (const uint16_t slot : slots) {
        bytes.push_back(static_cast<uint8_t>(slot & 0xFF));
        bytes.push_back(static_cast<uint8_t>(slot >> 8));
    }
    if (slots.size() % 2 != 0) {
        bytes.insert(bytes.end(), {0, 0}); // the code array always has an even length
    }
    return bytes;
}

// ---- The patcher ----

class Patcher {
public:
    Patcher(std::string_view module_name, const pe::Image& image, const kernel::MappedImage& mapped,
            const ModulePatches& patches, AppliedPatches& applied)
        : name_(module_name), image_(image), mapped_(mapped), patches_(patches), applied_(applied) {
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    }

    void run();

private:
    [[noreturn]] void refuse(Kind kind, uint32_t rva, std::string_view reason) const {
        throw kernel::LoadError(
            fmt::format("{}: {} at {:#x} refused: {}", name_, kind_name(kind), rva, reason));
    }

    uint64_t base() const { return mapped_.base; }
    void check_expected(Kind kind, uint32_t rva, const std::optional<ExpectedBytes>& expect) const;
    void check_free(Kind kind, uint32_t rva, uint32_t begin, uint32_t end) const;
    const FunctionFacts& facts_of(const DiscoveredFunction& function);
    Window plan_window(Kind kind, uint32_t site, bool runs, bool handler_checked);
    void check_cxx_states(uint32_t site, uint32_t end, const recomp::CxxFunction& function) const;
    void check_movable(Kind kind, const Window& window, const DiscoveredFunction& function) const;
    EndMap relocate(Kind kind, const Window& window);
    std::optional<std::vector<uint8_t>> moved_unwind_info(const Window& window, const EndMap& ends,
                                                          uint32_t copy_size) const;
    void jump_from_site(const Window& window, uint64_t target);

    void apply(const DataPatch& patch);
    void apply(const Replacement& patch);
    void apply(const MidFunctionHook& patch);
    void trap_sites();

    std::string_view name_;
    const pe::Image& image_;
    kernel::MappedImage mapped_;
    const ModulePatches& patches_;
    AppliedPatches& applied_;
    ZydisDecoder decoder_{};

    DiscoveryResult discovery_;   // empty unless the module has code patches or traps
    recomp::CxxExceptionMap cxx_; // empty unless the module has code patches
    std::vector<pe::RuntimeFunction> pdata_;
    std::map<uint32_t, FunctionFacts> facts_;
    std::vector<Occupied> occupied_;
    std::vector<AppliedPatches::Site> trap_candidates_; // sorted by rva
    std::vector<bool> trap_covered_; // inside a replacement window that never runs
    std::vector<std::pair<void**, uint64_t>> originals_;
};

void Patcher::run() {
    const size_t code_patches = patches_.replacements.size() + patches_.mid_hooks.size();
    if (code_patches != 0 || patches_.trap_cpuid) {
        discovery_ = recomp::discover_functions(image_);
        if (image_.directory(pe::Directory::Exception).present()) {
            pdata_ = pe::read_runtime_functions(image_);
        }
    }
    if (patches_.trap_cpuid) {
        for (const uint32_t rva : discovery_.cpuid_sites) {
            trap_candidates_.push_back({rva, false});
        }
        for (const uint32_t rva : discovery_.xgetbv_sites) {
            trap_candidates_.push_back({rva, true});
        }
        std::ranges::sort(trap_candidates_, {}, &AppliedPatches::Site::rva);
        trap_covered_.assign(trap_candidates_.size(), false);
    }
    if (code_patches != 0) {
        cxx_ = recomp::map_cxx_exceptions(image_, discovery_);
        const size_t size = align_up(kArenaFixed + (code_patches * kArenaPerPatch), kGranularity);
        applied_.arena = std::make_unique<PatchArena>(mapped_.base, mapped_.size, size);
    }

    for (const DataPatch& patch : patches_.data) {
        apply(patch);
    }
    for (const Replacement& patch : patches_.replacements) {
        apply(patch);
    }
    for (const MidFunctionHook& patch : patches_.mid_hooks) {
        apply(patch);
    }
    trap_sites();
    if (applied_.arena) {
        applied_.arena->seal();
    }
    // Only now that every patch is in: the caller may call the original as soon as the module
    // is published.
    for (const auto& [pointer, trampoline] : originals_) {
        *pointer = reinterpret_cast<void*>(trampoline);
        applied_.originals.push_back(pointer);
    }
    LOG_INFO(hooks,
             "{}: {} replacements, {} mid-function hooks, {} data patches, {} cpuid and xgetbv "
             "sites trapped",
             name_, patches_.replacements.size(), patches_.mid_hooks.size(), patches_.data.size(),
             applied_.sites.size());
}

void Patcher::check_expected(Kind kind, uint32_t rva,
                             const std::optional<ExpectedBytes>& expect) const {
    if (!expect) {
        return;
    }
    if (expect->size == 0 || !image_.contains(rva, expect->size)) {
        refuse(kind, rva, "its expected bytes lie outside the image");
    }
    if (hash_bytes(image_.bytes(rva, expect->size)) != expect->hash) {
        refuse(kind, rva,
               fmt::format("the {} bytes there don't match the expected hash; is the patch meant "
                           "for another build?",
                           expect->size));
    }
}

void Patcher::check_free(Kind kind, uint32_t rva, uint32_t begin, uint32_t end) const {
    for (const Occupied& other : occupied_) {
        if (begin < other.end && other.begin < end) {
            refuse(kind, rva, fmt::format("it overlaps the {} at {:#x}", other.what, other.begin));
        }
    }
}

const FunctionFacts& Patcher::facts_of(const DiscoveredFunction& function) {
    if (const auto it = facts_.find(function.start); it != facts_.end()) {
        return it->second;
    }
    FunctionFacts facts;
    std::vector<uint32_t> entries{function.start};
    std::vector<Range> ranges{{function.start, function.end}};
    ranges.insert(ranges.end(), function.fragments.begin(), function.fragments.end());

    // Code after a catch block is reached only through the C++ handler: it is an entry, and a
    // window must not cover it (rule 2).
    for (const Range& range : ranges) {
        for (auto it = std::ranges::lower_bound(cxx_.continuations, range.begin);
             it != cxx_.continuations.end() && *it < range.end; ++it) {
            facts.continuations.push_back(*it);
            entries.push_back(*it);
        }
    }

    if (const pe::RuntimeFunction* entry = pe::find_runtime_function(pdata_, function.start);
        entry != nullptr && entry->begin == function.start) {
        const pe::UnwindInfo info =
            pe::read_unwind_info(image_, pe::resolve_indirect(image_, *entry).unwind_info);
        const recomp::CxxFunction* cxx = cxx_.find(function.start);
        if (!info.has_handler() || cxx_.is_gs_only(function.start)) {
            // Nothing sees exceptions in this function's frame, or the handler only checks the
            // stack cookie: it never catches and runs no cleanups, so an exception from the hook
            // or the moved instructions skips only that check.
        } else if (cxx != nullptr && cxx->funclet) {
            facts.unchecked_handler = "the function is a C++ catch or cleanup funclet, which runs "
                                      "in another function's exception states";
        } else if (cxx != nullptr && !cxx->verifiable) {
            facts.unchecked_handler = "the function has a C++ catch block whose continuation "
                                      "isn't known, so a catch could resume inside the window";
        } else if (cxx != nullptr) {
            facts.cxx = cxx;
        } else if (const auto scopes = read_scope_table(image_, info.handler_data_rva, function)) {
            for (const ScopeEntry& scope : *scopes) {
                facts.try_ranges.push_back({scope.begin, scope.end});
                if (scope.target != 0) {
                    // An __except block: reached only from the handler, a branch target.
                    entries.push_back(scope.target);
                    facts.targets.push_back(scope.target);
                }
            }
        } else if (const auto error = std::ranges::find(cxx_.errors, function.start,
                                                        &recomp::CxxTableError::start);
                   error != cxx_.errors.end()) {
            facts.unchecked_handler =
                fmt::format("the function's C++ exception tables don't read ({})", error->message);
        } else {
            facts.unchecked_handler = "the function's exception handler is none the engine reads "
                                      "(a C scope table, a C++ handler, or a stack cookie "
                                      "check), so it can't be checked that no try block or catch "
                                      "target is involved";
        }
    }

    const recomp::CodeWalker walker(image_);
    const recomp::WalkResult walk = walker.walk(
        entries, ranges,
        [&](uint32_t rva) { return rva != function.start && discovery_.find(rva) != nullptr; },
        [&](uint32_t rva, const ZydisDecodedInstruction& insn) {
            facts.starts.push_back(rva);
            if (insn.mnemonic == ZYDIS_MNEMONIC_CALL) {
                facts.return_sites.push_back({rva + insn.length, rva});
            }
        });
    facts.targets.insert(facts.targets.end(), walk.branch_targets.begin(),
                         walk.branch_targets.end());
    for (std::vector<uint32_t>* list : {&facts.starts, &facts.targets}) {
        std::ranges::sort(*list);
        const auto [first, last] = std::ranges::unique(*list);
        list->erase(first, last);
    }
    std::ranges::sort(facts.return_sites, {}, &ReturnSite::rva);
    return facts_.emplace(function.start, std::move(facts)).first->second;
}

Window Patcher::plan_window(Kind kind, uint32_t site, bool runs, bool handler_checked) {
    const DiscoveredFunction* function = discovery_.containing(site);
    if (function == nullptr) {
        refuse(kind, site, "it isn't inside a function that function discovery found");
    }
    if (kind == Kind::Replacement && site != function->start) {
        refuse(kind, site,
               fmt::format("a replacement goes at a function's entry, and this function starts "
                           "at {:#x}",
                           function->start));
    }
    Range range{function->start, function->end};
    for (const Range& fragment : function->fragments) {
        if (fragment.contains(site)) {
            range = fragment;
        }
    }
    const FunctionFacts& facts = facts_of(*function);
    if (!std::ranges::binary_search(facts.starts, site)) {
        refuse(kind, site, "it isn't the start of an instruction the function's code reaches");
    }
    // Unwinding out of a call, the unwinder reads the code at the return address to tell whether
    // this frame is in an epilogue, and would take the patch jump for an epilogue's jump out of
    // the function. Every kind, whatever the handler.
    if (const auto it = std::ranges::lower_bound(facts.return_sites, site, {}, &ReturnSite::rva);
        it != facts.return_sites.end() && it->rva == site) {
        refuse(kind, site,
               fmt::format("it is the return address of the call at {:#x}, so unwinding out of "
                           "that call would read the patch jump as an epilogue and unwind this "
                           "frame wrongly; hook the next instruction instead",
                           it->call));
    }
    if (kind == Kind::MidHook && !facts.unchecked_handler.empty() && !handler_checked) {
        refuse(kind, site,
               fmt::format("{}; check by hand and set handler_checked", facts.unchecked_handler));
    }

    // Rule 2: whole instructions inside the function. Only a replacement whose original never
    // runs may cover int3 padding after it, which never runs either.
    Window window;
    window.site = site;
    uint32_t pc = site;
    while (pc - site < kJumpSize) {
        if (!range.contains(pc)) {
            if (runs || kind != Kind::Replacement) {
                refuse(kind, site, "the window runs past the end of the function");
            }
            if (!image_.is_executable(pc) || image_.read<uint8_t>(pc) != kInt3 ||
                discovery_.containing(pc) != nullptr) {
                refuse(kind, site,
                       "the window runs past the end of the function into bytes that aren't "
                       "int3 padding");
            }
            ++pc;
            continue;
        }
        ZydisDecoderContext context;
        WindowInstruction instruction{pc, {}};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder_, &context,
                                                        image_.mapped().data() + pc, range.end - pc,
                                                        &instruction.insn))) {
            refuse(kind, site, fmt::format("the instruction at {:#x} doesn't decode", pc));
        }
        pc += instruction.insn.length;
        window.instructions.push_back(instruction);
    }
    window.size = pc - site;
    const uint32_t end = pc;

    for (const uint32_t target : facts.continuations) {
        if (target > site && target < end) {
            refuse(kind, site,
                   fmt::format("a catch block resumes inside the window, at {:#x}", target));
        }
    }
    for (const uint32_t target : facts.targets) {
        if (target > site && target < end) {
            refuse(kind, site,
                   fmt::format("a jump inside the function lands in the window, at {:#x}", target));
        }
    }
    if (kind == Kind::MidHook && facts.cxx != nullptr && !handler_checked) {
        check_cxx_states(site, end, *facts.cxx);
    }
    if (runs) {
        check_movable(kind, window, *function);
        for (const Range& scope : facts.try_ranges) {
            if (site < scope.end && scope.begin < end) {
                refuse(kind, site,
                       fmt::format("the window is in the __try block [{:#x}, {:#x}), whose "
                                   "handler wouldn't see a fault in the moved instructions",
                                   scope.begin, scope.end));
            }
        }
    }
    for (size_t i = 0; i < trap_candidates_.size(); ++i) {
        const AppliedPatches::Site& trap = trap_candidates_[i];
        const uint32_t trap_end = trap.rva + (trap.xgetbv ? 3 : 2);
        if (trap.rva < end && site < trap_end) {
            if (runs) {
                refuse(kind, site,
                       fmt::format("the window holds the {} at {:#x}, which the loader traps",
                                   trap.xgetbv ? "xgetbv" : "cpuid", trap.rva));
            }
            trap_covered_[i] = true;
        }
    }
    check_free(kind, site, site, end);
    return window;
}

// Rule 4 under a C++ handler: the handler never sees an exception from the hook or the moved
// instructions, so the window must be where it has nothing to do, in state -1.
void Patcher::check_cxx_states(uint32_t site, uint32_t end,
                               const recomp::CxxFunction& function) const {
    for (const int32_t state : function.info.states_in(site, end)) {
        if (state == -1) {
            continue;
        }
        if (const pe::CxxTryBlock* block = function.info.try_block_for(state)) {
            refuse(Kind::MidHook, site,
                   fmt::format("the window is in C++ exception state {}, inside the try block of "
                               "states [{}, {}], whose catch blocks wouldn't see an exception "
                               "from the hook or the moved instructions; check by hand and set "
                               "handler_checked",
                               state, block->try_low, block->try_high));
        }
        refuse(Kind::MidHook, site,
               fmt::format("the window is in C++ exception state {}, where objects are live whose "
                           "destructors wouldn't run if the hook or the moved instructions threw; "
                           "check by hand and set handler_checked",
                           state));
    }
}

// Whether `insn` belongs to an epilogue: it frees the frame or leaves the function. The unwinder
// recognizes an epilogue at the original address by its code, but the moved copy is described
// as prolog, which can't say that the frame is already gone.
bool epilogue_instruction(const ZydisDecodedInstruction& insn, const ZydisDecodedOperand* operands,
                          uint32_t rva, const DiscoveredFunction& function) {
    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_POP: return true;
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_LEA:
    case ZYDIS_MNEMONIC_MOV:
        return insn.operand_count_visible > 0 && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
               operands[0].reg.value == ZYDIS_REGISTER_RSP;
    case ZYDIS_MNEMONIC_JMP:
        if (insn.raw.imm[0].is_relative == 0) {
            return true; // through a register or memory: a tail call, or a switch
        }
        return !function.contains(
            static_cast<uint32_t>(rva + insn.length + insn.raw.imm[0].value.s));
    default: return false;
    }
}

void Patcher::check_movable(Kind kind, const Window& window,
                            const DiscoveredFunction& function) const {
    for (const auto& [rva, insn] : window.instructions) {
        if (function.has_unwind_info) {
            std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
            ZydisDecodedInstruction again;
            if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, image_.mapped().data() + rva,
                                                    insn.length, &again, operands.data())) &&
                epilogue_instruction(again, operands.data(), rva, function)) {
                refuse(kind, window.site,
                       fmt::format("the window holds epilogue code at {:#x}; the unwinder can't "
                                   "describe a moved epilogue",
                                   rva));
            }
        }
        switch (insn.mnemonic) {
        case ZYDIS_MNEMONIC_LOOP:
        case ZYDIS_MNEMONIC_LOOPE:
        case ZYDIS_MNEMONIC_LOOPNE:
        case ZYDIS_MNEMONIC_JCXZ:
        case ZYDIS_MNEMONIC_JECXZ:
        case ZYDIS_MNEMONIC_JRCXZ:
            refuse(kind, window.site,
                   fmt::format("the window holds a loop or jrcxz at {:#x}, which has no long "
                               "form to move it with",
                               rva));
        default: break;
        }
        if (insn.meta.category == ZYDIS_CATEGORY_CALL) {
            refuse(kind, window.site,
                   fmt::format("the window holds a call at {:#x}; moved, it would return into "
                               "the patch arena",
                               rva));
        }
        const bool branch = relative_branch(insn);
        for (const auto& imm : insn.raw.imm) {
            if (imm.is_relative != 0 && !branch) {
                refuse(kind, window.site,
                       fmt::format("the instruction at {:#x} has a relative operand that can't "
                                   "be moved",
                                   rva));
            }
        }
        if (rip_relative(insn) && insn.address_width != 64) {
            refuse(kind, window.site,
                   fmt::format("the instruction at {:#x} uses a 32-bit RIP-relative address", rva));
        }
    }
}

EndMap Patcher::relocate(Kind kind, const Window& window) {
    PatchArena& arena = *applied_.arena;
    const uint64_t start = arena.here();
    EndMap ends;
    for (const auto& [rva, insn] : window.instructions) {
        const uint64_t address = base() + rva;
        const uint64_t next = address + insn.length;
        if (relative_branch(insn)) {
            // A short or near jump becomes a near jump to the same target.
            const uint64_t target = next + static_cast<uint64_t>(insn.raw.imm[0].value.s);
            if (insn.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                arena.bytes({0xE9});
            } else {
                arena.bytes({0x0F, static_cast<uint8_t>(0x80 | (insn.opcode & 0x0F))});
            }
            arena.rel32(target);
        } else {
            const uint64_t copy = arena.here();
            arena.bytes(std::span(reinterpret_cast<const uint8_t*>(address), insn.length));
            if (rip_relative(insn)) {
                const uint64_t target = next + static_cast<uint64_t>(insn.raw.disp.value);
                const auto displacement = static_cast<int64_t>(target - (copy + insn.length));
                if (!fits_rel32(displacement)) {
                    refuse(kind, window.site,
                           fmt::format("the patch arena is too far from {:#x}, which the "
                                       "instruction at {:#x} addresses",
                                       target, rva));
                }
                const auto value = static_cast<int32_t>(displacement);
                std::memcpy(reinterpret_cast<void*>(copy + insn.raw.disp.offset), &value,
                            sizeof(value));
            }
        }
        ends.emplace_back(rva + insn.length - window.site,
                          static_cast<uint32_t>(arena.here() - start));
    }
    return ends;
}

// Unwind info for a copy of the window, `copy_size` bytes with the jump back. The copy is
// described as all prolog: codes for instructions before the site always apply, codes for
// moved instructions apply once their copy has run, and codes for instructions after the window
// never do. Chained info is folded in, all of it applying. The handler is left out: it would
// look up the copy's addresses in tables about the original.
std::optional<std::vector<uint8_t>>
Patcher::moved_unwind_info(const Window& window, const EndMap& ends, uint32_t copy_size) const {
    const pe::RuntimeFunction* entry = pe::find_runtime_function(pdata_, window.site);
    if (entry == nullptr) {
        return std::nullopt; // a leaf: code without unwind info unwinds as one
    }
    if (copy_size > UINT8_MAX) {
        throw kernel::LoadError(
            fmt::format("{}: the moved window at {:#x} is too long", name_, window.site));
    }
    const auto prolog_size = static_cast<uint8_t>(copy_size);
    pe::RuntimeFunction current = pe::resolve_indirect(image_, *entry);
    const uint32_t before = window.site - current.begin;
    std::vector<pe::UnwindCode> codes;
    std::optional<std::pair<uint8_t, uint32_t>> frame;
    for (int depth = 0;; ++depth) {
        if (depth > kMaxChainDepth) {
            throw kernel::LoadError(
                fmt::format("{}: unwind info chained too deep at {:#x}", name_, window.site));
        }
        const pe::UnwindInfo info = pe::read_unwind_info(image_, current.unwind_info);
        for (pe::UnwindCode code : info.codes) {
            if (depth == 0 && code.prolog_offset > before) {
                const uint32_t into = code.prolog_offset - before;
                if (into > window.size) {
                    code.prolog_offset = prolog_size;
                } else {
                    const auto it = std::ranges::find(ends, into, &EndMap::value_type::first);
                    if (it == ends.end()) {
                        throw kernel::LoadError(fmt::format(
                            "{}: an unwind code at {:#x} isn't at an instruction boundary", name_,
                            current.begin + code.prolog_offset));
                    }
                    code.prolog_offset = static_cast<uint8_t>(it->second);
                }
            } else {
                code.prolog_offset = 0;
            }
            if (code.op == pe::UnwindOp::SetFramePointer) {
                const std::pair<uint8_t, uint32_t> this_frame{info.frame_register,
                                                              info.frame_offset};
                if (frame && *frame != this_frame) {
                    throw kernel::LoadError(
                        fmt::format("{}: chained unwind info at {:#x} sets two frame registers",
                                    name_, window.site));
                }
                frame = this_frame;
            }
            codes.push_back(code);
        }
        if (!info.chained) {
            break;
        }
        current = pe::resolve_indirect(image_, *info.chained);
    }
    return encode_unwind_info(prolog_size, frame, codes);
}

void Patcher::jump_from_site(const Window& window, uint64_t target) {
    auto* const site = reinterpret_cast<uint8_t*>(base() + window.site);
    const auto displacement = static_cast<int64_t>(target - (base() + window.site + kJumpSize));
    if (!fits_rel32(displacement)) {
        throw kernel::LoadError(
            fmt::format("{}: the patch arena is out of reach of {:#x}", name_, window.site));
    }
    const auto value = static_cast<int32_t>(displacement);
    site[0] = 0xE9;
    std::memcpy(site + 1, &value, sizeof(value));
    std::memset(site + kJumpSize, kInt3, window.size - kJumpSize);
}

void Patcher::apply(const DataPatch& patch) {
    if (patch.bytes.empty() || patch.bytes.size() > UINT32_MAX ||
        !image_.contains(patch.rva, patch.bytes.size())) {
        refuse(Kind::Data, patch.rva, "its bytes lie outside the image");
    }
    check_expected(Kind::Data, patch.rva, patch.expect);
    const uint32_t end = patch.rva + static_cast<uint32_t>(patch.bytes.size());
    check_free(Kind::Data, patch.rva, patch.rva, end);
    std::memcpy(reinterpret_cast<void*>(base() + patch.rva), patch.bytes.data(),
                patch.bytes.size());
    occupied_.push_back({patch.rva, end, "data patch"});
}

void Patcher::apply(const Replacement& patch) {
    if (patch.function == nullptr) {
        refuse(Kind::Replacement, patch.rva, "it has no function to go to");
    }
    check_expected(Kind::Replacement, patch.rva, patch.expect);
    const bool keeps_original = patch.original != nullptr;
    const Window window = plan_window(Kind::Replacement, patch.rva, keeps_original, false);
    PatchArena& arena = *applied_.arena;

    uint64_t trampoline = 0;
    if (keeps_original) {
        arena.align(16);
        trampoline = arena.here();
        const EndMap ends = relocate(Kind::Replacement, window);
        arena.bytes({0xE9}); // jmp back to the rest of the original
        arena.rel32(base() + window.site + window.size);
        const uint64_t end = arena.here();
        if (const auto unwind =
                moved_unwind_info(window, ends, static_cast<uint32_t>(end - trampoline))) {
            arena.add_function(trampoline, end, *unwind);
        }
    }
    // jmp [rip+0] to the host function, which may be anywhere. It leaves the stack as the call
    // made it, so it needs no unwind info.
    arena.align(16);
    const uint64_t thunk = arena.here();
    arena.bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    arena.u64(reinterpret_cast<uint64_t>(patch.function));

    jump_from_site(window, thunk);
    occupied_.push_back({window.site, window.site + window.size, "replacement"});
    if (keeps_original) {
        originals_.emplace_back(patch.original, trampoline);
    }
}

void Patcher::apply(const MidFunctionHook& patch) {
    if (patch.hook == nullptr) {
        refuse(Kind::MidHook, patch.rva, "it has no hook function");
    }
    check_expected(Kind::MidHook, patch.rva, patch.expect);
    const Window window = plan_window(Kind::MidHook, patch.rva, true, patch.handler_checked);
    PatchArena& arena = *applied_.arena;

    const uint64_t window_slot = arena.slot(0); // the moved window, filled in below
    const uint64_t site_slot = arena.slot(base() + window.site);
    const uint64_t hook_slot = arena.slot(reinterpret_cast<uint64_t>(patch.hook));
    const uint64_t runner_slot = arena.slot(reinterpret_cast<uint64_t>(&run_mid_hook));

    arena.align(16);
    const uint64_t entry = arena.here();
    arena.bytes({0xFF, 0x35}); // push [moved window]: the stub's return address, for unwinding
    arena.rel32(window_slot);
    const uint64_t body = arena.here();
    emit_mid_stub(arena, site_slot, hook_slot, runner_slot, window_slot);
    const uint64_t moved = arena.here();
    const EndMap ends = relocate(Kind::MidHook, window);
    arena.bytes({0xE9}); // jmp back after the window
    arena.rel32(base() + window.site + window.size);
    const uint64_t moved_end = arena.here();
    PatchArena::set_slot(window_slot, moved);

    arena.add_function(body, moved, kStubUnwindInfo);
    if (const auto unwind =
            moved_unwind_info(window, ends, static_cast<uint32_t>(moved_end - moved))) {
        arena.add_function(moved, moved_end, *unwind);
    }
    jump_from_site(window, entry);
    occupied_.push_back({window.site, window.site + window.size, "mid-function hook"});
}

void Patcher::trap_sites() {
    for (size_t i = 0; i < trap_candidates_.size(); ++i) {
        if (trap_covered_[i]) {
            continue; // under a replacement's jump, where it never runs
        }
        const AppliedPatches::Site& site = trap_candidates_[i];
        const char* const what = site.xgetbv ? "xgetbv" : "cpuid";
        const uint32_t end = site.rva + (site.xgetbv ? 3 : 2);
        for (const Occupied& other : occupied_) {
            if (site.rva < other.end && other.begin < end) {
                throw kernel::LoadError(fmt::format("{}: the {} at {:#x} overlaps the {} at {:#x}",
                                                    name_, what, site.rva, other.what,
                                                    other.begin));
            }
        }
        auto* const at = reinterpret_cast<uint8_t*>(base() + site.rva);
        at[0] = 0x0F; // ud2
        at[1] = 0x0B;
        applied_.sites.push_back(site);
    }
}

// xgetbv faults on an index the host CPU doesn't support; the caller then lets the guest see
// the fault instead.
bool try_guest_xgetbv(uint32_t index, uint64_t& value) {
    __try {
        value = cpu::guest_xgetbv(index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

AppliedPatches::AppliedPatches() = default;

AppliedPatches::~AppliedPatches() {
    for (void** original : originals) {
        *original = nullptr;
    }
}

bool AppliedPatches::answer_trap(uint32_t rva, CONTEXT& context) const {
    const auto it = std::ranges::lower_bound(sites, rva, {}, &Site::rva);
    if (it == sites.end() || it->rva != rva) {
        return false;
    }
    if (!it->xgetbv) {
        const cpu::CpuidResult result = cpu::guest_cpuid(static_cast<uint32_t>(context.Rax),
                                                         static_cast<uint32_t>(context.Rcx));
        context.Rax = result.eax;
        context.Rbx = result.ebx;
        context.Rcx = result.ecx;
        context.Rdx = result.edx;
        context.Rip += 2;
        return true;
    }
    uint64_t value = 0;
    if (!try_guest_xgetbv(static_cast<uint32_t>(context.Rcx), value)) {
        return false;
    }
    context.Rax = value & 0xFFFFFFFF;
    context.Rdx = value >> 32;
    context.Rip += 3;
    return true;
}

std::unique_ptr<AppliedPatches> apply_patches(std::string_view module_name, const pe::Image& image,
                                              const kernel::MappedImage& mapped,
                                              const ModulePatches& patches) {
    auto applied = std::make_unique<AppliedPatches>();
    Patcher(module_name, image, mapped, patches, *applied).run();
    return applied;
}

} // namespace g6::loader
