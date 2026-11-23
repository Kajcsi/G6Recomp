// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the TomBox developers (MIT; see NOTICES.md)
// Copyright (C) 2026 the xbox-emulator contributors
// Copyright (C) 2026 the G6Recomp contributors
//
// Opcode names for the RDNA 2 decoder, ported to C++ from tombox-xshader's name tables in
// xbox-emulator (https://github.com/omonkeyz/xbox-emulator, commit 51f5319,
// crates/tombox-xshader/src/rdna2/mnem.rs), licensed GPL-3.0-or-later. Most of that file comes
// from TomBox (https://github.com/drycheetah84/TomBox, commit 7e8064f), whose tombox-xshader
// crate is MIT OR Apache-2.0; that part is used here under MIT. The numbers follow AMD's
// public RDNA 2 reference, LLVM's AMDGPU target, and Mesa's ACO for GFX10. Added here: the
// VOP3P integer ops 0x00 to 0x0D; v_div_fixup_f32, v_div_scale_f32 and v_div_fmas_f32; the VOP3
// interpolation ops; the SOP1 exec, quadmask and movrel ops not listed before; s_call_b64 and
// the subvector loop; the image atomics; the d16 buffer formats; and the d16 LDS ops 0xA0 to 0xA7.
// Opcodes not listed have no name; none is guessed.

#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rdna2.h"

namespace g6::shader::rdna2 {

namespace {

using Entry = std::pair<uint16_t, std::string_view>;

// Names indexed by opcode.
class Table {
public:
    Table(std::initializer_list<Entry> entries) { add({entries.begin(), entries.size()}); }
    explicit Table(std::span<const Entry> entries) { add(entries); }

    std::optional<std::string_view> find(uint32_t opcode) const {
        if (opcode >= names_.size() || names_[opcode].empty()) {
            return std::nullopt;
        }
        return names_[opcode];
    }

private:
    void add(std::span<const Entry> entries) {
        for (const auto& [opcode, name] : entries) {
            if (opcode >= names_.size()) {
                names_.resize(opcode + 1);
            }
            names_[opcode] = name;
        }
    }

    std::vector<std::string_view> names_;
};

const Table& vop2_table() {
    static const Table table{
        {0x01, "v_cndmask_b32"},       {0x03, "v_add_f32"},
        {0x04, "v_sub_f32"},           {0x05, "v_subrev_f32"},
        {0x06, "v_mac_legacy_f32"},    {0x07, "v_mul_legacy_f32"},
        {0x08, "v_mul_f32"},           {0x09, "v_mul_i32_i24"},
        {0x0A, "v_mul_hi_i32_i24"},    {0x0B, "v_mul_u32_u24"},
        {0x0C, "v_mul_hi_u32_u24"},    {0x0F, "v_min_f32"},
        {0x10, "v_max_f32"},           {0x11, "v_min_i32"},
        {0x12, "v_max_i32"},           {0x13, "v_min_u32"},
        {0x14, "v_max_u32"},           {0x16, "v_lshrrev_b32"},
        {0x18, "v_ashrrev_i32"},       {0x1A, "v_lshlrev_b32"},
        {0x1B, "v_and_b32"},           {0x1C, "v_or_b32"},
        {0x1D, "v_xor_b32"},           {0x1E, "v_xnor_b32"},
        {0x1F, "v_mac_f32"},           {0x20, "v_madmk_f32"},
        {0x21, "v_madak_f32"},         {0x25, "v_add_nc_u32"},
        {0x26, "v_sub_nc_u32"},        {0x27, "v_subrev_nc_u32"},
        {0x28, "v_add_co_ci_u32"},     {0x29, "v_sub_co_ci_u32"},
        {0x2A, "v_subrev_co_ci_u32"},  {0x2B, "v_fmac_f32"},
        {0x2C, "v_fmamk_f32"},         {0x2D, "v_fmaak_f32"},
        {0x2F, "v_cvt_pkrtz_f16_f32"}, {0x32, "v_add_f16"},
        {0x33, "v_sub_f16"},           {0x34, "v_subrev_f16"},
        {0x35, "v_mul_f16"},           {0x36, "v_fmac_f16"},
        {0x37, "v_fmamk_f16"},         {0x38, "v_fmaak_f16"},
        {0x39, "v_max_f16"},           {0x3A, "v_min_f16"},
        {0x3B, "v_ldexp_f16"},         {0x3C, "v_pk_fmac_f16"},
    };
    return table;
}

const Table& vop1_table() {
    static const Table table{
        {0x00, "v_nop"},
        {0x01, "v_mov_b32"},
        {0x02, "v_readfirstlane_b32"},
        {0x03, "v_cvt_i32_f64"},
        {0x04, "v_cvt_f64_i32"},
        {0x05, "v_cvt_f32_i32"},
        {0x06, "v_cvt_f32_u32"},
        {0x07, "v_cvt_u32_f32"},
        {0x08, "v_cvt_i32_f32"},
        {0x0A, "v_cvt_f16_f32"},
        {0x0B, "v_cvt_f32_f16"},
        {0x0C, "v_cvt_rpi_i32_f32"},
        {0x0D, "v_cvt_flr_i32_f32"},
        {0x0E, "v_cvt_off_f32_i4"},
        {0x0F, "v_cvt_f32_f64"},
        {0x10, "v_cvt_f64_f32"},
        {0x11, "v_cvt_f32_ubyte0"},
        {0x12, "v_cvt_f32_ubyte1"},
        {0x13, "v_cvt_f32_ubyte2"},
        {0x14, "v_cvt_f32_ubyte3"},
        {0x15, "v_cvt_u32_f64"},
        {0x16, "v_cvt_f64_u32"},
        {0x17, "v_trunc_f64"},
        {0x18, "v_ceil_f64"},
        {0x19, "v_rndne_f64"},
        {0x1A, "v_floor_f64"},
        {0x1B, "v_pipeflush"},
        {0x20, "v_fract_f32"},
        {0x21, "v_trunc_f32"},
        {0x22, "v_ceil_f32"},
        {0x23, "v_rndne_f32"},
        {0x24, "v_floor_f32"},
        {0x25, "v_exp_f32"},
        {0x27, "v_log_f32"},
        {0x2A, "v_rcp_f32"},
        {0x2B, "v_rcp_iflag_f32"},
        {0x2E, "v_rsq_f32"},
        {0x2F, "v_rcp_f64"},
        {0x31, "v_rsq_f64"},
        {0x33, "v_sqrt_f32"},
        {0x34, "v_sqrt_f64"},
        {0x35, "v_sin_f32"},
        {0x36, "v_cos_f32"},
        {0x37, "v_not_b32"},
        {0x38, "v_bfrev_b32"},
        {0x39, "v_ffbh_u32"},
        {0x3A, "v_ffbl_b32"},
        {0x3B, "v_ffbh_i32"},
        {0x3C, "v_frexp_exp_i32_f64"},
        {0x3D, "v_frexp_mant_f64"},
        {0x3E, "v_fract_f64"},
        {0x3F, "v_frexp_exp_i32_f32"},
        {0x40, "v_frexp_mant_f32"},
        {0x41, "v_clrexcp"},
        {0x42, "v_movreld_b32"},
        {0x43, "v_movrels_b32"},
        {0x44, "v_movrelsd_b32"},
        {0x50, "v_cvt_f16_u16"},
        {0x51, "v_cvt_f16_i16"},
        {0x52, "v_cvt_u16_f16"},
        {0x53, "v_cvt_i16_f16"},
        {0x54, "v_rcp_f16"},
        {0x55, "v_sqrt_f16"},
        {0x56, "v_rsq_f16"},
        {0x57, "v_log_f16"},
        {0x58, "v_exp_f16"},
        {0x59, "v_frexp_mant_f16"},
        {0x5A, "v_frexp_exp_i16_f16"},
        {0x5B, "v_floor_f16"},
        {0x5C, "v_ceil_f16"},
        {0x5D, "v_trunc_f16"},
        {0x5E, "v_rndne_f16"},
        {0x5F, "v_fract_f16"},
        {0x60, "v_sin_f16"},
        {0x61, "v_cos_f16"},
        {0x62, "v_sat_pk_u8_i16"},
        {0x63, "v_cvt_norm_i16_f16"},
        {0x64, "v_cvt_norm_u16_f16"},
        {0x65, "v_swap_b32"},
    };
    return table;
}

// VOPC names follow a pattern: families of compares, each with a v_cmpx twin that writes EXEC.
// The f16 family is split in two: f to o at 0xC8, u to tru at 0xE8.
constexpr std::array<std::string_view, 16> kFloatCompares = {
    "f", "lt",  "eq",  "le",  "gt",  "lg",  "ge",  "o",
    "u", "nge", "nlg", "ngt", "nle", "neq", "nlt", "tru"};
constexpr std::array<std::string_view, 8> kIntCompares = {"f",  "lt", "eq", "le",
                                                          "gt", "ne", "ge", "t"};
// The 16-bit integer compares have no f and t forms.
constexpr std::array<std::string_view, 6> kInt16Compares = {"lt", "eq", "le", "gt", "ne", "ge"};

struct Family {
    uint16_t first;
    std::span<const std::string_view> compares;
    std::string_view prefix;
    std::string_view type;
};

std::string compare_name(uint32_t opcode) {
    static const std::array<Family, 20> kFamilies = {{
        {0x00, kFloatCompares, "v_cmp", "f32"},
        {0x10, kFloatCompares, "v_cmpx", "f32"},
        {0x20, kFloatCompares, "v_cmp", "f64"},
        {0x30, kFloatCompares, "v_cmpx", "f64"},
        {0x80, kIntCompares, "v_cmp", "i32"},
        {0x89, kInt16Compares, "v_cmp", "i16"},
        {0x90, kIntCompares, "v_cmpx", "i32"},
        {0x99, kInt16Compares, "v_cmpx", "i16"},
        {0xA0, kIntCompares, "v_cmp", "i64"},
        {0xA9, kInt16Compares, "v_cmp", "u16"},
        {0xB0, kIntCompares, "v_cmpx", "i64"},
        {0xB9, kInt16Compares, "v_cmpx", "u16"},
        {0xC0, kIntCompares, "v_cmp", "u32"},
        {0xC8, std::span(kFloatCompares).first(8), "v_cmp", "f16"},
        {0xD0, kIntCompares, "v_cmpx", "u32"},
        {0xD8, std::span(kFloatCompares).first(8), "v_cmpx", "f16"},
        {0xE0, kIntCompares, "v_cmp", "u64"},
        {0xE8, std::span(kFloatCompares).subspan(8), "v_cmp", "f16"},
        {0xF0, kIntCompares, "v_cmpx", "u64"},
        {0xF8, std::span(kFloatCompares).subspan(8), "v_cmpx", "f16"},
    }};
    for (const Family& family : kFamilies) {
        if (opcode >= family.first && opcode < family.first + family.compares.size()) {
            return std::string(family.prefix) + "_" +
                   std::string(family.compares[opcode - family.first]) + "_" +
                   std::string(family.type);
        }
    }
    switch (opcode) {
    case 0x88: return "v_cmp_class_f32";
    case 0x8F: return "v_cmp_class_f16";
    case 0x98: return "v_cmpx_class_f32";
    case 0x9F: return "v_cmpx_class_f16";
    case 0xA8: return "v_cmp_class_f64";
    case 0xB8: return "v_cmpx_class_f64";
    default: return {};
    }
}

const Table& vopc_table() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out(256);
        for (uint32_t opcode = 0; opcode < out.size(); ++opcode) {
            out[opcode] = compare_name(opcode);
        }
        return out;
    }();
    static const Table table = [] {
        std::vector<Entry> entries;
        for (uint32_t opcode = 0; opcode < names.size(); ++opcode) {
            if (!names[opcode].empty()) {
                entries.emplace_back(static_cast<uint16_t>(opcode), names[opcode]);
            }
        }
        return Table(std::span<const Entry>(entries));
    }();
    return table;
}

// VOP3-only opcodes. 0x000-0x0FF are VOPC, 0x100-0x13F VOP2, and 0x180-0x1FF VOP1, promoted.
const Table& vop3_table() {
    static const Table table{
        {0x140, "v_mad_legacy_f32"},
        {0x141, "v_mad_f32"},
        {0x142, "v_mad_i32_i24"},
        {0x143, "v_mad_u32_u24"},
        {0x144, "v_cubeid_f32"},
        {0x145, "v_cubesc_f32"},
        {0x146, "v_cubetc_f32"},
        {0x147, "v_cubema_f32"},
        {0x148, "v_bfe_u32"},
        {0x149, "v_bfe_i32"},
        {0x14A, "v_bfi_b32"},
        {0x14B, "v_fma_f32"},
        {0x14C, "v_fma_f64"},
        {0x14D, "v_lerp_u8"},
        {0x14E, "v_alignbit_b32"},
        {0x14F, "v_alignbyte_b32"},
        {0x150, "v_mullit_f32"},
        {0x151, "v_min3_f32"},
        {0x152, "v_min3_i32"},
        {0x153, "v_min3_u32"},
        {0x154, "v_max3_f32"},
        {0x155, "v_max3_i32"},
        {0x156, "v_max3_u32"},
        {0x157, "v_med3_f32"},
        {0x158, "v_med3_i32"},
        {0x159, "v_med3_u32"},
        {0x15A, "v_sad_u8"},
        {0x15B, "v_sad_hi_u8"},
        {0x15C, "v_sad_u16"},
        {0x15D, "v_sad_u32"},
        {0x15E, "v_cvt_pk_u8_f32"},
        {0x15F, "v_div_fixup_f32"},
        {0x160, "v_div_fixup_f64"},
        {0x164, "v_add_f64"},
        {0x165, "v_mul_f64"},
        {0x166, "v_min_f64"},
        {0x167, "v_max_f64"},
        {0x168, "v_ldexp_f64"},
        {0x169, "v_mul_lo_u32"},
        {0x16A, "v_mul_hi_u32"},
        {0x16B, "v_mul_lo_i32"},
        {0x16C, "v_mul_hi_i32"},
        {0x16D, "v_div_scale_f32"},
        {0x16E, "v_div_scale_f64"},
        {0x16F, "v_div_fmas_f32"},
        {0x170, "v_div_fmas_f64"},
        {0x171, "v_msad_u8"},
        {0x174, "v_trig_preop_f64"},
        {0x176, "v_mad_u64_u32"},
        {0x177, "v_mad_i64_i32"},
        {0x178, "v_xor3_b32"},
        {0x200, "v_interp_p1_f32"},
        {0x201, "v_interp_p2_f32"},
        {0x202, "v_interp_mov_f32"},
        {0x2FF, "v_lshlrev_b64"},
        {0x300, "v_lshrrev_b64"},
        {0x301, "v_ashrrev_i64"},
        {0x303, "v_add_nc_u16"},
        {0x304, "v_sub_nc_u16"},
        {0x305, "v_mul_lo_u16"},
        {0x307, "v_lshrrev_b16"},
        {0x308, "v_ashrrev_i16"},
        {0x309, "v_max_u16"},
        {0x30A, "v_max_i16"},
        {0x30B, "v_min_u16"},
        {0x30C, "v_min_i16"},
        {0x30D, "v_add_nc_i16"},
        {0x30E, "v_sub_nc_i16"},
        {0x30F, "v_add_co_u32"},
        {0x310, "v_sub_co_u32"},
        {0x311, "v_pack_b32_f16"},
        {0x312, "v_cvt_pknorm_i16_f16"},
        {0x313, "v_cvt_pknorm_u16_f16"},
        {0x314, "v_lshlrev_b16"},
        {0x319, "v_subrev_co_u32"},
        {0x340, "v_mad_u16"},
        {0x342, "v_interp_p1ll_f16"},
        {0x343, "v_interp_p1lv_f16"},
        {0x344, "v_perm_b32"},
        {0x345, "v_xad_u32"},
        {0x346, "v_lshl_add_u32"},
        {0x347, "v_add_lshl_u32"},
        {0x34B, "v_fma_f16"},
        {0x351, "v_min3_f16"},
        {0x352, "v_min3_i16"},
        {0x353, "v_min3_u16"},
        {0x354, "v_max3_f16"},
        {0x355, "v_max3_i16"},
        {0x356, "v_max3_u16"},
        {0x357, "v_med3_f16"},
        {0x358, "v_med3_i16"},
        {0x359, "v_med3_u16"},
        {0x35A, "v_interp_p2_f16"},
        {0x35E, "v_mad_i16"},
        {0x360, "v_readlane_b32"},
        {0x361, "v_writelane_b32"},
        {0x362, "v_ldexp_f32"},
        {0x363, "v_bfm_b32"},
        {0x364, "v_bcnt_u32_b32"},
        {0x365, "v_mbcnt_lo_u32_b32"},
        {0x366, "v_mbcnt_hi_u32_b32"},
        {0x368, "v_cvt_pknorm_i16_f32"},
        {0x369, "v_cvt_pknorm_u16_f32"},
        {0x36A, "v_cvt_pk_u16_u32"},
        {0x36B, "v_cvt_pk_i16_i32"},
        {0x36D, "v_add3_u32"},
        {0x36F, "v_lshl_or_b32"},
        {0x371, "v_and_or_b32"},
        {0x372, "v_or3_b32"},
        {0x373, "v_mad_u32_u16"},
        {0x375, "v_mad_i32_i16"},
        {0x376, "v_sub_nc_i32"},
        {0x377, "v_permlane16_b32"},
        {0x378, "v_permlanex16_b32"},
        {0x37F, "v_add_nc_i32"},
    };
    return table;
}

const Table& vop3p_table() {
    static const Table table{
        {0x00, "v_pk_mad_i16"},     {0x01, "v_pk_mul_lo_u16"},  {0x02, "v_pk_add_i16"},
        {0x03, "v_pk_sub_i16"},     {0x04, "v_pk_lshlrev_b16"}, {0x05, "v_pk_lshrrev_b16"},
        {0x06, "v_pk_ashrrev_i16"}, {0x07, "v_pk_max_i16"},     {0x08, "v_pk_min_i16"},
        {0x09, "v_pk_mad_u16"},     {0x0A, "v_pk_add_u16"},     {0x0B, "v_pk_sub_u16"},
        {0x0C, "v_pk_max_u16"},     {0x0D, "v_pk_min_u16"},     {0x0E, "v_pk_fma_f16"},
        {0x0F, "v_pk_add_f16"},     {0x10, "v_pk_mul_f16"},     {0x11, "v_pk_min_f16"},
        {0x12, "v_pk_max_f16"},     {0x20, "v_fma_mix_f32"},    {0x21, "v_fma_mixlo_f16"},
        {0x22, "v_fma_mixhi_f16"},
    };
    return table;
}

const Table& sop2_table() {
    static const Table table{
        {0x00, "s_add_u32"},         {0x01, "s_sub_u32"},         {0x02, "s_add_i32"},
        {0x03, "s_sub_i32"},         {0x04, "s_addc_u32"},        {0x05, "s_subb_u32"},
        {0x06, "s_min_i32"},         {0x07, "s_min_u32"},         {0x08, "s_max_i32"},
        {0x09, "s_max_u32"},         {0x0A, "s_cselect_b32"},     {0x0B, "s_cselect_b64"},
        {0x0E, "s_and_b32"},         {0x0F, "s_and_b64"},         {0x10, "s_or_b32"},
        {0x11, "s_or_b64"},          {0x12, "s_xor_b32"},         {0x13, "s_xor_b64"},
        {0x14, "s_andn2_b32"},       {0x15, "s_andn2_b64"},       {0x16, "s_orn2_b32"},
        {0x17, "s_orn2_b64"},        {0x18, "s_nand_b32"},        {0x19, "s_nand_b64"},
        {0x1A, "s_nor_b32"},         {0x1B, "s_nor_b64"},         {0x1C, "s_xnor_b32"},
        {0x1D, "s_xnor_b64"},        {0x1E, "s_lshl_b32"},        {0x1F, "s_lshl_b64"},
        {0x20, "s_lshr_b32"},        {0x21, "s_lshr_b64"},        {0x22, "s_ashr_i32"},
        {0x23, "s_ashr_i64"},        {0x24, "s_bfm_b32"},         {0x25, "s_bfm_b64"},
        {0x26, "s_mul_i32"},         {0x27, "s_bfe_u32"},         {0x28, "s_bfe_i32"},
        {0x29, "s_bfe_u64"},         {0x2A, "s_bfe_i64"},         {0x2C, "s_absdiff_i32"},
        {0x2E, "s_lshl1_add_u32"},   {0x2F, "s_lshl2_add_u32"},   {0x30, "s_lshl3_add_u32"},
        {0x31, "s_lshl4_add_u32"},   {0x32, "s_pack_ll_b32_b16"}, {0x33, "s_pack_lh_b32_b16"},
        {0x34, "s_pack_hh_b32_b16"}, {0x35, "s_mul_hi_u32"},      {0x36, "s_mul_hi_i32"},
    };
    return table;
}

const Table& sop1_table() {
    static const Table table{
        {0x03, "s_mov_b32"},
        {0x04, "s_mov_b64"},
        {0x05, "s_cmov_b32"},
        {0x06, "s_cmov_b64"},
        {0x07, "s_not_b32"},
        {0x08, "s_not_b64"},
        {0x09, "s_wqm_b32"},
        {0x0A, "s_wqm_b64"},
        {0x0B, "s_brev_b32"},
        {0x0C, "s_brev_b64"},
        {0x0D, "s_bcnt0_i32_b32"},
        {0x0E, "s_bcnt0_i32_b64"},
        {0x0F, "s_bcnt1_i32_b32"},
        {0x10, "s_bcnt1_i32_b64"},
        {0x11, "s_ff0_i32_b32"},
        {0x12, "s_ff0_i32_b64"},
        {0x13, "s_ff1_i32_b32"},
        {0x14, "s_ff1_i32_b64"},
        {0x15, "s_flbit_i32_b32"},
        {0x16, "s_flbit_i32_b64"},
        {0x17, "s_flbit_i32"},
        {0x18, "s_flbit_i32_i64"},
        {0x19, "s_sext_i32_i8"},
        {0x1A, "s_sext_i32_i16"},
        {0x1B, "s_bitset0_b32"},
        {0x1C, "s_bitset0_b64"},
        {0x1D, "s_bitset1_b32"},
        {0x1E, "s_bitset1_b64"},
        {0x1F, "s_getpc_b64"},
        {0x20, "s_setpc_b64"},
        {0x21, "s_swappc_b64"},
        {0x22, "s_rfe_b64"},
        {0x24, "s_and_saveexec_b64"},
        {0x25, "s_or_saveexec_b64"},
        {0x26, "s_xor_saveexec_b64"},
        {0x27, "s_andn2_saveexec_b64"},
        {0x28, "s_orn2_saveexec_b64"},
        {0x29, "s_nand_saveexec_b64"},
        {0x2A, "s_nor_saveexec_b64"},
        {0x2B, "s_xnor_saveexec_b64"},
        {0x2C, "s_quadmask_b32"},
        {0x2D, "s_quadmask_b64"},
        {0x2E, "s_movrels_b32"},
        {0x2F, "s_movrels_b64"},
        {0x30, "s_movreld_b32"},
        {0x31, "s_movreld_b64"},
        {0x34, "s_abs_i32"},
        {0x37, "s_andn1_saveexec_b64"},
        {0x38, "s_orn1_saveexec_b64"},
        {0x39, "s_andn1_wrexec_b64"},
        {0x3A, "s_andn2_wrexec_b64"},
        {0x3B, "s_bitreplicate_b64_b32"},
        {0x3C, "s_and_saveexec_b32"},
        {0x3D, "s_or_saveexec_b32"},
        {0x3E, "s_xor_saveexec_b32"},
        {0x3F, "s_andn2_saveexec_b32"},
        {0x40, "s_orn2_saveexec_b32"},
        {0x41, "s_nand_saveexec_b32"},
        {0x42, "s_nor_saveexec_b32"},
        {0x43, "s_xnor_saveexec_b32"},
        {0x44, "s_andn1_saveexec_b32"},
        {0x45, "s_orn1_saveexec_b32"},
        {0x46, "s_andn1_wrexec_b32"},
        {0x47, "s_andn2_wrexec_b32"},
        {0x49, "s_movrelsd_2_b32"},
    };
    return table;
}

const Table& sopk_table() {
    static const Table table{
        {0x00, "s_movk_i32"},           {0x01, "s_version"},
        {0x02, "s_cmovk_i32"},          {0x03, "s_cmpk_eq_i32"},
        {0x04, "s_cmpk_lg_i32"},        {0x05, "s_cmpk_gt_i32"},
        {0x06, "s_cmpk_ge_i32"},        {0x07, "s_cmpk_lt_i32"},
        {0x08, "s_cmpk_le_i32"},        {0x09, "s_cmpk_eq_u32"},
        {0x0A, "s_cmpk_lg_u32"},        {0x0B, "s_cmpk_gt_u32"},
        {0x0C, "s_cmpk_ge_u32"},        {0x0D, "s_cmpk_lt_u32"},
        {0x0E, "s_cmpk_le_u32"},        {0x0F, "s_addk_i32"},
        {0x10, "s_mulk_i32"},           {0x12, "s_getreg_b32"},
        {0x13, "s_setreg_b32"},         {0x15, "s_setreg_imm32_b32"},
        {0x16, "s_call_b64"},           {0x17, "s_waitcnt_vscnt"},
        {0x18, "s_waitcnt_vmcnt"},      {0x19, "s_waitcnt_expcnt"},
        {0x1A, "s_waitcnt_lgkmcnt"},    {0x1B, "s_subvector_loop_begin"},
        {0x1C, "s_subvector_loop_end"},
    };
    return table;
}

const Table& sopc_table() {
    static const Table table{
        {0x00, "s_cmp_eq_i32"},  {0x01, "s_cmp_lg_i32"},  {0x02, "s_cmp_gt_i32"},
        {0x03, "s_cmp_ge_i32"},  {0x04, "s_cmp_lt_i32"},  {0x05, "s_cmp_le_i32"},
        {0x06, "s_cmp_eq_u32"},  {0x07, "s_cmp_lg_u32"},  {0x08, "s_cmp_gt_u32"},
        {0x09, "s_cmp_ge_u32"},  {0x0A, "s_cmp_lt_u32"},  {0x0B, "s_cmp_le_u32"},
        {0x0C, "s_bitcmp0_b32"}, {0x0D, "s_bitcmp1_b32"}, {0x0E, "s_bitcmp0_b64"},
        {0x0F, "s_bitcmp1_b64"}, {0x12, "s_cmp_eq_u64"},  {0x13, "s_cmp_lg_u64"},
    };
    return table;
}

const Table& sopp_table() {
    static const Table table{
        {0x00, "s_nop"},
        {0x01, "s_endpgm"},
        {0x02, "s_branch"},
        {0x03, "s_wakeup"},
        {0x04, "s_cbranch_scc0"},
        {0x05, "s_cbranch_scc1"},
        {0x06, "s_cbranch_vccz"},
        {0x07, "s_cbranch_vccnz"},
        {0x08, "s_cbranch_execz"},
        {0x09, "s_cbranch_execnz"},
        {0x0A, "s_barrier"},
        {0x0B, "s_setkill"},
        {0x0C, "s_waitcnt"},
        {0x0D, "s_sethalt"},
        {0x0E, "s_sleep"},
        {0x0F, "s_setprio"},
        {0x10, "s_sendmsg"},
        {0x11, "s_sendmsghalt"},
        {0x12, "s_trap"},
        {0x13, "s_icache_inv"},
        {0x14, "s_incperflevel"},
        {0x15, "s_decperflevel"},
        {0x16, "s_ttracedata"},
        {0x17, "s_cbranch_cdbgsys"},
        {0x18, "s_cbranch_cdbguser"},
        {0x19, "s_cbranch_cdbgsys_or_user"},
        {0x1A, "s_cbranch_cdbgsys_and_user"},
        {0x1B, "s_endpgm_saved"},
        {0x1E, "s_endpgm_ordered_ps_done"},
        {0x1F, "s_code_end"},
        {0x20, "s_inst_prefetch"},
        {0x21, "s_clause"},
        {0x22, "s_wait_idle"},
        {0x23, "s_waitcnt_depctr"},
        {0x24, "s_round_mode"},
        {0x25, "s_denorm_mode"},
        {0x28, "s_ttracedata_imm"},
    };
    return table;
}

const Table& smem_table() {
    static const Table table{
        {0x00, "s_load_dword"},
        {0x01, "s_load_dwordx2"},
        {0x02, "s_load_dwordx4"},
        {0x03, "s_load_dwordx8"},
        {0x04, "s_load_dwordx16"},
        {0x08, "s_buffer_load_dword"},
        {0x09, "s_buffer_load_dwordx2"},
        {0x0A, "s_buffer_load_dwordx4"},
        {0x0B, "s_buffer_load_dwordx8"},
        {0x0C, "s_buffer_load_dwordx16"},
        {0x1F, "s_gl1_inv"},
        {0x20, "s_dcache_inv"},
        {0x24, "s_memtime"},
        {0x25, "s_memrealtime"},
        {0x26, "s_atc_probe"},
        {0x27, "s_atc_probe_buffer"},
    };
    return table;
}

// Sample and gather4 (0x20-0x5F) are named by pattern in mnemonic().
const Table& mimg_table() {
    static const Table table{
        {0x00, "image_load"},
        {0x01, "image_load_mip"},
        {0x08, "image_store"},
        {0x09, "image_store_mip"},
        {0x0E, "image_get_resinfo"},
        {0x0F, "image_atomic_swap"},
        {0x10, "image_atomic_cmpswap"},
        {0x11, "image_atomic_add"},
        {0x12, "image_atomic_sub"},
        {0x14, "image_atomic_smin"},
        {0x15, "image_atomic_umin"},
        {0x16, "image_atomic_smax"},
        {0x17, "image_atomic_umax"},
        {0x18, "image_atomic_and"},
        {0x19, "image_atomic_or"},
        {0x1A, "image_atomic_xor"},
        {0x1B, "image_atomic_inc"},
        {0x1C, "image_atomic_dec"},
        {0x60, "image_get_lod"},
        {0xE6, "image_bvh_intersect_ray"},
        {0xE7, "image_bvh64_intersect_ray"},
    };
    return table;
}

const Table& mubuf_table() {
    static const Table table{
        {0x00, "buffer_load_format_x"},
        {0x01, "buffer_load_format_xy"},
        {0x02, "buffer_load_format_xyz"},
        {0x03, "buffer_load_format_xyzw"},
        {0x04, "buffer_store_format_x"},
        {0x05, "buffer_store_format_xy"},
        {0x06, "buffer_store_format_xyz"},
        {0x07, "buffer_store_format_xyzw"},
        {0x08, "buffer_load_ubyte"},
        {0x09, "buffer_load_sbyte"},
        {0x0A, "buffer_load_ushort"},
        {0x0B, "buffer_load_sshort"},
        {0x0C, "buffer_load_dword"},
        {0x0D, "buffer_load_dwordx2"},
        {0x0E, "buffer_load_dwordx4"},
        {0x0F, "buffer_load_dwordx3"},
        {0x18, "buffer_store_byte"},
        {0x1A, "buffer_store_short"},
        {0x1C, "buffer_store_dword"},
        {0x1D, "buffer_store_dwordx2"},
        {0x1E, "buffer_store_dwordx4"},
        {0x1F, "buffer_store_dwordx3"},
        {0x30, "buffer_atomic_swap"},
        {0x31, "buffer_atomic_cmpswap"},
        {0x32, "buffer_atomic_add"},
        {0x33, "buffer_atomic_sub"},
        {0x35, "buffer_atomic_smin"},
        {0x36, "buffer_atomic_umin"},
        {0x37, "buffer_atomic_smax"},
        {0x38, "buffer_atomic_umax"},
        {0x39, "buffer_atomic_and"},
        {0x3A, "buffer_atomic_or"},
        {0x3B, "buffer_atomic_xor"},
        {0x3C, "buffer_atomic_inc"},
        {0x3D, "buffer_atomic_dec"},
        {0x71, "buffer_gl0_inv"},
        {0x72, "buffer_gl1_inv"},
        {0x80, "buffer_load_format_d16_x"},
        {0x81, "buffer_load_format_d16_xy"},
        {0x82, "buffer_load_format_d16_xyz"},
        {0x83, "buffer_load_format_d16_xyzw"},
        {0x84, "buffer_store_format_d16_x"},
        {0x85, "buffer_store_format_d16_xy"},
        {0x86, "buffer_store_format_d16_xyz"},
        {0x87, "buffer_store_format_d16_xyzw"},
    };
    return table;
}

// Bit 3 of the opcode comes from the second dword (OPM).
const Table& mtbuf_table() {
    static const Table table{
        {0x0, "tbuffer_load_format_x"},        {0x1, "tbuffer_load_format_xy"},
        {0x2, "tbuffer_load_format_xyz"},      {0x3, "tbuffer_load_format_xyzw"},
        {0x4, "tbuffer_store_format_x"},       {0x5, "tbuffer_store_format_xy"},
        {0x6, "tbuffer_store_format_xyz"},     {0x7, "tbuffer_store_format_xyzw"},
        {0x8, "tbuffer_load_format_d16_x"},    {0x9, "tbuffer_load_format_d16_xy"},
        {0xA, "tbuffer_load_format_d16_xyz"},  {0xB, "tbuffer_load_format_d16_xyzw"},
        {0xC, "tbuffer_store_format_d16_x"},   {0xD, "tbuffer_store_format_d16_xy"},
        {0xE, "tbuffer_store_format_d16_xyz"}, {0xF, "tbuffer_store_format_d16_xyzw"},
    };
    return table;
}

// Named for the FLAT segment; mnemonic() renames them for GLOBAL and SCRATCH.
const Table& flat_table() {
    static const Table table{
        {0x08, "flat_load_ubyte"},    {0x09, "flat_load_sbyte"},
        {0x0A, "flat_load_ushort"},   {0x0B, "flat_load_sshort"},
        {0x0C, "flat_load_dword"},    {0x0D, "flat_load_dwordx2"},
        {0x0E, "flat_load_dwordx4"},  {0x0F, "flat_load_dwordx3"},
        {0x18, "flat_store_byte"},    {0x19, "flat_store_byte_d16_hi"},
        {0x1A, "flat_store_short"},   {0x1B, "flat_store_short_d16_hi"},
        {0x1C, "flat_store_dword"},   {0x1D, "flat_store_dwordx2"},
        {0x1E, "flat_store_dwordx4"}, {0x1F, "flat_store_dwordx3"},
    };
    return table;
}

const Table& ds_table() {
    static const Table table{
        {0x00, "ds_add_u32"},
        {0x01, "ds_sub_u32"},
        {0x02, "ds_rsub_u32"},
        {0x03, "ds_inc_u32"},
        {0x04, "ds_dec_u32"},
        {0x05, "ds_min_i32"},
        {0x06, "ds_max_i32"},
        {0x07, "ds_min_u32"},
        {0x08, "ds_max_u32"},
        {0x09, "ds_and_b32"},
        {0x0A, "ds_or_b32"},
        {0x0B, "ds_xor_b32"},
        {0x0D, "ds_write_b32"},
        {0x0E, "ds_write2_b32"},
        {0x0F, "ds_write2st64_b32"},
        {0x10, "ds_cmpst_b32"},
        {0x18, "ds_gws_sema_release_all"},
        {0x19, "ds_gws_init"},
        {0x1A, "ds_gws_sema_v"},
        {0x1B, "ds_gws_sema_br"},
        {0x1C, "ds_gws_sema_p"},
        {0x1D, "ds_gws_barrier"},
        {0x1E, "ds_write_b8"},
        {0x1F, "ds_write_b16"},
        {0x20, "ds_add_rtn_u32"},
        {0x21, "ds_sub_rtn_u32"},
        {0x22, "ds_rsub_rtn_u32"},
        {0x23, "ds_inc_rtn_u32"},
        {0x24, "ds_dec_rtn_u32"},
        {0x25, "ds_min_rtn_i32"},
        {0x26, "ds_max_rtn_i32"},
        {0x27, "ds_min_rtn_u32"},
        {0x28, "ds_max_rtn_u32"},
        {0x29, "ds_and_rtn_b32"},
        {0x2A, "ds_or_rtn_b32"},
        {0x2B, "ds_xor_rtn_b32"},
        {0x2D, "ds_wrxchg_rtn_b32"},
        {0x30, "ds_cmpst_rtn_b32"},
        {0x35, "ds_swizzle_b32"},
        {0x36, "ds_read_b32"},
        {0x37, "ds_read2_b32"},
        {0x38, "ds_read2st64_b32"},
        {0x39, "ds_read_i8"},
        {0x3A, "ds_read_u8"},
        {0x3B, "ds_read_i16"},
        {0x3C, "ds_read_u16"},
        {0x3D, "ds_consume"},
        {0x3E, "ds_append"},
        {0x3F, "ds_ordered_count"},
        {0x4D, "ds_write_b64"},
        {0x4E, "ds_write2_b64"},
        {0x4F, "ds_write2st64_b64"},
        {0x76, "ds_read_b64"},
        {0x77, "ds_read2_b64"},
        {0x78, "ds_read2st64_b64"},
        {0xA0, "ds_write_b8_d16_hi"},
        {0xA1, "ds_write_b16_d16_hi"},
        {0xA2, "ds_read_u8_d16"},
        {0xA3, "ds_read_u8_d16_hi"},
        {0xA4, "ds_read_i8_d16"},
        {0xA5, "ds_read_i8_d16_hi"},
        {0xA6, "ds_read_u16_d16"},
        {0xA7, "ds_read_u16_d16_hi"},
        {0xB2, "ds_permute_b32"},
        {0xB3, "ds_bpermute_b32"},
    };
    return table;
}

const Table& vinterp_table() {
    static const Table table{
        {0, "v_interp_p1_f32"},
        {1, "v_interp_p2_f32"},
        {2, "v_interp_mov_f32"},
    };
    return table;
}

} // namespace

std::optional<std::string_view> opcode_name(Encoding encoding, uint16_t opcode) {
    switch (encoding) {
    case Encoding::kSop2: return sop2_table().find(opcode);
    case Encoding::kSopk: return sopk_table().find(opcode);
    case Encoding::kSop1: return sop1_table().find(opcode);
    case Encoding::kSopc: return sopc_table().find(opcode);
    case Encoding::kSopp: return sopp_table().find(opcode);
    case Encoding::kSmem: return smem_table().find(opcode);
    case Encoding::kVop2: return vop2_table().find(opcode);
    case Encoding::kVop1: return vop1_table().find(opcode);
    case Encoding::kVopc: return vopc_table().find(opcode);
    case Encoding::kVop3:
        if (opcode < 0x100) {
            return vopc_table().find(opcode);
        }
        if (opcode < 0x140) {
            // The forms with an inline constant K (madmk, madak, fmamk, fmaak) and v_pk_fmac_f16
            // exist only as VOP2.
            switch (opcode - 0x100) {
            case 0x20:
            case 0x21:
            case 0x2C:
            case 0x2D:
            case 0x37:
            case 0x38:
            case 0x3C: return std::nullopt;
            default: return vop2_table().find(opcode - 0x100);
            }
        }
        if (opcode >= 0x180 && opcode < 0x200) {
            if (opcode == 0x180 + 0x65) { // v_swap_b32 exists only as VOP1
                return std::nullopt;
            }
            return vop1_table().find(opcode - 0x180);
        }
        return vop3_table().find(opcode);
    case Encoding::kVop3p: return vop3p_table().find(opcode);
    case Encoding::kVinterp: return vinterp_table().find(opcode);
    case Encoding::kDs: return ds_table().find(opcode);
    case Encoding::kMubuf: return mubuf_table().find(opcode);
    case Encoding::kMtbuf: return mtbuf_table().find(opcode);
    case Encoding::kMimg: return mimg_table().find(opcode);
    case Encoding::kFlat: return flat_table().find(opcode);
    case Encoding::kExport: return "exp";
    case Encoding::kUnknown: return std::nullopt;
    }
    return std::nullopt;
}

} // namespace g6::shader::rdna2
