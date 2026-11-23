#include "cxx_exceptions.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "code_walker.h"
#include "g6/pe/directories.h"
#include "g6/pe/unwind.h"

namespace g6::recomp {
namespace {

// IMAGE_GUARD_EH_CONTINUATION_TABLE_PRESENT in the load config's guard flags.
constexpr uint32_t kGuardEhContinuationTablePresent = 0x00400000;

constexpr int kMaxJumpHops = 4;

// A handler that only checks the stack cookie is a handful of instructions; a longer one is
// something else.
constexpr int kMaxHandlerInstructions = 16;
// How many more direct calls or jumps past the handler's own call the cookie read may be.
constexpr int kMaxCookieDepth = 2;
// Bytes walked from a call or jump target that function discovery didn't find, such as a leaf
// function without unwind info.
constexpr uint32_t kProbeBytes = 64;

enum class HandlerKind : uint8_t { kUnknown, kFh3, kFh4 };

struct Handler {
    HandlerKind kind = HandlerKind::kUnknown;
    bool gs_wrapper = false;
};

HandlerKind kind_of_name(std::string_view name) {
    if (name == "__CxxFrameHandler3") {
        return HandlerKind::kFh3;
    }
    if (name == "__CxxFrameHandler4") {
        return HandlerKind::kFh4;
    }
    return HandlerKind::kUnknown;
}

void sort_unique(std::vector<uint32_t>& list) {
    std::ranges::sort(list);
    const auto [first, last] = std::ranges::unique(list);
    list.erase(first, last);
}

std::vector<Range> ranges_of(const DiscoveredFunction& function) {
    std::vector<Range> ranges{{function.start, function.end}};
    ranges.insert(ranges.end(), function.fragments.begin(), function.fragments.end());
    return ranges;
}

bool rip_relative(const ZydisDecodedInstruction& insn) {
    return (insn.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0 && insn.raw.modrm.mod == 0 &&
           insn.raw.modrm.rm == 5;
}

uint32_t rip_target(uint32_t rva, const ZydisDecodedInstruction& insn) {
    return static_cast<uint32_t>(int64_t{rva} + insn.length + insn.raw.disp.value);
}

// Whether the instruction writes any part of rax.
bool writes_rax(const ZydisDecodedInstruction& insn,
                std::span<const ZydisDecodedOperand> operands) {
    return std::ranges::any_of(operands.first(insn.operand_count), [](const auto& operand) {
        return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
               (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0 &&
               ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, operand.reg.value) ==
                   ZYDIS_REGISTER_RAX;
    });
}

// Whether the instruction sets all of rax to 1: `mov eax, 1` or `mov rax, 1`.
bool sets_rax_to_one(const ZydisDecodedInstruction& insn,
                     std::span<const ZydisDecodedOperand> operands) {
    return insn.mnemonic == ZYDIS_MNEMONIC_MOV && insn.operand_count_visible == 2 &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           (operands[0].reg.value == ZYDIS_REGISTER_EAX ||
            operands[0].reg.value == ZYDIS_REGISTER_RAX) &&
           operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[1].imm.value.u == 1;
}

// Whether the instruction branches, traps, or enters the system, other than a call or a return.
bool other_control_transfer(const ZydisDecodedInstruction& insn) {
    switch (insn.meta.category) {
    case ZYDIS_CATEGORY_COND_BR:
    case ZYDIS_CATEGORY_UNCOND_BR:
    case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_SYSCALL:
    case ZYDIS_CATEGORY_SYSTEM: return true;
    default: return insn.mnemonic == ZYDIS_MNEMONIC_UD2;
    }
}

class Mapper {
public:
    Mapper(const pe::Image& image, const DiscoveryResult& discovery)
        : image_(image), discovery_(discovery), walker_(image) {
        walker_.set_table_starts(discovery.table_starts);
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    }

    CxxExceptionMap run();

private:
    void index_names();
    std::optional<uint32_t> thunk_slot(uint32_t rva) const;
    uint32_t follow_jumps(uint32_t rva) const;
    HandlerKind kind_at(uint32_t rva) const;
    Handler classify(uint32_t handler);
    std::optional<uint32_t> sole_callee(uint32_t handler) const;
    bool reads_cookie(uint32_t rva) const;
    bool only_checks_cookie(uint32_t handler);
    void read_function(const DiscoveredFunction& function, const pe::UnwindInfo& info);
    void read_config();
    const std::vector<uint32_t>& returned_by(uint32_t funclet);
    CxxFunction* owner_of(uint32_t rva);
    void account_for_catches();
    void assign_continuations();

    const pe::Image& image_;
    const DiscoveryResult& discovery_;
    CodeWalker walker_;
    ZydisDecoder decoder_{};
    CxxExceptionMap map_;
    std::unordered_map<uint32_t, HandlerKind> slots_;   // import address slots of the handlers
    std::unordered_map<uint32_t, HandlerKind> exports_; // the handlers, when this module has them
    std::unordered_map<uint32_t, Handler> handlers_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> returned_;
    std::optional<uint32_t> cookie_;                   // the security cookie's RVA
    std::unordered_map<uint32_t, bool> cookie_checks_; // by handler, after its jumps
};

void Mapper::index_names() {
    try {
        for (const pe::ImportedModule& module : pe::read_imports(image_)) {
            for (const pe::ImportedSymbol& symbol : module.symbols) {
                if (const HandlerKind kind = kind_of_name(symbol.name);
                    kind != HandlerKind::kUnknown) {
                    slots_[symbol.iat_rva] = kind;
                }
            }
        }
    } catch (const pe::FormatError&) { // NOLINT(bugprone-empty-catch)
        // No usable imports: the handlers can still be found by the magic number.
    }
    try {
        for (const pe::Export& entry : pe::read_exports(image_).exports) {
            if (const HandlerKind kind = kind_of_name(entry.name);
                kind != HandlerKind::kUnknown && entry.rva != 0) {
                exports_[entry.rva] = kind;
            }
        }
    } catch (const pe::FormatError&) { // NOLINT(bugprone-empty-catch)
        // No usable exports: the handlers can still be found through imports or the magic number.
    }
}

// The import address slot an import thunk (`jmp [rip+disp32]`, with or without REX.W) at `rva`
// jumps through.
std::optional<uint32_t> Mapper::thunk_slot(uint32_t rva) const {
    if (!image_.contains(rva, 7)) {
        return std::nullopt;
    }
    const auto byte = [&](uint32_t offset) { return image_.read<uint8_t>(rva + offset); };
    if (byte(0) == 0xFF && byte(1) == 0x25) {
        return static_cast<uint32_t>(int64_t{rva} + 6 + image_.read<int32_t>(rva + 2));
    }
    if (byte(0) == 0x48 && byte(1) == 0xFF && byte(2) == 0x25) {
        return static_cast<uint32_t>(int64_t{rva} + 7 + image_.read<int32_t>(rva + 3));
    }
    return std::nullopt;
}

HandlerKind Mapper::kind_at(uint32_t rva) const {
    if (const auto it = exports_.find(rva); it != exports_.end()) {
        return it->second;
    }
    if (const auto slot = thunk_slot(rva)) {
        if (const auto it = slots_.find(*slot); it != slots_.end()) {
            return it->second;
        }
    }
    return HandlerKind::kUnknown;
}

// Where a chain of direct jumps starting at `rva` ends, such as an incremental-link thunk in front
// of the handler.
uint32_t Mapper::follow_jumps(uint32_t rva) const {
    for (int hop = 0; hop < kMaxJumpHops && image_.contains(rva, 5) && image_.is_executable(rva);
         ++hop) {
        int64_t target = 0;
        if (image_.read<uint8_t>(rva) == 0xE9) {
            target = int64_t{rva} + 5 + image_.read<int32_t>(rva + 1);
        } else if (image_.read<uint8_t>(rva) == 0xEB) {
            target = int64_t{rva} + 2 + image_.read<int8_t>(rva + 1);
        } else {
            break;
        }
        if (target <= 0 || target > int64_t{UINT32_MAX}) {
            break;
        }
        rva = static_cast<uint32_t>(target);
    }
    return rva;
}

// What the handler at `rva` is: one of the C++ handlers, a GS wrapper that calls exactly one of
// them, or something else.
Handler Mapper::classify(uint32_t rva) {
    if (const auto it = handlers_.find(rva); it != handlers_.end()) {
        return it->second;
    }
    const uint32_t handler = follow_jumps(rva);
    Handler result{kind_at(handler), false};
    const DiscoveredFunction* function = discovery_.find(handler);
    if (result.kind == HandlerKind::kUnknown && function != nullptr) {
        std::unordered_set<HandlerKind> kinds;
        const auto note_slot = [&](uint32_t slot) {
            if (const auto it = slots_.find(slot); it != slots_.end()) {
                kinds.insert(it->second);
            }
        };
        const std::vector<Range> ranges = ranges_of(*function);
        const std::vector<uint32_t> entries{handler};
        const WalkResult walk = walker_.walk(
            entries, ranges,
            [&](uint32_t address) {
                return address != handler && discovery_.find(address) != nullptr;
            },
            [&](uint32_t site, const ZydisDecodedInstruction& insn) {
                if ((insn.mnemonic == ZYDIS_MNEMONIC_CALL || insn.mnemonic == ZYDIS_MNEMONIC_JMP) &&
                    rip_relative(insn)) {
                    note_slot(rip_target(site, insn));
                }
            });
        for (const std::vector<uint32_t>* targets : {&walk.call_targets, &walk.tail_targets}) {
            for (const uint32_t target : *targets) {
                if (const HandlerKind kind = kind_at(follow_jumps(target));
                    kind != HandlerKind::kUnknown) {
                    kinds.insert(kind);
                }
            }
        }
        if (kinds.size() == 1) {
            result = {*kinds.begin(), true};
        }
    }
    return handlers_.emplace(rva, result).first->second;
}

// The function the handler at `rva` calls, when the handler's code runs straight through: one
// direct call, no other branch, then a return of 1 (ExceptionContinueSearch). Such a handler never
// catches and leaves every exception to the frames above. nullopt for any other shape.
std::optional<uint32_t> Mapper::sole_callee(uint32_t rva) const {
    const DiscoveredFunction* function = discovery_.find(rva);
    if (function == nullptr) {
        return std::nullopt;
    }
    const auto mapped = image_.mapped();
    const auto end = static_cast<uint32_t>(std::min<size_t>(function->end, mapped.size()));
    std::optional<uint32_t> callee;
    bool returns_search = false; // rax holds 1, set after the call
    uint32_t pc = rva;
    for (int count = 0; count < kMaxHandlerInstructions && pc < end; ++count) {
        ZydisDecodedInstruction insn;
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, mapped.data() + pc, end - pc, &insn,
                                                 operands.data()))) {
            return std::nullopt;
        }
        if (insn.mnemonic == ZYDIS_MNEMONIC_RET) {
            return returns_search ? callee : std::nullopt;
        }
        if (insn.mnemonic == ZYDIS_MNEMONIC_CALL) {
            if (callee || insn.raw.imm[0].is_relative == 0) {
                return std::nullopt; // a second call, or one through a pointer
            }
            const int64_t target = int64_t{pc} + insn.length + insn.raw.imm[0].value.s;
            if (target <= 0 || target > int64_t{UINT32_MAX} ||
                !image_.is_executable(static_cast<uint32_t>(target))) {
                return std::nullopt;
            }
            callee = static_cast<uint32_t>(target);
            returns_search = false;
        } else if (other_control_transfer(insn)) {
            return std::nullopt;
        } else if (writes_rax(insn, operands)) {
            returns_search = callee.has_value() && sets_rax_to_one(insn, operands);
        }
        pc += insn.length;
    }
    return std::nullopt;
}

// Whether the code at `rva`, or code it reaches through at most kMaxCookieDepth more direct calls
// or jumps, reads the module's security cookie.
bool Mapper::reads_cookie(uint32_t rva) const {
    std::vector<uint32_t> level{rva};
    std::unordered_set<uint32_t> seen;
    for (int depth = 0; depth <= kMaxCookieDepth && !level.empty(); ++depth) {
        std::vector<uint32_t> next;
        for (const uint32_t target : level) {
            const uint32_t start = follow_jumps(target);
            if (!seen.insert(start).second || !image_.is_executable(start)) {
                continue;
            }
            const DiscoveredFunction* function = discovery_.find(start);
            const std::vector<Range> ranges =
                function != nullptr
                    ? ranges_of(*function)
                    : std::vector<Range>{
                          {start, static_cast<uint32_t>(std::min<uint64_t>(
                                      uint64_t{start} + kProbeBytes, image_.size_of_image()))}};
            const std::vector<uint32_t> entries{start};
            bool found = false;
            const WalkResult walk = walker_.walk(
                entries, ranges,
                [&](uint32_t address) {
                    return address != start && discovery_.find(address) != nullptr;
                },
                [&](uint32_t site, const ZydisDecodedInstruction& insn) {
                    found = found || (rip_relative(insn) && rip_target(site, insn) == *cookie_);
                });
            if (found) {
                return true;
            }
            next.insert(next.end(), walk.call_targets.begin(), walk.call_targets.end());
            next.insert(next.end(), walk.tail_targets.begin(), walk.tail_targets.end());
        }
        level = std::move(next);
    }
    return false;
}

// Whether the handler at `rva` does nothing but check the stack cookie, the way the C runtime's
// __GSHandlerCheck does: told by what it does, not by its bytes. Its own code makes one call and
// returns ExceptionContinueSearch, and that call reaches a read of the module's security cookie.
bool Mapper::only_checks_cookie(uint32_t rva) {
    const uint32_t handler = follow_jumps(rva);
    if (const auto it = cookie_checks_.find(handler); it != cookie_checks_.end()) {
        return it->second;
    }
    bool result = false;
    if (cookie_) {
        if (const auto callee = sole_callee(handler)) {
            result = reads_cookie(*callee);
        }
    }
    return cookie_checks_.emplace(handler, result).first->second;
}

void Mapper::read_function(const DiscoveredFunction& function, const pe::UnwindInfo& info) {
    ++map_.stats.handlers;
    Handler handler = classify(*info.handler_rva);
    bool by_magic = false;
    if (handler.kind == HandlerKind::kUnknown && image_.contains(info.handler_data_rva, 4) &&
        pe::has_fh3_magic(image_, image_.read<uint32_t>(info.handler_data_rva))) {
        handler.kind = HandlerKind::kFh3;
        by_magic = true;
    }
    if (handler.kind == HandlerKind::kUnknown) {
        if (only_checks_cookie(*info.handler_rva)) {
            ++map_.stats.gs_only;
            map_.gs_only.push_back(function.start);
        } else {
            ++map_.stats.other_handlers;
        }
        return;
    }
    try {
        CxxFunction entry;
        entry.start = function.start;
        entry.gs_wrapper = handler.gs_wrapper;
        entry.tables = image_.read<uint32_t>(info.handler_data_rva);
        entry.info = handler.kind == HandlerKind::kFh3
                         ? pe::read_fh3_func_info(image_, entry.tables)
                         : pe::read_fh4_func_info(image_, entry.tables, function.start);
        for (const pe::CxxStateEntry& state : entry.info.ip_to_state) {
            if (discovery_.containing(state.rva) == nullptr) {
                throw pe::FormatError(fmt::format(
                    "C++ EH tables at {:#x}: state {} starts at {:#x}, outside any function",
                    entry.tables, state.state, state.rva));
            }
        }
        ++(handler.kind == HandlerKind::kFh3 ? map_.stats.fh3 : map_.stats.fh4);
        map_.stats.by_magic += by_magic ? 1 : 0;
        map_.stats.gs_wrapped += handler.gs_wrapper ? 1 : 0;
        map_.functions.push_back(std::move(entry));
    } catch (const pe::FormatError& error) {
        ++map_.stats.unreadable;
        map_.errors.push_back({function.start, error.what()});
    }
}

// The security cookie's address and the CFG EH continuation table, from the load config.
void Mapper::read_config() {
    try {
        const auto config = pe::read_load_config(image_);
        if (!config) {
            return;
        }
        if (config->security_cookie_va != 0) {
            cookie_ = image_.try_va_to_rva(config->security_cookie_va);
        }
        if ((config->guard_flags & kGuardEhContinuationTablePresent) != 0) {
            map_.guard_table = true;
            map_.guard = config->guard_eh_continuations;
            sort_unique(map_.guard);
        }
    } catch (const pe::FormatError&) { // NOLINT(bugprone-empty-catch)
        // Without a readable table, the other two sources have to account for every catch, and
        // without the cookie's address no handler counts as a cookie check.
    }
}

// The C++ function whose code holds `rva`, or null.
CxxFunction* Mapper::owner_of(uint32_t rva) {
    const DiscoveredFunction* function = discovery_.containing(rva);
    if (function == nullptr) {
        return nullptr;
    }
    const auto it =
        std::ranges::lower_bound(map_.functions, function->start, {}, &CxxFunction::start);
    return it != map_.functions.end() && it->start == function->start ? &*it : nullptr;
}

// The code addresses a catch funclet takes with a RIP-relative `lea`: where it tells the C++
// handler to resume, when the handler doesn't record that itself. The funclet's whole range is
// decoded front to back, not only the code its entry reaches: compilers leave blocks that nothing
// branches to, and the CFG EH continuation table still lists where those would resume.
const std::vector<uint32_t>& Mapper::returned_by(uint32_t funclet) {
    if (const auto it = returned_.find(funclet); it != returned_.end()) {
        return it->second;
    }
    std::vector<uint32_t> targets;
    if (const DiscoveredFunction* function = discovery_.find(funclet)) {
        const auto mapped = image_.mapped();
        for (const Range& range : ranges_of(*function)) {
            const auto end = static_cast<uint32_t>(std::min<size_t>(range.end, mapped.size()));
            for (uint32_t rva = range.begin; rva < end;) {
                ZydisDecoderContext context;
                ZydisDecodedInstruction insn;
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
                        &decoder_, &context, mapped.data() + rva, end - rva, &insn))) {
                    ++rva; // resynchronize on the next byte
                    continue;
                }
                if (insn.mnemonic == ZYDIS_MNEMONIC_LEA && rip_relative(insn)) {
                    const uint32_t target = rip_target(rva, insn);
                    if (const DiscoveredFunction* owner = discovery_.containing(target);
                        owner != nullptr && owner->start != target) {
                        targets.push_back(target);
                    }
                }
                rva += insn.length;
            }
        }
        sort_unique(targets);
    }
    return returned_.emplace(funclet, std::move(targets)).first->second;
}

// Finds where each catch resumes and which functions have a catch no source accounts for. A
// continuation must lie in a function that shares the catch's tables: the function that owns the
// try block, a separated part of it, or, for a catch nested in a catch, the outer catch funclet.
void Mapper::account_for_catches() {
    std::unordered_set<uint32_t> funclets;
    for (const CxxFunction& function : map_.functions) {
        for (const pe::CxxTryBlock& block : function.info.try_blocks) {
            for (const pe::CxxCatch& handler : block.catches) {
                funclets.insert(handler.handler);
            }
        }
        funclets.insert(function.info.cleanup_funclets.begin(),
                        function.info.cleanup_funclets.end());
    }
    const auto shares_tables = [&](uint32_t rva, uint32_t tables) {
        const CxxFunction* owner = owner_of(rva);
        return owner != nullptr && owner->tables == tables;
    };

    for (CxxFunction& function : map_.functions) {
        function.funclet = function.info.catch_funclet || funclets.contains(function.start);
        bool accounted = true;
        bool has_catch = false;
        for (const pe::CxxTryBlock& block : function.info.try_blocks) {
            for (const pe::CxxCatch& handler : block.catches) {
                has_catch = true;
                bool found = map_.guard_table;
                for (const uint32_t target : handler.continuations) {
                    if (shares_tables(target, function.tables)) {
                        map_.recorded.push_back(target);
                        found = true;
                    }
                }
                if (handler.continuations.empty()) {
                    for (const uint32_t target : returned_by(handler.handler)) {
                        if (shares_tables(target, function.tables)) {
                            map_.returned.push_back(target);
                            found = true;
                        }
                    }
                }
                accounted = accounted && found;
            }
        }
        function.verifiable = !function.funclet && accounted;
        if (function.funclet) {
            ++map_.stats.funclets;
        } else if (has_catch) {
            ++map_.stats.with_catches;
            map_.stats.unverifiable += function.verifiable ? 0 : 1;
        }
    }
    sort_unique(map_.recorded);
    sort_unique(map_.returned);
    if (map_.guard_table) {
        for (const std::vector<uint32_t>* found : {&map_.recorded, &map_.returned}) {
            for (const uint32_t target : *found) {
                map_.stats.stray_continuations +=
                    std::ranges::binary_search(map_.guard, target) ? 0 : 1;
            }
        }
    }
}

void Mapper::assign_continuations() {
    for (const std::vector<uint32_t>* source : {&map_.guard, &map_.recorded, &map_.returned}) {
        map_.continuations.insert(map_.continuations.end(), source->begin(), source->end());
    }
    sort_unique(map_.continuations);
    for (const uint32_t target : map_.continuations) {
        if (CxxFunction* owner = owner_of(target)) {
            owner->continuations.push_back(target);
        }
    }
}

CxxExceptionMap Mapper::run() {
    index_names();
    read_config();
    if (!image_.directory(pe::Directory::Exception).present()) {
        assign_continuations();
        return std::move(map_);
    }
    std::vector<pe::RuntimeFunction> pdata;
    try {
        pdata = pe::read_runtime_functions(image_);
    } catch (const pe::FormatError&) { // NOLINT(bugprone-empty-catch)
        // Function discovery reports a broken exception directory.
    }
    for (const DiscoveredFunction& function : discovery_.functions) {
        const pe::RuntimeFunction* entry = pe::find_runtime_function(pdata, function.start);
        if (!function.has_unwind_info || entry == nullptr || entry->begin != function.start) {
            continue;
        }
        try {
            const pe::UnwindInfo info =
                pe::read_unwind_info(image_, pe::resolve_indirect(image_, *entry).unwind_info);
            if (info.has_handler() && info.handler_rva) {
                read_function(function, info);
            }
        } catch (const pe::FormatError&) { // NOLINT(bugprone-empty-catch)
            // Function discovery counts unwind errors.
        }
    }
    map_.stats.gs_handlers = static_cast<size_t>(
        std::ranges::count_if(cookie_checks_, [](const auto& entry) { return entry.second; }));
    account_for_catches();
    assign_continuations();
    return std::move(map_);
}

} // namespace

const CxxFunction* CxxExceptionMap::find(uint32_t start) const {
    const auto it = std::ranges::lower_bound(functions, start, {}, &CxxFunction::start);
    return it != functions.end() && it->start == start ? &*it : nullptr;
}

bool CxxExceptionMap::is_gs_only(uint32_t start) const {
    return std::ranges::binary_search(gs_only, start);
}

CxxExceptionMap map_cxx_exceptions(const pe::Image& image, const DiscoveryResult& discovery) {
    return Mapper(image, discovery).run();
}

} // namespace g6::recomp
