#pragma once

// Recursive-descent walk over x86-64 code in a mapped image. It follows control flow from
// entry points inside a set of address ranges, including through switch jump tables, and
// reports where control leaves: direct calls, tail jumps, import thunks, tail calls through
// pointers, and indirect jumps it cannot resolve.

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <Zydis/Decoder.h>

#include "g6/pe/image.h"
#include "jump_table.h"

namespace g6::recomp {

struct Range {
    uint32_t begin = 0;
    uint32_t end = 0; // exclusive

    bool contains(uint32_t rva) const { return rva >= begin && rva < end; }
};

struct WalkResult {
    uint32_t instructions = 0;
    uint32_t decode_errors = 0;
    uint32_t ran_off_end = 0;           // paths that fell through the end of the ranges
    uint32_t bad_targets = 0;           // branches whose target is outside the image
    uint32_t indirect_jumps = 0;        // unresolved indirect jumps, rejected tables included
    uint32_t rejected_tables = 0;       // shaped like a bounded table, but an entry was invalid
    uint32_t indirect_tail_calls = 0;   // jumps through a pointer loaded from memory
    uint32_t extent_end = 0;            // one past the highest byte decoded
    bool first_is_trap = false;         // the first instruction at the first entry is int3/ud2/...
    std::vector<uint32_t> call_targets; // direct call destinations
    std::vector<uint32_t> tail_targets; // direct jumps that leave the ranges
    std::vector<uint32_t> fallthrough_targets; // other functions' entries reached sequentially
    std::vector<uint32_t> import_jump_slots;   // `jmp [rip+disp]` memory slots (import thunks)
    std::vector<JumpTable> jump_tables;        // resolved switch tables, sorted by jump address
    // Where jumps that stay inside the ranges land: direct jumps, conditional or not, and the
    // entries of resolved switch tables. Sorted, without duplicates.
    std::vector<uint32_t> branch_targets;
    std::vector<uint32_t> decode_error_sites; // where each decode error happened, in walk order

    bool ok() const { return decode_errors == 0 && ran_off_end == 0 && bad_targets == 0; }
};

// Called once for each instruction the walk decodes, in walk order rather than address order.
using InstructionVisitor = std::function<void(uint32_t rva, const ZydisDecodedInstruction& insn)>;

class CodeWalker {
public:
    explicit CodeWalker(const pe::Image& image);

    // Walks from `entries`, staying inside `ranges`. A direct jump to an address for which
    // `is_other_function` returns true is treated as a tail call rather than a branch, and
    // sequential flow into such an address is reported as a fall-through into that function.
    WalkResult walk(std::span<const uint32_t> entries, std::span<const Range> ranges,
                    const std::function<bool(uint32_t)>& is_other_function,
                    const InstructionVisitor& visit = {}) const;

private:
    const pe::Image& image_;
    ZydisDecoder decoder_{};
};

} // namespace g6::recomp
