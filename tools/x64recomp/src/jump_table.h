#pragma once

// Works out what an indirect `jmp` does from the instructions that led to it on the same path:
// a switch dispatch through a jump table, a tail call through a pointer, or something unknown.
//
// Recognized table shapes (registers and instruction order vary):
//   * 32-bit image-relative entries, MSVC's usual form: the image base is loaded with a
//     RIP-relative `lea`, an entry is loaded from `base + index*4 + table`, the base is added
//     back, and the sum is the jump target. The index may itself come from a byte table that
//     maps sparse case values to entry numbers.
//   * 32-bit table-relative entries, clang's form: the table address is loaded with `lea`, a
//     sign-extended entry is loaded from `table + index*4`, and the table address is added.
//   * 64-bit absolute entries, read either by `jmp [base + index*8 + table]` or by a load into a
//     register that is then jumped through.
// The entry count comes from the bounds check on the index (a `cmp` against a constant followed
// by an unsigned branch around the dispatch) or from a mask (`and index, 2^k - 1`). Without
// either, entries are read while their targets stay valid and the table is marked unbounded.
// An unbounded read also stops where a byte-wide index runs out (256 entries) and at the start
// of any other table the caller already knows, since compilers lay tables out back to back.

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <Zydis/Decoder.h>

#include "g6/pe/image.h"

namespace g6::recomp {

// How many of the most recent path steps the analysis looks at. Callers may pass more.
inline constexpr size_t kJumpTablePathWindow = 64;

// One instruction executed on the path to an indirect jump.
struct PathStep {
    uint32_t pc = 0;
    bool branch_taken = false; // for a conditional branch: the path continued at its target
};

struct JumpTable {
    uint32_t jump = 0;             // the indirect jmp
    uint32_t table = 0;            // first entry
    uint8_t entry_size = 0;        // 4 (32-bit offsets) or 8 (absolute pointers)
    uint32_t index_table = 0;      // byte table mapping case values to entry numbers, or 0
    uint32_t index_count = 0;      // entries in the byte table
    bool bounded = false;          // the entry count came from a bounds check or a mask
    std::vector<uint32_t> targets; // destination of each entry, in table order
};

enum class IndirectJumpKind : uint8_t {
    kJumpTable,     // resolved; see IndirectJump::table
    kTailCall,      // through a pointer loaded from memory (a vtable slot, a function pointer)
    kRejectedTable, // shaped like a bounded table, but an entry is unreadable or leaves the code
    kUnknown,
};

struct IndirectJump {
    IndirectJumpKind kind = IndirectJumpKind::kUnknown;
    JumpTable table;
};

// `path` holds the instructions executed before the jmp at `jump_pc`, oldest first.
// `valid_target` says whether an address can be a destination inside the current function.
// `table_starts` (sorted) are tables found elsewhere, which an unbounded table can't run into.
IndirectJump analyze_indirect_jump(const pe::Image& image, const ZydisDecoder& decoder,
                                   std::span<const PathStep> path, uint32_t jump_pc,
                                   const std::function<bool(uint32_t)>& valid_target,
                                   std::span<const uint32_t> table_starts = {});

} // namespace g6::recomp
