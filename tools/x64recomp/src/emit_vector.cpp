// SSE, AVX, AVX2, and FMA instructions. They execute as the same host instructions through
// intrinsics, so results match bit for bit. Registers are __m256i locals; an xmm operand is the
// low half. Legacy SSE writes keep bits 255:128 of the destination, VEX.128 writes clear them.

#include <cstring>
#include <unordered_map>

#include <fmt/format.h>

#include "function_emitter.h"

namespace g6::recomp {
namespace {

// How an instruction's operands map onto the intrinsic. `n` is the visible operand count.
enum class Shape : uint8_t {
    kMove,        // dst = src, any width
    kUnary,       // dst = fn(ops[n-1])
    kUnaryImm,    // dst = fn(ops[n-2], imm)
    kBinary,      // dst = fn(ops[n-2], ops[n-1]); legacy: ops[n-2] is the destination
    kBinaryImm,   // dst = fn(ops[n-3], ops[n-2], imm)
    kBlendv,      // dst = fn(a, b, mask); legacy: mask is xmm0
    kScalarUnary, // dst = move_ss(ops[n-2], fn(ops[n-1])): the intrinsic takes one operand
    kShift,       // dst = fn(ops[n-2], count): count is an immediate or an xmm/m128 register
};

enum Flags : uint8_t {
    kNone = 0,
    kSrc128 = 1 << 0,   // the source is 128 bits even when the vector length is 256
    kHalfDst = 1 << 1,  // the result is 128 bits even when the vector length is 256
    kOnly256 = 1 << 2,  // VEX.256 only
    kVexOnly = 1 << 3,  // no legacy SSE form (the name is looked up without its 'v')
    kImmMask7 = 1 << 4, // the legacy form uses only imm8[2:0] (CMPPS and friends)
};

struct VectorOp {
    Shape shape;
    char in;  // operand type: 'i' integer, 'f' float (ps/ss), 'd' double (pd/sd)
    char out; // result type
    const char* fn;
    uint8_t flags;
};

using Table = std::unordered_map<std::string_view, VectorOp>;

const Table& table() {
    using enum Shape;
    static const Table ops = {
        // Moves.
        {"movaps", {kMove, 'i', 'i', "", kNone}},
        {"movups", {kMove, 'i', 'i', "", kNone}},
        {"movapd", {kMove, 'i', 'i', "", kNone}},
        {"movupd", {kMove, 'i', 'i', "", kNone}},
        {"movdqa", {kMove, 'i', 'i', "", kNone}},
        {"movdqu", {kMove, 'i', 'i', "", kNone}},
        {"lddqu", {kMove, 'i', 'i', "", kNone}},
        {"movntdqa", {kMove, 'i', 'i', "", kNone}},
        {"movntps", {kMove, 'i', 'i', "", kNone}},
        {"movntpd", {kMove, 'i', 'i', "", kNone}},
        {"movntdq", {kMove, 'i', 'i', "", kNone}},

        // Packed floating point.
        {"addps", {kBinary, 'f', 'f', "add_ps", kNone}},
        {"addpd", {kBinary, 'd', 'd', "add_pd", kNone}},
        {"subps", {kBinary, 'f', 'f', "sub_ps", kNone}},
        {"subpd", {kBinary, 'd', 'd', "sub_pd", kNone}},
        {"mulps", {kBinary, 'f', 'f', "mul_ps", kNone}},
        {"mulpd", {kBinary, 'd', 'd', "mul_pd", kNone}},
        {"divps", {kBinary, 'f', 'f', "div_ps", kNone}},
        {"divpd", {kBinary, 'd', 'd', "div_pd", kNone}},
        {"minps", {kBinary, 'f', 'f', "min_ps", kNone}},
        {"minpd", {kBinary, 'd', 'd', "min_pd", kNone}},
        {"maxps", {kBinary, 'f', 'f', "max_ps", kNone}},
        {"maxpd", {kBinary, 'd', 'd', "max_pd", kNone}},
        {"andps", {kBinary, 'f', 'f', "and_ps", kNone}},
        {"andpd", {kBinary, 'd', 'd', "and_pd", kNone}},
        {"andnps", {kBinary, 'f', 'f', "andnot_ps", kNone}},
        {"andnpd", {kBinary, 'd', 'd', "andnot_pd", kNone}},
        {"orps", {kBinary, 'f', 'f', "or_ps", kNone}},
        {"orpd", {kBinary, 'd', 'd', "or_pd", kNone}},
        {"xorps", {kBinary, 'f', 'f', "xor_ps", kNone}},
        {"xorpd", {kBinary, 'd', 'd', "xor_pd", kNone}},
        {"unpcklps", {kBinary, 'f', 'f', "unpacklo_ps", kNone}},
        {"unpckhps", {kBinary, 'f', 'f', "unpackhi_ps", kNone}},
        {"unpcklpd", {kBinary, 'd', 'd', "unpacklo_pd", kNone}},
        {"unpckhpd", {kBinary, 'd', 'd', "unpackhi_pd", kNone}},
        {"haddps", {kBinary, 'f', 'f', "hadd_ps", kNone}},
        {"haddpd", {kBinary, 'd', 'd', "hadd_pd", kNone}},
        {"hsubps", {kBinary, 'f', 'f', "hsub_ps", kNone}},
        {"hsubpd", {kBinary, 'd', 'd', "hsub_pd", kNone}},
        {"addsubps", {kBinary, 'f', 'f', "addsub_ps", kNone}},
        {"addsubpd", {kBinary, 'd', 'd', "addsub_pd", kNone}},
        {"movhlps", {kBinary, 'f', 'f', "movehl_ps", kNone}},
        {"movlhps", {kBinary, 'f', 'f', "movelh_ps", kNone}},
        {"sqrtps", {kUnary, 'f', 'f', "sqrt_ps", kNone}},
        {"sqrtpd", {kUnary, 'd', 'd', "sqrt_pd", kNone}},
        {"rcpps", {kUnary, 'f', 'f', "rcp_ps", kNone}},
        {"rsqrtps", {kUnary, 'f', 'f', "rsqrt_ps", kNone}},
        {"movddup", {kUnary, 'd', 'd', "movedup_pd", kNone}},
        {"movsldup", {kUnary, 'f', 'f', "moveldup_ps", kNone}},
        {"movshdup", {kUnary, 'f', 'f', "movehdup_ps", kNone}},
        {"shufps", {kBinaryImm, 'f', 'f', "shuffle_ps", kNone}},
        {"shufpd", {kBinaryImm, 'd', 'd', "shuffle_pd", kNone}},
        {"blendps", {kBinaryImm, 'f', 'f', "blend_ps", kNone}},
        {"blendpd", {kBinaryImm, 'd', 'd', "blend_pd", kNone}},
        {"blendvps", {kBlendv, 'f', 'f', "blendv_ps", kNone}},
        {"blendvpd", {kBlendv, 'd', 'd', "blendv_pd", kNone}},
        {"cmpps", {kBinaryImm, 'f', 'f', "cmp_ps", kImmMask7}},
        {"cmppd", {kBinaryImm, 'd', 'd', "cmp_pd", kImmMask7}},
        {"roundps", {kUnaryImm, 'f', 'f', "round_ps", kNone}},
        {"roundpd", {kUnaryImm, 'd', 'd', "round_pd", kNone}},
        {"dpps", {kBinaryImm, 'f', 'f', "dp_ps", kNone}},
        {"dppd", {kBinaryImm, 'd', 'd', "dp_pd", kNone}},
        {"permilps", {kUnaryImm, 'f', 'f', "permute_ps", kVexOnly}},
        {"permilpd", {kUnaryImm, 'd', 'd', "permute_pd", kVexOnly}},
        {"permpd", {kUnaryImm, 'd', 'd', "permute4x64_pd", kVexOnly | kOnly256}},
        {"perm2f128", {kBinaryImm, 'i', 'i', "permute2x128_si256", kVexOnly | kOnly256}},
        {"broadcastss", {kUnary, 'f', 'f', "broadcastss_ps", kVexOnly | kSrc128}},
        {"broadcastsd", {kUnary, 'd', 'd', "broadcastsd_pd", kVexOnly | kSrc128 | kOnly256}},
        {"broadcastf128",
         {kUnary, 'i', 'i', "broadcastsi128_si256", kVexOnly | kSrc128 | kOnly256}},

        // Scalar floating point.
        {"addss", {kBinary, 'f', 'f', "add_ss", kNone}},
        {"addsd", {kBinary, 'd', 'd', "add_sd", kNone}},
        {"subss", {kBinary, 'f', 'f', "sub_ss", kNone}},
        {"subsd", {kBinary, 'd', 'd', "sub_sd", kNone}},
        {"mulss", {kBinary, 'f', 'f', "mul_ss", kNone}},
        {"mulsd", {kBinary, 'd', 'd', "mul_sd", kNone}},
        {"divss", {kBinary, 'f', 'f', "div_ss", kNone}},
        {"divsd", {kBinary, 'd', 'd', "div_sd", kNone}},
        {"minss", {kBinary, 'f', 'f', "min_ss", kNone}},
        {"minsd", {kBinary, 'd', 'd', "min_sd", kNone}},
        {"maxss", {kBinary, 'f', 'f', "max_ss", kNone}},
        {"maxsd", {kBinary, 'd', 'd', "max_sd", kNone}},
        {"sqrtsd", {kBinary, 'd', 'd', "sqrt_sd", kNone}},
        {"sqrtss", {kScalarUnary, 'f', 'f', "sqrt_ss", kNone}},
        {"rcpss", {kScalarUnary, 'f', 'f', "rcp_ss", kNone}},
        {"rsqrtss", {kScalarUnary, 'f', 'f', "rsqrt_ss", kNone}},
        {"cmpss", {kBinaryImm, 'f', 'f', "cmp_ss", kImmMask7}},
        {"cmpsd", {kBinaryImm, 'd', 'd', "cmp_sd", kImmMask7}},
        {"roundss", {kBinaryImm, 'f', 'f', "round_ss", kNone}},
        {"roundsd", {kBinaryImm, 'd', 'd', "round_sd", kNone}},

        // Conversions.
        {"cvtdq2ps", {kUnary, 'i', 'f', "cvtepi32_ps", kNone}},
        {"cvtps2dq", {kUnary, 'f', 'i', "cvtps_epi32", kNone}},
        {"cvttps2dq", {kUnary, 'f', 'i', "cvttps_epi32", kNone}},
        {"cvtdq2pd", {kUnary, 'i', 'd', "cvtepi32_pd", kSrc128}},
        {"cvtps2pd", {kUnary, 'f', 'd', "cvtps_pd", kSrc128}},
        {"cvtpd2ps", {kUnary, 'd', 'f', "cvtpd_ps", kHalfDst}},
        {"cvtpd2dq", {kUnary, 'd', 'i', "cvtpd_epi32", kHalfDst}},
        {"cvttpd2dq", {kUnary, 'd', 'i', "cvttpd_epi32", kHalfDst}},
        // F16C. The immediate of vcvtps2ph is the rounding mode (bit 2: use MXCSR's).
        {"cvtph2ps", {kUnary, 'i', 'f', "cvtph_ps", kVexOnly | kSrc128}},
        {"cvtps2ph", {kUnaryImm, 'f', 'i', "cvtps_ph", kVexOnly | kHalfDst}},

        // Packed integer.
        {"paddb", {kBinary, 'i', 'i', "add_epi8", kNone}},
        {"paddw", {kBinary, 'i', 'i', "add_epi16", kNone}},
        {"paddd", {kBinary, 'i', 'i', "add_epi32", kNone}},
        {"paddq", {kBinary, 'i', 'i', "add_epi64", kNone}},
        {"psubb", {kBinary, 'i', 'i', "sub_epi8", kNone}},
        {"psubw", {kBinary, 'i', 'i', "sub_epi16", kNone}},
        {"psubd", {kBinary, 'i', 'i', "sub_epi32", kNone}},
        {"psubq", {kBinary, 'i', 'i', "sub_epi64", kNone}},
        {"paddsb", {kBinary, 'i', 'i', "adds_epi8", kNone}},
        {"paddsw", {kBinary, 'i', 'i', "adds_epi16", kNone}},
        {"paddusb", {kBinary, 'i', 'i', "adds_epu8", kNone}},
        {"paddusw", {kBinary, 'i', 'i', "adds_epu16", kNone}},
        {"psubsb", {kBinary, 'i', 'i', "subs_epi8", kNone}},
        {"psubsw", {kBinary, 'i', 'i', "subs_epi16", kNone}},
        {"psubusb", {kBinary, 'i', 'i', "subs_epu8", kNone}},
        {"psubusw", {kBinary, 'i', 'i', "subs_epu16", kNone}},
        {"pand", {kBinary, 'i', 'i', "and_si128", kNone}},
        {"pandn", {kBinary, 'i', 'i', "andnot_si128", kNone}},
        {"por", {kBinary, 'i', 'i', "or_si128", kNone}},
        {"pxor", {kBinary, 'i', 'i', "xor_si128", kNone}},
        {"pcmpeqb", {kBinary, 'i', 'i', "cmpeq_epi8", kNone}},
        {"pcmpeqw", {kBinary, 'i', 'i', "cmpeq_epi16", kNone}},
        {"pcmpeqd", {kBinary, 'i', 'i', "cmpeq_epi32", kNone}},
        {"pcmpeqq", {kBinary, 'i', 'i', "cmpeq_epi64", kNone}},
        {"pcmpgtb", {kBinary, 'i', 'i', "cmpgt_epi8", kNone}},
        {"pcmpgtw", {kBinary, 'i', 'i', "cmpgt_epi16", kNone}},
        {"pcmpgtd", {kBinary, 'i', 'i', "cmpgt_epi32", kNone}},
        {"pcmpgtq", {kBinary, 'i', 'i', "cmpgt_epi64", kNone}},
        {"pmullw", {kBinary, 'i', 'i', "mullo_epi16", kNone}},
        {"pmulld", {kBinary, 'i', 'i', "mullo_epi32", kNone}},
        {"pmulhw", {kBinary, 'i', 'i', "mulhi_epi16", kNone}},
        {"pmulhuw", {kBinary, 'i', 'i', "mulhi_epu16", kNone}},
        {"pmulhrsw", {kBinary, 'i', 'i', "mulhrs_epi16", kNone}},
        {"pmuludq", {kBinary, 'i', 'i', "mul_epu32", kNone}},
        {"pmuldq", {kBinary, 'i', 'i', "mul_epi32", kNone}},
        {"pmaddwd", {kBinary, 'i', 'i', "madd_epi16", kNone}},
        {"pmaddubsw", {kBinary, 'i', 'i', "maddubs_epi16", kNone}},
        {"pavgb", {kBinary, 'i', 'i', "avg_epu8", kNone}},
        {"pavgw", {kBinary, 'i', 'i', "avg_epu16", kNone}},
        {"pmaxub", {kBinary, 'i', 'i', "max_epu8", kNone}},
        {"pmaxuw", {kBinary, 'i', 'i', "max_epu16", kNone}},
        {"pmaxud", {kBinary, 'i', 'i', "max_epu32", kNone}},
        {"pmaxsb", {kBinary, 'i', 'i', "max_epi8", kNone}},
        {"pmaxsw", {kBinary, 'i', 'i', "max_epi16", kNone}},
        {"pmaxsd", {kBinary, 'i', 'i', "max_epi32", kNone}},
        {"pminub", {kBinary, 'i', 'i', "min_epu8", kNone}},
        {"pminuw", {kBinary, 'i', 'i', "min_epu16", kNone}},
        {"pminud", {kBinary, 'i', 'i', "min_epu32", kNone}},
        {"pminsb", {kBinary, 'i', 'i', "min_epi8", kNone}},
        {"pminsw", {kBinary, 'i', 'i', "min_epi16", kNone}},
        {"pminsd", {kBinary, 'i', 'i', "min_epi32", kNone}},
        {"psadbw", {kBinary, 'i', 'i', "sad_epu8", kNone}},
        {"pshufb", {kBinary, 'i', 'i', "shuffle_epi8", kNone}},
        {"phaddw", {kBinary, 'i', 'i', "hadd_epi16", kNone}},
        {"phaddd", {kBinary, 'i', 'i', "hadd_epi32", kNone}},
        {"phsubw", {kBinary, 'i', 'i', "hsub_epi16", kNone}},
        {"phsubd", {kBinary, 'i', 'i', "hsub_epi32", kNone}},
        {"psignb", {kBinary, 'i', 'i', "sign_epi8", kNone}},
        {"psignw", {kBinary, 'i', 'i', "sign_epi16", kNone}},
        {"psignd", {kBinary, 'i', 'i', "sign_epi32", kNone}},
        {"punpcklbw", {kBinary, 'i', 'i', "unpacklo_epi8", kNone}},
        {"punpcklwd", {kBinary, 'i', 'i', "unpacklo_epi16", kNone}},
        {"punpckldq", {kBinary, 'i', 'i', "unpacklo_epi32", kNone}},
        {"punpcklqdq", {kBinary, 'i', 'i', "unpacklo_epi64", kNone}},
        {"punpckhbw", {kBinary, 'i', 'i', "unpackhi_epi8", kNone}},
        {"punpckhwd", {kBinary, 'i', 'i', "unpackhi_epi16", kNone}},
        {"punpckhdq", {kBinary, 'i', 'i', "unpackhi_epi32", kNone}},
        {"punpckhqdq", {kBinary, 'i', 'i', "unpackhi_epi64", kNone}},
        {"packsswb", {kBinary, 'i', 'i', "packs_epi16", kNone}},
        {"packssdw", {kBinary, 'i', 'i', "packs_epi32", kNone}},
        {"packuswb", {kBinary, 'i', 'i', "packus_epi16", kNone}},
        {"packusdw", {kBinary, 'i', 'i', "packus_epi32", kNone}},
        {"psllvd", {kBinary, 'i', 'i', "sllv_epi32", kVexOnly}},
        {"psllvq", {kBinary, 'i', 'i', "sllv_epi64", kVexOnly}},
        {"psrlvd", {kBinary, 'i', 'i', "srlv_epi32", kVexOnly}},
        {"psrlvq", {kBinary, 'i', 'i', "srlv_epi64", kVexOnly}},
        {"psravd", {kBinary, 'i', 'i', "srav_epi32", kVexOnly}},
        {"pabsb", {kUnary, 'i', 'i', "abs_epi8", kNone}},
        {"pabsw", {kUnary, 'i', 'i', "abs_epi16", kNone}},
        {"pabsd", {kUnary, 'i', 'i', "abs_epi32", kNone}},
        {"phminposuw", {kUnary, 'i', 'i', "minpos_epu16", kNone}}, // 128-bit only
        {"pmovzxbw", {kUnary, 'i', 'i', "cvtepu8_epi16", kSrc128}},
        {"pmovzxbd", {kUnary, 'i', 'i', "cvtepu8_epi32", kSrc128}},
        {"pmovzxbq", {kUnary, 'i', 'i', "cvtepu8_epi64", kSrc128}},
        {"pmovzxwd", {kUnary, 'i', 'i', "cvtepu16_epi32", kSrc128}},
        {"pmovzxwq", {kUnary, 'i', 'i', "cvtepu16_epi64", kSrc128}},
        {"pmovzxdq", {kUnary, 'i', 'i', "cvtepu32_epi64", kSrc128}},
        {"pmovsxbw", {kUnary, 'i', 'i', "cvtepi8_epi16", kSrc128}},
        {"pmovsxbd", {kUnary, 'i', 'i', "cvtepi8_epi32", kSrc128}},
        {"pmovsxbq", {kUnary, 'i', 'i', "cvtepi8_epi64", kSrc128}},
        {"pmovsxwd", {kUnary, 'i', 'i', "cvtepi16_epi32", kSrc128}},
        {"pmovsxwq", {kUnary, 'i', 'i', "cvtepi16_epi64", kSrc128}},
        {"pmovsxdq", {kUnary, 'i', 'i', "cvtepi32_epi64", kSrc128}},
        {"pbroadcastb", {kUnary, 'i', 'i', "broadcastb_epi8", kVexOnly | kSrc128}},
        {"pbroadcastw", {kUnary, 'i', 'i', "broadcastw_epi16", kVexOnly | kSrc128}},
        {"pbroadcastd", {kUnary, 'i', 'i', "broadcastd_epi32", kVexOnly | kSrc128}},
        {"pbroadcastq", {kUnary, 'i', 'i', "broadcastq_epi64", kVexOnly | kSrc128}},
        {"broadcasti128",
         {kUnary, 'i', 'i', "broadcastsi128_si256", kVexOnly | kSrc128 | kOnly256}},
        {"pshufd", {kUnaryImm, 'i', 'i', "shuffle_epi32", kNone}},
        {"pshuflw", {kUnaryImm, 'i', 'i', "shufflelo_epi16", kNone}},
        {"pshufhw", {kUnaryImm, 'i', 'i', "shufflehi_epi16", kNone}},
        {"pslldq", {kUnaryImm, 'i', 'i', "slli_si128", kNone}},
        {"psrldq", {kUnaryImm, 'i', 'i', "srli_si128", kNone}},
        {"permq", {kUnaryImm, 'i', 'i', "permute4x64_epi64", kVexOnly | kOnly256}},
        {"perm2i128", {kBinaryImm, 'i', 'i', "permute2x128_si256", kVexOnly | kOnly256}},
        {"palignr", {kBinaryImm, 'i', 'i', "alignr_epi8", kNone}},
        {"pblendw", {kBinaryImm, 'i', 'i', "blend_epi16", kNone}},
        {"pblendd", {kBinaryImm, 'i', 'i', "blend_epi32", kVexOnly}},
        {"mpsadbw", {kBinaryImm, 'i', 'i', "mpsadbw_epu8", kNone}},
        {"pblendvb", {kBlendv, 'i', 'i', "blendv_epi8", kNone}},
        {"psllw", {kShift, 'i', 'i', "sll_epi16", kNone}},
        {"pslld", {kShift, 'i', 'i', "sll_epi32", kNone}},
        {"psllq", {kShift, 'i', 'i', "sll_epi64", kNone}},
        {"psrlw", {kShift, 'i', 'i', "srl_epi16", kNone}},
        {"psrld", {kShift, 'i', 'i', "srl_epi32", kNone}},
        {"psrlq", {kShift, 'i', 'i', "srl_epi64", kNone}},
        {"psraw", {kShift, 'i', 'i', "sra_epi16", kNone}},
        {"psrad", {kShift, 'i', 'i', "sra_epi32", kNone}},
    };
    return ops;
}

std::string intrinsic(std::string_view fn, unsigned bits) {
    std::string name(fn);
    if (bits == 256) {
        if (name.ends_with("si128")) {
            name.replace(name.size() - 5, 5, "si256");
        }
        return "_mm256_" + name;
    }
    return "_mm_" + name;
}

// __m128i/__m256i to the intrinsic's operand type, and back.
std::string to_type(const std::string& value, char type, unsigned bits) {
    const char* prefix = bits == 256 ? "_mm256_castsi256_" : "_mm_castsi128_";
    switch (type) {
    case 'f': return fmt::format("{}ps({})", prefix, value);
    case 'd': return fmt::format("{}pd({})", prefix, value);
    default: return value;
    }
}

std::string from_type(const std::string& value, char type, unsigned bits) {
    const char* prefix = bits == 256 ? "_mm256_cast" : "_mm_cast";
    const char* suffix = bits == 256 ? "si256" : "si128";
    switch (type) {
    case 'f': return fmt::format("{}ps_{}({})", prefix, suffix, value);
    case 'd': return fmt::format("{}pd_{}({})", prefix, suffix, value);
    default: return value;
    }
}

std::string immediate_value(const ZydisDecodedOperand& operand) {
    if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        throw Unsupported{};
    }
    return fmt::format("{}", operand.imm.value.u & 0xFF);
}

bool is_vector_register(const ZydisDecodedOperand& operand) {
    if (operand.type != ZYDIS_OPERAND_TYPE_REGISTER) {
        return false;
    }
    const ZydisRegisterClass cls = ZydisRegisterGetClass(operand.reg.value);
    return cls == ZYDIS_REGCLASS_XMM || cls == ZYDIS_REGCLASS_YMM;
}

} // namespace

std::string FunctionEmitter::vreg(ZydisRegister reg) const {
    const ZydisRegisterClass cls = ZydisRegisterGetClass(reg);
    if (cls != ZYDIS_REGCLASS_XMM && cls != ZYDIS_REGCLASS_YMM) {
        throw Unsupported{};
    }
    return fmt::format("ymm{}", ZydisRegisterGetId(reg));
}

// The operand as a 128-bit (`bits` = 128) or 256-bit value. Memory reads exactly the operand's
// size and zero-extends.
std::string FunctionEmitter::vread(const ZydisDecodedOperand& operand, unsigned bits) const {
    if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        std::string name = vreg(operand.reg.value);
        if (bits == 256) {
            if (ZydisRegisterGetClass(operand.reg.value) != ZYDIS_REGCLASS_YMM) {
                throw Unsupported{};
            }
            return name;
        }
        return fmt::format("lo({})", name);
    }
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.type != ZYDIS_MEMOP_TYPE_MEM) {
        throw Unsupported{};
    }
    if (bits == 256) {
        if (operand.size != 256) {
            throw Unsupported{};
        }
        return "load256(ea)";
    }
    switch (operand.size) {
    case 8: return "_mm_cvtsi32_si128(load<uint8_t>(ea))";
    case 16: return "_mm_cvtsi32_si128(load<uint16_t>(ea))";
    case 32: return "load32(ea)";
    case 64: return "load64(ea)";
    case 128: return "load128(ea)";
    default: throw Unsupported{};
    }
}

// Writes a vector result. An xmm destination takes a 128-bit value (legacy SSE keeps the
// upper half, VEX clears it); a ymm destination takes a 256-bit value; memory stores the
// operand's size from the low end of a 128-bit value, or all of a 256-bit one.
std::string FunctionEmitter::vwrite(const ZydisDecodedOperand& operand,
                                    const std::string& value) const {
    if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const std::string name = vreg(operand.reg.value);
        if (ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_YMM) {
            return fmt::format("{} = {};", name, value);
        }
        return vex() ? fmt::format("{} = zext({});", name, value)
                     : fmt::format("{0} = with_lo({0}, {1});", name, value);
    }
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.type != ZYDIS_MEMOP_TYPE_MEM) {
        throw Unsupported{};
    }
    switch (operand.size) {
    case 32: return fmt::format("store32(ea, {});", value);
    case 64: return fmt::format("store64(ea, {});", value);
    case 128: return fmt::format("store128(ea, {});", value);
    case 256: return fmt::format("store256(ea, {});", value);
    default: throw Unsupported{};
    }
}

bool FunctionEmitter::emit_vector_table(std::string_view name, bool is_vex) {
    const auto it = table().find(name);
    if (it == table().end()) {
        return false;
    }
    const VectorOp& vop = it->second;
    if ((vop.flags & kVexOnly) != 0 && !is_vex) {
        return false;
    }
    const std::string_view fn_name = vop.fn;
    // Scalar instructions ignore VEX.L.
    const unsigned length =
        fn_name.ends_with("_ss") || fn_name.ends_with("_sd") ? 128 : vector_bits();
    if ((vop.flags & kOnly256) != 0 && length != 256) {
        throw Unsupported{};
    }
    const unsigned src_bits = (vop.flags & kSrc128) != 0 ? 128 : length;
    const unsigned dst_bits = (vop.flags & kHalfDst) != 0 ? 128 : length;
    const int n = visible();
    const std::string fn = intrinsic(vop.fn, length);
    const auto in = [&](const ZydisDecodedOperand& operand, unsigned bits) {
        return to_type(vread(operand, bits), vop.in, bits);
    };
    const auto out = [&](const std::string& value) { return from_type(value, vop.out, dst_bits); };
    switch (vop.shape) {
    case Shape::kMove:
        line(vwrite(op(0), vread(op(1), op(0).size == 256 ? 256 : 128)));
        return true;
    case Shape::kUnary:
        line(vwrite(op(0), out(fmt::format("{}({})", fn, in(op(n - 1), src_bits)))));
        return true;
    case Shape::kUnaryImm:
        line(vwrite(op(0), out(fmt::format("{}({}, {})", fn, in(op(n - 2), src_bits),
                                           immediate_value(op(n - 1))))));
        return true;
    case Shape::kBinary:
        line(vwrite(op(0), out(fmt::format("{}({}, {})", fn, in(op(n - 2), length),
                                           in(op(n - 1), length)))));
        return true;
    case Shape::kBinaryImm: {
        std::string imm = immediate_value(op(n - 1));
        if ((vop.flags & kImmMask7) != 0 && !is_vex) {
            imm = fmt::format("{}", op(n - 1).imm.value.u & 7);
        }
        line(vwrite(op(0), out(fmt::format("{}({}, {}, {})", fn, in(op(n - 3), length),
                                           in(op(n - 2), length), imm))));
        return true;
    }
    case Shape::kBlendv: {
        const std::string a = is_vex ? in(op(1), length) : in(op(0), length);
        const std::string b = is_vex ? in(op(2), length) : in(op(1), length);
        const std::string mask = is_vex ? in(op(3), length) : to_type("lo(ymm0)", vop.in, 128);
        line(vwrite(op(0), out(fmt::format("{}({}, {}, {})", fn, a, b, mask))));
        return true;
    }
    case Shape::kScalarUnary: {
        const std::string move = vop.in == 'd' ? "_mm_move_sd" : "_mm_move_ss";
        line(vwrite(op(0), out(fmt::format("{}({}, {}({}))", move, in(op(n - 2), 128), fn,
                                           in(op(n - 1), 128)))));
        return true;
    }
    case Shape::kShift: {
        const ZydisDecodedOperand& count = op(n - 1);
        std::string shift_fn = fn;
        std::string count_value;
        if (count.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            // _mm_sll_epi16 -> _mm_slli_epi16
            shift_fn.insert(shift_fn.rfind('_'), "i");
            count_value = immediate_value(count);
        } else {
            count_value = vread(count, 128);
        }
        line(vwrite(op(0),
                    fmt::format("{}({}, {})", shift_fn, vread(op(n - 2), length), count_value)));
        return true;
    }
    }
    return false;
}

void FunctionEmitter::emit_fma(std::string_view name) {
    // v f [n] (madd|msub|maddsub|msubadd) (132|213|231) (ps|pd|ss|sd)
    std::string_view rest = name.substr(2);
    const bool negate = rest.starts_with('n');
    if (negate) {
        rest.remove_prefix(1);
    }
    if (rest.size() < 7) {
        throw Unsupported{};
    }
    const std::string_view type = rest.substr(rest.size() - 2);
    const std::string_view order = rest.substr(rest.size() - 5, 3);
    const std::string_view kind = rest.substr(0, rest.size() - 5);
    if (kind != "madd" && kind != "msub" && kind != "maddsub" && kind != "msubadd") {
        throw Unsupported{};
    }
    const bool scalar = type == "ss" || type == "sd";
    const char t = type[1] == 's' ? 'f' : 'd';
    const unsigned length = scalar ? 128 : vector_bits();
    const std::string fn =
        intrinsic(fmt::format("f{}{}_{}", negate ? "n" : "", kind, type), length);
    const std::string x1 = to_type(vread(op(0), length), t, length);
    const std::string x2 = to_type(vread(op(1), length), t, length);
    const std::string x3 = to_type(vread(op(2), length), t, length);
    std::string result;
    if (order == "132") {
        result = fmt::format("{}({}, {}, {})", fn, x1, x3, x2);
    } else if (order == "213") {
        result = fmt::format("{}({}, {}, {})", fn, x1, x2, x3); // multiplication commutes
    } else if (order == "231") {
        result = fmt::format("{}({}, {}, {})", fn, x2, x3, x1);
        if (scalar) {
            // The intrinsic copies the upper elements from its first operand; they must
            // come from the destination.
            result =
                fmt::format("{}({}, {})", t == 'f' ? "_mm_move_ss" : "_mm_move_sd", x1, result);
        }
    } else {
        throw Unsupported{};
    }
    line(vwrite(op(0), from_type(result, t, length)));
}

// SSE4.2 string comparisons (PCMPISTRI, PCMPISTRM, PCMPESTRI, PCMPESTRM, and their VEX forms).
// The host runs the same instruction: the result intrinsic and the four flag intrinsics take
// the same operands and compile to a single instruction. The index goes to ECX, the mask to
// XMM0; AF and PF are cleared.
void FunctionEmitter::emit_string_compare(const Insn& insn, std::string_view name) {
    const bool explicit_length = name[4] == 'e';
    const bool mask = name.back() == 'm';
    const std::string imm = immediate_value(op(2));
    line(fmt::format("const __m128i a = {}; const __m128i b = {};", vread(op(0), 128),
                     vread(op(1), 128)));
    std::string args = fmt::format("a, b, {}", imm);
    if (explicit_length) {
        // The lengths are EAX and EDX, or RAX and RDX when the operand size is 64 bits.
        bool wide = false;
        for (int i = 0; i < int{insn.insn.operand_count}; ++i) {
            const ZydisDecodedOperand& operand = insn.ops[i];
            wide |= operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    operand.reg.value == ZYDIS_REGISTER_RAX;
        }
        line(wide ? "const int la = string_length64(rax); const int lb = string_length64(rdx);"
                  : "const int la = int32_t(rax); const int lb = int32_t(rdx);");
        args = fmt::format("a, la, b, lb, {}", imm);
    }
    const std::string fn = explicit_length ? "_mm_cmpestr" : "_mm_cmpistr";
    if (mask) {
        line(fmt::format("const __m128i m = {}m({});", fn, args));
        line(vex() ? "ymm0 = zext(m);" : "ymm0 = with_lo(ymm0, m);");
    } else {
        line(write_gpr(ZYDIS_REGISTER_ECX, fmt::format("uint32_t({}i({}))", fn, args)));
    }
    line(fmt::format("f.cf = {0}c({1}) != 0; f.zf = {0}z({1}) != 0; f.sf = {0}s({1}) != 0; "
                     "f.of = {0}o({1}) != 0; f.af = f.pf = false;",
                     fn, args));
}

void FunctionEmitter::emit_vector(const Insn& insn) {
    const ZydisDecodedInstruction& d = insn.insn;
    std::string_view name = ZydisMnemonicGetString(d.mnemonic);
    const bool is_vex = vex();
    if (is_vex) {
        if (!name.starts_with('v')) {
            throw Unsupported{}; // BMI and other VEX-encoded integer instructions not handled above
        }
        name.remove_prefix(1);
    }
    if (is_vex && (name.starts_with("fm") || name.starts_with("fnm"))) {
        emit_fma(ZydisMnemonicGetString(d.mnemonic));
        return;
    }
    if (emit_vector_table(name, is_vex)) {
        return;
    }
    const int n = visible();
    const unsigned length = vector_bits();

    if (name == "zeroupper" || name == "zeroall") {
        for (int i = 0; i < 16; ++i) {
            const bool local = ((vec_used_ >> i) & 1) != 0;
            const std::string reg = local ? fmt::format("ymm{}", i) : fmt::format("ctx.ymm[{}]", i);
            line(name == "zeroall" ? fmt::format("{} = _mm256_setzero_si256();", reg)
                                   : fmt::format("{0} = zext(lo({0}));", reg));
        }
        return;
    }
    if (name == "ldmxcsr") {
        line("_mm_setcsr(load<uint32_t>(ea));");
        return;
    }
    if (name == "stmxcsr") {
        line("store<uint32_t>(ea, _mm_getcsr());");
        return;
    }
    if (name == "movss" || name == "movsd") {
        const char* move = name == "movss" ? "_mm_move_ss" : "_mm_move_sd";
        const char t = name == "movss" ? 'f' : 'd';
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY ||
            op(n - 1).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            line(vwrite(op(0), vread(op(n - 1), 128)));
        } else {
            line(vwrite(op(0), from_type(fmt::format("{}({}, {})", move,
                                                     to_type(vread(op(n - 2), 128), t, 128),
                                                     to_type(vread(op(n - 1), 128), t, 128)),
                                         t, 128)));
        }
        return;
    }
    if (name == "movd" || name == "movq") {
        const ZydisDecodedOperand& dst = op(0);
        const ZydisDecodedOperand& src = op(1);
        if (is_vector_register(dst)) {
            if (src.type == ZYDIS_OPERAND_TYPE_REGISTER && !is_vector_register(src)) {
                line(vwrite(dst, src.size == 64
                                     ? fmt::format("_mm_cvtsi64_si128(int64_t({}))", get(src))
                                     : fmt::format("_mm_cvtsi32_si128(int32_t({}))", get(src))));
            } else {
                line(vwrite(dst, fmt::format("_mm_move_epi64({})", vread(src, 128))));
            }
            return;
        }
        if (dst.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            line(vwrite(dst, vread(src, 128)));
            return;
        }
        line(put(dst, dst.size == 64 ? fmt::format("_mm_cvtsi128_si64({})", vread(src, 128))
                                     : fmt::format("_mm_cvtsi128_si32({})", vread(src, 128))));
        return;
    }
    if (name == "movlps" || name == "movlpd" || name == "movhps" || name == "movhpd") {
        const bool high = name[3] == 'h';
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const std::string src = vread(op(n - 1), 128);
            line(fmt::format("store64(ea, {});",
                             high ? fmt::format("_mm_unpackhi_epi64({0}, {0})", src) : src));
        } else {
            const std::string keep = vread(op(n - 2), 128);
            line(vwrite(op(0), high ? fmt::format("_mm_unpacklo_epi64({}, load64(ea))", keep)
                                    : fmt::format("_mm_blend_epi32({}, load64(ea), 0x3)", keep)));
        }
        return;
    }
    if (name == "comiss" || name == "ucomiss" || name == "comisd" || name == "ucomisd") {
        line(fmt::format("{}(f, {}, {});", name.ends_with("ss") ? "comiss" : "comisd",
                         vread(op(0), 128), vread(op(1), 128)));
        return;
    }
    if (name == "ptest") {
        const std::string a = vread(op(0), length);
        const std::string b = vread(op(1), length);
        const char* prefix = length == 256 ? "_mm256_test" : "_mm_test";
        const char* suffix = length == 256 ? "si256" : "si128";
        line(fmt::format("f.zf = {0}z_{1}({2}, {3}) != 0; f.cf = {0}c_{1}({2}, {3}) != 0; f.of = "
                         "f.sf = f.af = f.pf = false;",
                         prefix, suffix, a, b));
        return;
    }
    if (name == "pcmpistri" || name == "pcmpistrm" || name == "pcmpestri" || name == "pcmpestrm") {
        emit_string_compare(insn, name);
        return;
    }
    if (is_vex && (name == "pmaskmovd" || name == "pmaskmovq" || name == "maskmovps" ||
                   name == "maskmovpd")) {
        // Masked loads and stores touch only the elements whose mask sign bit is set, so a
        // masked-off element may lie on an unmapped page. The host instruction does the same.
        const char t = name.ends_with("ps") ? 'f' : (name.ends_with("pd") ? 'd' : 'i');
        const char* suffix =
            t == 'f' ? "ps" : (t == 'd' ? "pd" : (name.back() == 'q' ? "epi64" : "epi32"));
        const char* element = t == 'f'   ? "float"
                              : t == 'd' ? "double"
                                         : (name.back() == 'q' ? "long long" : "int");
        const std::string mask = vread(op(1), length);
        if (op(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            line(fmt::format("{}(reinterpret_cast<{}*>(ea), {}, {});",
                             intrinsic(fmt::format("maskstore_{}", suffix), length), element, mask,
                             to_type(vread(op(2), length), t, length)));
        } else {
            line(vwrite(op(0),
                        from_type(fmt::format("{}(reinterpret_cast<const {}*>(ea), {})",
                                              intrinsic(fmt::format("maskload_{}", suffix), length),
                                              element, mask),
                                  t, length)));
        }
        return;
    }
    if (name == "pmovmskb" || name == "movmskps" || name == "movmskpd") {
        const std::string src = vread(op(1), length);
        std::string mask;
        if (name == "pmovmskb") {
            mask = fmt::format("{}({})", intrinsic("movemask_epi8", length), src);
        } else {
            const char t = name == "movmskps" ? 'f' : 'd';
            mask =
                fmt::format("{}({})", intrinsic(t == 'f' ? "movemask_ps" : "movemask_pd", length),
                            to_type(src, t, length));
        }
        line(put(op(0), fmt::format("uint32_t({})", mask)));
        return;
    }
    if (name == "pextrb" || name == "pextrw" || name == "pextrd" || name == "pextrq" ||
        name == "extractps") {
        const std::string src = vread(op(1), 128);
        const std::string imm = immediate_value(op(2));
        std::string value;
        if (name == "extractps") {
            value = fmt::format("uint32_t(_mm_extract_ps(_mm_castsi128_ps({}), {}))", src, imm);
        } else if (name == "pextrq") {
            value = fmt::format("uint64_t(_mm_extract_epi64({}, {}))", src, imm);
        } else {
            const char* lane = name == "pextrb" ? "epi8" : (name == "pextrw" ? "epi16" : "epi32");
            value = fmt::format("uint32_t(_mm_extract_{}({}, {}))", lane, src, imm);
        }
        line(put(op(0), value));
        return;
    }
    if (name == "pinsrb" || name == "pinsrw" || name == "pinsrd" || name == "pinsrq") {
        const std::string keep = vread(op(n - 3), 128);
        const std::string value = get(op(n - 2));
        const std::string imm = immediate_value(op(n - 1));
        const char* lane = name == "pinsrb"   ? "epi8"
                           : name == "pinsrw" ? "epi16"
                           : name == "pinsrd" ? "epi32"
                                              : "epi64";
        line(vwrite(op(0), fmt::format("_mm_insert_{}({}, {}({}), {})", lane, keep,
                                       name == "pinsrq" ? "int64_t" : "int", value, imm)));
        return;
    }
    if (name == "insertps") {
        std::string imm = immediate_value(op(n - 1));
        if (op(n - 2).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            imm = fmt::format("{}", op(n - 1).imm.value.u & 0x3F);
        }
        line(vwrite(op(0), fmt::format("_mm_castps_si128(_mm_insert_ps(_mm_castsi128_ps({}), "
                                       "_mm_castsi128_ps({}), {}))",
                                       vread(op(n - 3), 128), vread(op(n - 2), 128), imm)));
        return;
    }
    if (name == "cvtsi2ss" || name == "cvtsi2sd") {
        const ZydisDecodedOperand& src = op(n - 1);
        const bool wide = src.size == 64;
        const char t = name == "cvtsi2ss" ? 'f' : 'd';
        const std::string fn = fmt::format("_mm_cvtsi{}_s{}", wide ? 64 : 32, t == 'f' ? 's' : 'd');
        line(vwrite(op(0), from_type(fmt::format("{}({}, {}({}))", fn,
                                                 to_type(vread(op(n - 2), 128), t, 128),
                                                 wide ? "int64_t" : "int32_t", get(src)),
                                     t, 128)));
        return;
    }
    if (name == "cvtss2si" || name == "cvttss2si" || name == "cvtsd2si" || name == "cvttsd2si") {
        const bool truncate = name.starts_with("cvtt");
        const char t = name.find("ss2si") != std::string_view::npos ? 'f' : 'd';
        const bool wide = op(0).size == 64;
        const std::string fn = fmt::format("_mm_cvt{}s{}_si{}", truncate ? "t" : "",
                                           t == 'f' ? 's' : 'd', wide ? 64 : 32);
        line(put(op(0), fmt::format("{}({}({}))", wide ? "uint64_t" : "uint32_t", fn,
                                    to_type(vread(op(1), 128), t, 128))));
        return;
    }
    if (name == "cvtss2sd") {
        line(vwrite(op(0), fmt::format("_mm_castpd_si128(_mm_cvtss_sd(_mm_castsi128_pd({}), "
                                       "_mm_castsi128_ps({})))",
                                       vread(op(n - 2), 128), vread(op(n - 1), 128))));
        return;
    }
    if (name == "cvtsd2ss") {
        line(vwrite(op(0), fmt::format("_mm_castps_si128(_mm_cvtsd_ss(_mm_castsi128_ps({}), "
                                       "_mm_castsi128_pd({})))",
                                       vread(op(n - 2), 128), vread(op(n - 1), 128))));
        return;
    }
    if (is_vex && (name == "extracti128" || name == "extractf128")) {
        line(vwrite(op(0), fmt::format("_mm256_extracti128_si256({}, {})", vread(op(1), 256),
                                       immediate_value(op(2)))));
        return;
    }
    if (is_vex && (name == "inserti128" || name == "insertf128")) {
        line(vwrite(op(0), fmt::format("_mm256_inserti128_si256({}, {}, {})", vread(op(1), 256),
                                       vread(op(2), 128), immediate_value(op(3)))));
        return;
    }
    if (is_vex && (name == "permd" || name == "permps")) {
        if (name == "permd") {
            line(vwrite(op(0), fmt::format("_mm256_permutevar8x32_epi32({}, {})", vread(op(2), 256),
                                           vread(op(1), 256))));
        } else {
            line(vwrite(op(0), fmt::format("_mm256_castps_si256(_mm256_permutevar8x32_ps("
                                           "_mm256_castsi256_ps({}), {}))",
                                           vread(op(2), 256), vread(op(1), 256))));
        }
        return;
    }
    throw Unsupported{};
}

} // namespace g6::recomp
