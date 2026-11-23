#include "g6/pe/cxx_eh.h"

#include <algorithm>
#include <array>

#include <fmt/format.h>

namespace g6::pe {
namespace {

// More states, entries, or catches than any real function has; anything larger is malformed.
constexpr uint32_t kMaxEntries = 1u << 20;
constexpr uint32_t kMaxCatches = 1024;

// FH3: the low 29 bits of the first field. The later magic numbers add fields at the end.
constexpr uint32_t kFh3MagicMask = 0x1FFFFFFF;
constexpr std::array<uint32_t, 3> kFh3Magic = {0x19930520, 0x19930521, 0x19930522};
constexpr uint32_t kFh3FuncInfoSize = 28; // up to and including the IP-to-state map's RVA
constexpr uint32_t kFh3UnwindEntrySize = 8;
constexpr uint32_t kFh3TryBlockSize = 20;
constexpr uint32_t kFh3HandlerSize = 20;
constexpr uint32_t kFh3HandlerOffset = 12; // the catch funclet's RVA within a handler entry
constexpr uint32_t kFh3IpEntrySize = 8;

// FH4 FuncInfo4 header bits.
constexpr uint8_t kFh4IsCatch = 0x01;
constexpr uint8_t kFh4IsSeparated = 0x02;
constexpr uint8_t kFh4Bbt = 0x04;
constexpr uint8_t kFh4UnwindMap = 0x08;
constexpr uint8_t kFh4TryBlockMap = 0x10;

// FH4 unwind map entry types, in the low two bits of the entry's first value.
constexpr uint32_t kFh4DtorWithObject = 1;
constexpr uint32_t kFh4DtorWithPointer = 2;
constexpr uint32_t kFh4Funclet = 3;

// FH4 handler header bits.
constexpr uint8_t kFh4Adjectives = 0x01;
constexpr uint8_t kFh4DispType = 0x02;
constexpr uint8_t kFh4DispCatchObject = 0x04;
constexpr uint8_t kFh4ContinuationIsRva = 0x08;
constexpr uint8_t kFh4ContinuationCountShift = 4;
constexpr uint8_t kFh4ContinuationCountMask = 0x3;

uint32_t checked_count(uint32_t count, uint32_t limit, const char* what, uint32_t rva) {
    if (count > limit) {
        throw FormatError(
            fmt::format("C++ EH tables at {:#x}: implausible {} {}", rva, count, what));
    }
    return count;
}

uint32_t code_rva(const Image& image, int64_t rva, const char* what, uint32_t tables) {
    if (rva <= 0 || rva > int64_t{UINT32_MAX} || !image.is_executable(static_cast<uint32_t>(rva))) {
        throw FormatError(
            fmt::format("C++ EH tables at {:#x}: {} {:#x} isn't code", tables, what, rva));
    }
    return static_cast<uint32_t>(rva);
}

int32_t read_int(const Image& image, uint32_t& rva) {
    const auto value = image.read<int32_t>(rva);
    rva += 4;
    return value;
}

void check_states(const CxxFuncInfo& info, uint32_t tables) {
    for (const CxxStateEntry& entry : info.ip_to_state) {
        if (entry.state < -1 || entry.state >= info.states) {
            throw FormatError(fmt::format("C++ EH tables at {:#x}: state {} at {:#x} is out of "
                                          "range",
                                          tables, entry.state, entry.rva));
        }
    }
    for (const CxxTryBlock& block : info.try_blocks) {
        if (block.try_low < 0 || block.try_low > block.try_high ||
            block.try_high >= block.catch_high || block.catch_high >= info.states) {
            throw FormatError(fmt::format("C++ EH tables at {:#x}: try block states [{}, {}], "
                                          "catches to {}, out of range",
                                          tables, block.try_low, block.try_high, block.catch_high));
        }
    }
}

void finish(CxxFuncInfo& info, uint32_t tables) {
    std::ranges::stable_sort(info.ip_to_state, {}, &CxxStateEntry::rva);
    std::ranges::sort(info.cleanup_funclets);
    const auto [first, last] = std::ranges::unique(info.cleanup_funclets);
    info.cleanup_funclets.erase(first, last);
    check_states(info, tables);
}

// One FH4 IP-to-state map: offsets from `base`, each state stored plus one.
void read_fh4_ip_map(const Image& image, uint32_t rva, uint32_t base, CxxFuncInfo& info,
                     uint32_t tables) {
    const uint32_t count =
        checked_count(read_fh4_unsigned(image, rva), kMaxEntries, "IP-to-state entries", tables);
    uint64_t offset = 0;
    bool first = true;
    for (uint32_t i = 0; i < count; ++i) {
        offset += read_fh4_unsigned(image, rva);
        const int64_t state = int64_t{read_fh4_unsigned(image, rva)} - 1;
        if (first && offset != 0) {
            // The segment starts in no state; say so, so that merging segments can't carry the
            // previous segment's last state into it.
            info.ip_to_state.push_back({base, -1});
        }
        first = false;
        info.ip_to_state.push_back({code_rva(image, int64_t{base} + static_cast<int64_t>(offset),
                                             "IP-to-state entry", tables),
                                    static_cast<int32_t>(std::min<int64_t>(state, INT32_MAX))});
    }
}

} // namespace

int32_t CxxFuncInfo::state_at(uint32_t rva) const {
    const auto it = std::ranges::upper_bound(ip_to_state, rva, {}, &CxxStateEntry::rva);
    return it == ip_to_state.begin() ? -1 : std::prev(it)->state;
}

std::vector<int32_t> CxxFuncInfo::states_in(uint32_t begin, uint32_t end) const {
    std::vector<int32_t> states{state_at(begin)};
    for (auto it = std::ranges::upper_bound(ip_to_state, begin, {}, &CxxStateEntry::rva);
         it != ip_to_state.end() && it->rva < end; ++it) {
        states.push_back(it->state);
    }
    return states;
}

const CxxTryBlock* CxxFuncInfo::try_block_for(int32_t state) const {
    const CxxTryBlock* innermost = nullptr;
    for (const CxxTryBlock& block : try_blocks) {
        if (state >= block.try_low && state <= block.try_high &&
            (innermost == nullptr || block.try_low > innermost->try_low)) {
            innermost = &block;
        }
    }
    return innermost;
}

uint32_t read_fh4_unsigned(const Image& image, uint32_t& rva) {
    const auto first = image.read<uint8_t>(rva);
    uint32_t value = 0;
    if ((first & 0x1) == 0) {
        value = first >> 1;
        rva += 1;
    } else if ((first & 0x3) == 0x1) {
        value = uint32_t{image.read<uint16_t>(rva)} >> 2;
        rva += 2;
    } else if ((first & 0x7) == 0x3) {
        value = (uint32_t{image.read<uint16_t>(rva)} |
                 (uint32_t{image.read<uint8_t>(rva + 2)} << 16)) >>
                3;
        rva += 3;
    } else if ((first & 0xF) == 0x7) {
        value = image.read<uint32_t>(rva) >> 4;
        rva += 4;
    } else {
        value = image.read<uint32_t>(rva + 1);
        rva += 5;
    }
    return value;
}

bool has_fh3_magic(const Image& image, uint32_t rva) {
    if (!image.contains(rva, kFh3FuncInfoSize)) {
        return false;
    }
    const uint32_t magic = image.read<uint32_t>(rva) & kFh3MagicMask;
    return std::ranges::find(kFh3Magic, magic) != kFh3Magic.end();
}

CxxFuncInfo read_fh3_func_info(const Image& image, uint32_t rva) {
    if (!has_fh3_magic(image, rva)) {
        throw FormatError(fmt::format("{:#x} isn't an FH3 FuncInfo", rva));
    }
    CxxFuncInfo info;
    info.format = CxxEhFormat::kFh3;
    const auto max_state = image.read<int32_t>(rva + 4);
    const auto unwind_map = image.read<uint32_t>(rva + 8);
    const uint32_t try_count =
        checked_count(image.read<uint32_t>(rva + 12), kMaxEntries, "try blocks", rva);
    const auto try_map = image.read<uint32_t>(rva + 16);
    const uint32_t ip_count =
        checked_count(image.read<uint32_t>(rva + 20), kMaxEntries, "IP-to-state entries", rva);
    const auto ip_map = image.read<uint32_t>(rva + 24);
    if (max_state < 0) {
        throw FormatError(fmt::format("C++ EH tables at {:#x}: {} states", rva, max_state));
    }
    info.states = static_cast<int32_t>(
        checked_count(static_cast<uint32_t>(max_state), kMaxEntries, "states", rva));

    for (int32_t state = 0; state < info.states; ++state) {
        const auto action = image.read<int32_t>(
            unwind_map + (static_cast<uint32_t>(state) * kFh3UnwindEntrySize) + 4);
        if (action != 0) {
            info.cleanup_funclets.push_back(code_rva(image, action, "cleanup funclet", rva));
        }
    }
    for (uint32_t i = 0; i < try_count; ++i) {
        const uint32_t entry = try_map + (i * kFh3TryBlockSize);
        CxxTryBlock block{image.read<int32_t>(entry),
                          image.read<int32_t>(entry + 4),
                          image.read<int32_t>(entry + 8),
                          {}};
        const uint32_t catches =
            checked_count(image.read<uint32_t>(entry + 12), kMaxCatches, "catches", rva);
        const auto handlers = image.read<uint32_t>(entry + 16);
        for (uint32_t c = 0; c < catches; ++c) {
            const auto handler =
                image.read<int32_t>(handlers + (c * kFh3HandlerSize) + kFh3HandlerOffset);
            block.catches.push_back({code_rva(image, handler, "catch funclet", rva), {}});
        }
        info.try_blocks.push_back(std::move(block));
    }
    for (uint32_t i = 0; i < ip_count; ++i) {
        const uint32_t entry = ip_map + (i * kFh3IpEntrySize);
        info.ip_to_state.push_back(
            {code_rva(image, image.read<int32_t>(entry), "IP-to-state entry", rva),
             image.read<int32_t>(entry + 4)});
    }
    finish(info, rva);
    return info;
}

CxxFuncInfo read_fh4_func_info(const Image& image, uint32_t rva, uint32_t function_start) {
    CxxFuncInfo info;
    info.format = CxxEhFormat::kFh4;
    uint32_t p = rva;
    const auto header = image.read<uint8_t>(p++);
    info.catch_funclet = (header & kFh4IsCatch) != 0;
    if ((header & kFh4Bbt) != 0) {
        read_fh4_unsigned(image, p);
    }
    const auto unwind_map = (header & kFh4UnwindMap) != 0 ? read_int(image, p) : 0;
    const auto try_map = (header & kFh4TryBlockMap) != 0 ? read_int(image, p) : 0;
    const auto ip_map = static_cast<uint32_t>(read_int(image, p));

    if (unwind_map != 0) {
        auto u = static_cast<uint32_t>(unwind_map);
        info.states = static_cast<int32_t>(
            checked_count(read_fh4_unsigned(image, u), kMaxEntries, "states", rva));
        for (int32_t state = 0; state < info.states; ++state) {
            const uint32_t type = read_fh4_unsigned(image, u) & 0x3;
            if (type == kFh4DtorWithObject || type == kFh4DtorWithPointer) {
                read_int(image, u); // the destructor
                read_fh4_unsigned(image, u);
            } else if (type == kFh4Funclet) {
                info.cleanup_funclets.push_back(
                    code_rva(image, read_int(image, u), "cleanup funclet", rva));
            }
        }
    }
    if (try_map != 0) {
        auto t = static_cast<uint32_t>(try_map);
        const uint32_t count =
            checked_count(read_fh4_unsigned(image, t), kMaxEntries, "try blocks", rva);
        for (uint32_t i = 0; i < count; ++i) {
            CxxTryBlock block;
            block.try_low = static_cast<int32_t>(read_fh4_unsigned(image, t));
            block.try_high = static_cast<int32_t>(read_fh4_unsigned(image, t));
            block.catch_high = static_cast<int32_t>(read_fh4_unsigned(image, t));
            auto h = static_cast<uint32_t>(read_int(image, t));
            const uint32_t catches =
                checked_count(read_fh4_unsigned(image, h), kMaxCatches, "catches", rva);
            for (uint32_t c = 0; c < catches; ++c) {
                const auto flags = image.read<uint8_t>(h++);
                if ((flags & kFh4Adjectives) != 0) {
                    read_fh4_unsigned(image, h);
                }
                if ((flags & kFh4DispType) != 0) {
                    read_int(image, h);
                }
                if ((flags & kFh4DispCatchObject) != 0) {
                    read_fh4_unsigned(image, h);
                }
                CxxCatch entry{code_rva(image, read_int(image, h), "catch funclet", rva), {}};
                const uint32_t continuations =
                    (flags >> kFh4ContinuationCountShift) & kFh4ContinuationCountMask;
                if (continuations > 2) {
                    throw FormatError(
                        fmt::format("C++ EH tables at {:#x}: reserved continuation count", rva));
                }
                for (uint32_t k = 0; k < continuations; ++k) {
                    const int64_t target =
                        (flags & kFh4ContinuationIsRva) != 0
                            ? int64_t{read_int(image, h)}
                            : int64_t{function_start} + int64_t{read_fh4_unsigned(image, h)};
                    entry.continuations.push_back(code_rva(image, target, "continuation", rva));
                }
                block.catches.push_back(std::move(entry));
            }
            info.try_blocks.push_back(std::move(block));
        }
    }
    if ((header & kFh4IsSeparated) != 0) {
        uint32_t s = ip_map;
        const uint32_t segments =
            checked_count(read_fh4_unsigned(image, s), kMaxEntries, "code segments", rva);
        for (uint32_t i = 0; i < segments; ++i) {
            const uint32_t start = code_rva(image, read_int(image, s), "code segment", rva);
            const auto map = static_cast<uint32_t>(read_int(image, s));
            read_fh4_ip_map(image, map, start, info, rva);
        }
    } else {
        read_fh4_ip_map(image, ip_map, function_start, info, rva);
    }
    finish(info, rva);
    return info;
}

} // namespace g6::pe
