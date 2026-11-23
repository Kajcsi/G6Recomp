// The MSVC C++ exception table readers on synthetic tables, written from the public description
// of the formats: FH4's compressed integers in each length, FH3 FuncInfo and FH4 FuncInfo4 with
// try blocks, catches, cleanup funclets and IP-to-state maps, FH4's separated maps, and
// malformed tables of both kinds.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "g6/pe/cxx_eh.h"
#include "g6test.h"
#include "pe_builder.h"

using g6::pe::CxxEhFormat;
using g6::pe::CxxFuncInfo;
using g6::pe::FormatError;
using g6::pe::Image;
using g6::test::PeBuilder;

namespace {

constexpr uint32_t kCode = 0x1000; // .text, 0x400 bytes
constexpr uint32_t kData = 0x2000; // .rdata, where the tables go

// Where each test puts its tables.
constexpr uint32_t kFuncInfo = kData;
constexpr uint32_t kUnwindMap = kData + 0x40;
constexpr uint32_t kTryMap = kData + 0x80;
constexpr uint32_t kHandlers = kData + 0xC0;
constexpr uint32_t kIpMap = kData + 0x100;

// Writes fields one after another, FH4's compressed integers included.
class Emitter {
public:
    Emitter(PeBuilder& builder, uint32_t rva) : builder_(builder), rva_(rva) {}

    Emitter& byte(uint8_t value) {
        builder_.put<uint8_t>(rva_, value);
        rva_ += 1;
        return *this;
    }
    Emitter& int32(int32_t value) {
        builder_.put<int32_t>(rva_, value);
        rva_ += 4;
        return *this;
    }
    Emitter& rva(uint32_t value) { return int32(static_cast<int32_t>(value)); }

    // The shortest compressed form of `value`.
    Emitter& compressed(uint32_t value) {
        if (value < 0x80) {
            return byte(static_cast<uint8_t>(value << 1));
        }
        if (value < 0x4000) {
            const uint32_t encoded = (value << 2) | 0x1;
            return byte(static_cast<uint8_t>(encoded)).byte(static_cast<uint8_t>(encoded >> 8));
        }
        if (value < 0x200000) {
            const uint32_t encoded = (value << 3) | 0x3;
            return byte(static_cast<uint8_t>(encoded))
                .byte(static_cast<uint8_t>(encoded >> 8))
                .byte(static_cast<uint8_t>(encoded >> 16));
        }
        if (value < 0x10000000) {
            return int32(static_cast<int32_t>((value << 4) | 0x7));
        }
        return byte(0x0F).int32(static_cast<int32_t>(value));
    }

    uint32_t at() const { return rva_; }

private:
    PeBuilder& builder_;
    uint32_t rva_;
};

PeBuilder make_builder() {
    PeBuilder builder;
    builder.add_section(".text", PeBuilder::kCode, 0x400, 0xCC);
    builder.add_section(".rdata", PeBuilder::kRdata, 0x400);
    return builder;
}

std::vector<std::pair<uint32_t, int32_t>> states_of(const CxxFuncInfo& info) {
    std::vector<std::pair<uint32_t, int32_t>> states;
    states.reserve(info.ip_to_state.size());
    for (const auto& entry : info.ip_to_state) {
        states.emplace_back(entry.rva, entry.state);
    }
    return states;
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// An FH3 FuncInfo with three states: state 0 an object, state 1 a try body, and state 2 its catch,
// whose funclet is at 0x1200. States 0 and 2 run the same cleanup funclet, at 0x1100.
void write_fh3(PeBuilder& b, uint32_t magic = 0x19930522) {
    b.put<uint32_t>(kFuncInfo, magic);
    b.put<int32_t>(kFuncInfo + 4, 3);
    b.put<uint32_t>(kFuncInfo + 8, kUnwindMap);
    b.put<uint32_t>(kFuncInfo + 12, 1);
    b.put<uint32_t>(kFuncInfo + 16, kTryMap);
    b.put<uint32_t>(kFuncInfo + 20, 4);
    b.put<uint32_t>(kFuncInfo + 24, kIpMap);

    // Unwind entries: {state to go to, cleanup action}.
    b.put<int32_t>(kUnwindMap, -1);
    b.put<uint32_t>(kUnwindMap + 4, 0x1100);
    b.put<int32_t>(kUnwindMap + 8, 0);
    b.put<int32_t>(kUnwindMap + 16, 0);
    b.put<uint32_t>(kUnwindMap + 20, 0x1100);

    // Try block: states [1, 1], catches to 2, one handler.
    b.put<int32_t>(kTryMap, 1);
    b.put<int32_t>(kTryMap + 4, 1);
    b.put<int32_t>(kTryMap + 8, 2);
    b.put<uint32_t>(kTryMap + 12, 1);
    b.put<uint32_t>(kTryMap + 16, kHandlers);
    b.put<uint32_t>(kHandlers + 12, 0x1200);

    // The IP-to-state map, out of order on purpose.
    const std::vector<std::pair<uint32_t, int32_t>> map = {
        {0x1030, 1}, {0x1000, -1}, {0x1050, -1}, {0x1010, 0}};
    for (size_t i = 0; i < map.size(); ++i) {
        b.put<uint32_t>(kIpMap + static_cast<uint32_t>(i) * 8, map[i].first);
        b.put<int32_t>(kIpMap + static_cast<uint32_t>(i) * 8 + 4, map[i].second);
    }
}

// An FH4 FuncInfo4 for a function at 0x1000 with five states: a destructor (0), a cleanup
// funclet (1), a destructor through a pointer (2), another cleanup funclet (3), and no action (4).
// One try block, states [1, 1] with catches to 4, has two catches: one with every optional field
// and a continuation as an offset, one with two continuations as RVAs. The IP-to-state map
// starts 8 bytes into the function.
void write_fh4(PeBuilder& b) {
    Emitter(b, kFuncInfo).byte(0x18).rva(kUnwindMap).rva(kTryMap).rva(kIpMap);

    Emitter(b, kUnwindMap)
        .compressed(5)
        .compressed((1 << 2) | 1) // destructor of an object: the destructor, then the object
        .rva(0x1180)
        .compressed(0x10)
        .compressed(3) // funclet
        .rva(0x1100)
        .compressed((2 << 2) | 2) // destructor through a pointer
        .rva(0x1180)
        .compressed(0x14)
        .compressed((1 << 2) | 3) // funclet
        .rva(0x10F0)
        .compressed(0); // no action

    Emitter(b, kTryMap).compressed(1).compressed(1).compressed(1).compressed(4).rva(kHandlers);
    Emitter(b, kHandlers)
        .compressed(2)
        .byte(0x17) // adjectives, type, catch object, one continuation as an offset
        .compressed(0x01)
        .rva(kData + 0x300)
        .compressed(0x28)
        .rva(0x1200)
        .compressed(0x40)
        .byte(0x28) // two continuations as RVAs
        .rva(0x1220)
        .rva(0x1060)
        .rva(0x1070);

    Emitter(b, kIpMap)
        .compressed(3)
        .compressed(0x08)
        .compressed(0 + 1)
        .compressed(0x10)
        .compressed(1 + 1)
        .compressed(0x20)
        .compressed(-1 + 1);
}

// What reading the tables reports, or an empty string when they read.
std::string fh3_error(const std::function<void(PeBuilder&)>& corrupt) {
    PeBuilder b = make_builder();
    write_fh3(b);
    corrupt(b);
    const Image image = b.image();
    try {
        (void)read_fh3_func_info(image, kFuncInfo);
    } catch (const FormatError& error) {
        return error.what();
    }
    return {};
}

std::string fh4_error(const std::function<void(PeBuilder&)>& write) {
    PeBuilder b = make_builder();
    write(b);
    const Image image = b.image();
    try {
        (void)read_fh4_func_info(image, kFuncInfo, kCode);
    } catch (const FormatError& error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("cxx_eh: FH4 compressed integers in each length") {
    PeBuilder b = make_builder();
    b.put_bytes(kData, {0x0A,                         // x0: 5
                        0xD1, 0x48,                   // x01: 0x1234
                        0x2B, 0x1A, 0x09,             // x011: 0x12345
                        0x67, 0x45, 0x23, 0x01,       // x0111: 0x123456
                        0x0F, 0x78, 0x56, 0x34, 0x12, // 1111: 0x12345678
                        0xFE,                         // the largest one-byte value
                        0xFD, 0xFF});                 // the largest two-byte value
    const Image image = b.image();
    uint32_t rva = kData;
    const std::vector<std::pair<uint32_t, uint32_t>> expected = {
        {5, 1}, {0x1234, 2}, {0x12345, 3}, {0x123456, 4}, {0x12345678, 5}, {0x7F, 1}, {0x3FFF, 2}};
    for (const auto& [value, length] : expected) {
        INFO(fmt::format("at {:#x}", rva));
        const uint32_t start = rva;
        CHECK_EQ(g6::pe::read_fh4_unsigned(image, rva), value);
        CHECK_EQ(rva - start, length);
    }
}

TEST_CASE("cxx_eh: compressed integers read back at every length boundary") {
    const std::vector<uint32_t> values = {0,          0x7F,       0x80,      0x3FFF,
                                          0x4000,     0x1FFFFF,   0x200000,  0xFFFFFFF,
                                          0x10000000, 0x7FFFFFFF, 0xFFFFFFFF};
    PeBuilder b = make_builder();
    Emitter out(b, kData);
    for (const uint32_t value : values) {
        out.compressed(value);
    }
    const Image image = b.image();
    uint32_t rva = kData;
    for (const uint32_t value : values) {
        CHECK_EQ(g6::pe::read_fh4_unsigned(image, rva), value);
    }
    CHECK_EQ(rva, out.at());
}

TEST_CASE("cxx_eh: an FH3 FuncInfo gives its states, try blocks, and cleanups") {
    PeBuilder b = make_builder();
    write_fh3(b);
    const Image image = b.image();
    CHECK(g6::pe::has_fh3_magic(image, kFuncInfo));
    const CxxFuncInfo info = read_fh3_func_info(image, kFuncInfo);

    CHECK(info.format == CxxEhFormat::kFh3);
    CHECK(!info.catch_funclet);
    CHECK_EQ(info.states, 3);
    const std::vector<std::pair<uint32_t, int32_t>> sorted = {
        {0x1000, -1}, {0x1010, 0}, {0x1030, 1}, {0x1050, -1}};
    CHECK(states_of(info) == sorted);
    CHECK(info.cleanup_funclets == std::vector<uint32_t>{0x1100});
    REQUIRE(info.try_blocks.size() == 1);
    const auto& block = info.try_blocks[0];
    CHECK_EQ(block.try_low, 1);
    CHECK_EQ(block.try_high, 1);
    CHECK_EQ(block.catch_high, 2);
    REQUIRE(block.catches.size() == 1);
    CHECK_EQ(block.catches[0].handler, 0x1200u);
    CHECK(block.catches[0].continuations.empty());

    CHECK_EQ(info.state_at(0x0FFF), -1);
    CHECK_EQ(info.state_at(0x1000), -1);
    CHECK_EQ(info.state_at(0x1015), 0);
    CHECK_EQ(info.state_at(0x1030), 1);
    CHECK_EQ(info.state_at(0x104F), 1);
    CHECK_EQ(info.state_at(0x1060), -1);
    CHECK(info.states_in(0x1008, 0x1031) == (std::vector<int32_t>{-1, 0, 1}));
    CHECK(info.states_in(0x1012, 0x1020) == std::vector<int32_t>{0});
    CHECK(info.states_in(0x1030, 0x1050) == std::vector<int32_t>{1}); // the end is exclusive
    CHECK(info.try_block_for(1) == &block);
    CHECK(info.try_block_for(0) == nullptr);
    CHECK(info.try_block_for(2) == nullptr); // a catch block isn't in its own try block
    CHECK(info.try_block_for(-1) == nullptr);
}

TEST_CASE("cxx_eh: FH3 is recognized by each magic number, whatever the bits above it") {
    for (const uint32_t magic : {0x19930520u, 0x19930521u, 0x19930522u, 0x19930522u | (1u << 29),
                                 0x19930520u | 0xE0000000u}) {
        INFO(fmt::format("magic {:#x}", magic));
        PeBuilder b = make_builder();
        write_fh3(b, magic);
        const Image image = b.image();
        CHECK(g6::pe::has_fh3_magic(image, kFuncInfo));
        CHECK_EQ(read_fh3_func_info(image, kFuncInfo).states, 3);
    }
    PeBuilder b = make_builder();
    write_fh3(b, 0x19930523);
    const Image image = b.image();
    CHECK(!g6::pe::has_fh3_magic(image, kFuncInfo));
    CHECK(!g6::pe::has_fh3_magic(image, kFuncInfo + 4));
    CHECK(!g6::pe::has_fh3_magic(image, 0xFFFFFFF0));
    CHECK_THROWS_AS(read_fh3_func_info(image, kFuncInfo), FormatError);
}

TEST_CASE("cxx_eh: the innermost try block holds a state") {
    PeBuilder b = make_builder();
    write_fh3(b);
    b.put<int32_t>(kFuncInfo + 4, 6);
    b.put<uint32_t>(kFuncInfo + 12, 2);
    // The outer try block first: states [0, 2] with catches to 5. The inner one, states [1, 1]
    // with catches to 2, is in its body.
    b.put<int32_t>(kTryMap, 0);
    b.put<int32_t>(kTryMap + 4, 2);
    b.put<int32_t>(kTryMap + 8, 5);
    b.put<int32_t>(kTryMap + 20, 1);
    b.put<int32_t>(kTryMap + 24, 1);
    b.put<int32_t>(kTryMap + 28, 2);
    b.put<uint32_t>(kTryMap + 32, 1);
    b.put<uint32_t>(kTryMap + 36, kHandlers);
    const Image image = b.image();
    const CxxFuncInfo info = read_fh3_func_info(image, kFuncInfo);
    REQUIRE(info.try_blocks.size() == 2);
    const auto* outer = &info.try_blocks[0];
    const auto* inner = &info.try_blocks[1];
    CHECK(info.try_block_for(0) == outer);
    CHECK(info.try_block_for(1) == inner);
    CHECK(info.try_block_for(2) == outer); // the inner catch block
    CHECK(info.try_block_for(3) == nullptr);
}

TEST_CASE("cxx_eh: malformed FH3 tables are refused") {
    CHECK(fh3_error([](PeBuilder&) {}).empty());
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<int32_t>(kIpMap + 4, 3); }), "out of range"));
    CHECK(
        contains(fh3_error([](PeBuilder& b) { b.put<int32_t>(kIpMap + 4, -2); }), "out of range"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<int32_t>(kTryMap, 2); }), "out of range"));
    CHECK(
        contains(fh3_error([](PeBuilder& b) { b.put<int32_t>(kTryMap + 8, 3); }), "out of range"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<int32_t>(kTryMap, -1); }), "out of range"));
    CHECK(
        contains(fh3_error([](PeBuilder& b) { b.put<int32_t>(kFuncInfo + 4, -1); }), "-1 states"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<uint32_t>(kFuncInfo + 12, 0x7FFFFFFF); }),
                   "implausible"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<uint32_t>(kTryMap + 12, 5000); }),
                   "implausible"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<uint32_t>(kHandlers + 12, kData); }),
                   "catch funclet 0x2000 isn't code"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<uint32_t>(kUnwindMap + 4, kData); }),
                   "cleanup funclet 0x2000 isn't code"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<uint32_t>(kIpMap, 0); }),
                   "IP-to-state entry 0x0 isn't code"));
    CHECK(contains(fh3_error([](PeBuilder& b) { b.put<uint32_t>(kFuncInfo, 0x19930519); }),
                   "isn't an FH3 FuncInfo"));
    // A map outside the image fails in the image's bounds checks.
    CHECK(!fh3_error([](PeBuilder& b) { b.put<uint32_t>(kFuncInfo + 16, 0x7FFF0000); }).empty());
}

TEST_CASE("cxx_eh: an FH4 FuncInfo4 gives its states, catches, continuations, and cleanups") {
    PeBuilder b = make_builder();
    write_fh4(b);
    const Image image = b.image();
    const CxxFuncInfo info = read_fh4_func_info(image, kFuncInfo, kCode);

    CHECK(info.format == CxxEhFormat::kFh4);
    CHECK(!info.catch_funclet);
    CHECK_EQ(info.states, 5);
    // The map starts 8 bytes in, so the function's first bytes are in no state.
    const std::vector<std::pair<uint32_t, int32_t>> expected = {
        {0x1000, -1}, {0x1008, 0}, {0x1018, 1}, {0x1038, -1}};
    CHECK(states_of(info) == expected);
    CHECK(info.cleanup_funclets == (std::vector<uint32_t>{0x10F0, 0x1100}));
    REQUIRE(info.try_blocks.size() == 1);
    const auto& block = info.try_blocks[0];
    CHECK_EQ(block.try_low, 1);
    CHECK_EQ(block.try_high, 1);
    CHECK_EQ(block.catch_high, 4);
    REQUIRE(block.catches.size() == 2);
    CHECK_EQ(block.catches[0].handler, 0x1200u);
    CHECK(block.catches[0].continuations == std::vector<uint32_t>{0x1040});
    CHECK_EQ(block.catches[1].handler, 0x1220u);
    CHECK(block.catches[1].continuations == (std::vector<uint32_t>{0x1060, 0x1070}));
    CHECK(info.try_block_for(info.state_at(0x1020)) == &block);
    CHECK(info.try_block_for(info.state_at(0x1010)) == nullptr);
}

TEST_CASE("cxx_eh: a separated FH4 catch funclet merges its code segments' maps") {
    PeBuilder b = make_builder();
    // Catch funclet, separated, with a BBT value and an unwind map of two empty states.
    Emitter(b, kFuncInfo).byte(0x0F).compressed(5).rva(kUnwindMap).rva(kIpMap);
    Emitter(b, kUnwindMap).compressed(2).compressed(0).compressed(0);
    // Two segments, the later one listed first. The first starts in state 0; the second starts in
    // no state and enters state 1 four bytes in.
    Emitter(b, kIpMap).compressed(2).rva(0x1300).rva(kIpMap + 0x40).rva(0x1200).rva(kIpMap + 0x80);
    Emitter(b, kIpMap + 0x40)
        .compressed(2)
        .compressed(0)
        .compressed(1)
        .compressed(0x10)
        .compressed(0);
    Emitter(b, kIpMap + 0x80).compressed(1).compressed(4).compressed(2);
    const Image image = b.image();
    const CxxFuncInfo info = read_fh4_func_info(image, kFuncInfo, kCode);

    CHECK(info.catch_funclet);
    CHECK_EQ(info.states, 2);
    CHECK(info.try_blocks.empty());
    CHECK(info.cleanup_funclets.empty());
    const std::vector<std::pair<uint32_t, int32_t>> expected = {
        {0x1200, -1}, {0x1204, 1}, {0x1300, 0}, {0x1310, -1}};
    CHECK(states_of(info) == expected);
}

TEST_CASE("cxx_eh: malformed FH4 tables are refused") {
    CHECK(fh4_error(write_fh4).empty());
    // A catch with the reserved continuation count.
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       write_fh4(b);
                       b.put<uint8_t>(kHandlers + 1, 0x37);
                   }),
                   "reserved continuation count"));
    // The second catch's funclet, then its first continuation, in data.
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       write_fh4(b);
                       b.put<uint32_t>(kHandlers + 14, kData);
                   }),
                   "catch funclet 0x2000 isn't code"));
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       write_fh4(b);
                       b.put<uint32_t>(kHandlers + 18, kData);
                   }),
                   "continuation 0x2000 isn't code"));
    // A state past the last one.
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       write_fh4(b);
                       Emitter(b, kIpMap).compressed(1).compressed(0).compressed(5 + 1);
                   }),
                   "state 5 at 0x1000 is out of range"));
    // An IP-to-state entry outside the code.
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       Emitter(b, kFuncInfo).byte(0x00).rva(kIpMap);
                       Emitter(b, kIpMap).compressed(1).compressed(0x1000).compressed(0);
                   }),
                   "IP-to-state entry 0x2000 isn't code"));
    // Implausible counts.
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       Emitter(b, kFuncInfo).byte(0x00).rva(kIpMap);
                       Emitter(b, kIpMap).compressed(0xFFFFFFFF);
                   }),
                   "implausible"));
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       Emitter(b, kFuncInfo).byte(0x08).rva(kUnwindMap).rva(kIpMap);
                       Emitter(b, kUnwindMap).compressed(0xFFFFFFFF);
                   }),
                   "implausible"));
    // A separated map whose segment starts in data.
    CHECK(contains(fh4_error([](PeBuilder& b) {
                       Emitter(b, kFuncInfo).byte(0x02).rva(kIpMap);
                       Emitter(b, kIpMap).compressed(1).rva(kData).rva(kIpMap + 0x40);
                   }),
                   "code segment 0x2000 isn't code"));
    // A map outside the image fails in the image's bounds checks.
    CHECK(
        !fh4_error([](PeBuilder& b) { Emitter(b, kFuncInfo).byte(0x00).rva(0x7FFF0000); }).empty());
}
