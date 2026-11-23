#include "function_emitter.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <fmt/format.h>

#include "g6/pe/directories.h"

namespace g6::recomp {
namespace {

constexpr std::array<const char*, 16> kGprNames = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                                   "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                                   "r12", "r13", "r14", "r15"};
constexpr int kRsp = 4;
constexpr uint64_t kFastFailVector = 0x29;

std::string hex(uint64_t value) {
    return fmt::format("{:#x}ull", value);
}

const char* utype(unsigned bits) {
    switch (bits) {
    case 8: return "uint8_t";
    case 16: return "uint16_t";
    case 32: return "uint32_t";
    case 64: return "uint64_t";
    default: throw Unsupported{};
    }
}

const char* stype(unsigned bits) {
    switch (bits) {
    case 8: return "int8_t";
    case 16: return "int16_t";
    case 32: return "int32_t";
    case 64: return "int64_t";
    default: throw Unsupported{};
    }
}

int gpr_index(ZydisRegister reg) {
    switch (ZydisRegisterGetClass(reg)) {
    case ZYDIS_REGCLASS_GPR8:
    case ZYDIS_REGCLASS_GPR16:
    case ZYDIS_REGCLASS_GPR32:
    case ZYDIS_REGCLASS_GPR64: {
        const ZydisRegister full =
            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
        return ZydisRegisterGetId(full);
    }
    default: return -1;
    }
}

int vec_index(ZydisRegister reg) {
    switch (ZydisRegisterGetClass(reg)) {
    case ZYDIS_REGCLASS_XMM:
    case ZYDIS_REGCLASS_YMM: return ZydisRegisterGetId(reg);
    default: return -1;
    }
}

bool is_high_byte(ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH ||
           reg == ZYDIS_REGISTER_BH;
}

// The condition a Jcc, SETcc, or CMOVcc tests, from the suffix of its mnemonic.
std::string condition(ZydisMnemonic mnemonic) {
    std::string_view name = ZydisMnemonicGetString(mnemonic);
    for (const std::string_view prefix : {"cmov", "set", "j"}) {
        if (name.starts_with(prefix)) {
            name.remove_prefix(prefix.size());
            break;
        }
    }
    static const std::unordered_map<std::string_view, const char*> kConditions = {
        {"o", "f.of"},
        {"no", "!f.of"},
        {"b", "f.cf"},
        {"nb", "!f.cf"},
        {"z", "f.zf"},
        {"nz", "!f.zf"},
        {"be", "(f.cf || f.zf)"},
        {"nbe", "(!f.cf && !f.zf)"},
        {"s", "f.sf"},
        {"ns", "!f.sf"},
        {"p", "f.pf"},
        {"np", "!f.pf"},
        {"l", "(f.sf != f.of)"},
        {"nl", "(f.sf == f.of)"},
        {"le", "(f.zf || f.sf != f.of)"},
        {"nle", "(!f.zf && f.sf == f.of)"},
    };
    const auto it = kConditions.find(name);
    if (it == kConditions.end()) {
        throw Unsupported{};
    }
    return it->second;
}

bool is_trap(const ZydisDecodedInstruction& insn) {
    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_INT1:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_UD0:
    case ZYDIS_MNEMONIC_UD1:
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_HLT: return true;
    default: return false;
    }
}

} // namespace

void EmitStats::merge(const EmitStats& other) {
    functions += other.functions;
    complete_functions += other.complete_functions;
    instructions += other.instructions;
    unimplemented += other.unimplemented;
    decode_errors += other.decode_errors;
    indirect_calls += other.indirect_calls;
    indirect_jumps += other.indirect_jumps;
    jump_tables += other.jump_tables;
    for (const auto& [mnemonic, count] : other.missing) {
        missing[mnemonic] += count;
    }
}

ModuleInfo::ModuleInfo(const pe::Image& image_in, const DiscoveryResult& discovery_in,
                       uint64_t base_in)
    : image(image_in), discovery(discovery_in), base(base_in) {
    for (const pe::Relocation& reloc : pe::read_relocations(image)) {
        if (reloc.type == pe::kRelocDir64) {
            dir64_relocations.insert(reloc.rva);
        }
    }
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
}

std::string function_name(uint64_t va) {
    return fmt::format("sub_{:X}", va);
}

FunctionEmitter::FunctionEmitter(const ModuleInfo& module, const DiscoveredFunction& function)
    : module_(module), function_(function) {
    for (const JumpTable& table : function.jump_tables) {
        tables_.emplace(table.jump, &table);
    }
}

bool FunctionEmitter::in_function(uint32_t rva) const {
    return function_.contains(rva);
}

// A branch target that stays inside this function: in its ranges and not another function's
// entry (a jump there is a tail call).
bool FunctionEmitter::is_internal_target(uint32_t rva) const {
    return in_function(rva) && (rva == function_.start || !module_.is_function(rva));
}

// Decodes every instruction reachable from the entry inside the function's ranges.
void FunctionEmitter::collect() {
    const auto mapped = module_.image.mapped();
    std::vector<uint32_t> worklist{function_.start};
    std::unordered_set<uint32_t> visited;
    while (!worklist.empty()) {
        const uint32_t pc = worklist.back();
        worklist.pop_back();
        if (!visited.insert(pc).second) {
            continue;
        }
        Insn insn;
        insn.rva = pc;
        insn.decoded =
            pc < mapped.size() &&
            ZYAN_SUCCESS(ZydisDecoderDecodeFull(&module_.decoder, mapped.data() + pc,
                                                mapped.size() - pc, &insn.insn, insn.ops));
        insns_.push_back(insn);
        if (!insn.decoded) {
            continue;
        }
        const ZydisDecodedInstruction& d = insn.insn;
        const uint32_t next = insn.next();
        const auto follow = [&](uint32_t target) {
            if (is_internal_target(target)) {
                labels_.insert(target);
                worklist.push_back(target);
            }
        };
        const auto relative_target = [&]() -> int64_t {
            return int64_t{next} + d.raw.imm[0].value.s;
        };
        if (d.meta.category == ZYDIS_CATEGORY_RET || is_trap(d)) {
            continue;
        }
        if (d.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
            if (d.raw.imm[0].is_relative) {
                const int64_t target = relative_target();
                if (target >= 0 && target < int64_t{module_.image.size_of_image()}) {
                    follow(static_cast<uint32_t>(target));
                }
            } else if (const auto it = tables_.find(pc); it != tables_.end()) {
                for (const uint32_t target : it->second->targets) {
                    follow(target);
                }
            }
            continue;
        }
        if (d.meta.category == ZYDIS_CATEGORY_COND_BR) {
            const int64_t target = relative_target();
            if (target >= 0 && target < int64_t{module_.image.size_of_image()}) {
                follow(static_cast<uint32_t>(target));
            }
        }
        if (is_internal_target(next) && next != function_.start) {
            worklist.push_back(next);
        }
    }
    std::ranges::sort(insns_, {}, &Insn::rva);
    for (size_t i = 0; i < insns_.size(); ++i) {
        index_.emplace(insns_[i].rva, i);
    }
    // Sequential flow that doesn't reach the next instruction in address order (overlapping
    // code, or a fall-through into the entry) needs an explicit goto.
    for (size_t i = 0; i < insns_.size(); ++i) {
        const Insn& insn = insns_[i];
        if (!insn.decoded) {
            continue;
        }
        const uint32_t next = insn.next();
        const bool adjacent = i + 1 < insns_.size() && insns_[i + 1].rva == next;
        if (!adjacent && index_.contains(next)) {
            labels_.insert(next);
        }
    }
    if (!insns_.empty() && insns_.front().rva != function_.start) {
        labels_.insert(function_.start);
    }
}

void FunctionEmitter::analyze_registers() {
    const auto note = [this](ZydisRegister reg, bool written) {
        if (const int g = gpr_index(reg); g >= 0) {
            gpr_used_ |= static_cast<uint16_t>(1u << g);
            if (written) {
                gpr_written_ |= static_cast<uint16_t>(1u << g);
            }
        } else if (const int v = vec_index(reg); v >= 0) {
            vec_used_ |= static_cast<uint16_t>(1u << v);
            if (written) {
                vec_written_ |= static_cast<uint16_t>(1u << v);
            }
        }
    };
    for (const Insn& insn : insns_) {
        if (!insn.decoded) {
            continue;
        }
        const bool zero_upper = insn.insn.mnemonic == ZYDIS_MNEMONIC_VZEROUPPER ||
                                insn.insn.mnemonic == ZYDIS_MNEMONIC_VZEROALL;
        for (int i = 0; i < int{insn.insn.operand_count}; ++i) {
            const ZydisDecodedOperand& operand = insn.ops[i];
            if (zero_upper && operand.visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN) {
                continue; // handled through ctx for registers this function doesn't use
            }
            if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                note(operand.reg.value, (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0);
            } else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                note(operand.mem.base, false);
                note(operand.mem.index, false);
            }
        }
    }
    // vzeroupper changes every register; the ones this function holds in locals must be
    // written back.
    for (const Insn& insn : insns_) {
        if (insn.decoded && (insn.insn.mnemonic == ZYDIS_MNEMONIC_VZEROUPPER ||
                             insn.insn.mnemonic == ZYDIS_MNEMONIC_VZEROALL)) {
            vec_written_ |= vec_used_;
            break;
        }
    }
    gpr_used_ |= 1u << kRsp;
    gpr_written_ |= 1u << kRsp;
}

void FunctionEmitter::line(std::string_view text) {
    body_ += "    ";
    body_ += text;
    body_ += '\n';
}

std::string FunctionEmitter::store_registers() const {
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if ((gpr_written_ >> i) & 1) {
            out += fmt::format("ctx.gpr[{}] = {}; ", i, kGprNames[i]);
        }
    }
    for (int i = 0; i < 16; ++i) {
        if ((vec_written_ >> i) & 1) {
            out += fmt::format("ctx.ymm[{0}] = ymm{0}; ", i);
        }
    }
    return out;
}

std::string FunctionEmitter::load_registers() const {
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if ((gpr_used_ >> i) & 1) {
            out += fmt::format("{} = ctx.gpr[{}]; ", kGprNames[i], i);
        }
    }
    for (int i = 0; i < 16; ++i) {
        if ((vec_used_ >> i) & 1) {
            out += fmt::format("ymm{0} = ctx.ymm[{0}]; ", i);
        }
    }
    return out;
}

std::string FunctionEmitter::emit(EmitStats& stats) {
    collect();
    analyze_registers();
    const uint64_t entry_va = module_.va(function_.start);

    std::string out = fmt::format("void {}(GuestContext& ctx) {{\n", function_name(entry_va));
    for (int i = 0; i < 16; ++i) {
        if ((gpr_used_ >> i) & 1) {
            out += fmt::format("    uint64_t {} = ctx.gpr[{}];\n", kGprNames[i], i);
        }
    }
    for (int i = 0; i < 16; ++i) {
        if ((vec_used_ >> i) & 1) {
            out += fmt::format("    __m256i ymm{0} = ctx.ymm[{0}];\n", i);
        }
    }
    out += "    Flags f{};\n";

    if (!insns_.empty() && insns_.front().rva != function_.start) {
        line(fmt::format("goto loc_{:X};", entry_va));
    }
    for (size_t i = 0; i < insns_.size(); ++i) {
        const Insn& insn = insns_[i];
        if (labels_.contains(insn.rva)) {
            body_ += fmt::format("loc_{:X}:;\n", module_.va(insn.rva));
        }
        current_ = &insn;
        emit_instruction(insn);
        emit_fallthrough(insn, i + 1 < insns_.size() ? &insns_[i + 1] : nullptr);
    }
    out += body_;
    out += "}\n\n";

    stats_.functions = 1;
    stats_.complete_functions = stats_.unimplemented == 0 && stats_.decode_errors == 0 ? 1 : 0;
    stats.merge(stats_);
    return out;
}

void FunctionEmitter::emit_instruction(const Insn& insn) {
    if (!insn.decoded) {
        ++stats_.decode_errors;
        line(fmt::format("// {:#x}: (does not decode)", pc()));
        emit_trap(insn, "instruction does not decode");
        return;
    }
    ++stats_.instructions;
    char text[256] = {};
    ZydisFormatterFormatInstruction(&module_.formatter, &insn.insn, insn.ops,
                                    insn.insn.operand_count_visible, text, sizeof(text), pc(),
                                    nullptr);
    line(fmt::format("// {:#x}: {}", pc(), text));

    const ZydisDecodedInstruction& d = insn.insn;
    if (is_trap(d)) {
        if (d.mnemonic == ZYDIS_MNEMONIC_INT && d.raw.imm[0].value.u == kFastFailVector) {
            emit_trap(insn, "fast fail (__fastfail)");
        } else {
            emit_trap(insn, fmt::format("trap ({})", ZydisMnemonicGetString(d.mnemonic)));
        }
        return;
    }
    switch (d.meta.category) {
    case ZYDIS_CATEGORY_RET: emit_return(insn); return;
    case ZYDIS_CATEGORY_CALL: emit_call(insn); return;
    case ZYDIS_CATEGORY_UNCOND_BR: emit_jump(insn); return;
    case ZYDIS_CATEGORY_COND_BR: emit_conditional_jump(insn); return;
    default: break;
    }
    const size_t mark = body_.size();
    try {
        emit_operation(insn);
    } catch (const Unsupported&) {
        body_.resize(mark);
        const char* mnemonic = ZydisMnemonicGetString(d.mnemonic);
        ++stats_.unimplemented;
        ++stats_.missing[mnemonic];
        line(fmt::format("{{ {}unimplemented(ctx, {}, \"{}\"); }}", store_registers(), hex(pc()),
                         mnemonic));
    }
}

void FunctionEmitter::emit_fallthrough(const Insn& insn, const Insn* following) {
    if (!insn.decoded) {
        return;
    }
    const ZydisDecodedInstruction& d = insn.insn;
    if (d.meta.category == ZYDIS_CATEGORY_RET || d.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
        is_trap(d)) {
        return;
    }
    const uint32_t next = insn.next();
    if (following != nullptr && following->rva == next) {
        return;
    }
    if (index_.contains(next)) {
        line(fmt::format("goto loc_{:X};", module_.va(next)));
    } else if (d.meta.category == ZYDIS_CATEGORY_CALL) {
        line(fmt::format("{}fault(ctx, {}, \"returned from a call that should not return\");",
                         store_registers(), hex(pc())));
    } else if (module_.is_function(next)) {
        emit_tail_call(next);
    } else {
        line(fmt::format("{}fault(ctx, {}, \"ran off the end of the function\");",
                         store_registers(), hex(pc())));
    }
}

void FunctionEmitter::emit_trap(const Insn& insn, std::string_view what) {
    line(fmt::format("{}fault(ctx, {}, \"{}\");", store_registers(), hex(module_.va(insn.rva)),
                     what));
}

void FunctionEmitter::emit_tail_call(uint32_t target) {
    if (module_.is_function(target)) {
        line(fmt::format("{{ {}[[clang::musttail]] return {}(ctx); }}", store_registers(),
                         function_name(module_.va(target))));
    } else {
        line(fmt::format("{{ {}const HostFunction next = resolve(ctx, {}, {}); "
                         "[[clang::musttail]] return next(ctx); }}",
                         store_registers(), hex(module_.va(target)), hex(pc())));
    }
}

void FunctionEmitter::emit_branch_to(uint32_t target) {
    if (is_internal_target(target)) {
        line(fmt::format("goto loc_{:X};", module_.va(target)));
    } else {
        emit_tail_call(target);
    }
}

void FunctionEmitter::emit_return(const Insn& insn) {
    uint64_t pop = 8;
    if (insn.insn.operand_count_visible > 0 && op(0).type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        pop += op(0).imm.value.u;
    }
    line(fmt::format("rsp += {}; {}return;", pop, store_registers()));
}

void FunctionEmitter::emit_call(const Insn& insn) {
    const uint64_t return_va = module_.va(insn.next());
    const std::string push = fmt::format("rsp -= 8; store<uint64_t>(rsp, {}); ", hex(return_va));
    const ZydisDecodedOperand& target = op(0);
    if (target.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && target.imm.is_relative) {
        const int64_t rva = int64_t{insn.next()} + target.imm.value.s;
        if (rva >= 0 && module_.is_function(static_cast<uint32_t>(rva))) {
            line(fmt::format("{{ {}{}{}(ctx); {}}}", push, store_registers(),
                             function_name(module_.va(static_cast<uint32_t>(rva))),
                             load_registers()));
            return;
        }
        ++stats_.indirect_calls;
        line(fmt::format("{{ {}{}resolve(ctx, {}, {})(ctx); {}}}", push, store_registers(),
                         hex(module_.base + static_cast<uint64_t>(rva)), hex(pc()),
                         load_registers()));
        return;
    }
    ++stats_.indirect_calls;
    const size_t mark = body_.size();
    try {
        const std::string ea = target.type == ZYDIS_OPERAND_TYPE_MEMORY
                                   ? fmt::format("const uint64_t ea = {}; ", address(target))
                                   : std::string();
        line(fmt::format("{{ {}const uint64_t target = {}; {}{}resolve(ctx, target, {})(ctx); {}}}",
                         ea, get(target, 64), push, store_registers(), hex(pc()),
                         load_registers()));
    } catch (const Unsupported&) {
        body_.resize(mark);
        ++stats_.unimplemented;
        ++stats_.missing["call"];
        line(
            fmt::format("{{ {}unimplemented(ctx, {}, \"call\"); }}", store_registers(), hex(pc())));
    }
}

void FunctionEmitter::emit_jump(const Insn& insn) {
    const ZydisDecodedOperand& target = op(0);
    if (target.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && target.imm.is_relative) {
        const int64_t rva = int64_t{insn.next()} + target.imm.value.s;
        if (rva < 0 || rva >= int64_t{module_.image.size_of_image()}) {
            emit_trap(insn, "jump outside the module");
            return;
        }
        emit_branch_to(static_cast<uint32_t>(rva));
        return;
    }
    const std::string ea = target.type == ZYDIS_OPERAND_TYPE_MEMORY
                               ? fmt::format("const uint64_t ea = {}; ", address(target))
                               : std::string();
    if (const auto it = tables_.find(insn.rva); it != tables_.end()) {
        ++stats_.jump_tables;
        std::string cases;
        std::set<uint32_t> seen;
        for (const uint32_t entry : it->second->targets) {
            if (seen.insert(entry).second) {
                cases += fmt::format("case {}: goto loc_{:X}; ", hex(module_.va(entry)),
                                     module_.va(entry));
            }
        }
        line(fmt::format("{{ {}const uint64_t target = {}; switch (target) {{ {}default: break; }} "
                         "{}fault(ctx, {}, \"switch target outside its jump table\"); }}",
                         ea, get(target, 64), cases, store_registers(), hex(pc())));
        return;
    }
    ++stats_.indirect_jumps;
    line(fmt::format("{{ {}const uint64_t target = {}; {}const HostFunction next = resolve(ctx, "
                     "target, {}); [[clang::musttail]] return next(ctx); }}",
                     ea, get(target, 64), store_registers(), hex(pc())));
}

void FunctionEmitter::emit_conditional_jump(const Insn& insn) {
    const int64_t rva = int64_t{insn.next()} + op(0).imm.value.s;
    if (rva < 0 || rva >= int64_t{module_.image.size_of_image()}) {
        emit_trap(insn, "branch outside the module");
        return;
    }
    const auto target = static_cast<uint32_t>(rva);
    std::string test;
    switch (insn.insn.mnemonic) {
    case ZYDIS_MNEMONIC_JRCXZ: test = "rcx == 0"; break;
    case ZYDIS_MNEMONIC_JECXZ: test = "uint32_t(rcx) == 0"; break;
    case ZYDIS_MNEMONIC_LOOP: test = "--rcx != 0"; break;
    default: test = condition(insn.insn.mnemonic); break;
    }
    if (is_internal_target(target)) {
        line(fmt::format("if ({}) goto loc_{:X};", test, module_.va(target)));
    } else {
        line(fmt::format("if ({}) {{", test));
        emit_tail_call(target);
        line("}");
    }
}

// ---- Operands ------------------------------------------------------------------------------

std::string FunctionEmitter::address(const ZydisDecodedOperand& operand) const {
    const auto& mem = operand.mem;
    if (mem.type == ZYDIS_MEMOP_TYPE_VSIB || mem.type == ZYDIS_MEMOP_TYPE_MIB) {
        throw Unsupported{};
    }
    const ZydisDecodedInstruction& d = current_->insn;
    if (mem.base == ZYDIS_REGISTER_RIP) {
        if (mem.index != ZYDIS_REGISTER_NONE) {
            throw Unsupported{};
        }
        return hex(module_.va(current_->next()) + static_cast<uint64_t>(mem.disp.value));
    }
    std::string out;
    if (mem.segment == ZYDIS_REGISTER_GS) {
        out = "ctx.gs_base";
    } else if (mem.segment == ZYDIS_REGISTER_FS) {
        throw Unsupported{};
    }
    const auto append = [&out](const std::string& term) {
        out += out.empty() ? term : " + " + term;
    };
    if (mem.base != ZYDIS_REGISTER_NONE) {
        const int g = gpr_index(mem.base);
        if (g < 0 || ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, mem.base) != 64) {
            throw Unsupported{};
        }
        append(kGprNames[g]);
    }
    if (mem.index != ZYDIS_REGISTER_NONE) {
        const int g = gpr_index(mem.index);
        if (g < 0 || ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, mem.index) != 64) {
            throw Unsupported{};
        }
        append(mem.scale > 1 ? fmt::format("{} * {}", kGprNames[g], mem.scale) : kGprNames[g]);
    }
    if (mem.disp.has_displacement && mem.disp.value != 0) {
        auto disp = static_cast<uint64_t>(mem.disp.value);
        // An absolute 64-bit address (mov rax, [moffs64]) carries a relocation.
        if (module_.dir64_relocations.contains(current_->rva + d.raw.disp.offset)) {
            disp += module_.delta();
        }
        append(hex(disp));
    }
    return out.empty() ? "0ull" : "(" + out + ")";
}

std::string FunctionEmitter::immediate(const ZydisDecodedOperand& operand, unsigned bits) const {
    uint64_t value = operand.imm.value.u;
    const ZydisDecodedInstruction& d = current_->insn;
    for (const auto& imm : d.raw.imm) {
        if (imm.size == 64 && module_.dir64_relocations.contains(current_->rva + imm.offset)) {
            value += module_.delta();
        }
    }
    if (bits < 64) {
        value &= (uint64_t{1} << bits) - 1;
    }
    return fmt::format("{}({})", utype(bits), hex(value));
}

std::string FunctionEmitter::read_gpr(ZydisRegister reg) const {
    const int g = gpr_index(reg);
    if (g < 0) {
        throw Unsupported{};
    }
    const char* name = kGprNames[g];
    if (is_high_byte(reg)) {
        return fmt::format("uint8_t({} >> 8)", name);
    }
    switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
    case 64: return name;
    case 32: return fmt::format("uint32_t({})", name);
    case 16: return fmt::format("uint16_t({})", name);
    case 8: return fmt::format("uint8_t({})", name);
    default: throw Unsupported{};
    }
}

std::string FunctionEmitter::write_gpr(ZydisRegister reg, const std::string& value) const {
    const int g = gpr_index(reg);
    if (g < 0) {
        throw Unsupported{};
    }
    const char* name = kGprNames[g];
    if (is_high_byte(reg)) {
        return fmt::format("{0} = ({0} & ~0xFF00ull) | (uint64_t(uint8_t({1})) << 8);", name,
                           value);
    }
    switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
    case 64: return fmt::format("{} = uint64_t({});", name, value);
    case 32: return fmt::format("{} = uint32_t({});", name, value);
    case 16: return fmt::format("{0} = ({0} & ~0xFFFFull) | uint16_t({1});", name, value);
    case 8: return fmt::format("{0} = ({0} & ~0xFFull) | uint8_t({1});", name, value);
    default: throw Unsupported{};
    }
}

std::string FunctionEmitter::get(const ZydisDecodedOperand& operand) const {
    return get(operand, operand.size);
}

// The operand's value as a `bits`-wide unsigned integer. Immediates are sign- or zero-extended
// the way the decoder reports them; registers and memory are read at their own width and then
// converted.
std::string FunctionEmitter::get(const ZydisDecodedOperand& operand, unsigned bits) const {
    switch (operand.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        const std::string value = read_gpr(operand.reg.value);
        return bits == operand.size ? value : fmt::format("{}({})", utype(bits), value);
    }
    case ZYDIS_OPERAND_TYPE_MEMORY: {
        if (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM) {
            throw Unsupported{};
        }
        const std::string value = fmt::format("load<{}>(ea)", utype(operand.size));
        return bits == operand.size ? value : fmt::format("{}({})", utype(bits), value);
    }
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: return immediate(operand, bits);
    default: throw Unsupported{};
    }
}

std::string FunctionEmitter::put(const ZydisDecodedOperand& operand,
                                 const std::string& value) const {
    switch (operand.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: return write_gpr(operand.reg.value, value);
    case ZYDIS_OPERAND_TYPE_MEMORY:
        if (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM) {
            throw Unsupported{};
        }
        return fmt::format("store<{0}>(ea, {0}({1}));", utype(operand.size), value);
    default: throw Unsupported{};
    }
}

unsigned FunctionEmitter::vector_bits() const {
    return vex() && current_->insn.avx.vector_length == 256 ? 256 : 128;
}

// ---- Instructions ---------------------------------------------------------------------------

void FunctionEmitter::emit_operation(const Insn& insn) {
    const ZydisDecodedInstruction& d = insn.insn;
    if (d.encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX ||
        d.encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX ||
        d.encoding == ZYDIS_INSTRUCTION_ENCODING_XOP ||
        d.encoding == ZYDIS_INSTRUCTION_ENCODING_3DNOW) {
        throw Unsupported{};
    }
    if (d.meta.category == ZYDIS_CATEGORY_STRINGOP) {
        emit_string(insn);
        return;
    }
    // Every operation is a block, so its temporaries don't cross labels. An instruction with a
    // memory operand computes the address once, up front, into `ea`.
    std::string ea;
    for (int i = 0; i < int{d.operand_count_visible}; ++i) {
        if (op(i).type == ZYDIS_OPERAND_TYPE_MEMORY && op(i).mem.type == ZYDIS_MEMOP_TYPE_MEM &&
            d.mnemonic != ZYDIS_MNEMONIC_POP) {
            ea = fmt::format("const uint64_t ea = {}; ", address(op(i)));
            break;
        }
    }
    const size_t mark = body_.size();
    if (ea.empty()) {
        line("{");
    } else {
        line("{ " + ea);
    }
    const size_t inner = body_.size();
    emit_integer(insn);
    if (body_.size() == inner) {
        // emit_integer left it to the vector emitter.
        body_.resize(inner);
        emit_vector(insn);
    }
    if (body_.size() == inner && d.mnemonic != ZYDIS_MNEMONIC_NOP) {
        body_.resize(mark);
        throw Unsupported{};
    }
    line("}");
}

void FunctionEmitter::emit_string(const Insn& insn) {
    const ZydisDecodedInstruction& d = insn.insn;
    const bool rep = (d.attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
    const char* type = nullptr;
    bool move = false;
    switch (d.mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB:
        type = "uint8_t";
        move = true;
        break;
    case ZYDIS_MNEMONIC_MOVSW:
        type = "uint16_t";
        move = true;
        break;
    case ZYDIS_MNEMONIC_MOVSD:
        type = "uint32_t";
        move = true;
        break;
    case ZYDIS_MNEMONIC_MOVSQ:
        type = "uint64_t";
        move = true;
        break;
    case ZYDIS_MNEMONIC_STOSB: type = "uint8_t"; break;
    case ZYDIS_MNEMONIC_STOSW: type = "uint16_t"; break;
    case ZYDIS_MNEMONIC_STOSD: type = "uint32_t"; break;
    case ZYDIS_MNEMONIC_STOSQ: type = "uint64_t"; break;
    default: throw Unsupported{};
    }
    if (d.address_width != 64) {
        throw Unsupported{};
    }
    if (move) {
        line(rep ? fmt::format("rep_movs<{}>(rsi, rdi, rcx, f.df);", type)
                 : fmt::format("movs<{}>(rsi, rdi, f.df);", type));
    } else {
        line(rep ? fmt::format("rep_stos<{0}>(rdi, rcx, {0}(rax), f.df);", type)
                 : fmt::format("stos<{0}>(rdi, {0}(rax), f.df);", type));
    }
}

void FunctionEmitter::emit_integer(const Insn& insn) {
    const ZydisDecodedInstruction& d = insn.insn;
    const unsigned bits = visible() > 0 ? op(0).size : d.operand_width;
    const auto T = [&] { return std::string(utype(bits)); };
    const auto alu = [&](const char* helper) {
        const std::string b = get(op(1), bits);
        if (locked()) {
            line(fmt::format("const {0} b = {1}; atomic_update<{0}>(ea, [&]({0} old) {{ return "
                             "{2}<{0}>(f, old, b); }});",
                             T(), b, helper));
        } else {
            line(put(op(0), fmt::format("{}<{}>(f, {}, {})", helper, T(), get(op(0)), b)));
        }
    };
    const auto bitwise = [&](const char* op_text) {
        const std::string b = get(op(1), bits);
        if (locked()) {
            line(fmt::format("const {0} b = {1}; atomic_update<{0}>(ea, [&]({0} old) {{ return "
                             "logic<{0}>(f, {0}(old {2} b)); }});",
                             T(), b, op_text));
        } else {
            line(put(op(0),
                     fmt::format("logic<{0}>(f, {0}({1} {2} {3}))", T(), get(op(0)), op_text, b)));
        }
    };
    const auto unary = [&](const std::string& expression_of_old) {
        // `expression_of_old` uses `old` for the operand's value.
        if (locked()) {
            line(fmt::format("atomic_update<{0}>(ea, [&]({0} old) {{ return {0}({1}); }});", T(),
                             expression_of_old));
        } else {
            line(fmt::format("const {} old = {}; {}", T(), get(op(0)),
                             put(op(0), expression_of_old)));
        }
    };
    const auto shift = [&](const char* helper) {
        const std::string count = visible() > 1 ? get(op(1), 8) : std::string("uint8_t(1)");
        line(put(op(0), fmt::format("{}<{}>(f, {}, {})", helper, T(), get(op(0)), count)));
    };
    const auto shift_bmi = [&](const std::string& expression) { line(put(op(0), expression)); };
    // MUL/IMUL/DIV/IDIV with an implicit accumulator: AX for byte operands, otherwise the
    // DX:AX pair at the operand's width.
    const auto accumulator_multiply = [&](const char* helper) {
        const std::string src = get(op(0));
        if (bits == 8) {
            line(fmt::format("uint8_t high; const uint8_t low = {}<uint8_t>(f, uint8_t(rax), {}, "
                             "high); rax = (rax & ~0xFFFFull) | (uint64_t(high) << 8) | low;",
                             helper, src));
            return;
        }
        line(fmt::format("{0} high; const {0} low = {1}<{0}>(f, {0}(rax), {2}, high);", T(), helper,
                         src));
        line(write_gpr(bits == 64 ? ZYDIS_REGISTER_RAX
                                  : (bits == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_AX),
                       "low"));
        line(write_gpr(bits == 64 ? ZYDIS_REGISTER_RDX
                                  : (bits == 32 ? ZYDIS_REGISTER_EDX : ZYDIS_REGISTER_DX),
                       "high"));
    };
    const auto accumulator_divide = [&](const char* helper) {
        const std::string src = get(op(0));
        if (bits == 8) {
            line(fmt::format("uint8_t rem; const uint8_t quot = {}<uint8_t>(ctx, {}, uint8_t(rax "
                             ">> 8), uint8_t(rax), {}, rem); rax = (rax & ~0xFFFFull) | "
                             "(uint64_t(rem) << 8) | quot;",
                             helper, hex(pc()), src));
            return;
        }
        line(fmt::format("{0} rem; const {0} quot = {1}<{0}>(ctx, {2}, {0}(rdx), {0}(rax), {3}, "
                         "rem);",
                         T(), helper, hex(pc()), src));
        line(write_gpr(bits == 64 ? ZYDIS_REGISTER_RAX
                                  : (bits == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_AX),
                       "quot"));
        line(write_gpr(bits == 64 ? ZYDIS_REGISTER_RDX
                                  : (bits == 32 ? ZYDIS_REGISTER_EDX : ZYDIS_REGISTER_DX),
                       "rem"));
    };
    // BT/BTS/BTR/BTC. With a register bit offset and a memory operand the offset is signed and
    // can address any bit relative to the operand's address.
    const auto bit_test = [&](const char* update) {
        const unsigned shift_bits = bits == 64 ? 6 : (bits == 32 ? 5 : 4);
        std::string where = "ea";
        std::string bit;
        if (op(1).type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            bit = fmt::format("{}", op(1).imm.value.u & (bits - 1));
        } else if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            line(fmt::format("const int64_t offset = int64_t({}({})); const uint64_t at = ea + "
                             "uint64_t((offset >> {}) * {});",
                             stype(bits), get(op(1)), shift_bits, bits / 8));
            where = "at";
            bit = fmt::format("(offset & {})", bits - 1);
        } else {
            bit = fmt::format("({} & {})", get(op(1)), bits - 1);
        }
        const std::string mask = fmt::format("({0}(1) << {1})", T(), bit);
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (update == nullptr) {
                line(fmt::format("f.cf = (load<{}>({}) & {}) != 0;", T(), where, mask));
            } else if (locked()) {
                line(fmt::format("const {0} old = atomic_update<{0}>({1}, [&]({0} v) {{ return "
                                 "{0}({2}); }}); f.cf = (old & {3}) != 0;",
                                 T(), where, fmt::format(fmt::runtime(update), "v", mask), mask));
            } else {
                line(fmt::format("const {0} old = load<{0}>({1}); f.cf = (old & {2}) != 0; "
                                 "store<{0}>({1}, {0}({3}));",
                                 T(), where, mask, fmt::format(fmt::runtime(update), "old", mask)));
            }
            return;
        }
        line(fmt::format("const {} old = {}; f.cf = (old & {}) != 0;", T(), get(op(0)), mask));
        if (update != nullptr) {
            line(put(op(0), fmt::format(fmt::runtime(update), "old", mask)));
        }
    };

    switch (d.mnemonic) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_ENDBR64:
    case ZYDIS_MNEMONIC_PREFETCH:
    case ZYDIS_MNEMONIC_PREFETCHW:
    case ZYDIS_MNEMONIC_PREFETCHNTA:
    case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2: line("// no effect"); return;
    case ZYDIS_MNEMONIC_PAUSE: line("_mm_pause();"); return;
    case ZYDIS_MNEMONIC_LFENCE: line("_mm_lfence();"); return;
    case ZYDIS_MNEMONIC_SFENCE: line("_mm_sfence();"); return;
    case ZYDIS_MNEMONIC_MFENCE: line("_mm_mfence();"); return;

    case ZYDIS_MNEMONIC_MOV:
        if (op(0).type == ZYDIS_OPERAND_TYPE_REGISTER && gpr_index(op(0).reg.value) < 0) {
            throw Unsupported{}; // segment or control register
        }
        line(put(op(0), get(op(1), bits)));
        return;
    case ZYDIS_MNEMONIC_MOVZX: line(put(op(0), get(op(1), bits))); return;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD:
        line(put(op(0),
                 fmt::format("{}({}({}({})))", T(), stype(bits), stype(op(1).size), get(op(1)))));
        return;
    case ZYDIS_MNEMONIC_LEA: {
        if (op(1).mem.type != ZYDIS_MEMOP_TYPE_AGEN) {
            throw Unsupported{};
        }
        ZydisDecodedOperand plain = op(1);
        plain.mem.segment = ZYDIS_REGISTER_NONE; // lea ignores segment overrides
        line(put(op(0), fmt::format("{}({})", T(), address(plain))));
        return;
    }
    case ZYDIS_MNEMONIC_MOVBE:
        line(put(op(0), bits == 16   ? fmt::format("__builtin_bswap16({})", get(op(1)))
                        : bits == 32 ? fmt::format("__builtin_bswap32({})", get(op(1)))
                                     : fmt::format("__builtin_bswap64({})", get(op(1)))));
        return;
    case ZYDIS_MNEMONIC_BSWAP:
        line(put(op(0), bits == 32 ? fmt::format("__builtin_bswap32({})", get(op(0)))
                                   : fmt::format("__builtin_bswap64({})", get(op(0)))));
        return;

    case ZYDIS_MNEMONIC_ADD: alu("add"); return;
    case ZYDIS_MNEMONIC_ADC: alu("adc"); return;
    case ZYDIS_MNEMONIC_SUB: alu("sub"); return;
    case ZYDIS_MNEMONIC_SBB: alu("sbb"); return;
    case ZYDIS_MNEMONIC_CMP:
        line(fmt::format("sub<{}>(f, {}, {});", T(), get(op(0)), get(op(1), bits)));
        return;
    case ZYDIS_MNEMONIC_AND: bitwise("&"); return;
    case ZYDIS_MNEMONIC_OR: bitwise("|"); return;
    case ZYDIS_MNEMONIC_XOR: bitwise("^"); return;
    case ZYDIS_MNEMONIC_TEST:
        line(fmt::format("logic<{0}>(f, {0}({1} & {2}));", T(), get(op(0)), get(op(1), bits)));
        return;
    case ZYDIS_MNEMONIC_INC: unary(fmt::format("inc<{}>(f, old)", T())); return;
    case ZYDIS_MNEMONIC_DEC: unary(fmt::format("dec<{}>(f, old)", T())); return;
    case ZYDIS_MNEMONIC_NEG: unary(fmt::format("neg<{}>(f, old)", T())); return;
    case ZYDIS_MNEMONIC_NOT: unary(fmt::format("{}(~old)", T())); return;

    case ZYDIS_MNEMONIC_SHL: shift("shl"); return;
    case ZYDIS_MNEMONIC_SHR: shift("shr"); return;
    case ZYDIS_MNEMONIC_SAR: shift("sar"); return;
    case ZYDIS_MNEMONIC_ROL: shift("rol"); return;
    case ZYDIS_MNEMONIC_ROR: shift("ror"); return;
    case ZYDIS_MNEMONIC_SHLX:
        shift_bmi(fmt::format("{0}({1} << ({2} & {3}))", T(), get(op(1)), get(op(2)), bits - 1));
        return;
    case ZYDIS_MNEMONIC_SHRX:
        shift_bmi(fmt::format("{0}({1} >> ({2} & {3}))", T(), get(op(1)), get(op(2)), bits - 1));
        return;
    case ZYDIS_MNEMONIC_SARX:
        shift_bmi(fmt::format("{0}({1}({2}) >> ({3} & {4}))", T(), stype(bits), get(op(1)),
                              get(op(2)), bits - 1));
        return;
    case ZYDIS_MNEMONIC_RORX:
        shift_bmi(fmt::format("std::rotr({}, {})", get(op(1)), op(2).imm.value.u & (bits - 1)));
        return;
    case ZYDIS_MNEMONIC_ANDN:
        line(
            put(op(0), fmt::format("logic<{0}>(f, {0}(~{1} & {2}))", T(), get(op(1)), get(op(2)))));
        return;
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
        line(put(op(0), fmt::format("{}<{}>(f, {}, {}, {})",
                                    d.mnemonic == ZYDIS_MNEMONIC_SHLD ? "shld" : "shrd", T(),
                                    get(op(0)), get(op(1)), get(op(2), 8))));
        return;
    case ZYDIS_MNEMONIC_BLSR:
        line(put(op(0), fmt::format("blsr<{}>(f, {})", T(), get(op(1)))));
        return;
    case ZYDIS_MNEMONIC_BLSI:
        line(put(op(0), fmt::format("blsi<{}>(f, {})", T(), get(op(1)))));
        return;
    case ZYDIS_MNEMONIC_BLSMSK:
        line(put(op(0), fmt::format("blsmsk<{}>(f, {})", T(), get(op(1)))));
        return;
    case ZYDIS_MNEMONIC_BZHI:
        line(put(op(0), fmt::format("bzhi<{}>(f, {}, {})", T(), get(op(1)), get(op(2)))));
        return;
    case ZYDIS_MNEMONIC_BEXTR:
        line(put(op(0), fmt::format("bextr<{}>(f, {}, {})", T(), get(op(1)), get(op(2)))));
        return;
    case ZYDIS_MNEMONIC_PDEP:
    case ZYDIS_MNEMONIC_PEXT:
        line(put(op(0),
                 fmt::format("_{}_u{}({}, {})", d.mnemonic == ZYDIS_MNEMONIC_PDEP ? "pdep" : "pext",
                             bits, get(op(1)), get(op(2)))));
        return;
    case ZYDIS_MNEMONIC_MULX:
        // The high half goes to the first operand, written last, so it wins if both are the
        // same register. No flags change.
        if (bits == 64) {
            line(fmt::format("unsigned long long high = 0; const uint64_t low = _umul128(rdx, {}, "
                             "&high);",
                             get(op(2))));
        } else {
            line(fmt::format("const uint64_t product = uint64_t(uint32_t(rdx)) * {}; const "
                             "uint32_t low = uint32_t(product); const uint32_t high = "
                             "uint32_t(product >> 32);",
                             get(op(2))));
        }
        line(put(op(1), "low"));
        line(put(op(0), "high"));
        return;

    case ZYDIS_MNEMONIC_MUL: accumulator_multiply("mul"); return;
    case ZYDIS_MNEMONIC_IMUL:
        if (visible() == 1) {
            accumulator_multiply("imul");
        } else if (visible() == 2) {
            line(fmt::format("{} high; {}", T(),
                             put(op(0), fmt::format("imul<{}>(f, {}, {}, high)", T(), get(op(0)),
                                                    get(op(1), bits)))));
        } else {
            line(fmt::format("{} high; {}", T(),
                             put(op(0), fmt::format("imul<{}>(f, {}, {}, high)", T(), get(op(1)),
                                                    get(op(2), bits)))));
        }
        return;
    case ZYDIS_MNEMONIC_DIV: accumulator_divide("divide"); return;
    case ZYDIS_MNEMONIC_IDIV: accumulator_divide("divide_signed"); return;

    case ZYDIS_MNEMONIC_CBW:
        line("rax = (rax & ~0xFFFFull) | uint16_t(int16_t(int8_t(rax)));");
        return;
    case ZYDIS_MNEMONIC_CWDE: line("rax = uint32_t(int32_t(int16_t(rax)));"); return;
    case ZYDIS_MNEMONIC_CDQE: line("rax = uint64_t(int64_t(int32_t(rax)));"); return;
    case ZYDIS_MNEMONIC_CWD:
        line("rdx = (rdx & ~0xFFFFull) | uint16_t(int16_t(rax) >> 15);");
        return;
    case ZYDIS_MNEMONIC_CDQ: line("rdx = uint32_t(int32_t(rax) >> 31);"); return;
    case ZYDIS_MNEMONIC_CQO: line("rdx = uint64_t(int64_t(rax) >> 63);"); return;

    case ZYDIS_MNEMONIC_PUSH:
        if (d.operand_width != 64) {
            throw Unsupported{};
        }
        line(fmt::format("const uint64_t v = {}; rsp -= 8; store<uint64_t>(rsp, v);",
                         get(op(0), 64)));
        return;
    case ZYDIS_MNEMONIC_POP:
        if (d.operand_width != 64) {
            throw Unsupported{};
        }
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            // The address is computed after rsp moves past the popped value.
            line(fmt::format("const uint64_t v = load<uint64_t>(rsp); rsp += 8; const uint64_t "
                             "ea = {}; store<uint64_t>(ea, v);",
                             address(op(0))));
        } else {
            line(fmt::format("const uint64_t v = load<uint64_t>(rsp); rsp += 8; {}",
                             put(op(0), "v")));
        }
        return;
    case ZYDIS_MNEMONIC_LEAVE: line("rsp = rbp; rbp = load<uint64_t>(rsp); rsp += 8;"); return;
    case ZYDIS_MNEMONIC_PUSHFQ: line("rsp -= 8; store<uint64_t>(rsp, pack_flags(f));"); return;
    case ZYDIS_MNEMONIC_POPFQ: line("unpack_flags(f, load<uint64_t>(rsp)); rsp += 8;"); return;
    case ZYDIS_MNEMONIC_LAHF:
        line("rax = (rax & ~0xFF00ull) | (uint64_t(uint8_t(pack_flags(f))) << 8);");
        return;
    case ZYDIS_MNEMONIC_SAHF:
        line("{ Flags g = f; unpack_flags(g, (pack_flags(f) & ~0xFFull) | uint8_t(rax >> 8)); "
             "f = g; }");
        return;
    case ZYDIS_MNEMONIC_CLC: line("f.cf = false;"); return;
    case ZYDIS_MNEMONIC_STC: line("f.cf = true;"); return;
    case ZYDIS_MNEMONIC_CMC: line("f.cf = !f.cf;"); return;
    case ZYDIS_MNEMONIC_CLD: line("f.df = false;"); return;
    case ZYDIS_MNEMONIC_STD: line("f.df = true;"); return;

    case ZYDIS_MNEMONIC_XCHG:
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY || op(1).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const ZydisDecodedOperand& reg =
                op(0).type == ZYDIS_OPERAND_TYPE_MEMORY ? op(1) : op(0);
            line(write_gpr(reg.reg.value, fmt::format("atomic_exchange<{}>(ea, {})", T(),
                                                      read_gpr(reg.reg.value))));
        } else {
            line(fmt::format("const {} a = {}; const {} b = {}; {} {}", T(), get(op(0)), T(),
                             get(op(1)), put(op(0), "b"), put(op(1), "a")));
        }
        return;
    case ZYDIS_MNEMONIC_XADD:
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            line(fmt::format("const {0} b = {1}; const {0} old = atomic_update<{0}>(ea, [&]({0} v) "
                             "{{ return add<{0}>(f, v, b); }}); {2}",
                             T(), get(op(1)), put(op(1), "old")));
        } else {
            line(fmt::format("const {0} a = {1}; const {0} sum = add<{0}>(f, a, {2}); {3} {4}", T(),
                             get(op(0)), get(op(1)), put(op(1), "a"), put(op(0), "sum")));
        }
        return;
    case ZYDIS_MNEMONIC_CMPXCHG: {
        const std::string acc = read_gpr(bits == 64   ? ZYDIS_REGISTER_RAX
                                         : bits == 32 ? ZYDIS_REGISTER_EAX
                                         : bits == 16 ? ZYDIS_REGISTER_AX
                                                      : ZYDIS_REGISTER_AL);
        const std::string acc_write = write_gpr(bits == 64   ? ZYDIS_REGISTER_RAX
                                                : bits == 32 ? ZYDIS_REGISTER_EAX
                                                : bits == 16 ? ZYDIS_REGISTER_AX
                                                             : ZYDIS_REGISTER_AL,
                                                "seen");
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            line(fmt::format("{0} seen = {1}; const bool ok = atomic_compare_exchange<{0}>(ea, "
                             "seen, {2}); sub<{0}>(f, {1}, seen); if (!ok) {{ {3} }}",
                             T(), acc, get(op(1)), acc_write));
        } else {
            line(fmt::format("const {0} seen = {1}; sub<{0}>(f, {2}, seen); if (f.zf) {{ {3} }} "
                             "else {{ {4} }}",
                             T(), get(op(0)), acc, put(op(0), get(op(1))), acc_write));
        }
        return;
    }

    case ZYDIS_MNEMONIC_BT: bit_test(nullptr); return;
    case ZYDIS_MNEMONIC_BTS: bit_test("{0} | {1}"); return;
    case ZYDIS_MNEMONIC_BTR: bit_test("{0} & ~{1}"); return;
    case ZYDIS_MNEMONIC_BTC: bit_test("{0} ^ {1}"); return;
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: {
        // A zero source sets ZF and leaves the destination unchanged.
        const std::string index = d.mnemonic == ZYDIS_MNEMONIC_BSF
                                      ? "__builtin_ctzll(src)"
                                      : fmt::format("63 - __builtin_clzll(src)");
        line(fmt::format("const uint64_t src = {}; f.zf = src == 0; if (src != 0) {{ {} }}",
                         get(op(1), 64), put(op(0), index)));
        return;
    }
    case ZYDIS_MNEMONIC_TZCNT:
        line(put(op(0), fmt::format("tzcnt<{}>(f, {})", T(), get(op(1)))));
        return;
    case ZYDIS_MNEMONIC_LZCNT:
        line(put(op(0), fmt::format("lzcnt<{}>(f, {})", T(), get(op(1)))));
        return;
    case ZYDIS_MNEMONIC_POPCNT:
        line(put(op(0), fmt::format("popcnt<{}>(f, {})", T(), get(op(1)))));
        return;

    case ZYDIS_MNEMONIC_CRC32: {
        const unsigned src_bits = op(1).size;
        const std::string acc = bits == 64 ? get(op(0)) : fmt::format("uint32_t({})", get(op(0)));
        if (src_bits == 64) {
            line(put(op(0), fmt::format("_mm_crc32_u64({}, {})", acc, get(op(1)))));
        } else {
            line(put(op(0),
                     fmt::format("_mm_crc32_u{}(uint32_t({}), {})", src_bits, acc, get(op(1)))));
        }
        return;
    }

    case ZYDIS_MNEMONIC_RDTSC:
        line("const uint64_t t = __rdtsc(); rax = uint32_t(t); rdx = t >> 32;");
        return;
    case ZYDIS_MNEMONIC_RDTSCP:
        line("unsigned aux = 0; const uint64_t t = __rdtscp(&aux); rax = uint32_t(t); rdx = t >> "
             "32; rcx = aux;");
        return;
    // Both report the console's CPU features, not the host's (g6/cpu/cpuid.h).
    case ZYDIS_MNEMONIC_CPUID:
        line("const auto r = guest_cpuid(uint32_t(rax), uint32_t(rcx)); rax = r.eax; rbx = r.ebx; "
             "rcx = r.ecx; rdx = r.edx;");
        return;
    case ZYDIS_MNEMONIC_XGETBV:
        line("const uint64_t v = guest_xgetbv(uint32_t(rcx)); rax = uint32_t(v); rdx = v >> 32;");
        return;

    case ZYDIS_MNEMONIC_CMPXCHG16B: line("f.zf = cmpxchg16b(ea, rax, rdx, rbx, rcx);"); return;
    case ZYDIS_MNEMONIC_CLFLUSH: line("_mm_clflush(reinterpret_cast<const void*>(ea));"); return;
    // Zen 2 has no shadow stack, and without an active one RDSSP leaves its register alone.
    case ZYDIS_MNEMONIC_RDSSPD:
    case ZYDIS_MNEMONIC_RDSSPQ: line("// no effect: no shadow stack"); return;
    // The x87 control word and exception flags are the host thread's own (g6/cpu/guest.h).
    case ZYDIS_MNEMONIC_FNSTCW: line("fnstcw(ea);"); return;
    case ZYDIS_MNEMONIC_FLDCW: line("fldcw(ea);"); return;
    case ZYDIS_MNEMONIC_FNCLEX: line("fnclex();"); return;

    default: break;
    }

    const std::string_view name = ZydisMnemonicGetString(d.mnemonic);
    if (name.starts_with("cmov")) {
        line(fmt::format(
            "const {} src = {}; {}", T(), get(op(1)),
            put(op(0), fmt::format("{} ? src : {}", condition(d.mnemonic), get(op(0))))));
        return;
    }
    if (name.starts_with("set")) {
        line(put(op(0), fmt::format("{} ? 1 : 0", condition(d.mnemonic))));
        return;
    }
    // Anything else is left for the vector emitter (body unchanged).
}

} // namespace g6::recomp
