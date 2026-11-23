// The differential test's assembly probes, as an X-macro list (no include guard). The includer
// defines
//
//   PROBE(name, width, input, flags, param, body)
//
// diff_program.c turns each entry into an exported assembly function named `name` whose code
// is `body`; test_diff.cpp turns the same entries into the table it runs against both the
// native DLL and its recompiled version, using `width`, `input` (how arguments are chosen),
// `flags` (which status flags the instruction defines), and `param` (an immediate shift count,
// or the element size of a string operation). Each side ignores the fields it doesn't need.
//
// Integer probes are called as probe(a, b, c, out): a in rcx, b in rdx, c in r8, out in r9.
// They set CF from bit 0 of c, so an instruction that leaves CF alone is still checked through
// it, then store rcx and rdx to out[0] and out[1] and return RFLAGS.
//
// Vector probes are called as probe(a, b, out): 64-byte inputs at rcx and rdx, a 64-byte
// result at r8. ymm0 and ymm1 hold the first halves of a and b, ymm2 and ymm3 the second
// halves. The result is ymm0, stored to out[0..31]; a probe with a scalar result stores it to
// out[32..]. They return RFLAGS.

#define G6_SET_CF "    bt r8, 0\n"
#define G6_STORE_RETURN                                                                            \
    "    mov qword ptr [r9], rcx\n    mov qword ptr [r9 + 8], rdx\n    pushfq\n    pop rax\n"      \
    "    ret\n"
#define G6_INSN(name, width, input, flags, param, text)                                            \
    PROBE(name, width, input, flags, param, G6_SET_CF "    " text "\n" G6_STORE_RETURN)

// `op a, b` on rcx and rdx.
#define G6_RR(op, flags)                                                                           \
    G6_INSN("i_" op "8", 8, kPair, flags, 0, op " cl, dl")                                         \
    G6_RR16(op, flags)
#define G6_RR16(op, flags)                                                                         \
    G6_INSN("i_" op "16", 16, kPair, flags, 0, op " cx, dx")                                       \
    G6_INSN("i_" op "32", 32, kPair, flags, 0, op " ecx, edx")                                     \
    G6_INSN("i_" op "64", 64, kPair, flags, 0, op " rcx, rdx")

// `op a`.
#define G6_R(op, flags)                                                                            \
    G6_INSN("i_" op "8", 8, kPair, flags, 0, op " cl")                                             \
    G6_INSN("i_" op "16", 16, kPair, flags, 0, op " cx")                                           \
    G6_INSN("i_" op "32", 32, kPair, flags, 0, op " ecx")                                          \
    G6_INSN("i_" op "64", 64, kPair, flags, 0, op " rcx")

// Shift or rotate a by the count in b (cl).
#define G6_SHIFT_BODY(op, reg)                                                                     \
    "    mov r10, rcx\n    mov ecx, edx\n" G6_SET_CF "    " op " " reg                             \
    ", cl\n    mov rcx, r10\n" G6_STORE_RETURN
#define G6_SHIFT(op, flags)                                                                        \
    PROBE("i_" op "8_cl", 8, kShift, flags, 0, G6_SHIFT_BODY(op, "r10b"))                          \
    PROBE("i_" op "16_cl", 16, kShift, flags, 0, G6_SHIFT_BODY(op, "r10w"))                        \
    PROBE("i_" op "32_cl", 32, kShift, flags, 0, G6_SHIFT_BODY(op, "r10d"))                        \
    PROBE("i_" op "64_cl", 64, kShift, flags, 0, G6_SHIFT_BODY(op, "r10"))

// Shift or rotate a by an immediate.
#define G6_SHIFT_IMM(op, n, flags)                                                                 \
    G6_INSN("i_" op "8_" #n, 8, kPair, flags, n, op " cl, " #n)                                    \
    G6_INSN("i_" op "16_" #n, 16, kPair, flags, n, op " cx, " #n)                                  \
    G6_INSN("i_" op "32_" #n, 32, kPair, flags, n, op " ecx, " #n)                                 \
    G6_INSN("i_" op "64_" #n, 64, kPair, flags, n, op " rcx, " #n)

// Double-precision shifts of a, filling from b, by the count in c (cl).
#define G6_SHIFT_DOUBLE_BODY(op, a, b)                                                             \
    "    mov r10, rcx\n    mov ecx, r8d\n" G6_SET_CF "    " op " " a ", " b ", cl\n"               \
    "    mov rcx, r10\n" G6_STORE_RETURN
#define G6_SHIFT_DOUBLE(op)                                                                        \
    PROBE("i_" op "16_cl", 16, kShiftDouble, kShiftDouble, 0,                                      \
          G6_SHIFT_DOUBLE_BODY(op, "r10w", "dx"))                                                  \
    PROBE("i_" op "32_cl", 32, kShiftDouble, kShiftDouble, 0,                                      \
          G6_SHIFT_DOUBLE_BODY(op, "r10d", "edx"))                                                 \
    PROBE("i_" op "64_cl", 64, kShiftDouble, kShiftDouble, 0,                                      \
          G6_SHIFT_DOUBLE_BODY(op, "r10", "rdx"))

// One-operand multiply and divide: the accumulator is a (with c as the high half), the
// operand is b.
#define G6_MULDIV_BODY(op, reg)                                                                    \
    "    mov rax, rcx\n    mov r10, rdx\n    mov rdx, r8\n" G6_SET_CF "    " op " " reg "\n"       \
    "    mov rcx, rax\n" G6_STORE_RETURN
#define G6_MULDIV(op, input, flags)                                                                \
    PROBE("i_" op "8_acc", 8, input, flags, 0, G6_MULDIV_BODY(op, "r10b"))                         \
    PROBE("i_" op "16_acc", 16, input, flags, 0, G6_MULDIV_BODY(op, "r10w"))                       \
    PROBE("i_" op "32_acc", 32, input, flags, 0, G6_MULDIV_BODY(op, "r10d"))                       \
    PROBE("i_" op "64_acc", 64, input, flags, 0, G6_MULDIV_BODY(op, "r10"))

// `op [out], b` with a stored at out first; the memory value comes back in rcx.
#define G6_MEM_BODY(text, size, reg)                                                               \
    "    mov qword ptr [r9], rcx\n" G6_SET_CF "    " text " " size " ptr [r9], " reg "\n"          \
    "    mov rcx, qword ptr [r9]\n" G6_STORE_RETURN
#define G6_MEM(name, text, flags)                                                                  \
    PROBE("i_" name "8_mem", 8, kPair, flags, 0, G6_MEM_BODY(text, "byte", "dl"))                  \
    PROBE("i_" name "16_mem", 16, kPair, flags, 0, G6_MEM_BODY(text, "word", "dx"))                \
    PROBE("i_" name "32_mem", 32, kPair, flags, 0, G6_MEM_BODY(text, "dword", "edx"))              \
    PROBE("i_" name "64_mem", 64, kPair, flags, 0, G6_MEM_BODY(text, "qword", "rdx"))

// `op b, [out]` with a stored at out first.
#define G6_MEM_SRC_BODY(text, size, reg)                                                           \
    "    mov qword ptr [r9], rcx\n" G6_SET_CF "    " text " " reg ", " size                        \
    " ptr [r9]\n" G6_STORE_RETURN
#define G6_MEM_SRC(op, flags)                                                                      \
    PROBE("i_" op "8_load", 8, kPair, flags, 0, G6_MEM_SRC_BODY(op, "byte", "dl"))                 \
    PROBE("i_" op "16_load", 16, kPair, flags, 0, G6_MEM_SRC_BODY(op, "word", "dx"))               \
    PROBE("i_" op "32_load", 32, kPair, flags, 0, G6_MEM_SRC_BODY(op, "dword", "edx"))             \
    PROBE("i_" op "64_load", 64, kPair, flags, 0, G6_MEM_SRC_BODY(op, "qword", "rdx"))

// `op [out]` with a stored at out first.
#define G6_MEM1_BODY(text, size)                                                                   \
    "    mov qword ptr [r9], rcx\n" G6_SET_CF "    " text " " size " ptr [r9]\n"                   \
    "    mov rcx, qword ptr [r9]\n" G6_STORE_RETURN
#define G6_MEM1(name, text, flags)                                                                 \
    PROBE("i_" name "8_mem", 8, kPair, flags, 0, G6_MEM1_BODY(text, "byte"))                       \
    PROBE("i_" name "16_mem", 16, kPair, flags, 0, G6_MEM1_BODY(text, "word"))                     \
    PROBE("i_" name "32_mem", 32, kPair, flags, 0, G6_MEM1_BODY(text, "dword"))                    \
    PROBE("i_" name "64_mem", 64, kPair, flags, 0, G6_MEM1_BODY(text, "qword"))

// CMPXCHG on memory: a in memory, b the new value, c the expected value in the accumulator.
#define G6_CMPXCHG_BODY(text, size, reg)                                                           \
    "    mov qword ptr [r9], rcx\n    mov rax, r8\n    " text " " size " ptr [r9], " reg "\n"      \
    "    mov rcx, qword ptr [r9]\n    mov rdx, rax\n" G6_STORE_RETURN
#define G6_CMPXCHG(name, text)                                                                     \
    PROBE("i_" name "8", 8, kCmpxchg, kArith, 0, G6_CMPXCHG_BODY(text, "byte", "dl"))              \
    PROBE("i_" name "16", 16, kCmpxchg, kArith, 0, G6_CMPXCHG_BODY(text, "word", "dx"))            \
    PROBE("i_" name "32", 32, kCmpxchg, kArith, 0, G6_CMPXCHG_BODY(text, "dword", "edx"))          \
    PROBE("i_" name "64", 64, kCmpxchg, kArith, 0, G6_CMPXCHG_BODY(text, "qword", "rdx"))

// Bit operations on a bit string at out (which points into a larger buffer), bit offset b.
#define G6_BITMEM_BODY(text) G6_SET_CF "    " text "\n    pushfq\n    pop rax\n    ret\n"
#define G6_BITMEM(name, op)                                                                        \
    PROBE("i_" name "16_bitmem", 16, kBitMem, kCarry, 0, G6_BITMEM_BODY(op " word ptr [r9], dx"))  \
    PROBE("i_" name "32_bitmem", 32, kBitMem, kCarry, 0,                                           \
          G6_BITMEM_BODY(op " dword ptr [r9], edx"))                                               \
    PROBE("i_" name "64_bitmem", 64, kBitMem, kCarry, 0, G6_BITMEM_BODY(op " qword ptr [r9], rdx"))

// Every condition code after `cmp a, b`.
#define G6_EACH_CONDITION(M)                                                                       \
    M("o", "0")                                                                                    \
    M("no", "1")                                                                                   \
    M("b", "2")                                                                                    \
    M("ae", "3") M("e", "4") M("ne", "5") M("be", "6") M("a", "7") M("s", "8") M("ns", "9")        \
        M("p", "10") M("np", "11") M("l", "12") M("ge", "13") M("le", "14") M("g", "15")
#define G6_SETCC(cc, i) "    set" cc " byte ptr [r9 + " i "]\n"
#define G6_CMOV64(cc, i)                                                                           \
    "    mov rax, rcx\n    cmov" cc " rax, rdx\n    mov qword ptr [r9 + 8*" i "], rax\n"
#define G6_CMOV32(cc, i)                                                                           \
    "    mov rax, rcx\n    cmov" cc " eax, edx\n    mov qword ptr [r9 + 8*" i "], rax\n"
#define G6_CMOV16(cc, i)                                                                           \
    "    mov rax, rcx\n    cmov" cc " ax, dx\n    mov qword ptr [r9 + 8*" i "], rax\n"
#define G6_JCC(cc, i)                                                                              \
    "    j" cc " .Ljcc_" cc "_taken\n    mov byte ptr [r9 + " i "], 0\n    jmp .Ljcc_" cc          \
    "_done\n"                                                                                      \
    ".Ljcc_" cc "_taken:\n    mov byte ptr [r9 + " i "], 1\n.Ljcc_" cc "_done:\n"
#define G6_CONDITIONS(name, width_cmp, M)                                                          \
    PROBE(name, 64, kPair, kArith, 0,                                                              \
          "    " width_cmp "\n" G6_EACH_CONDITION(M) "    pushfq\n    pop rax\n    ret\n")

// String operations: probe(dst, src, count, value). Returns how far rdi and rsi moved and
// what is left in rcx.
#define G6_STRING_PROLOGUE                                                                         \
    "    push rsi\n    push rdi\n    mov r10, rcx\n    mov r11, rdx\n    mov rdi, rcx\n"           \
    "    mov rsi, rdx\n    mov rax, r9\n    mov rcx, r8\n"
#define G6_STRING_EPILOGUE                                                                         \
    "    sub rdi, r10\n    sub rsi, r11\n    shl rsi, 20\n    shl rcx, 40\n    lea rax, [rdi + "   \
    "rsi]\n"                                                                                       \
    "    add rax, rcx\n    pop rdi\n    pop rsi\n    ret\n"
#define G6_STRING(name, size, text)                                                                \
    PROBE("s_" name, size, kString, kNone, size,                                                   \
          G6_STRING_PROLOGUE "    " text "\n" G6_STRING_EPILOGUE)
// The same, walking down from the last element (`bytes` is the element size).
#define G6_STRING_DOWN(name, size, bytes, text)                                                    \
    PROBE("s_" name, size, kString, kNone, size,                                                   \
          G6_STRING_PROLOGUE "    lea rdi, [rdi + rcx*" bytes " - " bytes "]\n"                    \
                             "    lea rsi, [rsi + rcx*" bytes " - " bytes "]\n"                    \
                             "    mov r10, rdi\n    mov r11, rsi\n    std\n    " text              \
                             "\n    cld\n" G6_STRING_EPILOGUE)

// Vector probes.
#define G6_VLOAD                                                                                   \
    "    vmovdqu ymm0, ymmword ptr [rcx]\n    vmovdqu ymm1, ymmword ptr [rdx]\n"                   \
    "    vmovdqu ymm2, ymmword ptr [rcx + 32]\n    vmovdqu ymm3, ymmword ptr [rdx + 32]\n"
#define G6_VSTORE                                                                                  \
    "    vmovdqu ymmword ptr [r8], ymm0\n    pushfq\n    pop rax\n    vzeroupper\n    ret\n"
#define G6_VEC(name, input, flags, text)                                                           \
    PROBE(name, 0, input, flags, 0, G6_VLOAD "    " text "\n" G6_VSTORE)

// Legacy SSE, VEX.128, and VEX.256 forms. Legacy forms write xmm0 and must keep the upper half
// of ymm0; VEX.128 forms must clear it. G6_VFLAGS is kNone except around the commutative
// floating-point operations below.
#define G6_VFLAGS kNone
#define G6_V_SSE(op) G6_VEC("v_" op, kVector, G6_VFLAGS, op " xmm0, xmm1")
#define G6_V_VEX128(op) G6_VEC("v_v" op "_128", kVector, G6_VFLAGS, "v" op " xmm0, xmm2, xmm1")
#define G6_V_VEX256(op) G6_VEC("v_v" op "_256", kVector, G6_VFLAGS, "v" op " ymm0, ymm2, ymm1")
#define G6_V_ALL(op) G6_V_SSE(op) G6_V_VEX128(op) G6_V_VEX256(op)
#define G6_V_128(op) G6_V_SSE(op) G6_V_VEX128(op)
#define G6_V_AVX(op) G6_V_VEX128(op) G6_V_VEX256(op)

// Unary forms: `op dst, src`.
#define G6_V_UN(op)                                                                                \
    G6_VEC("v_" op, kVector, kNone, op " xmm0, xmm1")                                              \
    G6_VEC("v_v" op "_128", kVector, kNone, "v" op " xmm0, xmm1")                                  \
    G6_VEC("v_v" op "_256", kVector, kNone, "v" op " ymm0, ymm1")
#define G6_V_UN_WIDEN(op) /* the 256-bit form reads an xmm source */                               \
    G6_VEC("v_" op, kVector, kNone, op " xmm0, xmm1")                                              \
    G6_VEC("v_v" op "_128", kVector, kNone, "v" op " xmm0, xmm1")                                  \
    G6_VEC("v_v" op "_256", kVector, kNone, "v" op " ymm0, xmm1")
#define G6_V_UN_NARROW(op) /* the 256-bit form writes an xmm result */                             \
    G6_VEC("v_" op, kVector, kNone, op " xmm0, xmm1")                                              \
    G6_VEC("v_v" op "_128", kVector, kNone, "v" op " xmm0, xmm1")                                  \
    G6_VEC("v_v" op "_256", kVector, kNone, "v" op " xmm0, ymm1")
#define G6_V_UN_IMM(op, imm)                                                                       \
    G6_VEC("v_" op "_" #imm, kVector, kNone, op " xmm0, xmm1, " #imm)                              \
    G6_VEC("v_v" op "_" #imm "_128", kVector, kNone, "v" op " xmm0, xmm1, " #imm)                  \
    G6_VEC("v_v" op "_" #imm "_256", kVector, kNone, "v" op " ymm0, ymm1, " #imm)

// Binary forms with an immediate.
#define G6_V_IMM_128(op, imm)                                                                      \
    G6_VEC("v_" op "_" #imm, kVector, G6_VFLAGS, op " xmm0, xmm1, " #imm)                          \
    G6_VEC("v_v" op "_" #imm "_128", kVector, G6_VFLAGS, "v" op " xmm0, xmm2, xmm1, " #imm)
#define G6_V_IMM(op, imm)                                                                          \
    G6_V_IMM_128(op, imm)                                                                          \
    G6_VEC("v_v" op "_" #imm "_256", kVector, G6_VFLAGS, "v" op " ymm0, ymm2, ymm1, " #imm)

// Shifts by an immediate and by the count in the low quadword of b.
#define G6_V_SHIFT_IMM(op, imm)                                                                    \
    G6_VEC("v_" op "_" #imm, kVector, kNone, op " xmm0, " #imm)                                    \
    G6_VEC("v_v" op "_" #imm "_128", kVector, kNone, "v" op " xmm0, xmm1, " #imm)                  \
    G6_VEC("v_v" op "_" #imm "_256", kVector, kNone, "v" op " ymm0, ymm1, " #imm)
#define G6_V_SHIFT_XMM(op)                                                                         \
    G6_VEC("v_" op "_x", kVectorCount, kNone, op " xmm0, xmm1")                                    \
    G6_VEC("v_v" op "_x_128", kVectorCount, kNone, "v" op " xmm0, xmm2, xmm1")                     \
    G6_VEC("v_v" op "_x_256", kVectorCount, kNone, "v" op " ymm0, ymm2, xmm1")

// ---- Integer arithmetic and logic -----------------------------------------------------------

G6_RR("add", kArith)
G6_RR("adc", kArith)
G6_RR("sub", kArith)
G6_RR("sbb", kArith)
G6_RR("cmp", kArith)
G6_RR("and", kLogic)
G6_RR("or", kLogic)
G6_RR("xor", kLogic)
G6_RR("test", kLogic)
G6_RR("xchg", kKeep)
G6_RR("xadd", kArith)
G6_RR16("imul", kMul)
G6_RR16("bt", kCarry)
G6_RR16("bts", kCarry)
G6_RR16("btr", kCarry)
G6_RR16("btc", kCarry)
G6_RR16("bsf", kBitScan)
G6_RR16("bsr", kBitScan)
G6_RR16("tzcnt", kCount)
G6_RR16("lzcnt", kCount)
G6_RR16("popcnt", kArith)
G6_RR16("cmovb", kKeep)
G6_RR16("cmovnz", kKeep)

G6_INSN("i_add64_imm8", 64, kPair, kArith, 0, "add rcx, -3")
G6_INSN("i_add32_imm", 32, kPair, kArith, 0, "add ecx, 0x12345678")
G6_INSN("i_sub16_imm", 16, kPair, kArith, 0, "sub cx, 0x1234")
G6_INSN("i_and8_imm", 8, kPair, kLogic, 0, "and cl, 0x5a")
G6_INSN("i_or64_imm", 64, kPair, kLogic, 0, "or rcx, -0x80000000")
G6_INSN("i_xor64_imm", 64, kPair, kLogic, 0, "xor rcx, 0x7fffffff")
G6_INSN("i_cmp64_imm", 64, kPair, kArith, 0, "cmp rcx, -1")
G6_INSN("i_cmp8_imm", 8, kPair, kArith, 0, "cmp cl, 0x80")
G6_INSN("i_test32_imm", 32, kPair, kLogic, 0, "test ecx, 0x80000001")
G6_INSN("i_adc8_imm", 8, kPair, kArith, 0, "adc cl, 0x7f")
G6_INSN("i_sbb64_imm", 64, kPair, kArith, 0, "sbb rcx, 1")
G6_INSN("i_imul16_imm", 16, kPair, kMul, 0, "imul cx, dx, -0x1234")
G6_INSN("i_imul32_imm", 32, kPair, kMul, 0, "imul ecx, edx, 0x10001")
G6_INSN("i_imul64_imm", 64, kPair, kMul, 0, "imul rcx, rdx, -7")
G6_INSN("i_bt64_imm", 64, kPair, kCarry, 0, "bt rcx, 37")
G6_INSN("i_bts32_imm", 32, kPair, kCarry, 0, "bts ecx, 31")
G6_INSN("i_btr16_imm", 16, kPair, kCarry, 0, "btr cx, 3")
G6_INSN("i_btc64_imm", 64, kPair, kCarry, 0, "btc rcx, 63")
G6_INSN("i_add_high8", 8, kPair, kArith, 0, "add ch, dh")
G6_INSN("i_mov_to_high8", 8, kPair, kKeep, 0, "mov ch, dl")
G6_INSN("i_mov_from_high8", 8, kPair, kKeep, 0, "mov cl, dh")

G6_R("inc", kArith)
G6_R("dec", kArith)
G6_R("neg", kArith)
G6_R("not", kKeep)
G6_INSN("i_bswap32", 32, kPair, kKeep, 0, "bswap ecx")
G6_INSN("i_bswap64", 64, kPair, kKeep, 0, "bswap rcx")

// ---- Moves, extensions, and address arithmetic ------------------------------------------------

G6_INSN("i_movzx_8_16", 16, kPair, kKeep, 0, "movzx cx, dl")
G6_INSN("i_movzx_8_32", 32, kPair, kKeep, 0, "movzx ecx, dl")
G6_INSN("i_movzx_16_64", 64, kPair, kKeep, 0, "movzx rcx, dx")
G6_INSN("i_movsx_8_16", 16, kPair, kKeep, 0, "movsx cx, dl")
G6_INSN("i_movsx_8_64", 64, kPair, kKeep, 0, "movsx rcx, dl")
G6_INSN("i_movsx_16_32", 32, kPair, kKeep, 0, "movsx ecx, dx")
G6_INSN("i_movsxd", 64, kPair, kKeep, 0, "movsxd rcx, edx")
G6_INSN("i_mov32", 32, kPair, kKeep, 0, "mov ecx, edx")
G6_INSN("i_mov16", 16, kPair, kKeep, 0, "mov cx, dx")
G6_INSN("i_mov64_imm", 64, kPair, kKeep, 0, "movabs rcx, 0x123456789abcdef0")
G6_INSN("i_lea64", 64, kPair, kKeep, 0, "lea rcx, [rcx + 4*rdx + 0x7b]")
G6_INSN("i_lea32", 32, kPair, kKeep, 0, "lea ecx, [rcx + 8*rdx - 3]")
G6_INSN("i_lea16", 16, kPair, kKeep, 0, "lea cx, [rcx + rdx]")
G6_INSN("i_movbe32", 32, kPair, kKeep, 0, "mov qword ptr [r9], rdx\n    movbe ecx, dword ptr [r9]")
G6_INSN("i_movbe64_store", 64, kPair, kKeep, 0,
        "movbe qword ptr [r9], rdx\n    mov rcx, qword ptr [r9]")
G6_INSN("i_cbw", 16, kPair, kKeep, 0, "mov rax, rcx\n    cbw\n    mov rcx, rax")
G6_INSN("i_cwde", 32, kPair, kKeep, 0, "mov rax, rcx\n    cwde\n    mov rcx, rax")
G6_INSN("i_cdqe", 64, kPair, kKeep, 0, "mov rax, rcx\n    cdqe\n    mov rcx, rax")
G6_INSN("i_cwd", 16, kPair, kKeep, 0, "mov rax, rcx\n    cwd")
G6_INSN("i_cdq", 32, kPair, kKeep, 0, "mov rax, rcx\n    cdq")
G6_INSN("i_cqo", 64, kPair, kKeep, 0, "mov rax, rcx\n    cqo")

// ---- Shifts and rotates -----------------------------------------------------------------------

G6_SHIFT("shl", kShift)
G6_SHIFT("shr", kShift)
G6_SHIFT("sar", kShift)
G6_SHIFT("rol", kRotate)
G6_SHIFT("ror", kRotate)
G6_SHIFT_IMM("shl", 1, kShift)
G6_SHIFT_IMM("shr", 1, kShift)
G6_SHIFT_IMM("sar", 1, kShift)
G6_SHIFT_IMM("rol", 1, kRotate)
G6_SHIFT_IMM("ror", 1, kRotate)
G6_SHIFT_IMM("shl", 7, kShift)
G6_SHIFT_IMM("shr", 9, kShift)
G6_SHIFT_IMM("sar", 3, kShift)
G6_SHIFT_IMM("rol", 13, kRotate)
G6_SHIFT_IMM("ror", 5, kRotate)
G6_SHIFT_DOUBLE("shld")
G6_SHIFT_DOUBLE("shrd")
G6_INSN("i_shld32_imm", 32, kPair, kShiftDouble, 11, "shld ecx, edx, 11")
G6_INSN("i_shrd64_imm", 64, kPair, kShiftDouble, 40, "shrd rcx, rdx, 40")

// BMI1/BMI2 (three-operand forms write rcx from rcx, rdx, or c).
G6_INSN("i_andn32", 32, kPair, kBmi, 0, "andn ecx, ecx, edx")
G6_INSN("i_andn64", 64, kPair, kBmi, 0, "andn rcx, rcx, rdx")
G6_INSN("i_blsr32", 32, kPair, kBmi, 0, "blsr ecx, edx")
G6_INSN("i_blsr64", 64, kPair, kBmi, 0, "blsr rcx, rdx")
G6_INSN("i_blsi32", 32, kPair, kBmi, 0, "blsi ecx, edx")
G6_INSN("i_blsi64", 64, kPair, kBmi, 0, "blsi rcx, rdx")
G6_INSN("i_blsmsk32", 32, kPair, kBmi, 0, "blsmsk ecx, edx")
G6_INSN("i_blsmsk64", 64, kPair, kBmi, 0, "blsmsk rcx, rdx")
G6_INSN("i_bzhi32", 32, kPair, kBmi, 0, "bzhi ecx, ecx, edx")
G6_INSN("i_bzhi64", 64, kPair, kBmi, 0, "bzhi rcx, rcx, rdx")
G6_INSN("i_bextr32", 32, kPair, kBextr, 0, "bextr ecx, ecx, edx")
G6_INSN("i_bextr64", 64, kPair, kBextr, 0, "bextr rcx, rcx, rdx")
G6_INSN("i_shlx32", 32, kPair, kKeep, 0, "shlx ecx, ecx, edx")
G6_INSN("i_shrx64", 64, kPair, kKeep, 0, "shrx rcx, rcx, rdx")
G6_INSN("i_sarx32", 32, kPair, kKeep, 0, "sarx ecx, ecx, edx")
G6_INSN("i_sarx64", 64, kPair, kKeep, 0, "sarx rcx, rcx, rdx")
G6_INSN("i_rorx32", 32, kPair, kKeep, 0, "rorx ecx, edx, 7")
G6_INSN("i_rorx64", 64, kPair, kKeep, 0, "rorx rcx, rdx, 45")
G6_INSN("i_pdep32", 32, kPair, kKeep, 0, "pdep ecx, ecx, edx")
G6_INSN("i_pdep64", 64, kPair, kKeep, 0, "pdep rcx, rcx, rdx")
G6_INSN("i_pext32", 32, kPair, kKeep, 0, "pext ecx, ecx, edx")
G6_INSN("i_pext64", 64, kPair, kKeep, 0, "pext rcx, rcx, rdx")
G6_INSN("i_mulx32", 32, kPair, kKeep, 0, "mulx ecx, eax, ecx\n    mov edx, eax")
G6_INSN("i_mulx64", 64, kPair, kKeep, 0, "mulx rcx, rax, rcx\n    mov rdx, rax")
G6_INSN("i_mulx64_same", 64, kPair, kKeep, 0, "mulx rcx, rcx, rcx")

// ---- Multiply and divide ----------------------------------------------------------------------

G6_MULDIV("mul", kPair, kMul)
G6_MULDIV("imul", kPair, kMul)
G6_MULDIV("div", kDiv, kNone)
G6_MULDIV("idiv", kIdiv, kNone)

// ---- Memory operands, atomics -----------------------------------------------------------------

G6_MEM("add", "add", kArith)
G6_MEM("adc", "adc", kArith)
G6_MEM("sub", "sub", kArith)
G6_MEM("sbb", "sbb", kArith)
G6_MEM("and", "and", kLogic)
G6_MEM("or", "or", kLogic)
G6_MEM("xor", "xor", kLogic)
G6_MEM("cmp", "cmp", kArith)
G6_MEM("test", "test", kLogic)
G6_MEM("xchg", "xchg", kKeep)
G6_MEM("lock_add", "lock add", kArith)
G6_MEM("lock_adc", "lock adc", kArith)
G6_MEM("lock_sub", "lock sub", kArith)
G6_MEM("lock_and", "lock and", kLogic)
G6_MEM("lock_or", "lock or", kLogic)
G6_MEM("lock_xor", "lock xor", kLogic)
G6_MEM("lock_xadd", "lock xadd", kArith)
G6_MEM_SRC("add", kArith)
G6_MEM_SRC("sub", kArith)
G6_MEM_SRC("cmp", kArith)
G6_MEM_SRC("and", kLogic)
G6_MEM_SRC("xor", kLogic)
G6_MEM1("inc", "inc", kArith)
G6_MEM1("dec", "dec", kArith)
G6_MEM1("neg", "neg", kArith)
G6_MEM1("not", "not", kKeep)
G6_MEM1("lock_inc", "lock inc", kArith)
G6_MEM1("lock_dec", "lock dec", kArith)
G6_MEM1("lock_neg", "lock neg", kArith)
G6_MEM1("lock_not", "lock not", kKeep)
G6_CMPXCHG("cmpxchg", "cmpxchg")
G6_CMPXCHG("lock_cmpxchg", "lock cmpxchg")
G6_INSN("i_cmpxchg8_reg", 8, kCmpxchg, kArith, 0,
        "mov rax, r8\n    cmpxchg cl, dl\n    mov rdx, rax")
G6_INSN("i_cmpxchg64_reg", 64, kCmpxchg, kArith, 0,
        "mov rax, r8\n    cmpxchg rcx, rdx\n    mov rdx, rax")
G6_INSN("i_shl32_mem", 32, kPair, kShift, 5,
        "mov qword ptr [r9], rcx\n    shl dword ptr [r9], 5\n    mov rcx, qword ptr [r9]")
G6_INSN("i_sar16_mem", 16, kPair, kShift, 1,
        "mov qword ptr [r9], rcx\n    sar word ptr [r9], 1\n    mov rcx, qword ptr [r9]")
G6_INSN("i_rol64_mem", 64, kPair, kRotate, 17,
        "mov qword ptr [r9], rcx\n    rol qword ptr [r9], 17\n    mov rcx, qword ptr [r9]")
G6_INSN("i_setcc_mem", 8, kPair, kArith, 0,
        "cmp rcx, rdx\n    mov qword ptr [r9], rcx\n    setl byte ptr [r9]\n    mov rcx, qword ptr "
        "[r9]")
G6_INSN("i_push_pop", 64, kPair, kKeep, 0,
        "push rcx\n    push qword ptr [rsp]\n    pop rdx\n    pop qword ptr [r9 + 16]\n    mov "
        "rcx, qword ptr [r9 + 16]")
G6_BITMEM("bt", "bt")
G6_BITMEM("bts", "bts")
G6_BITMEM("btr", "btr")
G6_BITMEM("btc", "btc")
G6_BITMEM("lock_bts", "lock bts")
G6_BITMEM("lock_btr", "lock btr")
PROBE("i_bts32_bitmem_imm", 32, kBitMem, kCarry, 0, G6_BITMEM_BODY("bts dword ptr [r9], 13"))
PROBE("i_btc16_bitmem_imm", 16, kBitMem, kCarry, 0, G6_BITMEM_BODY("btc word ptr [r9], 15"))
PROBE("i_bt64_bitmem_imm", 64, kBitMem, kCarry, 0, G6_BITMEM_BODY("bt qword ptr [r9], 62"))

// CMPXCHG16B on the 16 bytes at out, which hold a:b. RDX:RAX (the expected value) is b:c, so
// the exchange succeeds whenever c equals a; RCX:RBX is a value derived from a and b. RDX:RAX
// afterwards goes to out[16..31]. Only ZF changes, so CF (set from c) must survive.
#define G6_CMPXCHG16B_BODY(text)                                                                   \
    "    push rbx\n    mov qword ptr [r9], rcx\n    mov qword ptr [r9 + 8], rdx\n"                 \
    "    mov rax, r8\n    mov rbx, rcx\n    not rbx\n    mov rcx, rdx\n    rol rcx, 7\n" G6_SET_CF \
    "    " text " xmmword ptr [r9]\n    mov qword ptr [r9 + 16], rax\n"                            \
    "    mov qword ptr [r9 + 24], rdx\n    pop rbx\n    pushfq\n    pop rax\n    ret\n"
PROBE("i_cmpxchg16b", 64, kCmpxchg, kCount, 0, G6_CMPXCHG16B_BODY("cmpxchg16b"))
PROBE("i_lock_cmpxchg16b", 64, kCmpxchg, kCount, 0, G6_CMPXCHG16B_BODY("lock cmpxchg16b"))
G6_INSN("i_clflush", 64, kPair, kKeep, 0, "clflush byte ptr [r9 + 8]")

// The x87 control word: save it to out[16], load one built from a's rounding and precision
// bits with every exception masked, read it back to out[20], clear the exception flags, and
// restore the original. None of these change RFLAGS.
PROBE("i_x87_control", 16, kPair, kKeep, 0,
      "    fnstcw word ptr [r9 + 16]\n    mov eax, ecx\n    and eax, 0x0f00\n    or eax, 0x007f\n"
      "    mov word ptr [r9 + 18], ax\n" G6_SET_CF "    fldcw word ptr [r9 + 18]\n"
      "    fnstcw word ptr [r9 + 20]\n    fnclex\n    fldcw word ptr [r9 + 16]\n" G6_STORE_RETURN)

// ---- Flags and conditions ---------------------------------------------------------------------

G6_CONDITIONS("c_setcc", "cmp rcx, rdx", G6_SETCC)
G6_CONDITIONS("c_setcc32", "cmp ecx, edx", G6_SETCC)
G6_CONDITIONS("c_cmov64", "cmp rcx, rdx", G6_CMOV64)
G6_CONDITIONS("c_cmov32", "cmp rcx, rdx", G6_CMOV32)
G6_CONDITIONS("c_cmov16", "cmp cx, dx", G6_CMOV16)
G6_CONDITIONS("c_jcc", "cmp rcx, rdx", G6_JCC)
G6_INSN("f_sahf", 64, kPair, kArith, 0, "xor eax, eax\n    mov ah, cl\n    sahf\n    mov rcx, rax")
G6_INSN("f_lahf", 64, kPair, kArith, 0, "add rcx, rdx\n    lahf\n    movzx ecx, ah")
G6_INSN("f_popfq", 64, kPair, kArith, 0, "and ecx, 0x8d5\n    push rcx\n    popfq")
G6_INSN("f_pushfq", 64, kPair, kArith, 0,
        "sub rcx, rdx\n    pushfq\n    pop rcx\n    and ecx, 0x8d5")
G6_INSN("f_cmc", 64, kPair, kKeep, 0, "cmc")
G6_INSN("f_stc", 64, kPair, kKeep, 0, "stc")
G6_INSN("f_clc", 64, kPair, kKeep, 0, "clc")

// ---- String operations ------------------------------------------------------------------------

G6_STRING("rep_movsb", 8, "rep movsb")
G6_STRING("rep_movsw", 16, "rep movsw")
G6_STRING("rep_movsd", 32, "rep movsd")
G6_STRING("rep_movsq", 64, "rep movsq")
G6_STRING("rep_stosb", 8, "rep stosb")
G6_STRING("rep_stosw", 16, "rep stosw")
G6_STRING("rep_stosd", 32, "rep stosd")
G6_STRING("rep_stosq", 64, "rep stosq")
G6_STRING("movsb", 8, "movsb")
G6_STRING("movsq", 64, "movsq")
G6_STRING("stosd", 32, "stosd")
G6_STRING_DOWN("rep_movsb_down", 8, "1", "rep movsb")
G6_STRING_DOWN("rep_movsq_down", 64, "8", "rep movsq")
G6_STRING_DOWN("rep_stosb_down", 8, "1", "rep stosb")
G6_STRING_DOWN("rep_stosd_down", 32, "4", "rep stosd")

// ---- Packed integer -----------------------------------------------------------------------

G6_V_ALL("paddb")
G6_V_ALL("paddw")
G6_V_ALL("paddd")
G6_V_ALL("paddq")
G6_V_ALL("psubb")
G6_V_ALL("psubw")
G6_V_ALL("psubd")
G6_V_ALL("psubq")
G6_V_ALL("paddsb")
G6_V_ALL("paddsw")
G6_V_ALL("paddusb")
G6_V_ALL("paddusw")
G6_V_ALL("psubsb")
G6_V_ALL("psubsw")
G6_V_ALL("psubusb")
G6_V_ALL("psubusw")
G6_V_ALL("pand")
G6_V_ALL("pandn")
G6_V_ALL("por")
G6_V_ALL("pxor")
G6_V_ALL("pcmpeqb")
G6_V_ALL("pcmpeqw")
G6_V_ALL("pcmpeqd")
G6_V_ALL("pcmpeqq")
G6_V_ALL("pcmpgtb")
G6_V_ALL("pcmpgtw")
G6_V_ALL("pcmpgtd")
G6_V_ALL("pcmpgtq")
G6_V_ALL("pmullw")
G6_V_ALL("pmulld")
G6_V_ALL("pmulhw")
G6_V_ALL("pmulhuw")
G6_V_ALL("pmulhrsw")
G6_V_ALL("pmuludq")
G6_V_ALL("pmuldq")
G6_V_ALL("pmaddwd")
G6_V_ALL("pmaddubsw")
G6_V_ALL("pavgb")
G6_V_ALL("pavgw")
G6_V_ALL("pmaxub")
G6_V_ALL("pmaxuw")
G6_V_ALL("pmaxud")
G6_V_ALL("pmaxsb")
G6_V_ALL("pmaxsw")
G6_V_ALL("pmaxsd")
G6_V_ALL("pminub")
G6_V_ALL("pminuw")
G6_V_ALL("pminud")
G6_V_ALL("pminsb")
G6_V_ALL("pminsw")
G6_V_ALL("pminsd")
G6_V_ALL("psadbw")
G6_V_ALL("pshufb")
G6_V_ALL("phaddw")
G6_V_ALL("phaddd")
G6_V_ALL("phsubw")
G6_V_ALL("phsubd")
G6_V_ALL("psignb")
G6_V_ALL("psignw")
G6_V_ALL("psignd")
G6_V_ALL("punpcklbw")
G6_V_ALL("punpcklwd")
G6_V_ALL("punpckldq")
G6_V_ALL("punpcklqdq")
G6_V_ALL("punpckhbw")
G6_V_ALL("punpckhwd")
G6_V_ALL("punpckhdq")
G6_V_ALL("punpckhqdq")
G6_V_ALL("packsswb")
G6_V_ALL("packssdw")
G6_V_ALL("packuswb")
G6_V_ALL("packusdw")
G6_V_UN("pabsb")
G6_V_UN("pabsw")
G6_V_UN("pabsd")
G6_VEC("v_phminposuw", kVector, kNone, "phminposuw xmm0, xmm1")
G6_VEC("v_vphminposuw", kVector, kNone, "vphminposuw xmm0, xmm1")
G6_V_UN_WIDEN("pmovzxbw")
G6_V_UN_WIDEN("pmovzxbd")
G6_V_UN_WIDEN("pmovzxbq")
G6_V_UN_WIDEN("pmovzxwd")
G6_V_UN_WIDEN("pmovzxwq")
G6_V_UN_WIDEN("pmovzxdq")
G6_V_UN_WIDEN("pmovsxbw")
G6_V_UN_WIDEN("pmovsxbd")
G6_V_UN_WIDEN("pmovsxbq")
G6_V_UN_WIDEN("pmovsxwd")
G6_V_UN_WIDEN("pmovsxwq")
G6_V_UN_WIDEN("pmovsxdq")
G6_V_UN_IMM("pshufd", 0x1b)
G6_V_UN_IMM("pshuflw", 0xb1)
G6_V_UN_IMM("pshufhw", 0x4e)
G6_V_IMM("palignr", 5)
G6_V_IMM("palignr", 19)
G6_V_IMM("pblendw", 0xa5)
G6_V_IMM("mpsadbw", 5)
G6_V_SHIFT_IMM("psllw", 3)
G6_V_SHIFT_IMM("pslld", 7)
G6_V_SHIFT_IMM("psllq", 33)
G6_V_SHIFT_IMM("psrlw", 15)
G6_V_SHIFT_IMM("psrlw", 16)
G6_V_SHIFT_IMM("psrld", 1)
G6_V_SHIFT_IMM("psrlq", 63)
G6_V_SHIFT_IMM("psraw", 4)
G6_V_SHIFT_IMM("psrad", 31)
G6_V_SHIFT_IMM("psrad", 40)
G6_V_SHIFT_IMM("pslldq", 3)
G6_V_SHIFT_IMM("psrldq", 9)
G6_V_SHIFT_XMM("psllw")
G6_V_SHIFT_XMM("pslld")
G6_V_SHIFT_XMM("psllq")
G6_V_SHIFT_XMM("psrlw")
G6_V_SHIFT_XMM("psrld")
G6_V_SHIFT_XMM("psrlq")
G6_V_SHIFT_XMM("psraw")
G6_V_SHIFT_XMM("psrad")

// ---- Packed and scalar floating point -------------------------------------------------------

// Commutative operations: additions, multiplications, dot products, and fused multiply-adds.
// When both inputs of a lane are NaN, x86 returns the first one, but LLVM may swap the operands
// of these operations, so the recompiled code can return the other NaN (see docs/codegen.md).
// These probes compare any two NaN results as equal, lane by lane.
#undef G6_VFLAGS
#define G6_VFLAGS kFloat32
G6_V_ALL("addps")
G6_V_ALL("mulps")
G6_V_ALL("addsubps")
G6_V_128("addss")
G6_V_128("mulss")
G6_V_IMM("dpps", 0xf1)
G6_V_IMM("dpps", 0x3c)
G6_V_AVX("fmadd132ps")
G6_V_AVX("fmadd213ps")
G6_V_AVX("fmadd231ps")
G6_V_AVX("fmsub231ps")
G6_V_AVX("fnmadd231ps")
G6_V_AVX("fnmsub213ps")
G6_V_AVX("fmaddsub231ps")
G6_V_VEX128("fmadd132ss")
G6_V_VEX128("fmadd231ss")
G6_V_VEX128("fnmadd213ss")
G6_VEC("v_vfmadd231ps_mem", kVector, G6_VFLAGS, "vfmadd231ps ymm0, ymm2, ymmword ptr [rdx + 32]")
G6_VEC("v_vfmadd213ss_mem", kVector, G6_VFLAGS, "vfmadd213ss xmm0, xmm2, dword ptr [rdx + 4]")
#undef G6_VFLAGS
#define G6_VFLAGS kFloat64
G6_V_ALL("addpd")
G6_V_ALL("mulpd")
G6_V_ALL("addsubpd")
G6_V_128("addsd")
G6_V_128("mulsd")
G6_V_IMM_128("dppd", 0x31)
G6_V_AVX("fmadd231pd")
G6_V_AVX("fmsub213pd")
G6_V_AVX("fnmadd132pd")
G6_V_AVX("fmsubadd213pd")
G6_V_VEX128("fmadd213sd")
G6_V_VEX128("fmsub231sd")
G6_V_VEX128("fnmsub132sd")
#undef G6_VFLAGS
#define G6_VFLAGS kNone

// Everything else must match bit for bit.
G6_V_ALL("subps")
G6_V_ALL("subpd")
G6_V_ALL("divps")
G6_V_ALL("divpd")
G6_V_ALL("minps")
G6_V_ALL("minpd")
G6_V_ALL("maxps")
G6_V_ALL("maxpd")
G6_V_ALL("andps")
G6_V_ALL("andpd")
G6_V_ALL("andnps")
G6_V_ALL("andnpd")
G6_V_ALL("orps")
G6_V_ALL("orpd")
G6_V_ALL("xorps")
G6_V_ALL("xorpd")
G6_V_ALL("unpcklps")
G6_V_ALL("unpckhps")
G6_V_ALL("unpcklpd")
G6_V_ALL("unpckhpd")
G6_V_ALL("haddps")
G6_V_ALL("haddpd")
G6_V_ALL("hsubps")
G6_V_ALL("hsubpd")
G6_V_128("movhlps")
G6_V_128("movlhps")
G6_V_128("subss")
G6_V_128("subsd")
G6_V_128("divss")
G6_V_128("divsd")
G6_V_128("minss")
G6_V_128("minsd")
G6_V_128("maxss")
G6_V_128("maxsd")
G6_V_128("sqrtss")
G6_V_128("sqrtsd")
G6_V_128("rcpss")
G6_V_128("rsqrtss")
G6_V_128("cvtss2sd")
G6_V_128("cvtsd2ss")
G6_V_128("movss")
G6_V_128("movsd")
G6_V_UN("sqrtps")
G6_V_UN("sqrtpd")
G6_V_UN("rcpps")
G6_V_UN("rsqrtps")
G6_V_UN("movsldup")
G6_V_UN("movshdup")
G6_V_UN("movddup")
G6_V_UN("cvtdq2ps")
G6_V_UN("cvtps2dq")
G6_V_UN("cvttps2dq")
G6_V_UN_WIDEN("cvtdq2pd")
G6_V_UN_WIDEN("cvtps2pd")
G6_V_UN_NARROW("cvtpd2ps")
G6_V_UN_NARROW("cvtpd2dq")
G6_V_UN_NARROW("cvttpd2dq")
G6_V_UN_IMM("roundps", 0)
G6_V_UN_IMM("roundps", 1)
G6_V_UN_IMM("roundps", 10)
G6_V_UN_IMM("roundpd", 3)
G6_V_UN_IMM("roundpd", 4)
G6_V_IMM("shufps", 0x9c)
G6_V_IMM("shufpd", 5)
G6_V_IMM("blendps", 5)
G6_V_IMM("blendpd", 2)
G6_V_IMM("cmpps", 0)
G6_V_IMM("cmpps", 1)
G6_V_IMM("cmpps", 3)
G6_V_IMM("cmpps", 4)
G6_V_IMM("cmppd", 2)
G6_V_IMM("cmppd", 7)
G6_V_IMM_128("cmpss", 1)
G6_V_IMM_128("cmpsd", 6)
G6_V_IMM_128("roundss", 1)
G6_V_IMM_128("roundsd", 10)
G6_V_IMM_128("insertps", 0x5c)
G6_VEC("v_vcmpps_29_256", kVector, kNone, "vcmpps ymm0, ymm2, ymm1, 29")
G6_VEC("v_vcmppd_16_128", kVector, kNone, "vcmppd xmm0, xmm2, xmm1, 16")

// ---- Blends, permutes, broadcasts, FMA --------------------------------------------------------

G6_VEC("v_blendvps", kVector, kNone, "blendvps xmm2, xmm1, xmm0\n    vmovdqa ymm0, ymm2")
G6_VEC("v_blendvpd", kVector, kNone, "blendvpd xmm2, xmm1, xmm0\n    vmovdqa ymm0, ymm2")
G6_VEC("v_pblendvb", kVector, kNone, "pblendvb xmm2, xmm1, xmm0\n    vmovdqa ymm0, ymm2")
G6_VEC("v_vblendvps_128", kVector, kNone, "vblendvps xmm0, xmm2, xmm1, xmm3")
G6_VEC("v_vblendvpd_256", kVector, kNone, "vblendvpd ymm0, ymm2, ymm1, ymm3")
G6_VEC("v_vpblendvb_256", kVector, kNone, "vpblendvb ymm0, ymm2, ymm1, ymm3")
G6_VEC("v_vpblendd_256", kVector, kNone, "vpblendd ymm0, ymm2, ymm1, 0x5a")
G6_VEC("v_vpblendd_128", kVector, kNone, "vpblendd xmm0, xmm2, xmm1, 0x6")
G6_VEC("v_vpermd", kVector, kNone, "vpermd ymm0, ymm1, ymm2")
G6_VEC("v_vpermps", kVector, kNone, "vpermps ymm0, ymm1, ymm2")
G6_VEC("v_vpermq", kVector, kNone, "vpermq ymm0, ymm1, 0x4e")
G6_VEC("v_vpermpd", kVector, kNone, "vpermpd ymm0, ymm1, 0x1b")
G6_VEC("v_vperm2i128_31", kVector, kNone, "vperm2i128 ymm0, ymm2, ymm1, 0x31")
G6_VEC("v_vperm2i128_88", kVector, kNone, "vperm2i128 ymm0, ymm2, ymm1, 0x88")
G6_VEC("v_vperm2f128_20", kVector, kNone, "vperm2f128 ymm0, ymm2, ymm1, 0x20")
G6_VEC("v_vpermilps_256", kVector, kNone, "vpermilps ymm0, ymm1, 0x1b")
G6_VEC("v_vpermilpd_128", kVector, kNone, "vpermilpd xmm0, xmm1, 0x1")
G6_VEC("v_vpbroadcastb_128", kVector, kNone, "vpbroadcastb xmm0, xmm1")
G6_VEC("v_vpbroadcastw_256", kVector, kNone, "vpbroadcastw ymm0, xmm1")
G6_VEC("v_vpbroadcastd_256", kVector, kNone, "vpbroadcastd ymm0, xmm1")
G6_VEC("v_vpbroadcastq_mem", kVector, kNone, "vpbroadcastq ymm0, qword ptr [rdx + 8]")
G6_VEC("v_vbroadcastss_256", kVector, kNone, "vbroadcastss ymm0, xmm1")
G6_VEC("v_vbroadcastss_mem", kVector, kNone, "vbroadcastss xmm0, dword ptr [rdx + 4]")
G6_VEC("v_vbroadcastsd_256", kVector, kNone, "vbroadcastsd ymm0, xmm1")
G6_VEC("v_vbroadcasti128", kVector, kNone, "vbroadcasti128 ymm0, xmmword ptr [rdx + 16]")
G6_VEC("v_vinserti128", kVector, kNone, "vinserti128 ymm0, ymm2, xmm1, 1")
G6_VEC("v_vinsertf128_mem", kVector, kNone, "vinsertf128 ymm0, ymm2, xmmword ptr [rdx + 32], 0")
G6_VEC("v_vextracti128", kVector, kNone, "vextracti128 xmm0, ymm1, 1")
G6_VEC("v_vextractf128_mem", kVector, kNone, "vextractf128 xmmword ptr [r8 + 32], ymm1, 1")
G6_V_AVX("psllvd")
G6_V_AVX("psllvq")
G6_V_AVX("psrlvd")
G6_V_AVX("psrlvq")
G6_V_AVX("psravd")

// ---- Moves, inserts, extracts, conversions to and from integer registers ---------------------

G6_VEC("v_movdqa", kVector, kNone, "movdqa xmm0, xmm1")
G6_VEC("v_movups_load", kVector, kNone, "movups xmm0, xmmword ptr [rdx + 4]")
G6_VEC("v_movaps_store", kVector, kNone, "movaps xmmword ptr [r8 + 32], xmm1")
G6_VEC("v_vmovdqu_128", kVector, kNone, "vmovdqu xmm0, xmm1")
G6_VEC("v_vmovups_store", kVector, kNone, "vmovups ymmword ptr [r8 + 32], ymm1")
G6_VEC("v_movntdq", kVector, kNone, "movntdq xmmword ptr [r8 + 32], xmm1\n    sfence")
G6_VEC("v_movss_load", kVector, kNone, "movss xmm0, dword ptr [rdx + 8]")
G6_VEC("v_movsd_load", kVector, kNone, "movsd xmm0, qword ptr [rdx + 8]")
G6_VEC("v_vmovss_load", kVector, kNone, "vmovss xmm0, dword ptr [rdx + 8]")
G6_VEC("v_movss_store", kVector, kNone, "movss dword ptr [r8 + 36], xmm1")
G6_VEC("v_vmovsd_store", kVector, kNone, "vmovsd qword ptr [r8 + 40], xmm1")
G6_VEC("v_movq_xmm", kVector, kNone, "movq xmm0, xmm1")
G6_VEC("v_vmovq_xmm", kVector, kNone, "vmovq xmm0, xmm1")
G6_VEC("v_movq_load", kVector, kNone, "movq xmm0, qword ptr [rdx + 24]")
G6_VEC("v_movd_load", kVector, kNone, "movd xmm0, dword ptr [rdx + 24]")
G6_VEC("v_movq_from_gpr", kVector, kNone, "mov r10, qword ptr [rdx]\n    movq xmm0, r10")
G6_VEC("v_movd_from_gpr", kVector, kNone, "mov r10, qword ptr [rdx]\n    movd xmm0, r10d")
G6_VEC("v_vmovd_from_gpr", kVector, kNone, "mov r10, qword ptr [rdx]\n    vmovd xmm0, r10d")
G6_VEC("v_movq_to_gpr", kVector, kNone, "movq rax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_movd_to_gpr", kVector, kNone,
       "mov rax, -1\n    movd eax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_movd_store", kVector, kNone, "movd dword ptr [r8 + 44], xmm1")
G6_VEC("v_movq_store", kVector, kNone, "movq qword ptr [r8 + 48], xmm1")
G6_VEC("v_movlps_load", kVector, kNone, "movlps xmm0, qword ptr [rdx + 8]")
G6_VEC("v_movhps_load", kVector, kNone, "movhps xmm0, qword ptr [rdx + 8]")
G6_VEC("v_movhpd_load", kVector, kNone, "movhpd xmm0, qword ptr [rdx + 16]")
G6_VEC("v_vmovhps_load", kVector, kNone, "vmovhps xmm0, xmm2, qword ptr [rdx + 8]")
G6_VEC("v_vmovlpd_load", kVector, kNone, "vmovlpd xmm0, xmm2, qword ptr [rdx + 8]")
G6_VEC("v_movhps_store", kVector, kNone, "movhps qword ptr [r8 + 32], xmm1")
G6_VEC("v_movlpd_store", kVector, kNone, "movlpd qword ptr [r8 + 40], xmm1")
G6_VEC("v_pinsrb", kVector, kNone, "mov r10, qword ptr [rdx]\n    pinsrb xmm0, r10d, 9")
G6_VEC("v_pinsrw", kVector, kNone, "mov r10, qword ptr [rdx]\n    pinsrw xmm0, r10d, 5")
G6_VEC("v_pinsrd", kVector, kNone, "pinsrd xmm0, dword ptr [rdx + 12], 2")
G6_VEC("v_pinsrq", kVector, kNone, "mov r10, qword ptr [rdx]\n    pinsrq xmm0, r10, 1")
G6_VEC("v_vpinsrq", kVector, kNone, "mov r10, qword ptr [rdx]\n    vpinsrq xmm0, xmm2, r10, 0")
G6_VEC("v_pextrb", kVector, kNone,
       "mov rax, -1\n    pextrb eax, xmm1, 13\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_pextrw", kVector, kNone,
       "mov rax, -1\n    pextrw eax, xmm1, 6\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_pextrw_mem", kVector, kNone, "pextrw word ptr [r8 + 40], xmm1, 3")
G6_VEC("v_pextrd", kVector, kNone,
       "mov rax, -1\n    pextrd eax, xmm1, 3\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_pextrq", kVector, kNone, "pextrq rax, xmm1, 1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_extractps", kVector, kNone,
       "mov rax, -1\n    extractps eax, xmm1, 2\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_extractps_mem", kVector, kNone, "extractps dword ptr [r8 + 36], xmm1, 1")
G6_VEC("v_insertps_mem", kVector, kNone, "insertps xmm0, dword ptr [rdx + 8], 0x9c")
G6_VEC("v_pmovmskb", kVector, kNone, "pmovmskb eax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_vpmovmskb_256", kVector, kNone, "vpmovmskb eax, ymm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_movmskps", kVector, kNone, "movmskps eax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_vmovmskpd_256", kVector, kNone, "vmovmskpd eax, ymm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_cvtsi2ss", kVector, kNone, "mov r10, qword ptr [rdx]\n    cvtsi2ss xmm0, r10d")
G6_VEC("v_cvtsi2sd64", kVector, kNone, "mov r10, qword ptr [rdx]\n    cvtsi2sd xmm0, r10")
G6_VEC("v_vcvtsi2sd", kVector, kNone, "mov r10, qword ptr [rdx]\n    vcvtsi2sd xmm0, xmm2, r10d")
G6_VEC("v_cvtsi2ss_mem", kVector, kNone, "cvtsi2ss xmm0, qword ptr [rdx + 8]")
G6_VEC("v_cvttss2si", kVector, kNone,
       "mov rax, -1\n    cvttss2si eax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_cvtss2si64", kVector, kNone, "cvtss2si rax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_cvttsd2si64", kVector, kNone, "cvttsd2si rax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_cvtsd2si", kVector, kNone,
       "mov rax, -1\n    cvtsd2si eax, xmm1\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_vcvttsd2si_mem", kVector, kNone,
       "vcvttsd2si rax, qword ptr [rdx + 8]\n    mov qword ptr [r8 + 32], rax")
G6_VEC("v_comiss", kVector, kArith, "comiss xmm0, xmm1")
G6_VEC("v_ucomiss", kVector, kArith, "ucomiss xmm0, xmm1")
G6_VEC("v_comisd", kVector, kArith, "comisd xmm0, xmm1")
G6_VEC("v_vucomisd", kVector, kArith, "vucomisd xmm0, xmm1")
G6_VEC("v_comiss_mem", kVector, kArith, "comiss xmm0, dword ptr [rdx + 4]")
G6_VEC("v_ptest", kVector, kArith, "ptest xmm0, xmm1")
G6_VEC("v_vptest_256", kVector, kArith, "vptest ymm0, ymm1")
G6_VEC("v_vptest_same", kVector, kArith, "vpand ymm1, ymm1, ymm3\n    vptest ymm1, ymm1")
G6_VEC("v_paddd_mem", kVector, kNone, "paddd xmm0, xmmword ptr [rdx + 16]")
G6_VEC("v_addss_mem", kVector, kNone, "addss xmm0, dword ptr [rdx + 12]")
G6_VEC("v_mulsd_mem", kVector, kNone, "mulsd xmm0, qword ptr [rdx + 8]")
G6_VEC("v_pmovzxbw_mem", kVector, kNone, "pmovzxbw xmm0, qword ptr [rdx + 8]")
G6_VEC("v_vpmovsxwd_mem", kVector, kNone, "vpmovsxwd ymm0, xmmword ptr [rdx + 16]")
G6_VEC("v_vpaddd_mem", kVector, kNone, "vpaddd ymm0, ymm2, ymmword ptr [rdx + 32]")
G6_VEC("v_vzeroupper", kVector, kNone, "vzeroupper")
// vzeroall also clears xmm6-xmm15, which the caller expects preserved.
G6_VEC("v_vzeroall", kVector, kNone,
       "sub rsp, 168\n    movdqu xmmword ptr [rsp], xmm6\n    movdqu xmmword ptr [rsp + 16], xmm7\n"
       "    movdqu xmmword ptr [rsp + 32], xmm8\n    movdqu xmmword ptr [rsp + 48], xmm9\n"
       "    movdqu xmmword ptr [rsp + 64], xmm10\n    movdqu xmmword ptr [rsp + 80], xmm11\n"
       "    movdqu xmmword ptr [rsp + 96], xmm12\n    movdqu xmmword ptr [rsp + 112], xmm13\n"
       "    movdqu xmmword ptr [rsp + 128], xmm14\n    movdqu xmmword ptr [rsp + 144], xmm15\n"
       "    vzeroall\n    vpor ymm0, ymm0, ymm6\n    vpor ymm0, ymm0, ymm15\n"
       "    movdqu xmm6, xmmword ptr [rsp]\n    movdqu xmm7, xmmword ptr [rsp + 16]\n"
       "    movdqu xmm8, xmmword ptr [rsp + 32]\n    movdqu xmm9, xmmword ptr [rsp + 48]\n"
       "    movdqu xmm10, xmmword ptr [rsp + 64]\n    movdqu xmm11, xmmword ptr [rsp + 80]\n"
       "    movdqu xmm12, xmmword ptr [rsp + 96]\n    movdqu xmm13, xmmword ptr [rsp + 112]\n"
       "    movdqu xmm14, xmmword ptr [rsp + 128]\n    movdqu xmm15, xmmword ptr [rsp + 144]\n"
       "    add rsp, 168")
// Round toward zero through MXCSR, then restore it. The exception flags (bits 0-5) are sticky
// and depend on everything that ran before, so they are cleared from the stored copies.
G6_VEC(
    "v_mxcsr_round", kVector, kNone,
    "stmxcsr dword ptr [r8 + 40]\n    mov eax, dword ptr [r8 + 40]\n    or eax, 0x6000\n"
    "    mov dword ptr [r8 + 44], eax\n    ldmxcsr dword ptr [r8 + 44]\n    cvtsd2si rax, xmm1\n"
    "    cvtps2dq xmm0, xmm0\n    ldmxcsr dword ptr [r8 + 40]\n    mov qword ptr [r8 + 32], rax\n"
    "    and dword ptr [r8 + 40], 0xffc0\n    and dword ptr [r8 + 44], 0xffc0")

// ---- SSE4.2 string comparisons ----------------------------------------------------------------
// Inputs are short strings (kVectorText). Implicit-length forms compare xmm0 with xmm1;
// explicit-length forms take their lengths from byte 48 of a (rax) and of b (rdx). An index
// result goes to out[32]; a mask result is xmm0. The immediates cover each data format,
// aggregation, polarity, and the index and mask selection bits.
#define G6_LENGTHS "movsx rax, byte ptr [rcx + 48]\n    movsx rdx, byte ptr [rdx + 48]\n    "
#define G6_PCMPSTRI(imm)                                                                           \
    G6_VEC("v_pcmpistri_" #imm, kVectorText, kArith,                                               \
           "pcmpistri xmm0, xmm1, " #imm "\n    mov qword ptr [r8 + 32], rcx")                     \
    G6_VEC("v_vpcmpistri_" #imm, kVectorText, kArith,                                              \
           "vpcmpistri xmm0, xmm1, " #imm "\n    mov qword ptr [r8 + 32], rcx")                    \
    G6_VEC("v_pcmpestri_" #imm, kVectorText, kArith,                                               \
           G6_LENGTHS "pcmpestri xmm0, xmm1, " #imm "\n    mov qword ptr [r8 + 32], rcx")          \
    G6_VEC("v_vpcmpestri_" #imm, kVectorText, kArith,                                              \
           G6_LENGTHS "vpcmpestri xmm0, xmm1, " #imm "\n    mov qword ptr [r8 + 32], rcx")
#define G6_PCMPSTRM(imm)                                                                           \
    G6_VEC("v_pcmpistrm_" #imm, kVectorText, kArith, "pcmpistrm xmm0, xmm1, " #imm)                \
    G6_VEC("v_vpcmpistrm_" #imm, kVectorText, kArith, "vpcmpistrm xmm0, xmm1, " #imm)              \
    G6_VEC("v_pcmpestrm_" #imm, kVectorText, kArith, G6_LENGTHS "pcmpestrm xmm0, xmm1, " #imm)     \
    G6_VEC("v_vpcmpestrm_" #imm, kVectorText, kArith, G6_LENGTHS "vpcmpestrm xmm0, xmm1, " #imm)
G6_PCMPSTRI(0x00) // unsigned bytes, equal any, least significant index
G6_PCMPSTRI(0x04) // ranges
G6_PCMPSTRI(0x08) // equal each
G6_PCMPSTRI(0x0c) // equal ordered (substring search)
G6_PCMPSTRI(0x0d) // unsigned words, equal ordered
G6_PCMPSTRI(0x1a) // signed bytes, equal each, negated
G6_PCMPSTRI(0x3a) // signed bytes, equal each, negated within the strings
G6_PCMPSTRI(0x44) // ranges, most significant index
G6_PCMPSTRI(0x71) // unsigned words, equal any, negated within the strings, most significant
G6_PCMPSTRM(0x00) // bit mask
G6_PCMPSTRM(0x08)
G6_PCMPSTRM(0x0c)
G6_PCMPSTRM(0x40) // byte mask
G6_PCMPSTRM(0x45) // unsigned words, ranges, word mask
G6_PCMPSTRM(0x7a) // signed bytes, equal each, negated within the strings, byte mask
G6_VEC("v_pcmpistri_mem", kVectorText, kArith,
       "pcmpistri xmm0, xmmword ptr [rdx + 1], 0x0c\n    mov qword ptr [r8 + 32], rcx")
G6_VEC("v_pcmpestrm_mem", kVectorText, kArith,
       "mov r9, rdx\n    " G6_LENGTHS "pcmpestrm xmm0, xmmword ptr [r9 + 3], 0x08")
// With a 64-bit operand size the lengths are RAX and RDX in full. Bit 0 of a's byte 49 sets
// bit 40 of RAX, which turns a short length into one that saturates. Raw encodings, because
// assemblers don't spell the REX.W and VEX.W1 forms.
#define G6_WIDE_LENGTHS                                                                            \
    G6_LENGTHS "movzx r10d, byte ptr [rcx + 49]\n    and r10d, 1\n    shl r10, 40\n"               \
               "    xor rax, r10\n    "
G6_VEC("v_pcmpestri_rexw", kVectorText, kArith,
       G6_WIDE_LENGTHS ".byte 0x66, 0x48, 0x0f, 0x3a, 0x61, 0xc1, 0x0c\n"
                       "    mov qword ptr [r8 + 32], rcx")
G6_VEC("v_vpcmpestrm_vexw", kVectorText, kArith,
       G6_WIDE_LENGTHS ".byte 0xc4, 0xe3, 0xf9, 0x60, 0xc1, 0x40")

// ---- Masked moves and half precision ------------------------------------------------------
// Masked loads read b's second half under the mask in ymm1 (b's first half); masked stores
// write ymm2 to out[32..63] under the same mask, so masked-off bytes keep out's initial values.
G6_VEC("v_vpmaskmovd_load_128", kVector, kNone, "vpmaskmovd xmm0, xmm1, xmmword ptr [rdx + 32]")
G6_VEC("v_vpmaskmovd_load_256", kVector, kNone, "vpmaskmovd ymm0, ymm1, ymmword ptr [rdx + 32]")
G6_VEC("v_vpmaskmovq_load_256", kVector, kNone, "vpmaskmovq ymm0, ymm1, ymmword ptr [rdx + 32]")
G6_VEC("v_vmaskmovps_load_128", kVector, kNone, "vmaskmovps xmm0, xmm1, xmmword ptr [rdx + 32]")
G6_VEC("v_vmaskmovps_load_256", kVector, kNone, "vmaskmovps ymm0, ymm1, ymmword ptr [rdx + 32]")
G6_VEC("v_vmaskmovpd_load_256", kVector, kNone, "vmaskmovpd ymm0, ymm1, ymmword ptr [rdx + 32]")
G6_VEC("v_vpmaskmovd_store_128", kVector, kNone, "vpmaskmovd xmmword ptr [r8 + 32], xmm1, xmm2")
G6_VEC("v_vpmaskmovd_store_256", kVector, kNone, "vpmaskmovd ymmword ptr [r8 + 32], ymm1, ymm2")
G6_VEC("v_vpmaskmovq_store_128", kVector, kNone, "vpmaskmovq xmmword ptr [r8 + 32], xmm1, xmm2")
G6_VEC("v_vmaskmovps_store_256", kVector, kNone, "vmaskmovps ymmword ptr [r8 + 32], ymm1, ymm2")
G6_VEC("v_vmaskmovpd_store_128", kVector, kNone, "vmaskmovpd xmmword ptr [r8 + 32], xmm1, xmm2")

// vcvtps2ph with each rounding mode: nearest, down, up, toward zero, and MXCSR's.
#define G6_CVTPS2PH(imm)                                                                           \
    G6_VEC("v_vcvtps2ph_" #imm "_128", kVector, kNone, "vcvtps2ph xmm0, xmm1, " #imm)              \
    G6_VEC("v_vcvtps2ph_" #imm "_256", kVector, kNone, "vcvtps2ph xmm0, ymm1, " #imm)
G6_CVTPS2PH(0)
G6_CVTPS2PH(1)
G6_CVTPS2PH(2)
G6_CVTPS2PH(3)
G6_CVTPS2PH(4)
G6_VEC("v_vcvtps2ph_mem", kVector, kNone, "vcvtps2ph xmmword ptr [r8 + 32], ymm1, 0")
G6_VEC("v_vcvtps2ph_mem64", kVector, kNone, "vcvtps2ph qword ptr [r8 + 40], xmm1, 3")
G6_VEC("v_vcvtph2ps_128", kVector, kNone, "vcvtph2ps xmm0, xmm1")
G6_VEC("v_vcvtph2ps_256", kVector, kNone, "vcvtph2ps ymm0, xmm1")
G6_VEC("v_vcvtph2ps_mem", kVector, kNone, "vcvtph2ps ymm0, xmmword ptr [rdx + 16]")
G6_VEC("v_vcvtph2ps_mem64", kVector, kNone, "vcvtph2ps xmm0, qword ptr [rdx + 8]")

#undef G6_SET_CF
#undef G6_STORE_RETURN
#undef G6_INSN
#undef G6_RR
#undef G6_RR16
#undef G6_R
#undef G6_SHIFT_BODY
#undef G6_SHIFT
#undef G6_SHIFT_IMM
#undef G6_SHIFT_DOUBLE_BODY
#undef G6_SHIFT_DOUBLE
#undef G6_MULDIV_BODY
#undef G6_MULDIV
#undef G6_MEM_BODY
#undef G6_MEM
#undef G6_MEM_SRC_BODY
#undef G6_MEM_SRC
#undef G6_MEM1_BODY
#undef G6_MEM1
#undef G6_CMPXCHG_BODY
#undef G6_CMPXCHG
#undef G6_BITMEM_BODY
#undef G6_BITMEM
#undef G6_CMPXCHG16B_BODY
#undef G6_LENGTHS
#undef G6_PCMPSTRI
#undef G6_PCMPSTRM
#undef G6_WIDE_LENGTHS
#undef G6_CVTPS2PH
#undef G6_EACH_CONDITION
#undef G6_SETCC
#undef G6_CMOV64
#undef G6_CMOV32
#undef G6_CMOV16
#undef G6_JCC
#undef G6_CONDITIONS
#undef G6_STRING_PROLOGUE
#undef G6_STRING_EPILOGUE
#undef G6_STRING
#undef G6_STRING_DOWN
#undef G6_VLOAD
#undef G6_VSTORE
#undef G6_VEC
#undef G6_VFLAGS
#undef G6_V_SSE
#undef G6_V_VEX128
#undef G6_V_VEX256
#undef G6_V_ALL
#undef G6_V_128
#undef G6_V_AVX
#undef G6_V_UN
#undef G6_V_UN_WIDEN
#undef G6_V_UN_NARROW
#undef G6_V_UN_IMM
#undef G6_V_IMM_128
#undef G6_V_IMM
#undef G6_V_SHIFT_IMM
#undef G6_V_SHIFT_XMM
