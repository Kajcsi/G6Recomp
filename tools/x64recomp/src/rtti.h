#pragma once

// MSVC run-time type information in an x64 image: the type descriptors, complete object locators,
// class hierarchies, and base class descriptors that typeid, dynamic_cast, and exception matching
// read, and the vtables that point at them. Written from the documented MSVC x64 layout, where
// every pointer between RTTI structures is an RVA. It names a module's polymorphic classes, their
// bases, and their vtables, which makes a starting map of a binary built without symbols
// (docs/re-workflow.md). Output for game files stays on the user's machine.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "g6/pe/image.h"

namespace g6::recomp {

// Class hierarchy attributes.
inline constexpr uint32_t kRttiMultipleInheritance = 1;
inline constexpr uint32_t kRttiVirtualInheritance = 2;
inline constexpr uint32_t kRttiAmbiguous = 4;

// One entry of a class's base class array: the class itself first, then every base, depth first.
struct RttiBase {
    uint32_t type_descriptor = 0; // RVA
    std::string name;             // decorated, as in the type descriptor
    uint32_t contained_bases = 0; // bases this one has below it in the array
    // Where the base sits in the class: at `member_displacement`, or, for a virtual base, at
    // `member_displacement` past the offset the virtual base table at `vbtable_displacement`
    // holds at `vbtable_index` (pdisp -1 means not virtual).
    int32_t member_displacement = 0;
    int32_t vbtable_displacement = -1;
    int32_t vbtable_index = 0;
    uint32_t attributes = 0;
};

struct RttiClass {
    uint32_t type_descriptor = 0; // RVA
    std::string name;             // decorated: ".?AV" for a class, ".?AU" for a struct
    uint32_t hierarchy = 0;       // RVA of the class hierarchy descriptor
    uint32_t attributes = 0;      // kRtti* bits
    std::vector<RttiBase> bases;
};

struct RttiVtable {
    uint32_t rva = 0;     // first slot; the pointer to the locator sits just before it
    uint32_t locator = 0; // RVA of the complete object locator
    uint32_t type_descriptor = 0;
    uint32_t offset = 0; // where the subobject this vtable serves sits in the complete object
    uint32_t constructor_displacement = 0;
    size_t slots = 0; // consecutive pointers into executable sections
};

// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's map move constructor allocates.
struct RttiReport {
    std::map<uint32_t, RttiClass> classes; // by type descriptor RVA, every class a locator names
    std::vector<RttiVtable> vtables;       // by RVA
    // Type descriptors found by name, with or without a vtable: polymorphic classes, and types
    // only typeid or exception matching needs.
    std::vector<uint32_t> type_descriptors;
    size_t locators = 0;
    size_t locators_without_vtable = 0;
    // Locators with a signature, self-reference, and named type descriptor whose class
    // hierarchy didn't read, and the first reason.
    size_t bad_locators = 0;
    std::string first_error;
};

RttiReport scan_rtti(const pe::Image& image);

// "class Foo", "struct ns::Bar<int>", and so on, from a type descriptor's decorated name; the
// decorated name itself if it doesn't undecorate.
std::string undecorate_type_name(std::string_view decorated);

} // namespace g6::recomp
