// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the G6Recomp contributors
//
// Operand meanings follow AMD's public "RDNA 2 Instruction Set Architecture" reference.

#include "regs.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace g6::shader::rdna2 {

namespace {

constexpr uint32_t field(uint32_t word, unsigned low, unsigned count) {
    return (word >> low) & ((1U << count) - 1);
}

constexpr size_t kTracked = kScalarRegisters + kVectorRegisters;
using RegisterSet = std::bitset<kTracked>;

// A type token in an opcode name: b32, f64, i16, u8 and the like.
bool is_type(std::string_view token) {
    if (token.size() < 2 || std::string_view("biuf").find(token[0]) == std::string_view::npos) {
        return false;
    }
    return std::all_of(token.begin() + 1, token.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Registers per operand for an opcode whose name ends in its types: one type for every operand
// (v_add_f64), or the destination's and then the sources' (v_cvt_f64_i32, s_bcnt1_i32_b64).
struct Widths {
    unsigned dst = 1;
    unsigned src = 1;
};

Widths widths(std::string_view name) {
    std::optional<std::string_view> first;
    std::optional<std::string_view> last;
    size_t start = 0;
    while (start < name.size()) {
        size_t end = name.find('_', start);
        if (end == std::string_view::npos) {
            end = name.size();
        }
        const std::string_view token = name.substr(start, end - start);
        if (is_type(token)) {
            if (!first) {
                first = token;
            }
            last = token;
        }
        start = end + 1;
    }
    if (!first) {
        return {};
    }
    const auto width = [](std::string_view type) { return type.ends_with("64") ? 2U : 1U; };
    return {width(*first), width(*last)};
}

// Dwords a load or store moves, from "dword", "dwordx2" ... "dwordx16"; one for bytes and shorts.
unsigned dword_count(std::string_view name) {
    const size_t at = name.find("dword");
    if (at == std::string_view::npos) {
        return 1;
    }
    const std::string_view rest = name.substr(at + 5);
    if (rest.size() < 2 || rest[0] != 'x') {
        return 1;
    }
    unsigned count = 0;
    for (const char c : rest.substr(1)) {
        if (c < '0' || c > '9') {
            break;
        }
        count = (count * 10) + static_cast<unsigned>(c - '0');
    }
    return count == 0 ? 1 : count;
}

bool contains(std::string_view name, std::string_view part) {
    return name.find(part) != std::string_view::npos;
}

// Data dwords an atomic sends: one, two for _x2, and twice that for a compare-swap, which sends the
// compare value too and returns half.
unsigned atomic_data_count(std::string_view name) {
    const unsigned width = name.ends_with("_x2") ? 2 : 1;
    return contains(name, "cmpswap") ? 2 * width : width;
}

// Dwords a buffer or flat memory instruction moves: format_x to format_xyzw (half as many, rounded
// up, for d16), dwords, the atomic's data, or one.
unsigned memory_data_count(std::string_view name) {
    if (contains(name, "_atomic_")) {
        return atomic_data_count(name);
    }
    const size_t at = name.find("format_");
    if (at == std::string_view::npos) {
        return dword_count(name);
    }
    std::string_view channels = name.substr(at + 7);
    const bool d16 = channels.starts_with("d16_");
    if (d16) {
        channels.remove_prefix(4);
    }
    if (channels.starts_with("hi_")) { // d16_hi_x: the high half of one register
        return 1;
    }
    const auto count = static_cast<unsigned>(channels.size());
    return d16 ? (count + 1) / 2 : count;
}

class Recorder {
public:
    explicit Recorder(Footprint& footprint) : f_(footprint) {}

    void read(RegisterFile file, uint32_t first, uint32_t count) {
        add(f_.read, f_.read_count, file, first, count);
    }
    void write(RegisterFile file, uint32_t first, uint32_t count) {
        add(f_.written, f_.written_count, file, first, count);
    }
    // Registers only: inline constants, literals, null and SCC are ignored.
    void read(const Operand& operand, uint32_t count) {
        if (const auto at = register_of(operand)) {
            read(at->first, at->second, count);
        }
    }
    void write(const Operand& operand, uint32_t count) {
        if (const auto at = register_of(operand)) {
            write(at->first, at->second, count);
        }
    }
    void read_m0() { read(RegisterFile::kScalar, kScalarM0, 1); }
    void read_vcc(uint32_t count) { read(RegisterFile::kScalar, kScalarVcc, count); }
    void write_vcc(uint32_t count) { write(RegisterFile::kScalar, kScalarVcc, count); }

    bool overflowed() const { return overflowed_; }

private:
    static std::optional<std::pair<RegisterFile, uint32_t>> register_of(const Operand& operand) {
        switch (operand.kind) {
        case OperandKind::kSgpr: return std::pair{RegisterFile::kScalar, operand.value};
        case OperandKind::kVgpr: return std::pair{RegisterFile::kVector, operand.value};
        case OperandKind::kVccLo: return std::pair{RegisterFile::kScalar, uint32_t{kScalarVcc}};
        case OperandKind::kVccHi: return std::pair{RegisterFile::kScalar, kScalarVcc + 1U};
        case OperandKind::kM0: return std::pair{RegisterFile::kScalar, uint32_t{kScalarM0}};
        case OperandKind::kExecLo: return std::pair{RegisterFile::kScalar, uint32_t{kScalarExec}};
        case OperandKind::kExecHi: return std::pair{RegisterFile::kScalar, kScalarExec + 1U};
        default: return std::nullopt;
        }
    }

    template <size_t N>
    void add(std::array<RegisterRange, N>& out, uint8_t& used, RegisterFile file, uint32_t first,
             uint32_t count) {
        const size_t limit = file == RegisterFile::kScalar ? kScalarRegisters : kVectorRegisters;
        if (count == 0 || first >= limit) {
            return;
        }
        if (used == N) {
            overflowed_ = true;
            return;
        }
        out.at(used++) = {.file = file,
                          .first = static_cast<uint16_t>(first),
                          .count = static_cast<uint16_t>(std::min<size_t>(count, limit - first))};
    }

    Footprint& f_;
    bool overflowed_ = false;
};

const Operand& source(const Instruction& insn, size_t i) {
    static const Operand kNone{.kind = OperandKind::kNull};
    return i < insn.src_count ? insn.src.at(i) : kNone;
}
const Operand& destination(const Instruction& insn, size_t i) {
    static const Operand kNone{.kind = OperandKind::kNull};
    return i < insn.dst_count ? insn.dst.at(i) : kNone;
}

bool sop2(Recorder& r, const Instruction& insn, std::string_view name) {
    const Widths w = widths(name);
    unsigned src0 = w.src;
    unsigned src1 = w.src;
    if (name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64" ||
        name == "s_bfe_u64" || name == "s_bfe_i64") {
        src1 = 1; // a shift count or a field descriptor
    } else if (name == "s_bfm_b64") {
        src0 = 1;
        src1 = 1;
    }
    r.read(source(insn, 0), src0);
    r.read(source(insn, 1), src1);
    r.write(destination(insn, 0), w.dst);
    return true;
}

bool sop1(Recorder& r, Footprint& f, const Instruction& insn, std::string_view name) {
    const Widths w = widths(name);
    const Operand& dst = destination(insn, 0);
    const Operand& src = source(insn, 0);
    if (name == "s_getpc_b64") {
        r.write(dst, 2);
    } else if (name == "s_setpc_b64" || name == "s_rfe_b64") {
        r.read(src, 2);
    } else if (name == "s_swappc_b64") {
        r.read(src, 2);
        r.write(dst, 2);
        f.call = true;
    } else if (name.starts_with("s_bitset")) { // sets or clears one bit of the destination
        r.read(src, 1);
        r.write(dst, w.dst);
    } else if (name.find("saveexec") != std::string_view::npos ||
               name.find("wrexec") != std::string_view::npos) {
        r.read(src, w.src);
        r.read(RegisterFile::kScalar, kScalarExec, w.dst);
        r.write(dst, w.dst);
        r.write(RegisterFile::kScalar, kScalarExec, w.dst);
    } else if (name.starts_with("s_movrel")) { // indexed by m0
        f.dynamic = true;
        r.read_m0();
        if (name.starts_with("s_movrels_")) {
            r.write(dst, w.dst);
        } else if (name.starts_with("s_movreld_")) {
            r.read(src, w.src);
        }
    } else {
        r.read(src, w.src);
        r.write(dst, w.dst);
    }
    return true;
}

bool sopk(Recorder& r, Footprint& f, const Instruction& insn, std::string_view name) {
    const Operand& sdst = destination(insn, 0);
    if (name == "s_movk_i32" || name == "s_cmovk_i32" || name == "s_getreg_b32") {
        r.write(sdst, 1);
    } else if (name.starts_with("s_cmpk_") || name == "s_setreg_b32" ||
               name.starts_with("s_waitcnt_")) {
        r.read(sdst, 1); // the SDST field names a source
    } else if (name == "s_addk_i32" || name == "s_mulk_i32") {
        r.read(sdst, 1);
        r.write(sdst, 1);
    } else if (name == "s_call_b64") {
        r.write(sdst, 2); // the return address
        f.call = true;
    } else if (name != "s_version" && name != "s_setreg_imm32_b32") {
        return false; // the subvector loop
    }
    return true;
}

bool sopc(Recorder& r, const Instruction& insn, std::string_view name) {
    const Widths w = widths(name);
    r.read(source(insn, 0), w.src);
    r.read(source(insn, 1), name.starts_with("s_bitcmp") ? 1 : w.src);
    return true;
}

bool sopp(Recorder& r, const Instruction& insn, std::string_view name, unsigned mask) {
    if (name == "s_cbranch_vccz" || name == "s_cbranch_vccnz") {
        r.read_vcc(mask);
    } else if (name == "s_sendmsg" || name == "s_sendmsghalt") {
        // INTERRUPT takes its data from m0, GS and GS_DONE the wave ID, GS_ALLOC_REQ the counts.
        const int64_t message = insn.imm & 0xF;
        if (message == 1 || message == 2 || message == 3 || message == 9) {
            r.read_m0();
        }
    } else if (name == "s_ttracedata") {
        r.read_m0();
    }
    return true;
}

bool smem(Recorder& r, const Instruction& insn, std::string_view name) {
    if (name.starts_with("s_load_dword") || name.starts_with("s_buffer_load_dword")) {
        r.read(source(insn, 0), name.starts_with("s_buffer") ? 4 : 2); // address or descriptor
        r.read(source(insn, 1), 1);
        r.write(destination(insn, 0), dword_count(name));
    } else if (name == "s_memtime" || name == "s_memrealtime") {
        r.write(destination(insn, 0), 2);
    } else if (name == "s_atc_probe" || name == "s_atc_probe_buffer") {
        r.read(source(insn, 0), name == "s_atc_probe" ? 2 : 4);
    } else if (name != "s_gl1_inv" && name != "s_dcache_inv") {
        return false;
    }
    return true;
}

// VOP1 opcodes, in their own encoding or promoted to VOP3.
bool vop1(Recorder& r, Footprint& f, const Instruction& insn, std::string_view name) {
    const Operand& dst = destination(insn, 0);
    const Operand& src = source(insn, 0);
    if (name == "v_nop" || name == "v_pipeflush" || name == "v_clrexcp") {
        return true;
    }
    if (name == "v_swap_b32") {
        r.read(src, 1);
        r.read(dst, 1);
        r.write(dst, 1);
        r.write(src, 1);
        return true;
    }
    if (name.starts_with("v_movrel") || name == "v_swaprel_b32") { // indexed by m0
        f.dynamic = true;
        r.read_m0();
        if (name == "v_movrels_b32") {
            r.write(dst, 1);
        } else if (name == "v_movreld_b32") {
            r.read(src, 1);
        }
        return true;
    }
    const Widths w = widths(name);
    r.read(src, w.src);
    r.write(dst, w.dst);
    return true;
}

// VOP2 opcodes, in their own encoding or promoted to VOP3. Promoted, the lane mask a VOP2
// instruction keeps in vcc is an explicit operand: src2 for the carry-in or the select mask, the
// scalar destination for the carry-out.
bool vop2(Recorder& r, const Instruction& insn, std::string_view name, unsigned mask,
          bool promoted) {
    const Operand& dst = destination(insn, 0);
    r.read(source(insn, 0), 1);
    r.read(source(insn, 1), 1);
    if (name.starts_with("v_mac_") || name.starts_with("v_fmac_") || name == "v_pk_fmac_f16" ||
        name.starts_with("v_dot")) {
        r.read(dst, 1); // accumulates into the destination
    }
    const bool carry = name.ends_with("_co_ci_u32");
    if (name == "v_cndmask_b32" || carry) {
        if (promoted) {
            r.read(source(insn, 2), mask);
        } else {
            r.read_vcc(mask);
        }
    }
    r.write(dst, 1);
    if (carry && !promoted) {
        r.write_vcc(mask);
    } else if (promoted && insn.dst_count == 2) {
        r.write(destination(insn, 1), mask);
    }
    return true;
}

// Compares: the result is a lane mask in vcc, an SGPR or exec.
bool vopc(Recorder& r, const Instruction& insn, std::string_view name, unsigned mask) {
    const Widths w = widths(name);
    r.read(source(insn, 0), w.src);
    r.read(source(insn, 1), name.find("_class_") != std::string_view::npos ? 1 : w.src);
    r.write(destination(insn, 0), mask);
    return true;
}

bool three_sources(std::string_view name) {
    static constexpr auto kPrefixes = std::to_array<std::string_view>({
        "v_mad_",       "v_fma_",       "v_cube",      "v_bfe_",     "v_bfi_",
        "v_lerp_",      "v_alignb",     "v_mullit_",   "v_min3_",    "v_max3_",
        "v_med3_",      "v_sad_",       "v_msad_",     "v_xor3_",    "v_perm_",
        "v_cvt_pk_u8_", "v_div_fixup_", "v_xad_",      "v_add3_",    "v_or3_",
        "v_and_or_",    "v_lshl_add_",  "v_add_lshl_", "v_lshl_or_", "v_div_scale_",
    });
    return std::ranges::any_of(kPrefixes,
                               [&](std::string_view prefix) { return name.starts_with(prefix); });
}

// Opcodes that exist only in VOP3.
bool vop3_only(Recorder& r, const Instruction& insn, std::string_view name, unsigned mask) {
    const Operand& dst = destination(insn, 0);
    const Operand& src0 = source(insn, 0);
    const Operand& src1 = source(insn, 1);
    const Operand& src2 = source(insn, 2);
    if (name.starts_with("v_interp_")) {
        // The attribute sits where src0 would; the I or J register is in the src1 field, and the
        // p1lv and f16 p2 forms take a second VGPR in src2. The attribute data comes through m0.
        r.read_m0();
        if (name != "v_interp_mov_f32") {
            r.read(src1, 1);
        }
        if (name == "v_interp_p1lv_f16" || name == "v_interp_p2_f16") {
            r.read(src2, 1);
        }
        if (name == "v_interp_p2_f32") {
            r.read(dst, 1);
        }
        r.write(dst, 1);
        return true;
    }
    if (name == "v_readlane_b32") {
        r.read(src0, 1);
        r.read(src1, 1);
        r.write(dst, 1);
        return true;
    }
    if (name == "v_writelane_b32" || name.starts_with("v_permlane")) {
        // They write one lane, or the lanes a permute fills: a partial write, counted as a write.
        r.read(src0, 1);
        r.read(src1, 1);
        if (name.starts_with("v_permlane")) {
            r.read(src2, 1);
        }
        r.write(dst, 1);
        return true;
    }
    if (name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_ashrrev_i64") {
        r.read(src0, 1);
        r.read(src1, 2);
        r.write(dst, 2);
        return true;
    }
    if (name == "v_ldexp_f64" || name == "v_trig_preop_f64") {
        r.read(src0, 2);
        r.read(src1, 1);
        r.write(dst, 2);
        return true;
    }
    if (name == "v_qsad_pk_u16_u8" || name == "v_mqsad_pk_u16_u8" || name == "v_mqsad_u32_u8") {
        // Eight bytes of reference, four of source, and two or four accumulators.
        const unsigned accumulators = name == "v_mqsad_u32_u8" ? 4 : 2;
        r.read(src0, 2);
        r.read(src1, 1);
        r.read(src2, accumulators);
        r.write(dst, accumulators);
        return true;
    }
    if (name == "v_mad_u64_u32" || name == "v_mad_i64_i32") {
        r.read(src0, 1);
        r.read(src1, 1);
        r.read(src2, 2);
        r.write(dst, 2);
        r.write(destination(insn, 1), mask);
        return true;
    }
    const Widths w = widths(name);
    if (name.starts_with("v_div_fmas_")) {
        r.read_vcc(mask);
    }
    const bool three = three_sources(name) || name.starts_with("v_div_fmas_");
    r.read(src0, w.src);
    r.read(src1, w.src);
    if (three) {
        r.read(src2, w.src);
    }
    r.write(dst, w.dst);
    if (insn.dst_count == 2) { // VOP3B: a carry-out or v_div_scale's flag
        r.write(destination(insn, 1), mask);
    }
    return true;
}

bool vop3(Recorder& r, Footprint& f, const Instruction& insn, std::string_view name,
          unsigned mask) {
    if (insn.opcode < 0x100) {
        return vopc(r, insn, name, mask);
    }
    if (insn.opcode < 0x140) {
        return vop2(r, insn, name, mask, true);
    }
    if (insn.opcode >= 0x180 && insn.opcode < 0x200) {
        return vop1(r, f, insn, name);
    }
    return vop3_only(r, insn, name, mask);
}

bool vop3p(Recorder& r, const Instruction& insn, std::string_view name) {
    r.read(source(insn, 0), 1);
    r.read(source(insn, 1), 1);
    if (name.starts_with("v_pk_mad_") || name.starts_with("v_pk_fma_") ||
        name.starts_with("v_fma_mix") || name.starts_with("v_dot")) {
        r.read(source(insn, 2), 1);
    }
    r.write(destination(insn, 0), 1); // mixlo and mixhi write half: a partial write
    return true;
}

bool vinterp(Recorder& r, const Instruction& insn) {
    r.read_m0();
    if (insn.opcode == 0 || insn.opcode == 1) { // p1 and p2 read I or J; mov reads neither
        r.read(source(insn, 0), 1);
    }
    if (insn.opcode == 1) {
        r.read(destination(insn, 0), 1); // p2 adds to p1's result
    }
    r.write(destination(insn, 0), 1);
    return insn.opcode <= 2;
}

// Registers per LDS data operand: by the name's type, and three or four for b96 and b128.
unsigned ds_width(std::string_view name) {
    if (contains(name, "b128")) {
        return 4;
    }
    if (contains(name, "b96")) {
        return 3;
    }
    return widths(name).src;
}

bool ds(Recorder& r, const Instruction& insn, std::string_view name) {
    const Operand& vdst = destination(insn, 0);
    const Operand& addr = source(insn, 0);
    const Operand& data0 = source(insn, 1);
    const Operand& data1 = source(insn, 2);
    if ((insn.mask & 1) != 0) {
        r.read_m0(); // GDS takes its base and size from m0
    }
    if (name.starts_with("ds_gws_")) {
        r.read_m0();
        if (name == "ds_gws_init" || name == "ds_gws_sema_br" || name == "ds_gws_barrier") {
            r.read(data0, 1);
        }
    } else if (name == "ds_append" || name == "ds_consume") {
        r.read_m0();
        r.write(vdst, 1);
    } else if (name == "ds_ordered_count") {
        r.read_m0();
        r.read(addr, 1);
        r.write(vdst, 1);
    } else if (name == "ds_nop") {
        return true;
    } else if (contains(name, "_addtid_")) { // the address is m0 plus the lane's index
        r.read_m0();
        if (name.starts_with("ds_read")) {
            r.write(vdst, 1);
        } else {
            r.read(data0, 1);
        }
    } else if (contains(name, "_src2_")) { // the second operand is memory too
        r.read(addr, 1);
    } else if (name.starts_with("ds_read")) {
        const unsigned per = ds_width(name);
        r.read(addr, 1);
        r.write(vdst, name.starts_with("ds_read2") ? 2 * per : per);
    } else if (name == "ds_swizzle_b32") {
        r.read(addr, 1);
        r.write(vdst, 1);
    } else {
        // Writes, atomics and permutes: an address, then one data operand or two (write2,
        // wrxchg2, a compare-swap's compare and source, mskor's mask and value).
        const unsigned per = ds_width(name);
        const bool two = name.starts_with("ds_write2") || name.starts_with("ds_wrxchg2") ||
                         contains(name, "cmpst") || contains(name, "mskor");
        r.read(addr, 1);
        r.read(data0, per);
        if (two) {
            r.read(data1, per);
        }
        if (contains(name, "_rtn_") || contains(name, "permute")) {
            r.write(vdst, name.starts_with("ds_wrxchg2") ? 2 * per : per);
        }
    }
    return true;
}

// MUBUF and MTBUF. The decoder lists the data VGPRs as the destination, for stores too.
bool buffer(Recorder& r, const Instruction& insn, std::string_view name) {
    if (name == "buffer_gl0_inv" || name == "buffer_gl1_inv") {
        return true;
    }
    const unsigned offen = insn.mask & 1;
    const unsigned idxen = (insn.mask >> 1) & 1;
    const bool glc = ((insn.mask >> 2) & 1) != 0;
    const unsigned tfe = field(insn.words[1], 23, 1);
    const bool lds = insn.encoding == Encoding::kMubuf && field(insn.words[0], 16, 1) != 0;
    const Operand& vdata = destination(insn, 0);
    r.read(source(insn, 0), offen + idxen); // index, then offset
    r.read(source(insn, 1), 4);             // the buffer descriptor
    r.read(source(insn, 2), 1);             // soffset
    const unsigned count = memory_data_count(name);
    if (contains(name, "_load_")) {
        if (lds) {
            r.read_m0(); // loads straight into LDS at m0
        } else {
            r.write(vdata, count + tfe);
        }
    } else if (contains(name, "_store_")) {
        r.read(vdata, count);
    } else if (contains(name, "_atomic_")) {
        r.read(vdata, count);
        if (glc) {
            r.write(vdata, contains(name, "cmpswap") ? count / 2 : count);
        }
    } else {
        return false;
    }
    return true;
}

bool image(Recorder& r, const Instruction& insn, std::string_view name) {
    const std::optional<unsigned> addresses = image_address_count(insn);
    if (!addresses) {
        return false;
    }
    const auto nsa = static_cast<unsigned>(insn.imm2);
    if (nsa == 0) {
        if (source(insn, 0).kind == OperandKind::kVgpr) {
            r.read(RegisterFile::kVector, source(insn, 0).value, *addresses);
        }
    } else {
        // vaddr and then four registers per extra dword; the last dword may be padded.
        if (*addresses > 1 + (4 * nsa) || *addresses <= 1 + (4 * (nsa - 1))) {
            return false;
        }
        for (unsigned i = 0; i < *addresses; ++i) {
            r.read(source(insn, i), 1);
        }
    }
    const bool r128 = field(insn.words[0], 15, 1) != 0;
    const bool glc = field(insn.words[0], 13, 1) != 0;
    const unsigned tfe = field(insn.words[0], 16, 1) | field(insn.words[0], 17, 1);
    const bool d16 = field(insn.words[1], 31, 1) != 0;
    const bool bvh = name.starts_with("image_bvh");
    const bool sampled = name.starts_with("image_sample") || name.starts_with("image_gather4") ||
                         name == "image_get_lod";
    r.read(source(insn, 1 + (4 * nsa)), bvh || r128 ? 4 : 8); // the resource descriptor
    if (sampled) {
        r.read(source(insn, 2 + (4 * nsa)), 4); // the sampler
    }
    // A dmask of zero returns one channel.
    unsigned channels = name.starts_with("image_gather4")
                            ? 4
                            : std::max(1U, static_cast<unsigned>(std::popcount(insn.mask)));
    if (d16) {
        channels = (channels + 1) / 2;
    }
    const Operand& vdata = destination(insn, 0);
    if (bvh) {
        r.write(vdata, 4);
    } else if (name.starts_with("image_store")) {
        r.read(vdata, channels);
    } else if (name.starts_with("image_atomic")) {
        r.read(vdata, channels);
        if (glc) {
            r.write(vdata, name == "image_atomic_cmpswap" ? channels / 2 : channels);
        }
    } else {
        r.write(vdata, channels + tfe);
    }
    return true;
}

bool flat(Recorder& r, const Instruction& insn, std::string_view name) {
    const Operand& vaddr = source(insn, 0);
    const Operand& saddr = source(insn, 2);
    const bool has_saddr = saddr.kind != OperandKind::kNull;
    switch (insn.mask) {
    case 0: r.read(vaddr, 2); break; // FLAT: a 64-bit address
    case 1:                          // SCRATCH: an offset in an SGPR or a VGPR
        if (has_saddr) {
            r.read(saddr, 1);
        } else {
            r.read(vaddr, 1);
        }
        break;
    default: // GLOBAL: a 64-bit address in VGPRs, or an SGPR base and a 32-bit VGPR offset
        if (has_saddr) {
            r.read(saddr, 2);
            r.read(vaddr, 1);
        } else {
            r.read(vaddr, 2);
        }
        break;
    }
    const Operand& vdst = destination(insn, 0);
    const unsigned count = memory_data_count(name);
    if (contains(name, "_load_")) {
        r.write(vdst, count);
    } else if (contains(name, "_store_")) {
        r.read(source(insn, 1), count);
    } else if (contains(name, "_atomic_")) {
        r.read(source(insn, 1), count);
        if (field(insn.words[0], 16, 1) != 0) { // glc: returns the old value
            r.write(vdst, contains(name, "cmpswap") ? count / 2 : count);
        }
    } else {
        return false;
    }
    return true;
}

bool exports(Recorder& r, const Instruction& insn) {
    const bool compressed = (insn.imm2 & 1) != 0;
    for (unsigned i = 0; i < 4; ++i) {
        // Compressed, enable bits 0-1 cover vsrc0 and bits 2-3 vsrc1.
        const bool used =
            compressed ? i < 2 && ((insn.mask >> (2 * i)) & 3) != 0 : ((insn.mask >> i) & 1) != 0;
        if (used) {
            r.read(source(insn, i), 1);
        }
    }
    return true;
}

bool model(Recorder& r, Footprint& f, const Instruction& insn, unsigned mask) {
    if (insn.truncated) {
        return false;
    }
    if (insn.encoding == Encoding::kExport) {
        return exports(r, insn);
    }
    if (insn.encoding == Encoding::kVinterp) {
        return vinterp(r, insn);
    }
    if (insn.encoding == Encoding::kMimg) {
        return image(r, insn, mnemonic(insn));
    }
    const std::optional<std::string_view> name = opcode_name(insn.encoding, insn.opcode);
    if (!name) {
        return false;
    }
    switch (insn.encoding) {
    case Encoding::kSop2: return sop2(r, insn, *name);
    case Encoding::kSop1: return sop1(r, f, insn, *name);
    case Encoding::kSopk: return sopk(r, f, insn, *name);
    case Encoding::kSopc: return sopc(r, insn, *name);
    case Encoding::kSopp: return sopp(r, insn, *name, mask);
    case Encoding::kSmem: return smem(r, insn, *name);
    case Encoding::kVop1: return vop1(r, f, insn, *name);
    case Encoding::kVop2: return vop2(r, insn, *name, mask, false);
    case Encoding::kVopc: return vopc(r, insn, *name, mask);
    case Encoding::kVop3: return vop3(r, f, insn, *name, mask);
    case Encoding::kVop3p: return vop3p(r, insn, *name);
    case Encoding::kDs: return ds(r, insn, *name);
    case Encoding::kMubuf:
    case Encoding::kMtbuf: return buffer(r, insn, *name);
    case Encoding::kFlat: return flat(r, insn, *name);
    default: return false;
    }
}

size_t bit_of(RegisterFile file, size_t number) {
    return file == RegisterFile::kScalar ? number : kScalarRegisters + number;
}

RegisterSet to_set(std::span<const RegisterRange> ranges) {
    RegisterSet set;
    for (const RegisterRange& range : ranges) {
        for (size_t i = 0; i < range.count; ++i) {
            set.set(bit_of(range.file, range.first + i));
        }
    }
    return set;
}

} // namespace

Footprint footprint(const Instruction& insn, unsigned wave_size) {
    Footprint f;
    Recorder r(f);
    const bool modelled = model(r, f, insn, wave_size == 32 ? 1 : 2);
    if (!modelled || r.overflowed()) {
        return {};
    }
    f.modelled = true;
    return f;
}

std::optional<unsigned> image_address_count(const Instruction& insn) {
    if (insn.encoding != Encoding::kMimg || insn.truncated) {
        return std::nullopt;
    }
    const bool a16 = field(insn.words[1], 30, 1) != 0;
    const auto dim = static_cast<size_t>(insn.imm & 7);
    // By dimension: 1D, 2D, 3D, cube, 1D array, 2D array, 2D MSAA, 2D MSAA array. The array
    // slice, cube face and sample index count as coordinates.
    static constexpr std::array<unsigned, 8> kCoordinates = {1, 2, 3, 3, 2, 3, 3, 4};
    static constexpr std::array<unsigned, 8> kGradients = {1, 2, 3, 2, 1, 2, 0, 0}; // per direction
    const auto packed = [a16](unsigned count) { return a16 ? (count + 1) / 2 : count; };
    if (insn.opcode >= 0x20 && insn.opcode < 0x60) { // sample and gather4
        const unsigned modifier = insn.opcode & 7;   // none, cl, d, d_cl, l, b, b_cl, lz
        const unsigned offset = (insn.opcode >> 4) & 1;
        const unsigned compare = (insn.opcode >> 3) & 1;
        const unsigned bias = modifier == 5 || modifier == 6 ? 1 : 0;
        const unsigned gradients =
            modifier == 2 || modifier == 3 ? 2 * packed(kGradients.at(dim)) : 0;
        const unsigned lod_or_clamp =
            modifier == 1 || modifier == 3 || modifier == 4 || modifier == 6 ? 1 : 0;
        // Offset, bias and compare stay 32-bit under A16; the rest packs two to a VGPR.
        return offset + bias + compare + gradients + packed(kCoordinates.at(dim) + lod_or_clamp);
    }
    const std::optional<std::string_view> name = opcode_name(insn.encoding, insn.opcode);
    if (!name) {
        return std::nullopt;
    }
    if (*name == "image_get_resinfo") {
        return 1; // the mip level
    }
    if (*name == "image_get_lod") {
        return packed(kCoordinates.at(dim));
    }
    if (*name == "image_bvh_intersect_ray") {
        return a16 ? 8 : 11; // node, extent, origin, direction, inverse direction
    }
    if (*name == "image_bvh64_intersect_ray") {
        return a16 ? 9 : 12;
    }
    if (name->starts_with("image_load") || name->starts_with("image_store") ||
        name->starts_with("image_atomic")) {
        return packed(kCoordinates.at(dim) + (name->ends_with("_mip") ? 1 : 0));
    }
    return std::nullopt;
}

std::optional<unsigned> wave_size(const Program& program) {
    bool wide = false;
    bool narrow = false;
    for (const Instruction& insn : program.instructions) {
        const bool scalar_alu = insn.encoding == Encoding::kSop1 ||
                                insn.encoding == Encoding::kSop2 ||
                                insn.encoding == Encoding::kSopc;
        const std::optional<std::string_view> name =
            scalar_alu ? opcode_name(insn.encoding, insn.opcode) : std::nullopt;
        // s_and_saveexec_b64 and the like read and write exec without naming it.
        if (name && (contains(*name, "saveexec") || contains(*name, "wrexec"))) {
            wide = wide || name->ends_with("_b64");
            narrow = narrow || name->ends_with("_b32");
        }
        const auto check = [&](const Operand& operand) {
            if (operand.kind == OperandKind::kExecHi || operand.kind == OperandKind::kVccHi) {
                wide = true;
            } else if (name && (operand.kind == OperandKind::kExecLo ||
                                operand.kind == OperandKind::kVccLo)) {
                if (name->ends_with("_b64") || name->ends_with("_u64")) {
                    wide = true;
                } else if (operand.kind == OperandKind::kExecLo && name->ends_with("_b32")) {
                    narrow = true;
                }
            }
        };
        std::ranges::for_each(insn.destinations(), check);
        std::ranges::for_each(insn.sources(), check);
    }
    if (wide == narrow) {
        return std::nullopt;
    }
    return wide ? 64U : 32U;
}

LiveIns live_ins(const Program& program, unsigned wave_size) {
    LiveIns result;
    const std::vector<Instruction>& insns = program.instructions;
    const size_t n = insns.size();
    if (n == 0) {
        return result;
    }
    constexpr size_t kNone = SIZE_MAX;
    std::unordered_map<size_t, size_t> index;
    index.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        index.emplace(insns[i].offset, i);
    }
    const auto at = [&](int64_t offset) {
        const auto it = offset < 0 ? index.end() : index.find(static_cast<size_t>(offset));
        return it == index.end() ? kNone : it->second;
    };
    std::vector<RegisterSet> reads(n);
    std::vector<RegisterSet> writes(n);
    std::vector<bool> calls(n);
    std::vector<std::array<size_t, 2>> next(n, {kNone, kNone});
    for (size_t i = 0; i < n; ++i) {
        const Footprint f = footprint(insns[i], wave_size);
        result.unmodelled += f.modelled ? 0 : 1;
        result.dynamic = result.dynamic || f.dynamic;
        result.call = result.call || f.call;
        calls[i] = f.call;
        reads[i] = to_set(f.reads());
        writes[i] = to_set(f.writes());
        const Flow flow = flow_of(insns[i]);
        if (flow.falls_through) {
            next[i][0] = at(static_cast<int64_t>(insns[i].offset + (4 * size_t{insns[i].dwords})));
        }
        if (flow.target) {
            next[i][1] = at(*flow.target);
        }
    }
    RegisterSet start;
    start.set(kScalarExec).set(kScalarExec + 1);

    // A register can be written before an instruction when some path from the start writes it.
    // Inputs are the registers an instruction reads that no path writes first.
    const auto inputs = [&](bool callee_writes_all) {
        std::vector<RegisterSet> before(n);
        std::vector<bool> reached(n);
        std::vector<size_t> work = {0};
        before[0] = start;
        reached[0] = true;
        while (!work.empty()) {
            const size_t i = work.back();
            work.pop_back();
            RegisterSet after = before[i] | writes[i];
            if (calls[i] && callee_writes_all) {
                after.set();
            }
            for (const size_t s : next[i]) {
                if (s == kNone) {
                    continue;
                }
                const RegisterSet merged = before[s] | after;
                if (!reached[s] || merged != before[s]) {
                    before[s] = merged;
                    reached[s] = true;
                    work.push_back(s);
                }
            }
        }
        RegisterSet read_first;
        for (size_t i = 0; i < n; ++i) {
            if (reached[i]) {
                read_first |= reads[i] & ~before[i];
            }
        }
        return read_first;
    };
    const RegisterSet live = inputs(true);
    const RegisterSet unexplained = result.call ? inputs(false) : live;
    const RegisterSet returned = unexplained & ~live;
    for (size_t i = 0; i < kScalarRegisters; ++i) {
        result.scalar[i] = live[i];
        result.scalar_after_call[i] = returned[i];
    }
    for (size_t i = 0; i < kVectorRegisters; ++i) {
        result.vector[i] = live[kScalarRegisters + i];
        result.vector_after_call[i] = returned[kScalarRegisters + i];
    }
    return result;
}

} // namespace g6::shader::rdna2
