// The RTTI scan (tools/x64recomp/src/rtti.h) on its original test guest, rtti_guest.dll, read as a
// file: every class the guest defines, its bases and inheritance kind, its vtables and their
// slots, and a type that is only thrown.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "g6/pe/image.h"
#include "g6test.h"
#include "rtti.h"

namespace pe = g6::pe;
namespace recomp = g6::recomp;

namespace {

const recomp::RttiReport& report() {
    static const pe::Image image = pe::Image::load_file(G6_RTTI_GUEST_PATH);
    static const recomp::RttiReport report = recomp::scan_rtti(image);
    return report;
}

const recomp::RttiClass* find_class(std::string_view name) {
    for (const auto& [rva, cls] : report().classes) {
        if (cls.name == name) {
            return &cls;
        }
    }
    return nullptr;
}

std::vector<recomp::RttiVtable> vtables_of(const recomp::RttiClass& cls) {
    std::vector<recomp::RttiVtable> found;
    for (const recomp::RttiVtable& vtable : report().vtables) {
        if (vtable.type_descriptor == cls.type_descriptor) {
            found.push_back(vtable);
        }
    }
    std::ranges::sort(found, {}, &recomp::RttiVtable::offset);
    return found;
}

std::vector<std::string> base_names(const recomp::RttiClass& cls) {
    std::vector<std::string> names;
    names.reserve(cls.bases.size());
    for (const recomp::RttiBase& base : cls.bases) {
        names.push_back(base.name);
    }
    return names;
}

} // namespace

TEST_CASE("every locator in the guest reads cleanly and has a vtable") {
    INFO(report().first_error);
    CHECK_EQ(report().bad_locators, size_t{0});
    CHECK(report().locators > 0);
    CHECK_EQ(report().locators_without_vtable, size_t{0});
    for (const recomp::RttiVtable& vtable : report().vtables) {
        CHECK(report().classes.contains(vtable.type_descriptor));
        CHECK(vtable.slots > 0);
    }
}

TEST_CASE("single inheritance") {
    const recomp::RttiClass* square = find_class(".?AVSquare@@");
    REQUIRE(square != nullptr);
    CHECK_EQ(square->attributes, uint32_t{0});
    const std::vector<std::string> bases = {".?AVSquare@@", ".?AVShape@@"};
    CHECK(base_names(*square) == bases);
    CHECK_EQ(square->bases[0].contained_bases, uint32_t{1});
    CHECK_EQ(square->bases[1].vbtable_displacement, int32_t{-1});

    const std::vector<recomp::RttiVtable> vtables = vtables_of(*square);
    REQUIRE(vtables.size() == 1);
    CHECK_EQ(vtables[0].offset, uint32_t{0});
    CHECK_EQ(vtables[0].slots, size_t{3}); // deleting destructor, area, label
}

TEST_CASE("multiple inheritance gives a vtable per base") {
    const recomp::RttiClass* ledger = find_class(".?AULedger@@");
    REQUIRE(ledger != nullptr);
    CHECK((ledger->attributes & recomp::kRttiMultipleInheritance) != 0);
    CHECK((ledger->attributes & recomp::kRttiVirtualInheritance) == 0);
    const std::vector<std::string> bases = {".?AULedger@@", ".?AUNamed@@", ".?AUCounted@@"};
    CHECK(base_names(*ledger) == bases);
    CHECK_EQ(ledger->bases[2].member_displacement, int32_t{8});

    const std::vector<recomp::RttiVtable> vtables = vtables_of(*ledger);
    REQUIRE(vtables.size() == 2);
    CHECK_EQ(vtables[0].offset, uint32_t{0});
    CHECK_EQ(vtables[0].slots, size_t{2}); // deleting destructor, name
    CHECK_EQ(vtables[1].offset, uint32_t{8});
    CHECK_EQ(vtables[1].slots, size_t{3}); // deleting destructor, count, reset
}

TEST_CASE("virtual inheritance marks the shared base") {
    const recomp::RttiClass* junction = find_class(".?AUJunction@@");
    REQUIRE(junction != nullptr);
    CHECK((junction->attributes & recomp::kRttiVirtualInheritance) != 0);
    bool virtual_node = false;
    for (const recomp::RttiBase& base : junction->bases) {
        virtual_node |= base.name == ".?AUNode@@" && base.vbtable_displacement >= 0;
    }
    CHECK(virtual_node);
    CHECK(vtables_of(*junction).size() >= 2);
}

TEST_CASE("templates and nested classes undecorate") {
    const recomp::RttiClass* holder_int = find_class(".?AV?$Holder@H@@");
    const recomp::RttiClass* holder_double = find_class(".?AV?$Holder@N@@");
    const recomp::RttiClass* entry = find_class(".?AVEntry@Registry@outer@@");
    REQUIRE(holder_int != nullptr);
    REQUIRE(holder_double != nullptr);
    REQUIRE(entry != nullptr);
    CHECK_EQ(recomp::undecorate_type_name(holder_int->name), std::string("class Holder<int>"));
    CHECK_EQ(recomp::undecorate_type_name(holder_double->name),
             std::string("class Holder<double>"));
    CHECK_EQ(recomp::undecorate_type_name(entry->name),
             std::string("class outer::Registry::Entry"));
    CHECK_EQ(recomp::undecorate_type_name(".?AUJunction@@"), std::string("struct Junction"));
    CHECK_EQ(recomp::undecorate_type_name("plain"), std::string("plain"));
    CHECK(base_names(*holder_int).back() == ".?AVShape@@");
}

TEST_CASE("a type that is only thrown has a type descriptor and no vtable") {
    CHECK(find_class(".?AUThrownCode@@") == nullptr);
    const pe::Image image = pe::Image::load_file(G6_RTTI_GUEST_PATH);
    bool found = false;
    for (const uint32_t rva : report().type_descriptors) {
        found |= image.read_cstring(rva + 16) == ".?AUThrownCode@@";
    }
    CHECK(found);
    for (const auto& [rva, cls] : report().classes) {
        CHECK(std::ranges::binary_search(report().type_descriptors, rva));
    }
}
