#include "jump_table.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include <Zydis/Zydis.h>

namespace g6::recomp {
namespace {

constexpr int kMaxDepth = 8;                    // nested register definitions followed
constexpr uint32_t kMaxBoundedEntries = 65536;  // a larger bound is not a switch
constexpr uint32_t kMaxUnboundedEntries = 4096; // read limit when no bound is known
constexpr uint32_t kMaxInstructionLength = 15;

struct Decoded {
    uint32_t pc = 0;
    bool taken = false;
    ZydisDecodedInstruction insn{};
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> ops{};

    uint32_t next() const { return pc + insn.length; }
    std::span<const ZydisDecodedOperand> operands() const {
        return {ops.data(), insn.operand_count};
    }
    std::span<const ZydisDecodedOperand> visible() const {
        return {ops.data(), insn.operand_count_visible};
    }
};

std::optional<Decoded> decode(const pe::Image& image, const ZydisDecoder& decoder, uint32_t pc,
                              bool taken) {
    const auto mapped = image.mapped();
    if (pc >= mapped.size()) {
        return std::nullopt;
    }
    Decoded decoded;
    decoded.pc = pc;
    decoded.taken = taken;
    const auto length = std::min<size_t>(kMaxInstructionLength, mapped.size() - pc);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, mapped.data() + pc, length, &decoded.insn,
                                             decoded.ops.data()))) {
        return std::nullopt;
    }
    return decoded;
}

ZydisRegister enclosing(ZydisRegister reg) {
    return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
}

// Registers a call may change under the Windows x64 calling convention.
bool is_volatile(ZydisRegister reg) {
    switch (reg) {
    case ZYDIS_REGISTER_RAX:
    case ZYDIS_REGISTER_RCX:
    case ZYDIS_REGISTER_RDX:
    case ZYDIS_REGISTER_R8:
    case ZYDIS_REGISTER_R9:
    case ZYDIS_REGISTER_R10:
    case ZYDIS_REGISTER_R11: return true;
    default: return false;
    }
}

bool writes_register(const Decoded& decoded, ZydisRegister reg) {
    return std::ranges::any_of(decoded.operands(), [reg](const ZydisDecodedOperand& operand) {
        return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
               (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0 &&
               enclosing(operand.reg.value) == reg;
    });
}

bool changes_flags(const Decoded& decoded) {
    const ZydisAccessedFlags* flags = decoded.insn.cpu_flags;
    return flags != nullptr &&
           (flags->modified | flags->set_0 | flags->set_1 | flags->undefined) != 0;
}

bool is_register(const ZydisDecodedOperand& operand) {
    return operand.type == ZYDIS_OPERAND_TYPE_REGISTER;
}

bool is_immediate(const ZydisDecodedOperand& operand) {
    return operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
}

// An immediate operand truncated to the width of the operation it takes part in.
uint64_t immediate(const ZydisDecodedOperand& operand, uint16_t width_bits) {
    const uint64_t value = operand.imm.value.u;
    return width_bits >= 64 ? value : value & ((uint64_t{1} << width_bits) - 1);
}

// A memory read from `table + index * scale`. When `relative_to` is set, the read is from
// `relative_to + table + index * scale` with the register's value unknown; once the same
// register is added to the result (the image-relative switch idiom), it is taken to hold the
// image base and recorded in `assumed_base`.
struct Load {
    int64_t table = 0; // RVA of element 0
    uint8_t size = 0;  // bytes read
    bool sign_extend = false;
    ZydisRegister index = ZYDIS_REGISTER_NONE; // 64-bit register, NONE when not indexed
    uint8_t scale = 0;
    size_t at = 0; // window position of the reading instruction
    ZydisRegister relative_to = ZYDIS_REGISTER_NONE;
    ZydisRegister assumed_base = ZYDIS_REGISTER_NONE;
    std::optional<size_t> base_definition; // where the base register was last written
};

enum class ValueKind : uint8_t {
    kUnknown,
    kConstant,    // an address, as an RVA
    kLoad,        // the value read by `load`
    kSum,         // the value read by `load`, plus `constant`
    kPointerLoad, // 8 bytes read through a register that is not a known address
};

struct Value {
    ValueKind kind = ValueKind::kUnknown;
    int64_t constant = 0;
    Load load;
};

struct Address {
    int64_t table = 0;
    ZydisRegister index = ZYDIS_REGISTER_NONE;
    uint8_t scale = 0;
    ZydisRegister relative_to = ZYDIS_REGISTER_NONE; // base register with an unknown value
};

struct EntryCount {
    std::optional<uint32_t> count;
    uint32_t index_table = 0;
    uint32_t index_count = 0;
    bool rejected = false;
};

class Analyzer {
public:
    Analyzer(const pe::Image& image, std::vector<Decoded> window,
             const std::function<bool(uint32_t)>& valid_target)
        : image_(image), window_(std::move(window)), valid_target_(valid_target) {}

    IndirectJump run(const Decoded& jump) const {
        const auto operands = jump.visible();
        if (operands.empty()) {
            return {};
        }
        const ZydisDecodedOperand& target = operands[0];
        if (target.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (target.mem.index == ZYDIS_REGISTER_NONE) {
                return {.kind = IndirectJumpKind::kTailCall};
            }
            const auto address = resolve_address(jump, target, window_.size(), 0);
            if (!address || address->scale != 8 || address->relative_to != ZYDIS_REGISTER_NONE) {
                return {};
            }
            return resolve(jump.pc,
                           {.table = address->table,
                            .size = 8,
                            .index = address->index,
                            .scale = 8,
                            .at = window_.size()},
                           0);
        }
        if (!is_register(target)) {
            return {};
        }
        const Value value = evaluate(enclosing(target.reg.value), window_.size(), 0);
        switch (value.kind) {
        case ValueKind::kSum:
            if (value.load.size == 4 && value.load.scale == 4 &&
                value.load.index != ZYDIS_REGISTER_NONE) {
                return resolve(jump.pc, value.load, value.constant);
            }
            break;
        case ValueKind::kLoad:
            if (value.load.size == 8 && value.load.index == ZYDIS_REGISTER_NONE) {
                return {.kind = IndirectJumpKind::kTailCall};
            }
            if (value.load.size == 8 && value.load.scale == 8 &&
                value.load.relative_to == ZYDIS_REGISTER_NONE) {
                return resolve(jump.pc, value.load, 0);
            }
            break;
        case ValueKind::kPointerLoad: return {.kind = IndirectJumpKind::kTailCall};
        default: break;
        }
        return {};
    }

private:
    // The latest instruction before window position `before` that writes `reg`. A call in
    // between ends the search for registers the call may change.
    std::optional<size_t> find_definition(ZydisRegister reg, size_t before) const {
        for (size_t i = before; i-- > 0;) {
            const Decoded& decoded = window_[i];
            if (decoded.insn.meta.category == ZYDIS_CATEGORY_CALL) {
                if (is_volatile(reg)) {
                    return std::nullopt;
                }
                continue;
            }
            if (writes_register(decoded, reg)) {
                return i;
            }
        }
        return std::nullopt;
    }

    // The value `reg` holds just before window position `before`.
    Value evaluate(ZydisRegister reg, size_t before, int depth) const {
        if (depth > kMaxDepth) {
            return {};
        }
        const auto at = find_definition(reg, before);
        if (!at) {
            return {};
        }
        const Decoded& decoded = window_[*at];
        const auto operands = decoded.visible();
        if (operands.size() < 2 || !is_register(operands[0]) ||
            enclosing(operands[0].reg.value) != reg) {
            return {};
        }
        const ZydisDecodedOperand& source = operands[1];

        switch (decoded.insn.mnemonic) {
        case ZYDIS_MNEMONIC_LEA: {
            if (source.mem.index != ZYDIS_REGISTER_NONE) {
                return {};
            }
            const auto address = resolve_address(decoded, source, *at, depth + 1);
            if (!address || address->relative_to != ZYDIS_REGISTER_NONE) {
                return {};
            }
            return {.kind = ValueKind::kConstant, .constant = address->table};
        }
        case ZYDIS_MNEMONIC_MOV:
        case ZYDIS_MNEMONIC_MOVSXD:
        case ZYDIS_MNEMONIC_MOVSX:
        case ZYDIS_MNEMONIC_MOVZX: {
            const bool copy = decoded.insn.mnemonic == ZYDIS_MNEMONIC_MOV ||
                              decoded.insn.mnemonic == ZYDIS_MNEMONIC_MOVSXD;
            if (is_register(source)) {
                return copy ? evaluate(enclosing(source.reg.value), *at, depth + 1) : Value{};
            }
            if (source.type != ZYDIS_OPERAND_TYPE_MEMORY ||
                source.mem.segment == ZYDIS_REGISTER_GS ||
                source.mem.segment == ZYDIS_REGISTER_FS) {
                return {};
            }
            const auto size = static_cast<uint8_t>(source.size / 8);
            const auto address = resolve_address(decoded, source, *at, depth + 1);
            if (!address) {
                return {};
            }
            if (address->relative_to != ZYDIS_REGISTER_NONE && size == 8 &&
                address->index == ZYDIS_REGISTER_NONE) {
                return {.kind = ValueKind::kPointerLoad}; // a vtable slot, a field
            }
            Load load{.table = address->table,
                      .size = size,
                      .sign_extend = decoded.insn.mnemonic == ZYDIS_MNEMONIC_MOVSXD ||
                                     decoded.insn.mnemonic == ZYDIS_MNEMONIC_MOVSX,
                      .index = address->index,
                      .scale = address->scale,
                      .at = *at,
                      .relative_to = address->relative_to};
            if (load.relative_to != ZYDIS_REGISTER_NONE) {
                load.base_definition = find_definition(load.relative_to, *at);
            }
            return {.kind = ValueKind::kLoad, .load = load};
        }
        case ZYDIS_MNEMONIC_ADD: {
            if (!is_register(source)) {
                return {};
            }
            const Value left = evaluate(reg, *at, depth + 1);
            const ZydisRegister added = enclosing(source.reg.value);
            if (left.kind == ValueKind::kLoad && left.load.relative_to == added &&
                find_definition(added, *at) == left.load.base_definition) {
                // `load [base + index*4 + table]` then `add result, base`: the entries are
                // offsets from whatever the base holds, and the table sits at an offset from it
                // too, so the base is the image base. Accepted only for bounded tables whose
                // entries all check out.
                Load load = left.load;
                load.relative_to = ZYDIS_REGISTER_NONE;
                load.assumed_base = added;
                return {.kind = ValueKind::kSum, .constant = 0, .load = load};
            }
            if (left.kind == ValueKind::kLoad && left.load.relative_to != ZYDIS_REGISTER_NONE) {
                return {};
            }
            const Value right = evaluate(added, *at, depth + 1);
            if (left.kind == ValueKind::kLoad && right.kind == ValueKind::kConstant) {
                return {.kind = ValueKind::kSum, .constant = right.constant, .load = left.load};
            }
            if (left.kind == ValueKind::kConstant && right.kind == ValueKind::kLoad &&
                right.load.relative_to == ZYDIS_REGISTER_NONE) {
                return {.kind = ValueKind::kSum, .constant = left.constant, .load = right.load};
            }
            return {};
        }
        default: return {};
        }
    }

    // The address a memory operand refers to, split into a known part and an index register.
    // With scale 1, base and index are interchangeable, so either may be the known one. When
    // neither is known, the base register is returned as `relative_to`.
    std::optional<Address> resolve_address(const Decoded& decoded,
                                           const ZydisDecodedOperand& operand, size_t at,
                                           int depth) const {
        const auto& mem = operand.mem;
        const int64_t displacement = mem.disp.has_displacement != 0 ? mem.disp.value : 0;
        const ZydisRegister index =
            mem.index == ZYDIS_REGISTER_NONE ? ZYDIS_REGISTER_NONE : enclosing(mem.index);
        if (mem.base == ZYDIS_REGISTER_RIP) {
            return Address{int64_t{decoded.next()} + displacement, index, mem.scale};
        }
        if (mem.base == ZYDIS_REGISTER_NONE) {
            // Absolute address: only possible for images based below 2 GB.
            const auto rva = image_.try_va_to_rva(static_cast<uint64_t>(displacement));
            if (!rva) {
                return std::nullopt;
            }
            return Address{int64_t{*rva}, index, mem.scale};
        }
        const ZydisRegister base = enclosing(mem.base);
        const Value base_value = evaluate(base, at, depth);
        if (base_value.kind == ValueKind::kConstant) {
            return Address{base_value.constant + displacement, index, mem.scale};
        }
        if (index != ZYDIS_REGISTER_NONE && mem.scale == 1) {
            const Value index_value = evaluate(index, at, depth);
            if (index_value.kind == ValueKind::kConstant) {
                return Address{index_value.constant + displacement, base, 1};
            }
        }
        return Address{displacement, index, mem.scale, base};
    }

    // How many values the index register can hold at window position `before`. Scans backward
    // along the path for a bounds check (`cmp` and an unsigned branch) or a small mask on the
    // index, or on a value the index was derived from by copies, extensions, non-negative
    // offsets (`lea`, `add`), masks (`and`) and right shifts (`shr`). Those steps are applied
    // to the bound found, in program order.
    std::optional<uint32_t> bound(ZydisRegister index, size_t before) const {
        enum class Op : uint8_t { kAdd, kAnd, kShiftRight };
        struct Step {
            Op op;
            uint64_t value;
        };
        std::vector<Step> steps; // latest first
        // The largest index value, given the largest value of the tracked register.
        const auto finish = [&steps](uint64_t largest) -> std::optional<uint32_t> {
            for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
                switch (it->op) {
                case Op::kAdd:
                    if (largest > UINT32_MAX || it->value > UINT32_MAX) {
                        return std::nullopt;
                    }
                    largest += it->value;
                    break;
                case Op::kAnd: largest = std::min(largest, it->value); break;
                case Op::kShiftRight: largest >>= it->value; break;
                }
            }
            if (largest >= kMaxBoundedEntries) {
                return std::nullopt;
            }
            return static_cast<uint32_t>(largest + 1);
        };

        std::vector<ZydisRegister> chain{index}; // registers holding the tracked value here
        const auto in_chain = [&chain](ZydisRegister reg) {
            return std::ranges::find(chain, enclosing(reg)) != chain.end();
        };
        for (size_t i = before; i-- > 0;) {
            const Decoded& decoded = window_[i];
            const auto operands = decoded.visible();
            const ZydisMnemonic mnemonic = decoded.insn.mnemonic;
            if (decoded.insn.meta.category == ZYDIS_CATEGORY_CALL) {
                std::erase_if(chain, is_volatile);
                if (chain.empty()) {
                    return std::nullopt;
                }
                continue;
            }
            const bool on_chain =
                operands.size() == 2 && is_register(operands[0]) && in_chain(operands[0].reg.value);
            const bool with_immediate = on_chain && is_immediate(operands[1]);
            const uint64_t value = with_immediate ? immediate(operands[1], operands[0].size) : 0;

            if (mnemonic == ZYDIS_MNEMONIC_CMP && with_immediate) {
                const auto largest = largest_after_compare(i, value);
                return largest ? finish(*largest) : std::nullopt;
            }
            if (!std::ranges::any_of(
                    chain, [&](ZydisRegister reg) { return writes_register(decoded, reg); })) {
                continue;
            }
            // A step that rewrites one register of the chain in place: the others no longer
            // hold the same value before it, so only that register is tracked further.
            const auto step_in_place = [&](Op op, uint64_t operand) {
                chain = {enclosing(operands[0].reg.value)};
                steps.push_back({op, operand});
            };
            if (mnemonic == ZYDIS_MNEMONIC_AND && with_immediate) {
                if (value < kMaxBoundedEntries) {
                    return finish(value);
                }
                step_in_place(Op::kAnd, value);
                continue;
            }
            if (mnemonic == ZYDIS_MNEMONIC_SHR && with_immediate) {
                step_in_place(Op::kShiftRight, value & 63);
                continue;
            }
            if (mnemonic == ZYDIS_MNEMONIC_ADD && with_immediate &&
                static_cast<int64_t>(operands[1].imm.value.s) >= 0) {
                step_in_place(Op::kAdd, value);
                continue;
            }
            if (mnemonic == ZYDIS_MNEMONIC_CDQE) {
                continue; // rax keeps the value eax had
            }
            const ZydisRegister source = !on_chain                  ? ZYDIS_REGISTER_NONE
                                         : is_register(operands[1]) ? operands[1].reg.value
                                                                    : ZYDIS_REGISTER_NONE;
            const ZydisRegister written =
                on_chain ? enclosing(operands[0].reg.value) : ZYDIS_REGISTER_NONE;
            std::erase_if(chain, [&](ZydisRegister reg) { return writes_register(decoded, reg); });
            switch (mnemonic) {
            case ZYDIS_MNEMONIC_MOV:
            case ZYDIS_MNEMONIC_MOVSXD:
            case ZYDIS_MNEMONIC_MOVZX:
            case ZYDIS_MNEMONIC_MOVSX:
                // A register copy: before it, the source held the value.
                if (source != ZYDIS_REGISTER_NONE) {
                    chain.push_back(enclosing(source));
                }
                break;
            case ZYDIS_MNEMONIC_LEA: {
                // `lea reg, [source + offset]`: track the source, then add the offset.
                const auto& mem = operands[1].mem;
                const int64_t offset = mem.disp.has_displacement != 0 ? mem.disp.value : 0;
                if (written != ZYDIS_REGISTER_NONE && mem.base != ZYDIS_REGISTER_NONE &&
                    mem.base != ZYDIS_REGISTER_RIP && mem.index == ZYDIS_REGISTER_NONE &&
                    offset >= 0) {
                    chain = {enclosing(mem.base)};
                    if (offset != 0) {
                        steps.push_back({Op::kAdd, static_cast<uint64_t>(offset)});
                    }
                }
                break;
            }
            default: break; // the register held something unrelated before
            }
            std::ranges::sort(chain);
            const auto [first, last] = std::ranges::unique(chain);
            chain.erase(first, last);
            if (chain.empty()) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // The largest value `cmp reg, limit` at window position `at` allows on this path, from the
    // unsigned branch that follows it and the direction the path took.
    std::optional<uint64_t> largest_after_compare(size_t at, uint64_t limit) const {
        for (size_t i = at + 1; i < window_.size(); ++i) {
            const Decoded& decoded = window_[i];
            if (decoded.insn.meta.category == ZYDIS_CATEGORY_COND_BR) {
                switch (decoded.insn.mnemonic) {
                case ZYDIS_MNEMONIC_JNBE: // ja: reg <= limit unless taken
                    return decoded.taken ? std::nullopt : std::optional{limit};
                case ZYDIS_MNEMONIC_JBE: return decoded.taken ? std::optional{limit} : std::nullopt;
                case ZYDIS_MNEMONIC_JNB: // jae: reg < limit unless taken
                    return decoded.taken || limit == 0 ? std::nullopt : std::optional{limit - 1};
                case ZYDIS_MNEMONIC_JB:
                    return decoded.taken && limit != 0 ? std::optional{limit - 1} : std::nullopt;
                case ZYDIS_MNEMONIC_JNZ: // reg == limit unless taken
                    return decoded.taken ? std::nullopt : std::optional{limit};
                case ZYDIS_MNEMONIC_JZ: return decoded.taken ? std::optional{limit} : std::nullopt;
                default: return std::nullopt;
                }
            }
            if (changes_flags(decoded)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // Entry count of a table indexed by `load`. When the index was itself read from a byte
    // table (sparse case values mapped to entry numbers), the count is the largest byte + 1.
    EntryCount entry_count(const Load& load) const {
        EntryCount result;
        const Value index = evaluate(load.index, load.at, 0);
        if (index.kind != ValueKind::kLoad || index.load.size != 1 || index.load.scale != 1 ||
            index.load.index == ZYDIS_REGISTER_NONE) {
            result.count = bound(load.index, load.at);
            return result;
        }
        Load bytes_load = index.load;
        if (bytes_load.relative_to != ZYDIS_REGISTER_NONE) {
            // The byte table must be read relative to the same image base as the entry table.
            // With scale 1 the base may sit in either register of the address.
            const auto is_image_base = [&](ZydisRegister reg) {
                return reg != ZYDIS_REGISTER_NONE && reg == load.assumed_base &&
                       find_definition(reg, bytes_load.at) == load.base_definition;
            };
            if (is_image_base(bytes_load.index)) {
                std::swap(bytes_load.index, bytes_load.relative_to);
            } else if (!is_image_base(bytes_load.relative_to)) {
                return result; // byte table address unknown: no count
            }
            bytes_load.relative_to = ZYDIS_REGISTER_NONE;
        }
        if (!std::in_range<uint32_t>(bytes_load.table)) {
            result.rejected = true;
            return result;
        }
        result.index_table = static_cast<uint32_t>(bytes_load.table);
        const auto bytes = bound(bytes_load.index, bytes_load.at);
        if (!bytes) {
            return result; // an unbounded byte table: read the dword table while it looks valid
        }
        if (!image_.contains(result.index_table, *bytes)) {
            result.rejected = true;
            return result;
        }
        result.index_count = *bytes;
        uint32_t largest = 0;
        for (const uint8_t value : image_.bytes(result.index_table, *bytes)) {
            largest = std::max<uint32_t>(largest, value);
        }
        result.count = largest + 1;
        return result;
    }

    // Reads the table described by `load`. 4-byte entries are offsets from `addend` (an RVA:
    // 0 for image-relative entries, the table itself for table-relative ones); 8-byte entries
    // are absolute addresses.
    IndirectJump resolve(uint32_t jump, const Load& load, int64_t addend) const {
        IndirectJump result{.kind = IndirectJumpKind::kRejectedTable};
        JumpTable& table = result.table;
        table.jump = jump;
        table.entry_size = load.size;
        if (!std::in_range<uint32_t>(load.table)) {
            return result;
        }
        table.table = static_cast<uint32_t>(load.table);

        const auto target_of = [&](uint32_t entry) -> std::optional<uint32_t> {
            const uint64_t offset = uint64_t{table.table} + (uint64_t{entry} * table.entry_size);
            if (offset > UINT32_MAX ||
                !image_.contains(static_cast<uint32_t>(offset), table.entry_size)) {
                return std::nullopt;
            }
            const auto rva = static_cast<uint32_t>(offset);
            std::optional<uint32_t> target;
            if (table.entry_size == 8) {
                target = image_.try_va_to_rva(image_.read<uint64_t>(rva));
            } else {
                const auto raw = image_.read<uint32_t>(rva);
                const int64_t delta =
                    load.sign_extend ? int64_t{static_cast<int32_t>(raw)} : int64_t{raw};
                const int64_t sum = addend + delta;
                if (sum >= 0 && sum < int64_t{image_.size_of_image()}) {
                    target = static_cast<uint32_t>(sum);
                }
            }
            if (target && !valid_target_(*target)) {
                target.reset();
            }
            return target;
        };

        const EntryCount count = entry_count(load);
        table.index_table = count.index_table;
        table.index_count = count.index_count;
        if (count.rejected) {
            return result;
        }
        if (count.count) {
            if (*count.count > kMaxBoundedEntries) {
                return result;
            }
            for (uint32_t entry = 0; entry < *count.count; ++entry) {
                const auto target = target_of(entry);
                if (!target) {
                    table.targets.clear();
                    return result;
                }
                table.targets.push_back(*target);
            }
            table.bounded = true;
        } else {
            if (load.assumed_base != ZYDIS_REGISTER_NONE) {
                return {}; // too little evidence that the register holds the image base
            }
            for (uint32_t entry = 0; entry < kMaxUnboundedEntries; ++entry) {
                const auto target = target_of(entry);
                if (!target) {
                    break;
                }
                table.targets.push_back(*target);
            }
            if (table.targets.empty()) {
                return {};
            }
        }
        result.kind = IndirectJumpKind::kJumpTable;
        return result;
    }

    const pe::Image& image_;
    std::vector<Decoded> window_;
    const std::function<bool(uint32_t)>& valid_target_;
};

} // namespace

IndirectJump analyze_indirect_jump(const pe::Image& image, const ZydisDecoder& decoder,
                                   std::span<const PathStep> path, uint32_t jump_pc,
                                   const std::function<bool(uint32_t)>& valid_target) {
    const auto jump = decode(image, decoder, jump_pc, false);
    if (!jump) {
        return {};
    }
    if (path.size() > kJumpTablePathWindow) {
        path = path.last(kJumpTablePathWindow);
    }
    std::vector<Decoded> window;
    window.reserve(path.size());
    for (const PathStep& step : path) {
        auto decoded = decode(image, decoder, step.pc, step.branch_taken);
        if (!decoded) {
            window.clear(); // nothing before an undecodable step can be trusted
            continue;
        }
        window.push_back(*decoded);
    }
    return Analyzer(image, std::move(window), valid_target).run(*jump);
}

} // namespace g6::recomp
