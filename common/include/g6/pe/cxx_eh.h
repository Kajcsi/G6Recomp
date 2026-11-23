#pragma once

// The tables the MSVC C++ exception handlers read: __CxxFrameHandler3's FuncInfo and
// __CxxFrameHandler4's compressed FuncInfo4. Written from the formats as they are publicly
// described, and checked against compiler output and the CFG EH continuation table
// (docs/hooks.md, "C++ exception functions"). Only what the hook engine needs is decoded: the
// IP-to-state map, the try blocks with their catches, and the cleanup funclets the unwind map
// calls. Malformed tables throw FormatError.

#include <cstdint>
#include <vector>

#include "g6/pe/image.h"

namespace g6::pe {

enum class CxxEhFormat : uint8_t {
    kFh3, // __CxxFrameHandler3: plain structures, IP-to-state map in RVAs
    kFh4, // __CxxFrameHandler4: compressed, offsets from the function's start
};

struct CxxCatch {
    uint32_t handler = 0; // the catch funclet
    // Where the catch resumes, when FH4 records it. Empty otherwise: the funclet returns it.
    std::vector<uint32_t> continuations;
};

struct CxxTryBlock {
    int32_t try_low = 0; // the try body is in states [try_low, try_high]
    int32_t try_high = 0;
    int32_t catch_high = 0; // the catch blocks are in states (try_high, catch_high]
    std::vector<CxxCatch> catches;
};

struct CxxStateEntry {
    uint32_t rva = 0;   // the state starts here and lasts until the next entry
    int32_t state = -1; // -1: nothing to destroy and no try block
};

struct CxxFuncInfo {
    CxxEhFormat format = CxxEhFormat::kFh3;
    bool catch_funclet = false;             // FH4 marks the tables of a catch funclet
    int32_t states = 0;                     // states are in [-1, states)
    std::vector<CxxStateEntry> ip_to_state; // sorted by rva
    std::vector<CxxTryBlock> try_blocks;
    std::vector<uint32_t> cleanup_funclets; // sorted, without duplicates

    // The state at `rva`: that of the last entry at or before it, or -1 before the first.
    int32_t state_at(uint32_t rva) const;
    // Each state in [begin, end): the state at `begin`, then those of the entries inside.
    std::vector<int32_t> states_in(uint32_t begin, uint32_t end) const;
    // The try block whose body is in `state`, innermost first; null when there is none.
    const CxxTryBlock* try_block_for(int32_t state) const;
};

// The FH3 FuncInfo at `rva`, recognized by its magic number.
CxxFuncInfo read_fh3_func_info(const Image& image, uint32_t rva);

// Whether `rva` holds something with FH3's magic number, without reading further.
bool has_fh3_magic(const Image& image, uint32_t rva);

// The FH4 FuncInfo4 at `rva`. `function_start` is where the function's primary .pdata entry
// begins: FH4 counts IP-to-state and continuation offsets from it.
CxxFuncInfo read_fh4_func_info(const Image& image, uint32_t rva, uint32_t function_start);

// One FH4 compressed unsigned integer at `rva`, advancing `rva` past it. The low bits of the
// first byte give its length: x0 is one byte, x01 two, x011 three, x0111 four, and 1111 means
// the value is the four bytes that follow.
uint32_t read_fh4_unsigned(const Image& image, uint32_t& rva);

} // namespace g6::pe
