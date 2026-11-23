// The pre-flight scan (docs/scan.md) against scan_guest.dll, an original fixture built by this
// project. The fixture holds instructions that fault in user mode or that this host may lack, so
// it is only read as a file, never loaded. Each check looks for a finding inside the exported
// function written to produce it; the static C runtime in the DLL adds findings of its own.

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "function_discovery.h"
#include "g6/pe/directories.h"
#include "g6/pe/image.h"
#include "g6test.h"
#include "scan.h"

namespace pe = g6::pe;
namespace recomp = g6::recomp;

namespace {

class Guest {
public:
    explicit Guest(const recomp::ScanOptions& options)
        : image(pe::Image::load_file(G6_SCAN_GUEST_PATH)),
          discovery(recomp::discover_functions(image)),
          report(recomp::scan_module(image, discovery, options)) {
        for (const pe::Export& entry : pe::read_exports(image).exports) {
            exports[entry.name] = entry.rva;
        }
    }
    Guest(const Guest&) = delete;
    Guest& operator=(const Guest&) = delete;
    Guest(Guest&&) = delete;
    Guest& operator=(Guest&&) = delete;
    ~Guest() = default;

    pe::Image image;
    recomp::DiscoveryResult discovery;
    recomp::ScanReport report;
    std::map<std::string, uint32_t> exports;

    const recomp::DiscoveredFunction& function(const std::string& name) const {
        const auto it = exports.find(name);
        if (it == exports.end()) {
            throw std::runtime_error("missing export " + name);
        }
        const recomp::DiscoveredFunction* found = discovery.find(it->second);
        if (found == nullptr) {
            throw std::runtime_error("export not discovered: " + name);
        }
        return *found;
    }
    uint32_t start(const std::string& name) const { return function(name).start; }
    bool in(uint32_t rva, const std::string& name) const { return function(name).contains(rva); }
    bool any_in(const std::vector<uint32_t>& sites, const std::string& name) const {
        return std::ranges::any_of(sites, [&](uint32_t rva) { return in(rva, name); });
    }
};

const Guest& guest() {
    static const Guest loaded{recomp::ScanOptions{}};
    return loaded;
}

const recomp::FeatureUse* feature(const recomp::ScanReport& report, ZydisISAExt extension) {
    const auto it = std::ranges::find(report.features, extension, &recomp::FeatureUse::extension);
    return it == report.features.end() ? nullptr : &*it;
}

const recomp::Sites* group(const std::vector<recomp::InstructionUse>& uses, const char* name) {
    const auto it = std::ranges::find(uses, std::string(name), &recomp::InstructionUse::name);
    return it == uses.end() ? nullptr : &it->sites;
}

const recomp::SegmentAccess* segment(const recomp::ScanReport& report, bool fs, bool via_self,
                                     bool computed, int64_t offset) {
    const auto it = std::ranges::find_if(report.segments, [&](const recomp::SegmentAccess& a) {
        return a.fs == fs && a.via_self == via_self && a.computed == computed && a.offset == offset;
    });
    return it == report.segments.end() ? nullptr : &*it;
}

bool has_note(const std::vector<std::string>& notes, const std::string& text) {
    return std::ranges::any_of(
        notes, [&](const std::string& note) { return note.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("scan reads the header facts") {
    const recomp::ScanReport& r = guest().report;
    CHECK(r.dll);
    CHECK(r.relocations.present);
    CHECK(!r.relocations.stripped);
    CHECK(r.relocations.dir64 > 0);
    CHECK_EQ(r.relocations.other, size_t{0});
    REQUIRE(r.tls.has_value());
    CHECK(r.tls->index_rva.has_value());
    CHECK(r.tls->template_size >= 4);
    REQUIRE(r.cfg.has_value());
    CHECK(r.cfg->security_cookie);
    CHECK(!r.imports.empty());
    CHECK_EQ(r.coverage.decode_errors, size_t{0});
    CHECK_EQ(r.coverage.directory_errors, size_t{0});
}

TEST_CASE("scan sorts CPU features by who has them") {
    const Guest& g = guest();
    const auto check = [&](ZydisISAExt extension, recomp::FeatureClass expected,
                           const char* function) {
        INFO(fmt::format("{} in {}", ZydisISAExtGetString(extension), function));
        const recomp::FeatureUse* use = feature(g.report, extension);
        REQUIRE(use != nullptr);
        CHECK(use->feature_class == expected);
        CHECK(g.any_in(use->sites.walked, function));
    };
    check(ZYDIS_ISA_EXT_ADOX_ADCX, recomp::FeatureClass::kHostCheck, "scan_adx");
    check(ZYDIS_ISA_EXT_SHA, recomp::FeatureClass::kHostCheck, "scan_sha");
    check(ZYDIS_ISA_EXT_SSE4A, recomp::FeatureClass::kHostCheck, "scan_sse4a");
    check(ZYDIS_ISA_EXT_CLWB, recomp::FeatureClass::kHostCheck, "scan_clwb");
    check(ZYDIS_ISA_EXT_AVX512EVEX, recomp::FeatureClass::kNotZen2, "scan_avx512");

    // Baseline extensions are counted without sites; endbr64 isn't counted at all.
    const recomp::FeatureUse* base = feature(g.report, ZYDIS_ISA_EXT_BASE);
    REQUIRE(base != nullptr);
    CHECK(base->feature_class == recomp::FeatureClass::kBaseline);
    CHECK(base->count > 0);
    CHECK(base->sites.empty());
    CHECK(feature(g.report, ZYDIS_ISA_EXT_CET) == nullptr);

    CHECK(!recomp::host_supports(ZYDIS_ISA_EXT_AVX2).has_value());
    CHECK(!recomp::host_supports(ZYDIS_ISA_EXT_AVX512EVEX).has_value());
    CHECK(recomp::host_supports(ZYDIS_ISA_EXT_SHA).has_value());
}

TEST_CASE("scan finds system instructions, traps, and timing") {
    const Guest& g = guest();
    const recomp::Sites* syscall = group(g.report.system, "syscall");
    REQUIRE(syscall != nullptr);
    CHECK(g.any_in(syscall->walked, "scan_syscall"));
    const recomp::Sites* int2e = group(g.report.system, "int 0x2e");
    REQUIRE(int2e != nullptr);
    CHECK(g.any_in(int2e->walked, "scan_int2e"));

    const recomp::Sites* assertion = group(g.report.traps, "int 0x2c (assertion)");
    REQUIRE(assertion != nullptr);
    CHECK(g.any_in(assertion->walked, "scan_traps"));
    const recomp::Sites* ud2 = group(g.report.traps, "ud2");
    REQUIRE(ud2 != nullptr);
    CHECK(g.any_in(ud2->walked, "scan_traps"));
    // Traps aren't system instructions.
    CHECK(group(g.report.system, "int 0x2c") == nullptr);

    const recomp::Sites* rdtsc = group(g.report.timing, "rdtsc");
    REQUIRE(rdtsc != nullptr);
    CHECK(g.any_in(rdtsc->walked, "scan_timing"));

    CHECK(g.any_in(g.report.cpuid.walked, "scan_cpu"));
    CHECK(g.any_in(g.report.xgetbv.walked, "scan_cpu"));
}

TEST_CASE("scan groups fs: and gs: accesses by TEB offset") {
    const Guest& g = guest();
    const auto check = [&](const recomp::SegmentAccess* access, const char* function) {
        INFO(function);
        REQUIRE(access != nullptr);
        CHECK(access->reads > 0);
        CHECK(g.any_in(access->sites.walked, function));
    };
    check(segment(g.report, true, false, false, 0x30), "scan_fs");
    check(segment(g.report, false, false, false, 0x60), "scan_peb");
    check(segment(g.report, false, false, false, 0x48), "scan_thread_id");
    check(segment(g.report, false, false, false, 0x30), "scan_last_error");
    check(segment(g.report, false, true, false, 0x68), "scan_last_error");
    check(segment(g.report, false, false, true, 0x1480), "scan_computed_slot");

    CHECK_EQ(recomp::teb_field_name(0x48), std::string("ClientId.UniqueThread"));
    CHECK_EQ(recomp::teb_field_name(0x1488), std::string("TlsSlots[1]"));
    CHECK(recomp::teb_field_name(0x2000).empty());
    CHECK(recomp::expected_teb_offset(0x58));
    CHECK(!recomp::expected_teb_offset(0x60));
}

TEST_CASE("scan tells how each TLS access picks its slot") {
    const Guest& g = guest();
    const recomp::TlsSlotUse& slots = g.report.tls_slots;
    CHECK(g.any_in(slots.indexed.walked, "scan_tls_bump"));
    CHECK(g.any_in(slots.fixed_slot.walked, "scan_fixed_slot"));
    CHECK(g.any_in(slots.other_index.walked, "scan_other_index"));
    CHECK(g.any_in(slots.not_followed.walked, "scan_lost_array"));
    CHECK(slots.index_reads >= 1);
}

TEST_CASE("scan finds code read and written as data") {
    const Guest& g = guest();
    const auto find = [&](const std::vector<recomp::CodeReference>& list, const char* function,
                          uint32_t target) {
        return std::ranges::any_of(list, [&](const recomp::CodeReference& reference) {
            return g.in(reference.site, function) && reference.target == target;
        });
    };
    // Through the register the lea loads, and through a RIP-relative operand.
    CHECK(find(g.report.code_reads, "scan_checksum", g.start("scan_adx")));
    CHECK(find(g.report.code_reads, "scan_checksum", g.start("scan_sha")));
    CHECK(find(g.report.code_writes, "scan_patch", g.start("scan_tiny")));
    CHECK(!find(g.report.code_writes, "scan_checksum", g.start("scan_adx")));
}

TEST_CASE("scan walks again from addresses taken inside functions") {
    const Guest& g = guest();
    // The lea in scan_continuation takes the address after its indirect jump.
    const auto taken = std::ranges::find_if(g.report.code_addresses, [&](const auto& reference) {
        return !reference.pointer && g.in(reference.site, "scan_continuation");
    });
    REQUIRE(taken != g.report.code_addresses.end());
    CHECK(g.in(taken->target, "scan_continuation"));
    CHECK(taken->target != g.start("scan_continuation"));

    // A relocated pointer in .data points into scan_pointed.
    const auto pointer = std::ranges::find_if(g.report.code_addresses, [&](const auto& reference) {
        return reference.pointer && g.in(reference.target, "scan_pointed");
    });
    REQUIRE(pointer != g.report.code_addresses.end());

    // The code behind both is walked, not just swept.
    const recomp::FeatureUse* rdpid = feature(g.report, ZYDIS_ISA_EXT_RDPID);
    REQUIRE(rdpid != nullptr);
    CHECK(g.any_in(rdpid->sites.walked, "scan_continuation"));
    const recomp::FeatureUse* clzero = feature(g.report, ZYDIS_ISA_EXT_CLZERO);
    REQUIRE(clzero != nullptr);
    CHECK(g.any_in(clzero->sites.walked, "scan_pointed"));
}

TEST_CASE("scan sweeps .pdata code the walk didn't reach") {
    const Guest& g = guest();
    const recomp::Sites* syscall = group(g.report.system, "syscall");
    REQUIRE(syscall != nullptr);
    CHECK(g.any_in(syscall->swept, "scan_sweep"));
    const recomp::Sites* hlt = group(g.report.system, "hlt");
    REQUIRE(hlt != nullptr);
    CHECK(hlt->walked.empty());
    CHECK(g.any_in(hlt->swept, "scan_sweep"));
    CHECK(g.report.coverage.swept_instructions > 0);
    CHECK(g.report.coverage.unreached_bytes > 0);

    const Guest unswept{{.sweep = false}};
    const recomp::Sites* walked_only = group(unswept.report.system, "syscall");
    REQUIRE(walked_only != nullptr);
    CHECK(walked_only->swept.empty());
    CHECK_EQ(unswept.report.coverage.swept_instructions, size_t{0});
    CHECK(unswept.report.coverage.unreached_bytes >= g.report.coverage.unreached_bytes);
}

TEST_CASE("scan review notes name each finding") {
    const std::vector<std::string> notes = recomp::review_notes(guest().report);
    for (const char* text :
         {"fs: has no TEB", "reads the PEB", "ClientId.UniqueThread", "LastErrorValue", "TlsSlots",
          "fixed slot", "never read the module's TLS index", "weren't followed", "syscall",
          "int 0x2e", "SHA", "AVX512EVEX", "loads from code", "stores to code",
          "the walk didn't reach"}) {
        INFO(text);
        CHECK(has_note(notes, text));
    }
    // A finding only the sweep made says so; one the walk also made doesn't.
    const auto note_for = [&](const std::string& start) {
        const auto it = std::ranges::find_if(
            notes, [&](const std::string& note) { return note.starts_with(start); });
        return it == notes.end() ? std::string() : *it;
    };
    CHECK(note_for("hlt ").find("only the sweep") != std::string::npos);
    CHECK(note_for("syscall ").find("only the sweep") == std::string::npos);
    // The runtime's own reads of StackBase and StackLimit are expected, so they aren't notes.
    CHECK(!has_note(notes, "StackBase"));
    CHECK(!has_note(notes, "StackLimit"));
    // Taken addresses are for the hook engine, not a finding.
    CHECK(!has_note(notes, "addresses inside"));
    // Relocations are present.
    CHECK(!has_note(notes, "no base relocations"));
}
