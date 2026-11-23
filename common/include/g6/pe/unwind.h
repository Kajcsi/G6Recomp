#pragma once

// x64 exception data: RUNTIME_FUNCTION entries from .pdata and the UNWIND_INFO records they
// point to, decoded per the public x64 exception-handling documentation. The recompiler uses
// these to find function boundaries; the runtime will use them to unwind guest frames.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "g6/pe/image.h"

namespace g6::pe {

struct RuntimeFunction {
    uint32_t begin = 0;
    uint32_t end = 0; // exclusive
    // RVA of the UNWIND_INFO. Bit 0 set marks an indirect entry whose real RUNTIME_FUNCTION
    // lives at (unwind_info - 1); primary_function() and resolve_indirect() handle that.
    uint32_t unwind_info = 0;

    bool contains(uint32_t rva) const { return rva >= begin && rva < end; }
    bool indirect() const { return (unwind_info & 1u) != 0; }
};

inline constexpr uint8_t kUnwindFlagExceptionHandler = 0x1;
inline constexpr uint8_t kUnwindFlagTerminationHandler = 0x2;
inline constexpr uint8_t kUnwindFlagChainInfo = 0x4;

enum class UnwindOp : uint8_t {
    PushNonVolatile = 0,
    AllocLarge = 1,
    AllocSmall = 2,
    SetFramePointer = 3,
    SaveNonVolatile = 4,
    SaveNonVolatileFar = 5,
    Epilog = 6,    // version 2 epilog descriptor; legacy SaveXmm in version 1
    SpareCode = 7, // legacy SaveXmmFar in version 1
    SaveXmm128 = 8,
    SaveXmm128Far = 9,
    PushMachineFrame = 10,
};

struct UnwindCode {
    uint8_t prolog_offset = 0; // offset just past the prolog instruction this code describes
    UnwindOp op = UnwindOp::PushNonVolatile;
    uint8_t info = 0;   // register number, or the op's own info field
    uint32_t value = 0; // allocation size or save offset in bytes, when the op has one
};

struct UnwindInfo {
    uint8_t version = 0;
    uint8_t flags = 0;
    uint8_t prolog_size = 0;
    uint8_t frame_register = 0;    // 0 when the function has no frame pointer
    uint32_t frame_offset = 0;     // bytes, already scaled by 16
    std::vector<UnwindCode> codes; // prolog codes in stored order (reverse of execution)

    // Version 2 epilog descriptors: each epilog starts this many bytes before the end of the
    // function and is `epilog_size` bytes long.
    uint8_t epilog_size = 0;
    bool epilog_at_end = false;
    std::vector<uint32_t> epilog_offsets_from_end;

    std::optional<uint32_t> handler_rva; // language-specific handler (EHANDLER/UHANDLER)
    uint32_t handler_data_rva = 0;       // handler-specific data follows the handler RVA
    std::optional<RuntimeFunction> chained;

    bool has_handler() const {
        return (flags & (kUnwindFlagExceptionHandler | kUnwindFlagTerminationHandler)) != 0;
    }
};

// All .pdata entries in table order (the format requires ascending `begin`).
std::vector<RuntimeFunction> read_runtime_functions(const Image& image);

// Decodes the UNWIND_INFO at `rva`. The rva must not have the indirect bit set.
UnwindInfo read_unwind_info(const Image& image, uint32_t rva);

// Follows an indirect entry to the RUNTIME_FUNCTION it stands for (identity otherwise).
RuntimeFunction resolve_indirect(const Image& image, RuntimeFunction function);

// Follows indirect entries and chained unwind info to the primary entry of the function a
// fragment belongs to. Throws FormatError on cycles or chains deeper than 32.
RuntimeFunction primary_function(const Image& image, RuntimeFunction function);

// Binary search over entries sorted by `begin`.
const RuntimeFunction* find_runtime_function(std::span<const RuntimeFunction> sorted, uint32_t rva);

} // namespace g6::pe
