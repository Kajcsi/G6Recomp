#pragma once

// Pre-flight scan of a module (docs/scan.md): the facts that decide whether its code can run
// natively on a desktop host, and what has to be patched or answered by HLE first.
//
// It walks the code function discovery accepted, then sweeps whatever .pdata says is code that
// the walk didn't reach, and reports by site:
//
//  * instructions whose CPU feature a supported host may lack, or that the console's CPU lacks;
//  * every fs: and gs: access, by TEB offset, and how each thread-local access picks its slot;
//  * system instructions, traps, timing instructions, and cpuid and xgetbv;
//  * code that reads or writes code, or takes the address of code that isn't a function start,
//    which is how self-verification and self-modification would show.
//
// It adds the relocation, TLS, Control Flow Guard, and import facts from the headers, and the
// C++ exception handler mix the hook engine depends on. The report is derived from the scanned
// file, so for game files it stays on the user's machine.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <Zydis/SharedTypes.h>

#include "cxx_exceptions.h"
#include "function_discovery.h"
#include "g6/pe/image.h"

namespace g6::recomp {

// Where a finding's instructions were decoded. Each list is sorted.
struct Sites {
    std::vector<uint32_t> walked; // by the code walk from function entries
    std::vector<uint32_t> swept;  // by the linear sweep of .pdata code the walk didn't reach

    size_t size() const { return walked.size() + swept.size(); }
    bool empty() const { return walked.empty() && swept.empty(); }
};

// Which hosts can run an instruction set extension.
enum class FeatureClass : uint8_t {
    kBaseline,  // every supported host: x86-64 up to AVX2, with FMA, BMI, AES, and the like
    kHostCheck, // Zen 2 has it, but a supported host may not: SHA, ADX, CLWB, RDPID, SSE4a, ...
    kNotZen2,   // the console's CPU lacks it, so the code must sit behind a feature check
};

const char* describe(FeatureClass feature_class);

struct FeatureUse {
    ZydisISAExt extension = ZYDIS_ISA_EXT_INVALID;
    std::string name; // Zydis's name for the extension
    FeatureClass feature_class = FeatureClass::kBaseline;
    size_t count = 0; // instructions, walked and swept
    Sites sites;      // empty for baseline extensions, which are everywhere
};

// Memory accesses through fs: or gs:, grouped by how the address is formed.
struct SegmentAccess {
    bool fs = false;       // fs: rather than gs:
    bool via_self = false; // through the pointer loaded from gs:[0x30] (TEB Self), not gs: itself
    bool computed = false; // a base or index register adds to the offset
    int64_t offset = 0;
    size_t reads = 0;
    size_t writes = 0;
    Sites sites;
};

// How code picks the thread's TLS block after loading the TLS array from gs:[0x58].
struct TlsSlotUse {
    Sites indexed;      // indexed by a register, in a function that reads the module's TLS index
    Sites other_index;  // indexed by a register, in a function that never reads the TLS index
    Sites fixed_slot;   // a constant slot: the code assumes its module's TLS index
    Sites not_followed; // the array pointer wasn't dereferenced in the next few instructions
    size_t index_reads = 0; // instructions that read the module's TLS index variable
};

// Instructions grouped by name, such as "syscall", "int 0x2e", or "rdtsc".
struct InstructionUse {
    std::string name;
    Sites sites;
};

// An instruction or pointer that treats code as data.
struct CodeReference {
    uint32_t site = 0;    // the instruction, or where a relocated pointer is stored
    uint32_t target = 0;  // the code address
    bool swept = false;   // found by the sweep rather than the walk
    bool pointer = false; // a relocated pointer in the image, not an instruction
};

struct RelocationFacts {
    bool present = false;  // the base relocation directory has entries
    bool stripped = false; // the file header says relocations were stripped
    size_t dir64 = 0;
    size_t other = 0; // anything but 64-bit: absolute 32-bit addresses and the like
};

struct TlsFacts {
    uint32_t template_size = 0; // initialized data copied to each thread
    uint32_t zero_fill = 0;
    uint32_t alignment = 0; // from the characteristics; 0 when unspecified
    std::optional<uint32_t> index_rva;
    size_t callbacks = 0;
};

struct CfgFacts {
    uint32_t guard_flags = 0;
    size_t functions = 0;
    size_t eh_continuations = 0;
    bool check_pointer = false;
    bool dispatch_pointer = false;
    bool security_cookie = false;
};

struct ImportFacts {
    std::string dll;
    bool delay_load = false;
    size_t symbols = 0;
    size_t by_ordinal = 0;
};

struct CoverageFacts {
    size_t functions = 0;
    size_t instructions = 0; // decoded by the walk
    size_t swept_instructions = 0;
    uint64_t pdata_bytes = 0;     // bytes .pdata says are code
    uint64_t unreached_bytes = 0; // of those, bytes the walk didn't decode, padding excluded
    size_t unresolved_jumps = 0;
    size_t decode_errors = 0;
    std::vector<uint32_t> decode_error_sites; // sorted, without duplicates
    size_t directory_errors = 0;
};

// The C++ exception facts (docs/hooks.md, "C++ exception functions"): which handlers the module
// uses, and in how many functions the hook engine can check a mid-function hook by itself.
struct CxxFacts {
    CxxExceptionStats stats;
    bool guard_table = false;
    size_t guard = 0; // continuations by source
    size_t recorded = 0;
    size_t returned = 0;
    std::vector<CxxTableError> errors;
};

struct ScanReport {
    // Headers.
    bool dll = false;
    uint64_t image_base = 0;
    uint32_t size_of_image = 0;
    uint16_t characteristics = 0;
    uint16_t dll_characteristics = 0;
    RelocationFacts relocations;
    std::optional<TlsFacts> tls;
    std::optional<CfgFacts> cfg;
    std::vector<ImportFacts> imports;
    CxxFacts cxx;

    // Code.
    CoverageFacts coverage;
    std::vector<FeatureUse> features;    // by class, then name
    std::vector<SegmentAccess> segments; // by segment, via_self, computed, then offset
    TlsSlotUse tls_slots;
    std::vector<InstructionUse> system; // privileged, OS-level, and I/O instructions
    std::vector<InstructionUse> traps;  // int3, ud2, and the fail-fast and assertion interrupts
    std::vector<InstructionUse> timing; // rdtsc, rdtscp, rdpru
    Sites cpuid;
    Sites xgetbv;
    std::vector<CodeReference> code_reads;     // loads from code
    std::vector<CodeReference> code_writes;    // stores to code
    std::vector<CodeReference> code_addresses; // addresses inside code that aren't function starts
};

struct ScanOptions {
    bool sweep = true; // also decode the .pdata code the walk didn't reach
};

ScanReport scan_module(const pe::Image& image, const DiscoveryResult& discovery,
                       const ScanOptions& options = {});
ScanReport scan_module(const pe::Image& image, const ScanOptions& options = {});

// The first `limit` sites, walked ones first: "0x1000, 0x1040, 0x2000 (swept), +5 more".
std::string describe_sites(const Sites& sites, size_t limit = 3);

// How an access forms its address: "gs:[0x58]", "gs:[reg+0x1480]", "[gs:[0x30]+0x48]".
std::string describe(const SegmentAccess& access);

// The desktop x64 TEB field at a gs: offset, or an empty string.
std::string teb_field_name(int64_t offset);

// Whether a gs: offset is one of the TEB fields the side DLLs use (StackBase, StackLimit, Self,
// and ThreadLocalStoragePointer), which sit at the same offsets on desktop Windows.
bool expected_teb_offset(int64_t offset);

// Whether the CPU running this process has a host-check extension; nullopt for other classes.
std::optional<bool> host_supports(ZydisISAExt extension);

// One line for each finding that needs a decision before the module runs natively.
std::vector<std::string> review_notes(const ScanReport& report);

} // namespace g6::recomp
