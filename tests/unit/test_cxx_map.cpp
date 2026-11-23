// The C++ exception map on a synthetic image with hand-assembled code and tables (written for this
// test). Eight functions have handlers: an import thunk to __CxxFrameHandler4 (A), a GS wrapper
// that calls __CxxFrameHandler3 through the import table (B), a handler known only by its data's
// FH3 magic number (C), __CxxFrameHandler4 with tables that don't read (D), a handler that is
// none of those (E), a handler that only checks the stack cookie (I), and two that come close but
// aren't one (J returns 0, L makes a second call). A's catch records where it resumes, B's catch
// funclet returns it with a `lea` (and has a block nothing reaches that returns another), and C's
// catch funclet says nothing. The same image is checked again with a CFG EH continuation table.

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "cxx_exceptions.h"
#include "function_discovery.h"
#include "g6test.h"
#include "pe_builder.h"

using g6::pe::Directory;
using g6::recomp::CxxExceptionMap;
using g6::recomp::CxxFunction;
using g6::test::PeBuilder;

namespace {

constexpr uint32_t kA = 0x1000;  // FH4 through an import thunk; its catch resumes at kA + 0x30
constexpr uint32_t kB = 0x1040;  // FH3 through a GS wrapper; its catch resumes at kB + 0x30
constexpr uint32_t kC = 0x1080;  // FH3 by the magic number; its catch's continuation is unknown
constexpr uint32_t kD = 0x10C0;  // FH4, but its tables don't read
constexpr uint32_t kE = 0x10E0;  // some other handler
constexpr uint32_t kFa = 0x1100; // A's catch funclet
// B's catch funclet: lea rax,[kB + 0x30]; ret, then a block nothing reaches that resumes at
// kB + 0x38 instead.
constexpr uint32_t kFb = 0x1120;
constexpr uint32_t kFc = 0x1140; // C's catch funclet: ret
constexpr uint32_t kG = 0x1200;  // the GS wrapper: sub rsp,28h; call [FH3 slot]; add rsp,28h; ret
constexpr uint32_t kH = 0x1220;  // the other handler: ret
constexpr uint32_t kT4 = 0x1240; // jmp [FH4 slot]
// Cookie checks: sub rsp,28h; call kK; mov eax,1; add rsp,28h; ret
constexpr uint32_t kS = 0x1260;
constexpr uint32_t kS0 = 0x1280; // the same, but returns 0: xor eax,eax instead of mov eax,1
constexpr uint32_t kS2 = 0x12A0; // the same, with a second call, to kH, after the first
constexpr uint32_t kK = 0x12C0;  // cmp rcx,[cookie]; ret (a leaf, so no .pdata)
constexpr uint32_t kI = 0x12E0;  // ret, under kS
constexpr uint32_t kJ = 0x12F0;  // ret, under kS0
constexpr uint32_t kL = 0x1300;  // ret, under kS2
constexpr uint32_t kEnd = 0x1310;

constexpr uint32_t kRdata = 0x2000;
constexpr uint32_t kSlot4 = kRdata + 0x860; // the import address slots
constexpr uint32_t kSlot3 = kRdata + 0x868;
constexpr uint32_t kGsData = kRdata + 0x600; // the cookie checks' handler data: a cookie offset
constexpr uint32_t kCookie = kRdata + 0xC00;

struct Function {
    uint32_t begin;
    uint32_t end;
    uint32_t handler; // 0: none
    uint32_t handler_data;
};

PeBuilder synthetic_image(bool guard_table) {
    PeBuilder b;
    b.add_section(".text", PeBuilder::kCode, kEnd - 0x1000, 0xCC);
    b.add_section(".rdata", PeBuilder::kRdata, 0x1000);

    // The five functions with handlers are NOPs up to a ret.
    for (uint32_t rva = kA; rva < kFa; ++rva) {
        b.put<uint8_t>(rva, 0x90);
    }
    for (const uint32_t ret : {kB - 1, kC - 1, kD - 1, kE - 1, kFa - 1}) {
        b.put<uint8_t>(ret, 0xC3);
    }
    b.put_bytes(kFa, {0xC3});
    b.put_bytes(kFb, {0x48, 0x8D, 0x05});
    b.put_rel32(kFb + 3, kB + 0x30);
    b.put_bytes(kFb + 7, {0xC3, 0x48, 0x8D, 0x05});
    b.put_rel32(kFb + 11, kB + 0x38);
    b.put_bytes(kFb + 15, {0xC3});
    b.put_bytes(kFc, {0xC3});
    b.put_bytes(kG, {0x48, 0x83, 0xEC, 0x28, 0xFF, 0x15});
    b.put_rel32(kG + 6, kSlot3);
    b.put_bytes(kG + 10, {0x48, 0x83, 0xC4, 0x28, 0xC3});
    b.put_bytes(kH, {0xC3});
    b.put_bytes(kT4, {0xFF, 0x25});
    b.put_rel32(kT4 + 2, kSlot4);

    const auto cookie_check = [&](uint32_t at, std::initializer_list<uint8_t> after_call) {
        b.put_bytes(at, {0x48, 0x83, 0xEC, 0x28, 0xE8});
        b.put_rel32(at + 5, kK);
        b.put_bytes(at + 9, after_call);
    };
    cookie_check(kS, {0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3});
    cookie_check(kS0, {0x31, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3});
    cookie_check(kS2,
                 {0xE8, 0, 0, 0, 0, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3});
    b.put_rel32(kS2 + 10, kH);
    b.put_bytes(kK, {0x48, 0x3B, 0x0D});
    b.put_rel32(kK + 3, kCookie);
    b.put_bytes(kK + 7, {0xC3});
    for (const uint32_t ret : {kI, kJ, kL}) {
        b.put_bytes(ret, {0xC3});
    }
    b.put<uint32_t>(kGsData, 0x20);

    // A's FH4 tables: two states, one try block in state 0 whose catch resumes at kA + 0x30.
    constexpr uint32_t kTablesA = kRdata + 0x200;
    b.put_bytes(kTablesA, {0x18});
    b.put<uint32_t>(kTablesA + 1, kTablesA + 0x20);
    b.put<uint32_t>(kTablesA + 5, kTablesA + 0x30);
    b.put<uint32_t>(kTablesA + 9, kTablesA + 0x50);
    b.put_bytes(kTablesA + 0x20, {0x04, 0x00, 0x00});
    b.put_bytes(kTablesA + 0x30, {0x02, 0x00, 0x00, 0x02});
    b.put<uint32_t>(kTablesA + 0x34, kTablesA + 0x40);
    b.put_bytes(kTablesA + 0x40, {0x02, 0x10});
    b.put<uint32_t>(kTablesA + 0x42, kFa);
    b.put_bytes(kTablesA + 0x46, {0x60});
    b.put_bytes(kTablesA + 0x50, {0x04, 0x10, 0x02, 0x20, 0x00});

    // B's and C's FH3 tables: two states, one try block in state 0 with one catch.
    const auto fh3 = [&](uint32_t tables, uint32_t start, uint32_t funclet) {
        b.put<uint32_t>(tables, 0x19930522);
        b.put<int32_t>(tables + 4, 2);
        b.put<uint32_t>(tables + 8, tables + 0x40);
        b.put<uint32_t>(tables + 12, 1);
        b.put<uint32_t>(tables + 16, tables + 0x60);
        b.put<uint32_t>(tables + 20, 3);
        b.put<uint32_t>(tables + 24, tables + 0xA0);
        b.put<int32_t>(tables + 0x40, -1);
        b.put<int32_t>(tables + 0x60 + 8, 1);
        b.put<uint32_t>(tables + 0x60 + 12, 1);
        b.put<uint32_t>(tables + 0x60 + 16, tables + 0x80);
        b.put<uint32_t>(tables + 0x80 + 12, funclet);
        const std::vector<std::pair<uint32_t, int32_t>> map = {
            {start, -1}, {start + 8, 0}, {start + 0x18, -1}};
        for (size_t i = 0; i < map.size(); ++i) {
            b.put<uint32_t>(tables + 0xA0 + static_cast<uint32_t>(i) * 8, map[i].first);
            b.put<int32_t>(tables + 0xA4 + static_cast<uint32_t>(i) * 8, map[i].second);
        }
    };
    constexpr uint32_t kTablesB = kRdata + 0x300;
    constexpr uint32_t kTablesC = kRdata + 0x400;
    fh3(kTablesB, kB, kFb);
    fh3(kTablesC, kC, kFc);

    // D's FH4 tables: a catch with the reserved continuation count.
    constexpr uint32_t kTablesD = kRdata + 0x500;
    b.put_bytes(kTablesD, {0x10});
    b.put<uint32_t>(kTablesD + 1, kTablesD + 0x10);
    b.put<uint32_t>(kTablesD + 5, kTablesD + 0x30);
    b.put_bytes(kTablesD + 0x10, {0x02, 0x00, 0x00, 0x02});
    b.put<uint32_t>(kTablesD + 0x14, kTablesD + 0x20);
    b.put_bytes(kTablesD + 0x20, {0x02, 0x30});
    b.put<uint32_t>(kTablesD + 0x22, kFa);

    // Unwind info and .pdata, sorted by start. Every unwind record is empty but for the handler.
    const std::vector<Function> functions = {
        {kA, kB, kT4, kTablesA},   {kB, kC, kG, kTablesB},    {kC, kD, kH, kTablesC},
        {kD, kE, kT4, kTablesD},   {kE, kFa, kH, 1},          {kFa, kFa + 1, 0, 0},
        {kFb, kFb + 16, 0, 0},     {kFc, kFc + 1, 0, 0},      {kG, kG + 15, 0, 0},
        {kH, kH + 1, 0, 0},        {kS, kS + 19, 0, 0},       {kS0, kS0 + 16, 0, 0},
        {kS2, kS2 + 24, 0, 0},     {kI, kI + 1, kS, kGsData}, {kJ, kJ + 1, kS0, kGsData},
        {kL, kL + 1, kS2, kGsData}};
    const uint32_t pdata = kRdata + 0x100;
    for (size_t i = 0; i < functions.size(); ++i) {
        const Function& f = functions[i];
        const uint32_t unwind = kRdata + static_cast<uint32_t>(i) * 0x10;
        b.put_bytes(unwind, {static_cast<uint8_t>(f.handler != 0 ? 0x09 : 0x01), 0, 0, 0});
        if (f.handler != 0) {
            b.put<uint32_t>(unwind + 4, f.handler);
            b.put<uint32_t>(unwind + 8, f.handler_data);
        }
        const uint32_t entry = pdata + static_cast<uint32_t>(i) * 12;
        b.put<uint32_t>(entry, f.begin);
        b.put<uint32_t>(entry + 4, f.end);
        b.put<uint32_t>(entry + 8, unwind);
    }
    b.set_directory(Directory::Exception, pdata, static_cast<uint32_t>(functions.size()) * 12);

    // Imports: the two handlers from one module, by name.
    constexpr uint32_t kImports = kRdata + 0x800;
    b.put<uint32_t>(kImports, kRdata + 0x840);
    b.put<uint32_t>(kImports + 12, kRdata + 0x880);
    b.put<uint32_t>(kImports + 16, kSlot4);
    for (const uint32_t table : {kRdata + 0x840, kSlot4}) {
        b.put<uint64_t>(table, kRdata + 0x8A0);
        b.put<uint64_t>(table + 8, kRdata + 0x8C0);
    }
    b.put_string(kRdata + 0x880, "vcruntime140_1.dll");
    b.put_string(kRdata + 0x8A2, "__CxxFrameHandler4");
    b.put_string(kRdata + 0x8C2, "__CxxFrameHandler3");
    b.set_directory(Directory::Import, kImports, 40);

    // A load config with the security cookie's address. With the guard table, its EH
    // continuation table lists A's continuation and one in C, but not B's.
    constexpr uint32_t kConfig = kRdata + 0xA00;
    constexpr uint32_t kTable = kRdata + 0xB80;
    b.put<uint32_t>(kConfig, 0x118);
    b.put<uint64_t>(kConfig + 0x58, PeBuilder::kImageBase + kCookie);
    if (guard_table) {
        b.put<uint32_t>(kConfig + 0x90, 0x00400000);
        b.put<uint64_t>(kConfig + 0x108, PeBuilder::kImageBase + kTable);
        b.put<uint64_t>(kConfig + 0x110, 2);
        b.put<uint32_t>(kTable, kA + 0x30);
        b.put<uint32_t>(kTable + 4, kC + 0x20);
    }
    b.set_directory(Directory::LoadConfig, kConfig, 0x118);
    return b;
}

const CxxFunction& require_cxx(const CxxExceptionMap& map, uint32_t start) {
    const CxxFunction* function = map.find(start);
    if (function == nullptr) {
        g6::test::report_failure(__FILE__, __LINE__,
                                 fmt::format("no C++ function at {:#x}", start));
        throw g6::test::RequireFailed{};
    }
    return *function;
}

} // namespace

TEST_CASE("cxx map: handlers by import, GS wrapper, and magic number, and where catches resume") {
    const g6::pe::Image image = synthetic_image(false).image();
    const auto discovery = g6::recomp::discover_functions(image);
    const CxxExceptionMap map = g6::recomp::map_cxx_exceptions(image, discovery);

    CHECK_EQ(map.stats.handlers, size_t{8});
    CHECK_EQ(map.stats.fh4, size_t{1});
    CHECK_EQ(map.stats.fh3, size_t{2});
    CHECK_EQ(map.stats.by_magic, size_t{1});
    CHECK_EQ(map.stats.gs_wrapped, size_t{1});
    CHECK_EQ(map.stats.unreadable, size_t{1});
    // E's handler, and the two that aren't cookie checks.
    CHECK_EQ(map.stats.other_handlers, size_t{3});
    CHECK_EQ(map.stats.gs_only, size_t{1});
    CHECK_EQ(map.stats.gs_handlers, size_t{1});
    CHECK(map.gs_only == std::vector<uint32_t>{kI});
    CHECK(map.is_gs_only(kI));
    CHECK(!map.is_gs_only(kJ));
    CHECK(!map.is_gs_only(kL));
    CHECK_EQ(map.stats.with_catches, size_t{3});
    CHECK_EQ(map.stats.unverifiable, size_t{1});
    CHECK_EQ(map.stats.funclets, size_t{0});
    CHECK_EQ(map.stats.stray_continuations, size_t{0});
    CHECK(!map.guard_table);

    REQUIRE(map.functions.size() == 3);
    const CxxFunction& a = require_cxx(map, kA);
    CHECK(a.info.format == g6::pe::CxxEhFormat::kFh4);
    CHECK(!a.gs_wrapper);
    CHECK(a.verifiable);
    CHECK(a.continuations == std::vector<uint32_t>{kA + 0x30});
    const CxxFunction& b = require_cxx(map, kB);
    CHECK(b.info.format == g6::pe::CxxEhFormat::kFh3);
    CHECK(b.gs_wrapper);
    CHECK(b.verifiable);
    CHECK(b.continuations == (std::vector<uint32_t>{kB + 0x30, kB + 0x38}));
    const CxxFunction& c = require_cxx(map, kC);
    CHECK(!c.gs_wrapper);
    CHECK(!c.verifiable);
    CHECK(c.continuations.empty());

    CHECK(map.recorded == std::vector<uint32_t>{kA + 0x30});
    CHECK(map.returned == (std::vector<uint32_t>{kB + 0x30, kB + 0x38}));
    CHECK(map.continuations == (std::vector<uint32_t>{kA + 0x30, kB + 0x30, kB + 0x38}));
    REQUIRE(map.errors.size() == 1);
    CHECK_EQ(map.errors[0].start, kD);
    CHECK(map.errors[0].message.find("reserved continuation count") != std::string::npos);
}

TEST_CASE("cxx map: a CFG EH continuation table accounts for every catch") {
    const g6::pe::Image image = synthetic_image(true).image();
    const auto discovery = g6::recomp::discover_functions(image);
    const CxxExceptionMap map = g6::recomp::map_cxx_exceptions(image, discovery);

    CHECK(map.guard_table);
    CHECK(map.guard == (std::vector<uint32_t>{kA + 0x30, kC + 0x20}));
    CHECK_EQ(map.stats.with_catches, size_t{3});
    CHECK_EQ(map.stats.unverifiable, size_t{0});
    // B's funclet returns two addresses the table doesn't list.
    CHECK_EQ(map.stats.stray_continuations, size_t{2});
    CHECK(require_cxx(map, kC).verifiable);
    CHECK(require_cxx(map, kC).continuations == std::vector<uint32_t>{kC + 0x20});
    CHECK(map.continuations == (std::vector<uint32_t>{kA + 0x30, kB + 0x30, kB + 0x38, kC + 0x20}));
}
