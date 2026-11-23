// An original executable for test_crt_exit, built /MD like the game's, so its atexit, static
// destructors, and thread_local destructors reach the CRT's exit functions through its imports.
// Each handler and destructor records an event in g6_exit_log; g6_exit_mode chooses how main
// ends (exit_guest_api.h). The test sets the mode and reads the log through the exports.

#include <cstdlib>
#include <process.h>

#include "exit_guest_api.h"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
extern "C" {
__declspec(dllexport) int g6_exit_mode = 0;
__declspec(dllexport) char g6_exit_log[G6_EXIT_LOG_SIZE] = {};

__declspec(dllexport) void g6_exit_note(char event) {
    int length = 0;
    while (g6_exit_log[length] != 0) {
        ++length;
    }
    if (length + 1 < G6_EXIT_LOG_SIZE) {
        g6_exit_log[length] = event;
    }
}
}

namespace {

class Logged {
public:
    constexpr explicit Logged(int event) : event_(static_cast<char>(event)) {}
    Logged(const Logged&) = delete;
    Logged& operator=(const Logged&) = delete;
    ~Logged() { g6_exit_note(event_); }

    char event() const { return event_; }

private:
    char event_;
};

// The compiler registers its destructor through atexit before main runs.
Logged g_static(G6_EXIT_EVENT_STATIC);
// Its destructor runs through the thread_local exit callback, or when the thread ends.
thread_local Logged t_local(G6_EXIT_EVENT_THREAD_LOCAL);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void first() {
    g6_exit_note(G6_EXIT_EVENT_FIRST);
}
void late() {
    g6_exit_note(G6_EXIT_EVENT_LATE);
}
void adder() {
    g6_exit_note(G6_EXIT_EVENT_ADDER);
    std::atexit(late);
}
void third() {
    g6_exit_note(G6_EXIT_EVENT_THIRD);
}
void quick() {
    g6_exit_note(G6_EXIT_EVENT_QUICK);
}

} // namespace

int main() {
    g6_exit_note(G6_EXIT_EVENT_MAIN);
    const int mode = g6_exit_mode;
    // Makes sure the main thread's object exists, whichever way the compiler initializes it.
    const volatile char touched = t_local.event();
    (void)touched;
    std::atexit(first);
    std::atexit(adder);
    std::atexit(third);
    std::at_quick_exit(quick);

    switch (mode) {
    case G6_EXIT_RETURN: return 3;
    case G6_EXIT_EXIT: std::exit(4);
    case G6_EXIT_QUICK: std::quick_exit(5);
    case G6_EXIT_PLAIN: _exit(6);
    case G6_EXIT_CEXIT:
        _cexit();
        g6_exit_note(G6_EXIT_EVENT_AFTER_CEXIT);
        std::exit(7);
    default: return 99;
    }
}
