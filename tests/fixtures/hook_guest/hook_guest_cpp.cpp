// The C++ half of the hook engine's original test guest: C++ callers of the hand-written
// functions in hook_guest_asm.c that catch exceptions thrown below them. Built with /EHs and
// without /EHc, so the compiler doesn't assume that the extern "C" functions never throw.

#include "hook_guest_api.h"

namespace {

struct Cleanup {
    Cleanup() = default;
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;
    ~Cleanup() { g6h_note_cleanup(); }
};

__declspec(noinline) int framed_with_cleanup(int x) {
    const Cleanup cleanup;
    return g6h_mid_framed(x) + 1;
}

int throw_if_negative(int x) {
    if (x < 0) {
        throw x;
    }
    return x;
}

} // namespace

extern "C" int g6h_cpp_catch_mid(int x) {
    try {
        return framed_with_cleanup(x);
    } catch (int value) {
        return value;
    }
}

extern "C" int g6h_cpp_through_framed(int x) {
    try {
        return g6h_framed(&throw_if_negative, x);
    } catch (int value) {
        return value;
    }
}
