#include "code_walker.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <unordered_set>

#include <Zydis/Zydis.h>

#include "g6/log.h"

namespace g6::recomp {
namespace {

constexpr uint64_t kFastFailVector = 0x29;  // int 29h: __fastfail
constexpr uint64_t kAssertionVector = 0x2C; // int 2Ch: __int2c assertion failure

bool is_trap(const ZydisDecodedInstruction& insn) {
    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_UD0:
    case ZYDIS_MNEMONIC_UD1:
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_HLT: return true;
    case ZYDIS_MNEMONIC_INT: {
        const uint64_t vector = insn.raw.imm[0].value.u;
        return vector == kFastFailVector || vector == kAssertionVector;
    }
    default: return false;
    }
}

void sort_unique(std::vector<uint32_t>& values) {
    std::ranges::sort(values);
    const auto [first, last] = std::ranges::unique(values);
    values.erase(first, last);
}

} // namespace

CodeWalker::CodeWalker(const pe::Image& image) : image_(image) {
    ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
}

WalkResult CodeWalker::walk(std::span<const uint32_t> entries, std::span<const Range> ranges,
                            const std::function<bool(uint32_t)>& is_other_function,
                            const InstructionVisitor& visit) const {
    WalkResult result;
    const auto mapped = image_.mapped();
    const auto range_of = [&](uint32_t rva) -> const Range* {
        for (const Range& range : ranges) {
            if (range.contains(rva)) {
                return &range;
            }
        }
        return nullptr;
    };
    // Relative branch target, or nullopt when it lands outside the image.
    const auto branch_target = [&](uint32_t pc,
                                   const ZydisDecodedInstruction& insn) -> std::optional<uint32_t> {
        const int64_t target = int64_t{pc} + insn.length + insn.raw.imm[0].value.s;
        if (target < 0 || target >= int64_t{image_.size_of_image()}) {
            ++result.bad_targets;
            return std::nullopt;
        }
        return static_cast<uint32_t>(target);
    };
    const auto stays_inside = [&](uint32_t target) {
        return range_of(target) != nullptr && !is_other_function(target);
    };

    // Each pending path carries the instructions that led to it, so an indirect jump at its end
    // can be traced back to the table load and the bounds check.
    struct Pending {
        uint32_t pc = 0;
        std::vector<PathStep> history;
    };
    std::vector<Pending> worklist;
    for (const uint32_t entry : std::views::reverse(entries)) {
        worklist.push_back({entry, {}});
    }
    std::unordered_set<uint32_t> visited;
    bool first_instruction = true;

    while (!worklist.empty()) {
        Pending pending = std::move(worklist.back());
        worklist.pop_back();
        uint32_t pc = pending.pc;
        std::vector<PathStep>& path = pending.history;
        const auto record = [&path](uint32_t at, bool taken) {
            path.push_back({at, taken});
            if (path.size() >= 2 * kJumpTablePathWindow) {
                path.erase(path.begin(), path.end() - kJumpTablePathWindow);
            }
        };

        for (bool first_step = true;; first_step = false) {
            // Worklist entries are the walk's own entries or branch targets already checked
            // against is_other_function; only sequential flow needs the check here.
            if (!first_step && is_other_function(pc)) {
                result.fallthrough_targets.push_back(pc);
                break;
            }
            const Range* range = range_of(pc);
            if (range == nullptr) {
                ++result.ran_off_end;
                break;
            }
            if (!visited.insert(pc).second) {
                break;
            }
            ZydisDecoderContext context;
            ZydisDecodedInstruction insn;
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder_, &context, mapped.data() + pc,
                                                            range->end - pc, &insn))) {
                ++result.decode_errors;
                result.decode_error_sites.push_back(pc);
                break;
            }
            ++result.instructions;
            if (visit) {
                visit(pc, insn);
            }
            const uint32_t next = pc + insn.length;
            result.extent_end = std::max(result.extent_end, next);
            const bool relative = insn.raw.imm[0].is_relative != 0;

            if (first_instruction) {
                result.first_is_trap = is_trap(insn);
                first_instruction = false;
            }

            if (insn.meta.category == ZYDIS_CATEGORY_RET || is_trap(insn)) {
                break;
            }
            if (insn.meta.category == ZYDIS_CATEGORY_CALL) {
                if (relative) {
                    if (const auto target = branch_target(pc, insn)) {
                        result.call_targets.push_back(*target);
                    }
                }
                record(pc, false);
                pc = next;
                continue;
            }
            if (insn.meta.category == ZYDIS_CATEGORY_COND_BR) {
                if (const auto target = branch_target(pc, insn)) {
                    if (stays_inside(*target)) {
                        result.branch_targets.push_back(*target);
                        const size_t keep = std::min(path.size(), kJumpTablePathWindow - 1);
                        std::vector<PathStep> history(path.end() - static_cast<ptrdiff_t>(keep),
                                                      path.end());
                        history.push_back({pc, true});
                        worklist.push_back({*target, std::move(history)});
                    } else {
                        result.tail_targets.push_back(*target); // conditional tail call
                    }
                }
                record(pc, false);
                pc = next;
                continue;
            }
            if (insn.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                if (relative) {
                    if (const auto target = branch_target(pc, insn)) {
                        if (stays_inside(*target)) {
                            result.branch_targets.push_back(*target);
                            pc = *target; // changes no register or flag, so not recorded
                            continue;
                        }
                        result.tail_targets.push_back(*target);
                    }
                } else if ((insn.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0 &&
                           insn.raw.modrm.mod == 0 && insn.raw.modrm.rm == 5) {
                    // jmp [rip+disp32]: a jump through a pointer slot, typically an import thunk.
                    const int64_t slot = int64_t{next} + insn.raw.disp.value;
                    if (slot >= 0 && slot < int64_t{image_.size_of_image()}) {
                        result.import_jump_slots.push_back(static_cast<uint32_t>(slot));
                    }
                } else {
                    const IndirectJump jump =
                        analyze_indirect_jump(image_, decoder_, path, pc, stays_inside);
                    switch (jump.kind) {
                    case IndirectJumpKind::kJumpTable: {
                        std::vector<uint32_t> targets = jump.table.targets;
                        sort_unique(targets);
                        for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
                            worklist.push_back({*it, {}});
                        }
                        result.branch_targets.insert(result.branch_targets.end(), targets.begin(),
                                                     targets.end());
                        result.jump_tables.push_back(jump.table);
                        break;
                    }
                    case IndirectJumpKind::kTailCall: ++result.indirect_tail_calls; break;
                    case IndirectJumpKind::kRejectedTable:
                        LOG_DEBUG(walker, "jump at {:#x}: table at {:#x} has an invalid entry", pc,
                                  jump.table.table);
                        ++result.rejected_tables;
                        ++result.indirect_jumps;
                        break;
                    case IndirectJumpKind::kUnknown:
                        LOG_DEBUG(walker, "jump at {:#x}: unresolved", pc);
                        ++result.indirect_jumps;
                        break;
                    }
                }
                break;
            }
            record(pc, false);
            pc = next;
        }
    }

    sort_unique(result.call_targets);
    sort_unique(result.tail_targets);
    sort_unique(result.fallthrough_targets);
    sort_unique(result.import_jump_slots);
    sort_unique(result.branch_targets);
    std::ranges::sort(result.jump_tables, {}, &JumpTable::jump);
    return result;
}

} // namespace g6::recomp
