// x64 unwind data decoding on synthetic UNWIND_INFO records, written from the public format
// description: every unwind op, version 2 epilog descriptors, handlers, chaining, indirect
// .pdata entries, and malformed records.

#include <vector>

#include "g6/pe/unwind.h"
#include "g6test.h"
#include "pe_builder.h"

using g6::pe::Directory;
using g6::pe::FormatError;
using g6::pe::Image;
using g6::pe::RuntimeFunction;
using g6::pe::UnwindOp;
using g6::test::PeBuilder;

namespace {

constexpr uint8_t kRbx = 3;
constexpr uint8_t kRbp = 5;
constexpr uint8_t kRsi = 6;
constexpr uint8_t kRdi = 7;

uint16_t code(uint8_t prolog_offset, UnwindOp op, uint8_t info) {
    return static_cast<uint16_t>(prolog_offset | (static_cast<uint8_t>(op) << 8) | (info << 12));
}

struct Fixture {
    PeBuilder builder;
    uint32_t text = 0;
    uint32_t xdata = 0; // unwind records go here
    uint32_t pdata = 0;

    Fixture() {
        text = builder.add_section(".text", PeBuilder::kCode, 0x400, 0xCC);
        xdata = builder.add_section(".xdata", PeBuilder::kRdata, 0x400);
        pdata = builder.add_section(".pdata", PeBuilder::kRdata, 0x100);
    }

    // Writes an UNWIND_INFO record at `rva` and returns the RVA just past its code array.
    uint32_t unwind(uint32_t rva, uint8_t version, uint8_t flags, uint8_t prolog_size,
                    uint8_t frame, const std::vector<uint16_t>& codes) {
        builder.put<uint8_t>(rva, static_cast<uint8_t>(version | (flags << 3)));
        builder.put<uint8_t>(rva + 1, prolog_size);
        builder.put<uint8_t>(rva + 2, static_cast<uint8_t>(codes.size()));
        builder.put<uint8_t>(rva + 3, frame);
        for (size_t i = 0; i < codes.size(); ++i) {
            builder.put<uint16_t>(rva + 4 + static_cast<uint32_t>(i) * 2, codes[i]);
        }
        return rva + 4 + static_cast<uint32_t>((codes.size() + 1) & ~size_t{1}) * 2;
    }

    void runtime_function(uint32_t index, uint32_t begin, uint32_t end, uint32_t unwind_info) {
        const uint32_t at = pdata + index * 12;
        builder.put<uint32_t>(at, begin);
        builder.put<uint32_t>(at + 4, end);
        builder.put<uint32_t>(at + 8, unwind_info);
    }

    void set_pdata_count(uint32_t count) {
        builder.set_directory(Directory::Exception, pdata, count * 12);
    }
};

} // namespace

TEST_CASE("unwind: version 1 decodes every op with its operand") {
    Fixture f;
    const uint32_t tail =
        f.unwind(f.xdata, 1, g6::pe::kUnwindFlagExceptionHandler, 0x30, (2 << 4) | kRbp,
                 {
                     code(0x30, UnwindOp::PushMachineFrame, 1),
                     code(0x2C, UnwindOp::SaveXmm128Far, 7),
                     0x0040,
                     0x0000,
                     code(0x28, UnwindOp::SaveXmm128, 6),
                     0x0002,
                     code(0x24, UnwindOp::SaveNonVolatileFar, kRdi),
                     0x1234,
                     0x0001,
                     code(0x20, UnwindOp::SaveNonVolatile, kRsi),
                     0x0010,
                     code(0x1C, UnwindOp::SetFramePointer, 0),
                     code(0x18, UnwindOp::AllocLarge, 1),
                     0x0000,
                     0x0001,
                     code(0x10, UnwindOp::AllocLarge, 0),
                     0x0020,
                     code(0x08, UnwindOp::AllocSmall, 3),
                     code(0x02, UnwindOp::PushNonVolatile, kRbx),
                 });
    f.builder.put<uint32_t>(tail, f.text + 0x200); // handler
    f.builder.put<uint32_t>(tail + 4, 0xDEADBEEF); // handler data

    const auto info = g6::pe::read_unwind_info(f.builder.image(), f.xdata);
    CHECK_EQ(info.version, uint8_t{1});
    CHECK_EQ(info.prolog_size, uint8_t{0x30});
    CHECK_EQ(info.frame_register, kRbp);
    CHECK_EQ(info.frame_offset, 32u);
    REQUIRE(info.codes.size() == 10);
    CHECK(info.codes[0].op == UnwindOp::PushMachineFrame);
    CHECK_EQ(info.codes[0].info, uint8_t{1});
    CHECK(info.codes[1].op == UnwindOp::SaveXmm128Far);
    CHECK_EQ(info.codes[1].value, 0x40u);
    CHECK(info.codes[2].op == UnwindOp::SaveXmm128);
    CHECK_EQ(info.codes[2].value, 32u); // scaled by 16
    CHECK(info.codes[3].op == UnwindOp::SaveNonVolatileFar);
    CHECK_EQ(info.codes[3].info, kRdi);
    CHECK_EQ(info.codes[3].value, 0x11234u);
    CHECK(info.codes[4].op == UnwindOp::SaveNonVolatile);
    CHECK_EQ(info.codes[4].value, 0x80u); // scaled by 8
    CHECK(info.codes[5].op == UnwindOp::SetFramePointer);
    CHECK_EQ(info.codes[6].value, 0x10000u);
    CHECK_EQ(info.codes[7].value, 0x100u); // scaled by 8
    CHECK_EQ(info.codes[8].value, 32u);    // info * 8 + 8
    CHECK(info.codes[9].op == UnwindOp::PushNonVolatile);
    CHECK_EQ(info.codes[9].info, kRbx);
    CHECK_EQ(info.codes[9].prolog_offset, uint8_t{0x02});
    CHECK(info.has_handler());
    REQUIRE(info.handler_rva.has_value());
    CHECK_EQ(*info.handler_rva, f.text + 0x200);
    CHECK_EQ(info.handler_data_rva, tail + 4);
    CHECK(!info.chained.has_value());
}

TEST_CASE("unwind: version 2 epilog descriptors precede the prolog codes") {
    Fixture f;
    f.unwind(f.xdata, 2, 0, 4, 0,
             {
                 static_cast<uint16_t>(0x05 | (6 << 8) | (1 << 12)), // size 5, epilog at end
                 static_cast<uint16_t>(0x23 | (6 << 8) | (1 << 12)), // epilog at end - 0x123
                 static_cast<uint16_t>(0x00 | (6 << 8)),             // padding
                 code(0x04, UnwindOp::AllocSmall, 4),
             });
    const auto info = g6::pe::read_unwind_info(f.builder.image(), f.xdata);
    CHECK_EQ(info.version, uint8_t{2});
    CHECK_EQ(info.epilog_size, uint8_t{5});
    CHECK(info.epilog_at_end);
    REQUIRE(info.epilog_offsets_from_end.size() == 2);
    CHECK_EQ(info.epilog_offsets_from_end[0], 5u);
    CHECK_EQ(info.epilog_offsets_from_end[1], 0x123u);
    REQUIRE(info.codes.size() == 1);
    CHECK_EQ(info.codes[0].value, 40u);
}

TEST_CASE("unwind: chained records lead to the primary function") {
    Fixture f;
    const uint32_t primary_info = f.xdata;
    f.unwind(primary_info, 1, 0, 4, 0, {code(4, UnwindOp::AllocSmall, 4)});
    // A cold fragment whose unwind info chains to the primary's RUNTIME_FUNCTION.
    const uint32_t cold_info = f.xdata + 0x40;
    const uint32_t tail = f.unwind(cold_info, 1, g6::pe::kUnwindFlagChainInfo, 0, 0, {});
    f.builder.put<uint32_t>(tail, f.text);
    f.builder.put<uint32_t>(tail + 4, f.text + 0x20);
    f.builder.put<uint32_t>(tail + 8, primary_info);
    f.runtime_function(0, f.text, f.text + 0x20, primary_info);
    f.runtime_function(1, f.text + 0x300, f.text + 0x340, cold_info);
    f.set_pdata_count(2);

    const Image image = f.builder.image();
    const auto entries = g6::pe::read_runtime_functions(image);
    REQUIRE(entries.size() == 2);
    const auto cold = g6::pe::read_unwind_info(image, cold_info);
    REQUIRE(cold.chained.has_value());
    CHECK_EQ(cold.chained->begin, f.text);
    const RuntimeFunction primary = g6::pe::primary_function(image, entries[1]);
    CHECK_EQ(primary.begin, f.text);
    CHECK_EQ(primary.unwind_info, primary_info);
    CHECK_EQ(g6::pe::primary_function(image, entries[0]).begin, f.text);

    const RuntimeFunction* found = g6::pe::find_runtime_function(entries, f.text + 0x310);
    REQUIRE(found != nullptr);
    CHECK_EQ(found->begin, f.text + 0x300);
    CHECK(g6::pe::find_runtime_function(entries, f.text + 0x100) == nullptr);
    CHECK(g6::pe::find_runtime_function(entries, f.text - 1) == nullptr);
}

TEST_CASE("unwind: indirect .pdata entries resolve to the entry they name") {
    Fixture f;
    f.unwind(f.xdata, 1, 0, 4, 0, {code(4, UnwindOp::AllocSmall, 4)});
    f.runtime_function(0, f.text, f.text + 0x20, f.xdata);
    // Entry 1 covers other code but borrows entry 0's unwind data indirectly.
    f.runtime_function(1, f.text + 0x40, f.text + 0x60, (f.pdata + 0) | 1u);
    f.set_pdata_count(2);

    const Image image = f.builder.image();
    const auto entries = g6::pe::read_runtime_functions(image);
    REQUIRE(entries.size() == 2);
    CHECK(entries[1].indirect());
    CHECK_EQ(g6::pe::resolve_indirect(image, entries[1]).begin, f.text);
    CHECK_EQ(g6::pe::primary_function(image, entries[1]).begin, f.text);
    CHECK_THROWS_AS(g6::pe::read_unwind_info(image, entries[1].unwind_info), FormatError);
}

TEST_CASE("unwind: cyclic chains are rejected") {
    Fixture f;
    const uint32_t a = f.xdata;
    const uint32_t b = f.xdata + 0x40;
    const uint32_t a_tail = f.unwind(a, 1, g6::pe::kUnwindFlagChainInfo, 0, 0, {});
    const uint32_t b_tail = f.unwind(b, 1, g6::pe::kUnwindFlagChainInfo, 0, 0, {});
    f.builder.put<uint32_t>(a_tail, f.text + 0x40);
    f.builder.put<uint32_t>(a_tail + 4, f.text + 0x60);
    f.builder.put<uint32_t>(a_tail + 8, b);
    f.builder.put<uint32_t>(b_tail, f.text);
    f.builder.put<uint32_t>(b_tail + 4, f.text + 0x20);
    f.builder.put<uint32_t>(b_tail + 8, a);
    const Image image = f.builder.image();
    CHECK_THROWS_AS(g6::pe::primary_function(image, {f.text, f.text + 0x20, a}), FormatError);

    // Indirect entries pointing at each other.
    f.runtime_function(0, f.text, f.text + 0x20, (f.pdata + 12) | 1u);
    f.runtime_function(1, f.text + 0x40, f.text + 0x60, (f.pdata + 0) | 1u);
    f.set_pdata_count(2);
    const Image looped = f.builder.image();
    const auto entries = g6::pe::read_runtime_functions(looped);
    REQUIRE(entries.size() == 2);
    CHECK_THROWS_AS(g6::pe::resolve_indirect(looped, entries[0]), FormatError);
}

TEST_CASE("unwind: malformed records are rejected") {
    Fixture f;
    const auto rejects = [&](uint8_t version, const std::vector<uint16_t>& codes) {
        f.unwind(f.xdata, version, 0, 0, 0, codes);
        const Image image = f.builder.image();
        bool threw = false;
        try {
            (void)g6::pe::read_unwind_info(image, f.xdata);
        } catch (const FormatError&) {
            threw = true;
        }
        return threw;
    };
    CHECK(rejects(3, {}));                                            // unknown version
    CHECK(rejects(1, {code(0, UnwindOp::AllocLarge, 0)}));            // missing operand slot
    CHECK(rejects(1, {code(0, UnwindOp::SaveNonVolatileFar, 0), 1})); // one slot short
    CHECK(rejects(1, {code(0, UnwindOp::AllocLarge, 2), 0, 0}));      // bad AllocLarge info
    CHECK(rejects(1, {static_cast<uint16_t>(11 << 8)}));              // unknown op
    CHECK(rejects(1, {code(0, UnwindOp::PushMachineFrame, 2)}));      // bad machine-frame info
    CHECK(rejects(2, {code(4, UnwindOp::AllocSmall, 1),
                      static_cast<uint16_t>(0x05 | (6 << 8) | (1 << 12)), 0})); // late epilog
    CHECK(!rejects(1, {code(4, UnwindOp::AllocSmall, 1)}));
}

TEST_CASE("unwind: .pdata zero padding is skipped and inverted ranges rejected") {
    Fixture f;
    f.unwind(f.xdata, 1, 0, 0, 0, {});
    f.runtime_function(0, f.text, f.text + 0x10, f.xdata);
    f.runtime_function(1, 0, 0, 0);
    f.set_pdata_count(2);
    CHECK_EQ(g6::pe::read_runtime_functions(f.builder.image()).size(), size_t{1});

    f.runtime_function(1, f.text + 0x20, f.text + 0x20, f.xdata);
    CHECK_THROWS_AS(g6::pe::read_runtime_functions(f.builder.image()), FormatError);
}
