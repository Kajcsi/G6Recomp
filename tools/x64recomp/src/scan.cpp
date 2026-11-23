#include "scan.h"

#include <algorithm>
#include <array>
#include <bit>
#include <intrin.h>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <Zydis/Zydis.h>
#include <fmt/format.h>

#include "code_walker.h"
#include "g6/log.h"
#include "g6/pe/directories.h"

namespace g6::recomp {
namespace {

using pe::Image;

constexpr int64_t kTebStackBase = 0x08;
constexpr int64_t kTebStackLimit = 0x10;
constexpr int64_t kTebSelf = 0x30;
constexpr int64_t kTebTlsArray = 0x58;
constexpr int64_t kTebPeb = 0x60;
constexpr int64_t kTebTlsSlots = 0x1480;
constexpr int64_t kTebTlsSlotCount = 64;

constexpr uint64_t kFastFailVector = 0x29;
constexpr uint64_t kAssertionVector = 0x2C;
constexpr uint8_t kInt3 = 0xCC;

constexpr uint16_t kRelocsStripped = 0x0001;
constexpr uint32_t kTlsAlignShift = 20;
constexpr uint32_t kTlsAlignMask = 0xF;

// Instructions followed after a load of a TEB pointer or a code address, looking for the first
// memory access through it. Compilers schedule the TLS index load and unrelated moves in between.
constexpr int kFollowWindow = 16;

// Rounds of walking from addresses taken inside functions; each round can take new ones.
constexpr int kExtraWalkRounds = 8;

// Desktop x64 TEB fields. 0x00 to 0x30 are NT_TIB (winnt.h); the PEB, TlsSlots, and
// TlsExpansionSlots offsets follow from winternl.h; the rest are stable fields that public
// debugger symbols name.
struct TebField {
    int64_t offset;
    const char* name;
};
constexpr TebField kTebFields[] = {
    {0x00, "ExceptionList"},
    {kTebStackBase, "StackBase"},
    {kTebStackLimit, "StackLimit"},
    {0x18, "SubSystemTib"},
    {0x20, "FiberData"},
    {0x28, "ArbitraryUserPointer"},
    {kTebSelf, "Self"},
    {0x38, "EnvironmentPointer"},
    {0x40, "ClientId.UniqueProcess"},
    {0x48, "ClientId.UniqueThread"},
    {0x50, "ActiveRpcHandle"},
    {kTebTlsArray, "ThreadLocalStoragePointer"},
    {kTebPeb, "ProcessEnvironmentBlock"},
    {0x68, "LastErrorValue"},
    {0x1780, "TlsExpansionSlots"},
};

// ---- CPU features ----

enum class CpuidRegister : uint8_t { kEax, kEbx, kEcx, kEdx };

struct CpuidBit {
    uint32_t leaf = 0;
    uint32_t subleaf = 0;
    CpuidRegister reg = CpuidRegister::kEax;
    uint8_t bit = 0;
};

struct ExtensionInfo {
    ZydisISAExt extension;
    FeatureClass feature_class;
    CpuidBit cpuid; // set for host-check extensions
};

// Every extension not listed here is one Zen 2 lacks (AVX-512, XOP, FMA4, TBM, VAES, GFNI,
// AMX, and so on). The baseline is what every supported host has: AVX2 hosts (Intel Haswell and
// AMD Zen onwards) all have these. The host-check rows are Zen 2 features that some of those
// hosts lack, with the cpuid bit that reports each.
constexpr CpuidRegister kEax = CpuidRegister::kEax;
constexpr CpuidRegister kEbx = CpuidRegister::kEbx;
constexpr CpuidRegister kEcx = CpuidRegister::kEcx;
constexpr ExtensionInfo kExtensions[] = {
    {ZYDIS_ISA_EXT_BASE, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_LONGMODE, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_X87, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_MMX, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_SSE, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_SSE2, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_SSE3, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_SSSE3, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_SSE4, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_AVX, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_AVX2, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_AVX2GATHER, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_AVXAES, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_AES, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_PCLMULQDQ, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_BMI1, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_BMI2, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_FMA, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_F16C, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_LZCNT, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_MOVBE, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_XSAVE, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_XSAVEOPT, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_PAUSE, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_CLFSH, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_RDRAND, FeatureClass::kBaseline, {}},
    {ZYDIS_ISA_EXT_RDTSCP, FeatureClass::kBaseline, {}},
    // prefetchw: a no-op on the Intel hosts that predate it.
    {ZYDIS_ISA_EXT_AMD3DNOW_PREFETCH, FeatureClass::kBaseline, {}},

    {ZYDIS_ISA_EXT_ADOX_ADCX, FeatureClass::kHostCheck, {7, 0, kEbx, 19}},
    {ZYDIS_ISA_EXT_RDSEED, FeatureClass::kHostCheck, {7, 0, kEbx, 18}},
    {ZYDIS_ISA_EXT_CLFLUSHOPT, FeatureClass::kHostCheck, {7, 0, kEbx, 23}},
    {ZYDIS_ISA_EXT_CLWB, FeatureClass::kHostCheck, {7, 0, kEbx, 24}},
    {ZYDIS_ISA_EXT_SHA, FeatureClass::kHostCheck, {7, 0, kEbx, 29}},
    {ZYDIS_ISA_EXT_RDPID, FeatureClass::kHostCheck, {7, 0, kEcx, 22}},
    {ZYDIS_ISA_EXT_XSAVEC, FeatureClass::kHostCheck, {0xD, 1, kEax, 1}},
    {ZYDIS_ISA_EXT_SSE4A, FeatureClass::kHostCheck, {0x80000001, 0, kEcx, 6}},
    {ZYDIS_ISA_EXT_MONITORX, FeatureClass::kHostCheck, {0x80000001, 0, kEcx, 29}},
    {ZYDIS_ISA_EXT_CLZERO, FeatureClass::kHostCheck, {0x80000008, 0, kEbx, 0}},
    {ZYDIS_ISA_EXT_RDPRU, FeatureClass::kHostCheck, {0x80000008, 0, kEbx, 4}},
    {ZYDIS_ISA_EXT_MCOMMIT, FeatureClass::kHostCheck, {0x80000008, 0, kEbx, 8}},
};

const ExtensionInfo* find_extension(ZydisISAExt extension) {
    for (const ExtensionInfo& info : kExtensions) {
        if (info.extension == extension) {
            return &info;
        }
    }
    return nullptr;
}

FeatureClass class_of(ZydisISAExt extension) {
    const ExtensionInfo* info = find_extension(extension);
    return info != nullptr ? info->feature_class : FeatureClass::kNotZen2;
}

// Shadow-stack instructions that are no-ops without CET (endbr, rdssp), and incssp, which the C
// runtime's longjmp runs only after rdssp has reported a shadow stack.
bool is_cet_noop(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_ENDBR32:
    case ZYDIS_MNEMONIC_ENDBR64:
    case ZYDIS_MNEMONIC_RDSSPD:
    case ZYDIS_MNEMONIC_RDSSPQ:
    case ZYDIS_MNEMONIC_INCSSPD:
    case ZYDIS_MNEMONIC_INCSSPQ: return true;
    default: return false;
    }
}

// ---- Instruction groups ----

const char* trap_name(const ZydisDecodedInstruction& insn) {
    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_INT3: return "int3";
    case ZYDIS_MNEMONIC_UD0: return "ud0";
    case ZYDIS_MNEMONIC_UD1: return "ud1";
    case ZYDIS_MNEMONIC_UD2: return "ud2";
    case ZYDIS_MNEMONIC_INT:
        switch (insn.raw.imm[0].value.u) {
        case 3: return "int 3";
        case kFastFailVector: return "int 0x29 (fail fast)";
        case kAssertionVector: return "int 0x2c (assertion)";
        default: return nullptr;
        }
    default: return nullptr;
    }
}

const char* timing_name(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_RDTSC: return "rdtsc";
    case ZYDIS_MNEMONIC_RDTSCP: return "rdtscp";
    case ZYDIS_MNEMONIC_RDPRU: return "rdpru";
    default: return nullptr;
    }
}

// Instructions that user-mode game code has no business running on a desktop: system calls,
// other interrupts, I/O, privileged and segment instructions, far branches. Traps, timing,
// cpuid, and xgetbv are sorted out before this is asked.
std::optional<std::string> system_name(const ZydisDecodedInstruction& insn) {
    const char* mnemonic = ZydisMnemonicGetString(insn.mnemonic);
    if (insn.mnemonic == ZYDIS_MNEMONIC_INT) {
        return fmt::format("int {:#x}", insn.raw.imm[0].value.u);
    }
    switch (insn.meta.category) {
    case ZYDIS_CATEGORY_SYSCALL:
    case ZYDIS_CATEGORY_SYSRET:
    case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_IO:
    case ZYDIS_CATEGORY_IOSTRINGOP:
    case ZYDIS_CATEGORY_SYSTEM:
    case ZYDIS_CATEGORY_SEGOP:
    case ZYDIS_CATEGORY_RDWRFSGS:
    case ZYDIS_CATEGORY_VTX:
    case ZYDIS_CATEGORY_SGX: return std::string(mnemonic);
    default: break;
    }
    if ((insn.attributes & ZYDIS_ATTRIB_IS_PRIVILEGED) != 0) {
        return std::string(mnemonic);
    }
    if (insn.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
        return fmt::format("far {}", mnemonic);
    }
    // mov to or from a segment register, and push or pop of fs or gs.
    const bool sreg_mov = insn.mnemonic == ZYDIS_MNEMONIC_MOV &&
                          insn.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT &&
                          (insn.opcode == 0x8C || insn.opcode == 0x8E);
    const bool sreg_stack =
        (insn.mnemonic == ZYDIS_MNEMONIC_PUSH || insn.mnemonic == ZYDIS_MNEMONIC_POP) &&
        insn.opcode_map == ZYDIS_OPCODE_MAP_0F &&
        (insn.opcode == 0xA0 || insn.opcode == 0xA1 || insn.opcode == 0xA8 || insn.opcode == 0xA9);
    if (sreg_mov || sreg_stack) {
        return fmt::format("{} (segment register)", mnemonic);
    }
    return std::nullopt;
}

bool is_rip_relative(const ZydisDecodedInstruction& insn) {
    return (insn.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0 && insn.raw.modrm.mod == 0 &&
           insn.raw.modrm.rm == 5 && insn.address_width == 64;
}

bool is_branch(const ZydisDecodedInstruction& insn) {
    switch (insn.meta.category) {
    case ZYDIS_CATEGORY_CALL:
    case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_COND_BR:
    case ZYDIS_CATEGORY_UNCOND_BR:
    case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_SYSCALL: return true;
    default: return false;
    }
}

ZydisRegister enclosing(ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_NONE
               ? reg
               : ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
}

void add_site(Sites& sites, uint32_t rva, bool swept) {
    (swept ? sites.swept : sites.walked).push_back(rva);
}

void sort_unique(std::vector<uint32_t>& values) {
    std::ranges::sort(values);
    const auto [first, last] = std::ranges::unique(values);
    values.erase(first, last);
}

void finish_sites(Sites& sites) {
    sort_unique(sites.walked);
    sort_unique(sites.swept);
}

std::vector<InstructionUse> to_uses(std::map<std::string, Sites>& groups) {
    std::vector<InstructionUse> uses;
    for (auto& [name, sites] : groups) {
        finish_sites(sites);
        uses.push_back({name, std::move(sites)});
    }
    return uses;
}

template <typename Reader>
auto read_directory(const char* name, size_t& errors, Reader&& reader) -> decltype(reader()) {
    try {
        return reader();
    } catch (const pe::FormatError& error) {
        ++errors;
        LOG_WARN(scan, "{} directory unreadable: {}", name, error.what());
        return {};
    }
}

// The first memory access through `reg` in the instructions from `pc` on, before a branch or a
// write to the register ends the window.
struct RegisterUse {
    bool found = false;
    uint32_t rva = 0;
    ZydisDecodedOperand operand{};
};

// ---- The scan ----

class Scanner {
public:
    Scanner(const Image& image, const DiscoveryResult& discovery, ScanReport& report)
        : image_(image), discovery_(discovery), report_(report), covered_(image.size_of_image()) {
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        for (const DiscoveredFunction& function : discovery.functions) {
            for (const JumpTable& table : function.jump_tables) {
                tables_.push_back(
                    {table.table,
                     table.table + static_cast<uint32_t>(table.targets.size()) * table.entry_size});
                if (table.index_table != 0) {
                    tables_.push_back({table.index_table, table.index_table + table.index_count});
                }
            }
        }
        std::ranges::sort(tables_, {}, &Range::begin);
        if (report.tls && report.tls->index_rva) {
            tls_index_ = *report.tls->index_rva;
        }
    }

    void walk_functions(const std::vector<uint32_t>& eh_continuations) {
        for (const DiscoveredFunction& function : discovery_.functions) {
            // Code reached only by unwinding (C++ catch continuations, for example) has no edge
            // from the entry; the EH continuation table names it when the module has one.
            std::vector<uint32_t> entries{function.start};
            for (const Range& range : ranges_of(function)) {
                auto it = std::ranges::lower_bound(eh_continuations, range.begin);
                for (; it != eh_continuations.end() && *it < range.end; ++it) {
                    entries.push_back(*it);
                }
            }
            walk(function, entries, false);
        }
    }

    // Walks again from the addresses inside functions that code or relocated pointers take,
    // such as the continuation a catch funclet hands back when its module has no EH
    // continuation table, until no new code turns up.
    void walk_taken_addresses() {
        size_t done = 0;
        for (int round = 0; round < kExtraWalkRounds && done < report_.code_addresses.size();
             ++round) {
            std::map<const DiscoveredFunction*, std::vector<uint32_t>> entries;
            for (; done < report_.code_addresses.size(); ++done) {
                const uint32_t target = report_.code_addresses[done].target;
                if (!covered_[target]) {
                    entries[discovery_.containing(target)].push_back(target);
                }
            }
            for (const auto& [function, starts] : entries) {
                walk(*function, starts, true);
            }
        }
    }

    // Decodes the .pdata ranges the walk didn't reach, linearly, skipping int3 padding and
    // jump tables. What it finds there may be data, so it is reported apart from the walk.
    void sweep(bool decode) {
        const auto mapped = image_.mapped();
        for (const DiscoveredFunction& function : discovery_.functions) {
            if (!function.has_unwind_info) {
                continue;
            }
            for (const Range& range : ranges_of(function)) {
                const uint32_t end = std::min<uint32_t>(range.end, image_.size_of_image());
                if (range.begin >= end) {
                    continue;
                }
                report_.coverage.pdata_bytes += end - range.begin;
                uint32_t pc = range.begin;
                while (pc < end) {
                    if (covered_[pc] || mapped[pc] == kInt3 || in_table(pc)) {
                        ++pc;
                        continue;
                    }
                    ZydisDecodedInstruction insn;
                    const bool ok =
                        decode && ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
                                      &decoder_, nullptr, mapped.data() + pc, end - pc, &insn));
                    if (!ok || overlaps_covered(pc, insn.length)) {
                        ++report_.coverage.unreached_bytes;
                        ++pc;
                        continue;
                    }
                    mark(pc, insn.length);
                    const bool padding = insn.meta.category == ZYDIS_CATEGORY_NOP ||
                                         insn.meta.category == ZYDIS_CATEGORY_WIDENOP;
                    if (!padding) {
                        report_.coverage.unreached_bytes += insn.length;
                        ++report_.coverage.swept_instructions;
                        instruction(pc, insn, function.start, true);
                    }
                    pc += insn.length;
                }
            }
        }
    }

    // Relocated pointers into code that don't point at a function's start.
    void scan_pointers(const std::vector<pe::Relocation>& relocations) {
        for (const pe::Relocation& relocation : relocations) {
            if (relocation.type != pe::kRelocDir64 || !image_.contains(relocation.rva, 8)) {
                continue;
            }
            const auto target = image_.try_va_to_rva(image_.read<uint64_t>(relocation.rva));
            if (target && is_inner_code(*target)) {
                report_.code_addresses.push_back({relocation.rva, *target, false, true});
            }
        }
    }

    void finish() {
        for (const PendingSlot& pending : pending_indexed_) {
            Sites& sites = reading_index_.contains(pending.function)
                               ? report_.tls_slots.indexed
                               : report_.tls_slots.other_index;
            add_site(sites, pending.rva, pending.swept);
        }
        for (Sites* sites : {&report_.tls_slots.indexed, &report_.tls_slots.other_index,
                             &report_.tls_slots.fixed_slot, &report_.tls_slots.not_followed,
                             &report_.cpuid, &report_.xgetbv}) {
            finish_sites(*sites);
        }
        report_.system = to_uses(system_);
        report_.traps = to_uses(traps_);
        report_.timing = to_uses(timing_);

        for (size_t i = 0; i < features_.size(); ++i) {
            FeatureUse& use = features_[i];
            if (use.count == 0) {
                continue;
            }
            use.extension = static_cast<ZydisISAExt>(i);
            use.name = ZydisISAExtGetString(use.extension);
            use.feature_class = class_of(use.extension);
            finish_sites(use.sites);
            report_.features.push_back(std::move(use));
        }
        std::ranges::sort(report_.features, [](const FeatureUse& a, const FeatureUse& b) {
            return std::tie(a.feature_class, a.name) < std::tie(b.feature_class, b.name);
        });

        for (auto& [key, access] : segments_) {
            std::tie(access.fs, access.via_self, access.computed, access.offset) = key;
            finish_sites(access.sites);
            report_.segments.push_back(std::move(access));
        }

        for (auto* references :
             {&report_.code_reads, &report_.code_writes, &report_.code_addresses}) {
            std::ranges::sort(*references, [](const CodeReference& a, const CodeReference& b) {
                return std::tie(a.site, a.target) < std::tie(b.site, b.target);
            });
            const auto [first, last] = std::ranges::unique(
                *references, [](const CodeReference& a, const CodeReference& b) {
                    return a.site == b.site && a.target == b.target;
                });
            references->erase(first, last);
        }
    }

private:
    using SegmentKey = std::tuple<bool, bool, bool, int64_t>; // fs, via_self, computed, offset

    struct PendingSlot {
        uint32_t rva;
        uint32_t function;
        bool swept;
    };

    static std::vector<Range> ranges_of(const DiscoveredFunction& function) {
        std::vector<Range> ranges{{function.start, function.end}};
        ranges.insert(ranges.end(), function.fragments.begin(), function.fragments.end());
        return ranges;
    }

    // Walks a function's ranges from `entries`, classifying each instruction no earlier walk
    // decoded. Code two functions share (a common tail, say) counts once. A walk from taken
    // addresses (`again`) stops where it meets code already walked, so nothing counts twice.
    void walk(const DiscoveredFunction& function, const std::vector<uint32_t>& entries,
              bool again) {
        const uint32_t start = function.start;
        const std::vector<Range> ranges = ranges_of(function);
        const WalkResult result = walker_.walk(
            entries, ranges,
            [&](uint32_t rva) {
                return (rva != start && discovery_.find(rva) != nullptr) ||
                       (again && rva < covered_.size() && covered_[rva]);
            },
            [&](uint32_t rva, const ZydisDecodedInstruction& insn) {
                if (rva >= covered_.size() || covered_[rva]) {
                    return;
                }
                mark(rva, insn.length);
                ++report_.coverage.instructions;
                instruction(rva, insn, start, false);
            });
        report_.coverage.unresolved_jumps += result.indirect_jumps;
        report_.coverage.decode_errors += result.decode_errors;
    }

    void mark(uint32_t rva, uint32_t length) {
        for (uint32_t i = rva; i < rva + length && i < covered_.size(); ++i) {
            covered_[i] = true;
        }
    }

    bool overlaps_covered(uint32_t rva, uint32_t length) const {
        for (uint32_t i = rva; i < rva + length && i < covered_.size(); ++i) {
            if (covered_[i]) {
                return true;
            }
        }
        return false;
    }

    bool in_table(uint32_t rva) const {
        const auto it = std::ranges::upper_bound(tables_, rva, {}, &Range::begin);
        return std::any_of(tables_.begin(), it, [rva](const Range& r) { return r.contains(rva); });
    }

    // Inside a discovered function but not at its start, and not in a jump table.
    bool is_inner_code(uint32_t rva) const {
        const DiscoveredFunction* function = discovery_.containing(rva);
        return function != nullptr && function->start != rva && !in_table(rva);
    }

    bool decode_full(uint32_t rva, ZydisDecodedInstruction& insn,
                     ZydisDecodedOperand (&operands)[ZYDIS_MAX_OPERAND_COUNT]) const {
        const pe::Section* section = image_.section_at(rva);
        if (section == nullptr || !section->executable()) {
            return false;
        }
        const uint32_t end = section->virtual_address + section->mapped_size();
        return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, image_.mapped().data() + rva,
                                                   end - rva, &insn, operands));
    }

    RegisterUse follow(uint32_t pc, ZydisRegister reg) const {
        reg = enclosing(reg);
        for (int i = 0; i < kFollowWindow; ++i) {
            ZydisDecodedInstruction insn;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_full(pc, insn, operands)) {
                return {};
            }
            for (uint8_t k = 0; k < insn.operand_count; ++k) {
                const ZydisDecodedOperand& op = operands[k];
                if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.type == ZYDIS_MEMOP_TYPE_MEM &&
                    (enclosing(op.mem.base) == reg || enclosing(op.mem.index) == reg)) {
                    return {true, pc, op};
                }
            }
            if (is_branch(insn)) {
                return {};
            }
            for (uint8_t k = 0; k < insn.operand_count; ++k) {
                const ZydisDecodedOperand& op = operands[k];
                if (op.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0 &&
                    enclosing(op.reg.value) == reg) {
                    return {};
                }
            }
            pc += insn.length;
        }
        return {};
    }

    // The register a `mov reg, [mem]` loads, or none.
    static ZydisRegister loaded_register(const ZydisDecodedInstruction& insn,
                                         const ZydisDecodedOperand* operands) {
        if (insn.mnemonic != ZYDIS_MNEMONIC_MOV || insn.operand_count_visible < 2 ||
            operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return ZYDIS_REGISTER_NONE;
        }
        return operands[0].reg.value;
    }

    void instruction(uint32_t rva, const ZydisDecodedInstruction& insn, uint32_t function,
                     bool swept) {
        if (insn.mnemonic == ZYDIS_MNEMONIC_CPUID) {
            add_site(report_.cpuid, rva, swept);
            return;
        }
        if (insn.mnemonic == ZYDIS_MNEMONIC_XGETBV) {
            add_site(report_.xgetbv, rva, swept);
            return;
        }
        if (const char* name = trap_name(insn)) {
            add_site(traps_[name], rva, swept);
            return;
        }
        if (const char* name = timing_name(insn.mnemonic)) {
            add_site(timing_[name], rva, swept);
        } else if (auto name = system_name(insn)) {
            add_site(system_[*name], rva, swept);
            return;
        }
        if (!is_cet_noop(insn.mnemonic)) {
            FeatureUse& use = features_[insn.meta.isa_ext];
            ++use.count;
            if (class_of(insn.meta.isa_ext) != FeatureClass::kBaseline) {
                add_site(use.sites, rva, swept);
            }
        }
        if ((insn.attributes & (ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS)) != 0) {
            segment_access(rva, function, swept);
        } else if (is_rip_relative(insn)) {
            rip_reference(rva, insn, function, swept);
        }
    }

    SegmentAccess& segment(bool fs, bool via_self, bool computed, int64_t offset) {
        return segments_[{fs, via_self, computed, offset}];
    }

    static void count_access(SegmentAccess& access, const ZydisDecodedOperand& op, uint32_t rva,
                             bool swept) {
        access.reads += (op.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0 ? 1 : 0;
        access.writes += (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0 ? 1 : 0;
        add_site(access.sites, rva, swept);
    }

    void segment_access(uint32_t rva, uint32_t function, bool swept) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_full(rva, insn, operands)) {
            return;
        }
        for (uint8_t k = 0; k < insn.operand_count; ++k) {
            const ZydisDecodedOperand& op = operands[k];
            if (op.type != ZYDIS_OPERAND_TYPE_MEMORY || op.mem.type != ZYDIS_MEMOP_TYPE_MEM ||
                (op.mem.segment != ZYDIS_REGISTER_FS && op.mem.segment != ZYDIS_REGISTER_GS)) {
                continue;
            }
            const bool fs = op.mem.segment == ZYDIS_REGISTER_FS;
            const bool computed =
                op.mem.base != ZYDIS_REGISTER_NONE || op.mem.index != ZYDIS_REGISTER_NONE;
            const int64_t offset = op.mem.disp.value;
            count_access(segment(fs, false, computed, offset), op, rva, swept);
            if (fs || computed) {
                continue;
            }
            const ZydisRegister loaded = loaded_register(insn, operands);
            if (offset == kTebTlsArray) {
                tls_array_load(rva + insn.length, loaded, rva, function, swept);
            } else if (offset == kTebSelf && loaded != ZYDIS_REGISTER_NONE) {
                const RegisterUse use = follow(rva + insn.length, loaded);
                if (use.found && enclosing(use.operand.mem.base) == enclosing(loaded)) {
                    const bool indexed = use.operand.mem.index != ZYDIS_REGISTER_NONE;
                    count_access(segment(false, true, indexed, use.operand.mem.disp.value),
                                 use.operand, use.rva, swept);
                }
            }
        }
    }

    void tls_array_load(uint32_t next, ZydisRegister loaded, uint32_t rva, uint32_t function,
                        bool swept) {
        TlsSlotUse& slots = report_.tls_slots;
        if (loaded == ZYDIS_REGISTER_NONE) {
            add_site(slots.not_followed, rva, swept);
            return;
        }
        const RegisterUse use = follow(next, loaded);
        const ZydisDecodedOperandMem& mem = use.operand.mem;
        const bool through_array = use.found && enclosing(mem.base) == enclosing(loaded);
        if (through_array && mem.index == ZYDIS_REGISTER_NONE) {
            add_site(slots.fixed_slot, rva, swept);
        } else if (through_array && mem.scale == 8 && mem.disp.value == 0) {
            pending_indexed_.push_back({rva, function, swept});
        } else {
            add_site(slots.not_followed, rva, swept);
        }
    }

    void rip_reference(uint32_t rva, const ZydisDecodedInstruction& insn, uint32_t function,
                       bool swept) {
        const int64_t target = int64_t{rva} + insn.length + insn.raw.disp.value;
        if (target < 0 || target >= int64_t{image_.size_of_image()}) {
            return;
        }
        const auto t = static_cast<uint32_t>(target);
        const bool lea = insn.mnemonic == ZYDIS_MNEMONIC_LEA;
        if (tls_index_ && t >= *tls_index_ && t - *tls_index_ < 4 && !lea) {
            ++report_.tls_slots.index_reads;
            reading_index_.insert(function);
            return;
        }
        // Calls and jumps through a pointer slot read the slot, which is data; prefetches and
        // hinting nops touch nothing.
        switch (insn.meta.category) {
        case ZYDIS_CATEGORY_CALL:
        case ZYDIS_CATEGORY_UNCOND_BR:
        case ZYDIS_CATEGORY_PREFETCH:
        case ZYDIS_CATEGORY_NOP:
        case ZYDIS_CATEGORY_WIDENOP: return;
        default: break;
        }
        const DiscoveredFunction* target_function = discovery_.containing(t);
        if (target_function == nullptr || in_table(t)) {
            return; // not code: data in an executable section, or a jump table
        }
        ZydisDecodedInstruction full;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_full(rva, full, operands)) {
            return;
        }
        if (lea) {
            if (t != target_function->start) {
                report_.code_addresses.push_back({rva, t, swept, false});
            }
            if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                const RegisterUse use = follow(rva + insn.length, operands[0].reg.value);
                if (use.found) {
                    code_access(use.operand, use.rva, t, swept);
                }
            }
            return;
        }
        for (uint8_t k = 0; k < full.operand_count; ++k) {
            if (operands[k].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[k].mem.base == ZYDIS_REGISTER_RIP) {
                code_access(operands[k], rva, t, swept);
            }
        }
    }

    void code_access(const ZydisDecodedOperand& op, uint32_t site, uint32_t target, bool swept) {
        if ((op.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0) {
            report_.code_reads.push_back({site, target, swept, false});
        }
        if ((op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) {
            report_.code_writes.push_back({site, target, swept, false});
        }
    }

    const Image& image_;
    const DiscoveryResult& discovery_;
    ScanReport& report_;
    CodeWalker walker_{image_};
    ZydisDecoder decoder_{};
    std::vector<bool> covered_;
    std::vector<Range> tables_; // jump tables and their index tables, by start
    std::optional<uint32_t> tls_index_;

    std::array<FeatureUse, ZYDIS_ISA_EXT_MAX_VALUE + 1> features_{};
    std::map<SegmentKey, SegmentAccess> segments_;
    std::map<std::string, Sites> system_;
    std::map<std::string, Sites> traps_;
    std::map<std::string, Sites> timing_;
    std::vector<PendingSlot> pending_indexed_;
    std::set<uint32_t> reading_index_; // functions that read the module's TLS index
};

void read_headers(const Image& image, ScanReport& report, std::vector<pe::Relocation>& relocations,
                  std::vector<uint32_t>& eh_continuations) {
    size_t& errors = report.coverage.directory_errors;
    report.dll = image.is_dll();
    report.image_base = image.image_base();
    report.size_of_image = image.size_of_image();
    report.characteristics = image.characteristics();
    report.dll_characteristics = image.dll_characteristics();

    relocations = read_directory("relocation", errors, [&] { return pe::read_relocations(image); });
    report.relocations.present = !relocations.empty();
    report.relocations.stripped = (image.characteristics() & kRelocsStripped) != 0;
    for (const pe::Relocation& relocation : relocations) {
        ++(relocation.type == pe::kRelocDir64 ? report.relocations.dir64
                                              : report.relocations.other);
    }

    if (const auto tls = read_directory("TLS", errors, [&] { return pe::read_tls(image); })) {
        TlsFacts facts;
        facts.template_size = static_cast<uint32_t>(tls->raw_data_end_va - tls->raw_data_start_va);
        facts.zero_fill = tls->zero_fill_size;
        const uint32_t align = (tls->characteristics >> kTlsAlignShift) & kTlsAlignMask;
        facts.alignment = align == 0 ? 0 : 1u << (align - 1);
        facts.index_rva = image.try_va_to_rva(tls->index_va);
        facts.callbacks = tls->callback_rvas.size();
        report.tls = facts;
    }

    if (const auto config =
            read_directory("load config", errors, [&] { return pe::read_load_config(image); })) {
        CfgFacts facts;
        facts.guard_flags = config->guard_flags;
        facts.functions = config->guard_cf_functions.size();
        facts.eh_continuations = config->guard_eh_continuations.size();
        facts.check_pointer = config->guard_cf_check_va != 0;
        facts.dispatch_pointer = config->guard_cf_dispatch_va != 0;
        facts.security_cookie = config->security_cookie_va != 0;
        report.cfg = facts;
        eh_continuations = config->guard_eh_continuations;
        sort_unique(eh_continuations);
    }

    for (const pe::ImportedModule& module :
         read_directory("import", errors, [&] { return pe::read_imports(image); })) {
        ImportFacts facts{module.dll, module.delay_load, module.symbols.size(), 0};
        facts.by_ordinal = static_cast<size_t>(std::ranges::count_if(
            module.symbols, [](const pe::ImportedSymbol& s) { return s.ordinal.has_value(); }));
        report.imports.push_back(std::move(facts));
    }
}

std::string sites_text(const Sites& sites) {
    return describe_sites(sites);
}

// The sweep decodes bytes nothing was seen to reach, which can be data that happens to decode.
const char* swept_only(const Sites& sites) {
    return sites.walked.empty() && !sites.swept.empty()
               ? "; only the sweep found these, so they may be data"
               : "";
}

} // namespace

std::string describe_sites(const Sites& sites, size_t limit) {
    if (sites.empty()) {
        return "none";
    }
    std::string text;
    size_t shown = 0;
    const auto append = [&](const std::vector<uint32_t>& list, const char* suffix) {
        for (const uint32_t rva : list) {
            if (shown == limit) {
                return;
            }
            text += fmt::format("{}{:#x}{}", shown == 0 ? "" : ", ", rva, suffix);
            ++shown;
        }
    };
    append(sites.walked, "");
    append(sites.swept, " (swept)");
    if (sites.size() > shown) {
        text += fmt::format("{}+{} more", shown == 0 ? "" : ", ", sites.size() - shown);
    }
    return text;
}

std::string describe(const SegmentAccess& access) {
    const char* reg = access.computed ? "reg+" : "";
    if (access.via_self) {
        return fmt::format("[gs:[0x30]+{}{:#x}]", reg, access.offset);
    }
    return fmt::format("{}:[{}{:#x}]", access.fs ? "fs" : "gs", reg, access.offset);
}

const char* describe(FeatureClass feature_class) {
    switch (feature_class) {
    case FeatureClass::kBaseline: return "baseline";
    case FeatureClass::kHostCheck: return "host-check";
    case FeatureClass::kNotZen2: return "not-zen2";
    }
    return "?";
}

std::string teb_field_name(int64_t offset) {
    for (const TebField& field : kTebFields) {
        if (field.offset == offset) {
            return field.name;
        }
    }
    if (offset >= kTebTlsSlots && offset < kTebTlsSlots + kTebTlsSlotCount * 8) {
        return offset % 8 == 0 ? fmt::format("TlsSlots[{}]", (offset - kTebTlsSlots) / 8)
                               : "TlsSlots";
    }
    return {};
}

bool expected_teb_offset(int64_t offset) {
    return offset == kTebStackBase || offset == kTebStackLimit || offset == kTebSelf ||
           offset == kTebTlsArray;
}

std::optional<bool> host_supports(ZydisISAExt extension) {
    const ExtensionInfo* info = find_extension(extension);
    if (info == nullptr || info->feature_class != FeatureClass::kHostCheck) {
        return std::nullopt;
    }
    const CpuidBit& bit = info->cpuid;
    std::array<int, 4> regs{};
    __cpuid(regs.data(), static_cast<int>(bit.leaf & 0x80000000u));
    const uint32_t max_leaf = std::bit_cast<std::array<uint32_t, 4>>(regs)[0];
    if (max_leaf < bit.leaf) {
        return false;
    }
    __cpuidex(regs.data(), static_cast<int>(bit.leaf), static_cast<int>(bit.subleaf));
    const uint32_t value =
        std::bit_cast<std::array<uint32_t, 4>>(regs)[static_cast<size_t>(bit.reg)];
    return ((value >> bit.bit) & 1u) != 0;
}

ScanReport scan_module(const Image& image, const DiscoveryResult& discovery,
                       const ScanOptions& options) {
    ScanReport report;
    std::vector<pe::Relocation> relocations;
    std::vector<uint32_t> eh_continuations;
    read_headers(image, report, relocations, eh_continuations);
    report.coverage.functions = discovery.functions.size();

    Scanner scanner(image, discovery, report);
    scanner.walk_functions(eh_continuations);
    scanner.scan_pointers(relocations);
    scanner.walk_taken_addresses();
    scanner.sweep(options.sweep);
    scanner.finish();

    CxxExceptionMap cxx = map_cxx_exceptions(image, discovery);
    report.cxx = {cxx.stats,           cxx.guard_table,     cxx.guard.size(),
                  cxx.recorded.size(), cxx.returned.size(), std::move(cxx.errors)};
    return report;
}

ScanReport scan_module(const Image& image, const ScanOptions& options) {
    return scan_module(image, discover_functions(image), options);
}

std::vector<std::string> review_notes(const ScanReport& report) {
    std::vector<std::string> notes;
    if (!report.relocations.present) {
        notes.push_back(fmt::format(
            "no base relocations: the module can only load at its preferred base {:#x} (size "
            "{:#x}), which must be free in the host process",
            report.image_base, report.size_of_image));
    }
    if (report.relocations.other > 0) {
        notes.push_back(fmt::format("{} relocations that aren't 64-bit: 32-bit absolute addresses "
                                    "break if the module loads above 4 GiB",
                                    report.relocations.other));
    }
    for (const SegmentAccess& access : report.segments) {
        const std::string where = fmt::format("{} ({} sites: {})", describe(access),
                                              access.sites.size(), sites_text(access.sites));
        if (access.fs) {
            notes.push_back(fmt::format("{}: fs: has no TEB in 64-bit Windows code", where));
            continue;
        }
        if (!access.computed && access.offset == kTebPeb) {
            notes.push_back(fmt::format("{}: reads the PEB, whose loader lists lead to host "
                                        "modules the import policy doesn't see (rule 5)",
                                        where));
            continue;
        }
        const std::string name = teb_field_name(access.offset);
        if (access.computed) {
            notes.push_back(fmt::format("{}: a register picks the TEB field, starting from {}; "
                                        "check what it can hold{}",
                                        where, name.empty() ? "an unnamed offset" : name,
                                        swept_only(access.sites)));
        } else if (!expected_teb_offset(access.offset)) {
            notes.push_back(fmt::format("{}: {}, a TEB field the side DLLs don't use; check that "
                                        "the console and desktop Windows agree on it{}",
                                        where, name.empty() ? "unnamed" : name,
                                        swept_only(access.sites)));
        }
    }
    const TlsSlotUse& slots = report.tls_slots;
    if (!slots.fixed_slot.empty()) {
        notes.push_back(fmt::format("{} TLS accesses use a fixed slot ({}): they assume the "
                                    "module's TLS index, which holds only for an executable whose "
                                    "host lends it index 0; a DLL gets a donor's index",
                                    slots.fixed_slot.size(), sites_text(slots.fixed_slot)));
    }
    if (!slots.other_index.empty()) {
        notes.push_back(fmt::format("{} TLS accesses index the TLS array in functions that never "
                                    "read the module's TLS index ({})",
                                    slots.other_index.size(), sites_text(slots.other_index)));
    }
    if (!slots.not_followed.empty()) {
        notes.push_back(fmt::format("{} loads of the TLS array weren't followed to a slot ({})",
                                    slots.not_followed.size(), sites_text(slots.not_followed)));
    }
    for (const InstructionUse& use : report.system) {
        notes.push_back(fmt::format("{} ({} sites: {}): a system instruction{}", use.name,
                                    use.sites.size(), sites_text(use.sites),
                                    swept_only(use.sites)));
    }
    for (const FeatureUse& use : report.features) {
        if (use.feature_class == FeatureClass::kHostCheck) {
            const auto here = host_supports(use.extension);
            notes.push_back(fmt::format("{} ({} sites: {}): Zen 2 has it, but not every supported "
                                        "host does{}{}",
                                        use.name, use.count, sites_text(use.sites),
                                        here && !*here ? ", and this one doesn't" : "",
                                        swept_only(use.sites)));
        } else if (use.feature_class == FeatureClass::kNotZen2) {
            notes.push_back(fmt::format("{} ({} sites: {}): Zen 2 lacks it, so these must sit "
                                        "behind a feature check the cpuid mask answers{}",
                                        use.name, use.count, sites_text(use.sites),
                                        swept_only(use.sites)));
        }
    }
    const auto references = [&](const std::vector<CodeReference>& list, const char* what) {
        if (list.empty()) {
            return;
        }
        Sites sites;
        for (const CodeReference& reference : list) {
            add_site(sites, reference.site, reference.swept);
        }
        notes.push_back(
            fmt::format("{} {} ({}){}", list.size(), what, sites_text(sites), swept_only(sites)));
    };
    // Addresses taken inside functions aren't a finding by themselves (catch continuations are
    // the usual case); the hook engine reads them as branch targets a patch must not cover.
    references(report.code_reads, "loads from code: self-verification would look like this");
    references(report.code_writes, "stores to code: self-modification would look like this");
    const CxxExceptionStats& cxx = report.cxx.stats;
    if (cxx.unreadable > 0) {
        const CxxTableError& first = report.cxx.errors.front();
        notes.push_back(fmt::format("{} functions have a C++ handler whose tables don't read (the "
                                    "first at {:#x}: {}); the hook engine treats them as having "
                                    "an unknown handler",
                                    cxx.unreadable, first.start, first.message));
    }
    if (cxx.unverifiable > 0) {
        notes.push_back(fmt::format("{} of {} functions with a catch have one whose continuation "
                                    "no source names; mid-function hooks in them need a hand "
                                    "check (handler_checked)",
                                    cxx.unverifiable, cxx.with_catches));
    }
    if (cxx.stray_continuations > 0) {
        notes.push_back(fmt::format("{} catch continuations from FH4's records or catch funclets "
                                    "aren't in the EH continuation table; one of the sources is "
                                    "being misread",
                                    cxx.stray_continuations));
    }
    if (report.coverage.unreached_bytes > 0) {
        notes.push_back(fmt::format("{} bytes of .pdata code the walk didn't reach; the sweep "
                                    "decoded {} instructions there",
                                    report.coverage.unreached_bytes,
                                    report.coverage.swept_instructions));
    }
    if (report.coverage.decode_errors > 0 || report.coverage.directory_errors > 0) {
        notes.push_back(fmt::format("{} decode errors and {} unreadable directories",
                                    report.coverage.decode_errors,
                                    report.coverage.directory_errors));
    }
    return notes;
}

} // namespace g6::recomp
