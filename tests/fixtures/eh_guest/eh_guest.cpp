// The hook engine's original C++ exception test guest (docs/hooks.md, "C++ exception
// functions"). tests/CMakeLists.txt builds it four ways: by clang-cl, whose functions use
// __CxxFrameHandler3, and by cl.exe, whose functions use __CxxFrameHandler4, each with and
// without /guard:ehcont. All four are optimized /MD builds with /GS, like the console's side
// DLLs, so their handlers are imports or the C runtime's GS wrappers. They use /EHs without /EHc,
// since the test's callers catch what these extern "C" functions throw.

namespace {

volatile int g_cleanups = 0;
// Read before each function's first state, so that both compilers give the function a few bytes
// outside every state at its entry, where a hook needs no hand check.
volatile int g_bias = 100;
// Always 0. Reading it puts plain code between and after the calls in a try block, where a
// window neither holds a call nor starts at a call's return address.
volatile int g_step = 0;

__declspec(noinline) void note_cleanup() {
    g_cleanups = g_cleanups + 1;
}

struct Cleanup {
    Cleanup() = default;
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;
    ~Cleanup() { note_cleanup(); }
};

__declspec(noinline) int maybe_throw(int x) {
    if (x < 0) {
        throw x;
    }
    return x + 1;
}

__declspec(noinline) void fill(char* buffer, int size, int x) {
    for (int i = 0; i < size; ++i) {
        buffer[i] = static_cast<char>(x + i);
    }
}

} // namespace

// A /GS buffer and no exception handling, around a call that may throw. cl.exe gives the function
// the C runtime's __GSHandlerCheck as its only handler, which checks the stack cookie as an
// exception passes and does nothing else. clang-cl checks the cookie without a handler.
extern "C" __declspec(dllexport, noinline) int ehg_buffer(int x) {
    const int bias = g_bias;
    char buffer[32];
    fill(buffer, 32, x);
    return maybe_throw(buffer[x & 31]) + bias - 1;
}

// A try block whose catch resumes in the function. The entry is outside the try block. Only the
// first call throws: the second gets a positive argument.
extern "C" __declspec(dllexport, noinline) int ehg_catch(int x) {
    const int bias = g_bias;
    int result = 0;
    try {
        result = maybe_throw(x) + g_step;
        result = maybe_throw(result + g_step) - 1;
        result += g_step;
    } catch (int value) {
        result = value * 2;
    }
    return result + bias;
}

// No try block, but an object whose destructor must run if the call throws.
extern "C" __declspec(dllexport, noinline) int ehg_live(int x) {
    const int bias = g_bias;
    const Cleanup cleanup;
    return maybe_throw(x) + (bias * 10);
}

// A catch that always rethrows, so it never resumes in the function.
extern "C" __declspec(dllexport, noinline) int ehg_rethrow(int x) {
    const int bias = g_bias;
    try {
        return maybe_throw(x + bias - 100);
    } catch (...) {
        note_cleanup();
        throw;
    }
}

// Callers that catch what the functions above let through, so no exception reaches the host.
extern "C" __declspec(dllexport, noinline) int ehg_catch_buffer(int x) {
    try {
        return ehg_buffer(x);
    } catch (int value) {
        return value;
    }
}

extern "C" __declspec(dllexport, noinline) int ehg_catch_live(int x) {
    try {
        return ehg_live(x);
    } catch (int value) {
        return value;
    }
}

extern "C" __declspec(dllexport, noinline) int ehg_catch_rethrown(int x) {
    try {
        return ehg_rethrow(x);
    } catch (int value) {
        return value - 1000;
    }
}

extern "C" __declspec(dllexport) int ehg_cleanups() {
    return g_cleanups;
}
