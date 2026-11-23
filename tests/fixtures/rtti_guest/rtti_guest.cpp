// Original test guest for the RTTI scan (tools/x64recomp/src/rtti.h): polymorphic classes with
// single, multiple, and virtual inheritance, a class template, a nested class, and a type that is
// only ever thrown. test_rtti reads the DLL as a file and never loads it. The exports keep every
// vtable and type descriptor alive.

#include <cstdint>

#define RTTI_GUEST_EXPORT extern "C" __declspec(dllexport)

// Single inheritance: three virtual functions each.
class Shape {
public:
    virtual ~Shape() = default;
    virtual int area() const = 0;
    virtual const char* label() const { return "shape"; }
};

class Square : public Shape {
public:
    explicit Square(int side) : side_(side) {}
    int area() const override { return side_ * side_; }
    const char* label() const override { return "square"; }

private:
    int side_;
};

// Multiple inheritance: a vtable for each base, the second at offset 8 in the object.
struct Named {
    virtual ~Named() = default;
    virtual const char* name() const = 0;
};

struct Counted {
    virtual ~Counted() = default;
    virtual int count() const = 0;
    virtual void reset() {}
};

struct Ledger : Named, Counted {
    const char* name() const override { return "ledger"; }
    int count() const override { return entries; }
    int entries = 4;
};

// Virtual inheritance: a diamond with one shared Node.
struct Node {
    virtual ~Node() = default;
    virtual int id() const { return 1; }
    int tag = 0;
};

struct LeftNode : virtual Node {
    int id() const override { return 2; }
};

struct RightNode : virtual Node {
    virtual int weight() const { return 3; }
};

struct Junction : LeftNode, RightNode {
    int id() const override { return 4; }
    int weight() const override { return 5; }
};

// A class template and a nested class.
template <typename T>
class Holder : public Shape {
public:
    explicit Holder(T value) : value_(value) {}
    int area() const override { return static_cast<int>(value_); }

private:
    T value_;
};

namespace outer {
class Registry {
public:
    class Entry {
    public:
        virtual ~Entry() = default;
        virtual int key() const { return 6; }
    };
};
} // namespace outer

// Thrown but never polymorphic: a type descriptor with no vtable.
struct ThrownCode {
    int code;
};

RTTI_GUEST_EXPORT Shape* rtti_guest_square(int side) {
    return new Square(side);
}
RTTI_GUEST_EXPORT Named* rtti_guest_ledger() {
    return new Ledger();
}
RTTI_GUEST_EXPORT Node* rtti_guest_junction() {
    return new Junction();
}
RTTI_GUEST_EXPORT Shape* rtti_guest_holder_int(int value) {
    return new Holder<int>(value);
}
RTTI_GUEST_EXPORT Shape* rtti_guest_holder_double(double value) {
    return new Holder<double>(value);
}
RTTI_GUEST_EXPORT outer::Registry::Entry* rtti_guest_entry() {
    return new outer::Registry::Entry();
}

RTTI_GUEST_EXPORT int rtti_guest_count(const Named* named) {
    const auto* counted = dynamic_cast<const Counted*>(named);
    return counted != nullptr ? counted->count() : -1;
}

RTTI_GUEST_EXPORT int rtti_guest_throw(int code) {
    try {
        if (code != 0) {
            throw ThrownCode{code};
        }
    } catch (const ThrownCode& thrown) {
        return thrown.code;
    }
    return 0;
}

RTTI_GUEST_EXPORT void rtti_guest_destroy(Shape* shape) {
    delete shape;
}
