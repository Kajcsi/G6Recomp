#pragma once

// Turns one discovered guest function into a C++ function on g6::cpu::GuestContext. The shape of
// the output is described in docs/codegen.md: registers live in locals, status flags in a
// Flags local, branches become goto, and every instruction becomes a statement block with a
// comment naming it.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Zydis/Zydis.h>

#include "function_discovery.h"
#include "g6/pe/image.h"

namespace g6::recomp {

// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's map move constructor allocates.
struct EmitStats {
    size_t functions = 0;
    size_t complete_functions = 0; // every reachable instruction was translated
    size_t instructions = 0;
    size_t unimplemented = 0;  // instructions that became a call to cpu::unimplemented
    size_t decode_errors = 0;  // reachable bytes that don't decode
    size_t indirect_calls = 0; // dispatched at run time
    size_t indirect_jumps = 0; // dispatched at run time (switch tables excluded)
    size_t jump_tables = 0;
    std::map<std::string, size_t> missing; // mnemonic -> instructions not translated

    void merge(const EmitStats& other);
};

// What every function of one module shares.
struct ModuleInfo {
    ModuleInfo(const pe::Image& image, const DiscoveryResult& discovery, uint64_t base);

    const pe::Image& image;
    const DiscoveryResult& discovery;
    uint64_t base = 0;                              // assigned load address
    std::unordered_set<uint32_t> dir64_relocations; // RVAs of 64-bit relocated values
    ZydisDecoder decoder{};
    ZydisFormatter formatter{};

    uint64_t va(uint32_t rva) const { return base + rva; }
    uint64_t delta() const { return base - image.image_base(); }
    bool is_function(uint32_t rva) const { return discovery.find(rva) != nullptr; }
};

std::string function_name(uint64_t va);

// Thrown while translating an instruction the emitter has no implementation for; the
// instruction becomes a call to cpu::unimplemented instead.
struct Unsupported {};

class FunctionEmitter {
public:
    FunctionEmitter(const ModuleInfo& module, const DiscoveredFunction& function);

    // The function's definition. Adds to `stats`.
    std::string emit(EmitStats& stats);

private:
    struct Insn {
        uint32_t rva = 0;
        bool decoded = false;
        ZydisDecodedInstruction insn{};
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};

        uint32_t next() const { return rva + insn.length; }
    };

    // Analysis.
    bool in_function(uint32_t rva) const;
    bool is_internal_target(uint32_t rva) const;
    void collect();
    void analyze_registers();

    // Control flow.
    void emit_instruction(const Insn& insn);
    void emit_fallthrough(const Insn& insn, const Insn* following);
    void emit_call(const Insn& insn);
    void emit_jump(const Insn& insn);
    void emit_conditional_jump(const Insn& insn);
    void emit_return(const Insn& insn);
    void emit_trap(const Insn& insn, std::string_view what);
    void emit_branch_to(uint32_t target);
    void emit_tail_call(uint32_t target);
    void emit_operation(const Insn& insn);

    // Instruction groups (function_emitter.cpp and emit_vector.cpp).
    void emit_integer(const Insn& insn);
    void emit_string(const Insn& insn);
    void emit_vector(const Insn& insn);
    bool emit_vector_table(std::string_view name, bool vex);
    void emit_fma(std::string_view name);
    void emit_string_compare(const Insn& insn, std::string_view name);

    // Output.
    void line(std::string_view text);
    std::string store_registers() const;
    std::string load_registers() const;

    // Operands of the current instruction.
    const ZydisDecodedOperand& op(int index) const { return current_->ops[index]; }
    int visible() const { return current_->insn.operand_count_visible; }
    uint64_t pc() const { return module_.va(current_->rva); }
    bool locked() const { return (current_->insn.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0; }
    bool vex() const { return current_->insn.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX; }
    unsigned vector_bits() const;

    std::string address(const ZydisDecodedOperand& operand) const;
    std::string immediate(const ZydisDecodedOperand& operand, unsigned bits) const;
    std::string get(const ZydisDecodedOperand& operand) const;
    std::string get(const ZydisDecodedOperand& operand, unsigned bits) const;
    std::string put(const ZydisDecodedOperand& operand, const std::string& value) const;
    std::string read_gpr(ZydisRegister reg) const;
    std::string write_gpr(ZydisRegister reg, const std::string& value) const;

    std::string vread(const ZydisDecodedOperand& operand, unsigned bits) const;
    std::string vwrite(const ZydisDecodedOperand& operand, const std::string& value) const;
    std::string vreg(ZydisRegister reg) const;

    const ModuleInfo& module_;
    const DiscoveredFunction& function_;
    std::vector<Insn> insns_; // sorted by address
    std::unordered_map<uint32_t, size_t> index_;
    std::set<uint32_t> labels_;
    std::unordered_map<uint32_t, const JumpTable*> tables_;
    uint16_t gpr_used_ = 0;
    uint16_t gpr_written_ = 0;
    uint16_t vec_used_ = 0;
    uint16_t vec_written_ = 0;

    const Insn* current_ = nullptr;
    std::string body_;
    EmitStats stats_;
};

} // namespace g6::recomp
