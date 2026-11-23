// The guest executable's exit tables (g6/imports/crt_exit.h). Written from the UCRT's documented
// contract and the behaviour measured on the host (docs/execution-model.md, phase 5, "Exit").

#include <atomic>
#include <intrin.h>
#include <mutex>
#include <new>
#include <vector>
#include <windows.h>

#include "g6/imports/crt_exit.h"
#include "g6/loader/import_registry.h"
#include "g6/log.h"

// NOLINTBEGIN(bugprone-reserved-identifier)
namespace g6::imports::ucrtbase {
namespace {

// The CRT's exit lock. The thread that holds it may take it again; any other thread waits.
class ExitLock {
public:
    // Whether this call took the lock; false when the calling thread already held it.
    bool acquire() {
        const DWORD self = GetCurrentThreadId();
        for (;;) {
            DWORD owner = 0;
            if (owner_.compare_exchange_strong(owner, self)) {
                return true;
            }
            if (owner == self) {
                return false;
            }
            owner_.wait(owner);
        }
    }

    void release() {
        owner_.store(0);
        owner_.notify_all();
    }

    // Forgets the owner without waking anyone: for a new guest process, when the owner of the
    // last one ended without releasing the lock.
    void reset() { owner_.store(0); }

private:
    std::atomic<DWORD> owner_{0};
};

struct GuestProcess {
    // Guards the tables and the callback, never while a handler runs.
    std::mutex tables_lock;
    std::vector<AtExitFunction> atexit_table;
    std::vector<AtExitFunction> quick_exit_table;
    PIMAGE_TLS_CALLBACK thread_local_callback = nullptr;
    std::atomic<EndProcess> end{nullptr};
    ExitLock exit_lock;
};

GuestProcess& process() {
    static GuestProcess instance;
    return instance;
}

int add(std::vector<AtExitFunction>& table, AtExitFunction function) {
    if (function == nullptr) {
        return -1;
    }
    GuestProcess& guest = process();
    const std::scoped_lock lock(guest.tables_lock);
    try {
        table.push_back(function);
    } catch (const std::bad_alloc&) {
        return -1;
    }
    return 0;
}

// Newest first, one at a time, so that a handler registered meanwhile runs next and a handler
// that exits again continues where this left off.
void run(std::vector<AtExitFunction>& table) {
    GuestProcess& guest = process();
    for (;;) {
        AtExitFunction function = nullptr;
        {
            const std::scoped_lock lock(guest.tables_lock);
            if (table.empty()) {
                return;
            }
            function = table.back();
            table.pop_back();
        }
        function();
    }
}

enum class Cleanup : uint8_t { Full, Quick, None };

void clean_up(Cleanup cleanup) {
    GuestProcess& guest = process();
    if (cleanup == Cleanup::Full) {
        PIMAGE_TLS_CALLBACK callback = nullptr;
        {
            const std::scoped_lock lock(guest.tables_lock);
            callback = guest.thread_local_callback;
        }
        if (callback != nullptr) {
            callback(nullptr, DLL_PROCESS_DETACH, nullptr);
        }
        run(guest.atexit_table);
    } else if (cleanup == Cleanup::Quick) {
        run(guest.quick_exit_table);
    }
}

// Runs the cleanup under the exit lock, which it keeps, and ends the guest process.
[[noreturn]] void leave(Cleanup cleanup, int code) {
    GuestProcess& guest = process();
    guest.exit_lock.acquire();
    clean_up(cleanup);
    LOG_INFO(crt, "the guest process ends with exit code {}", code);
    if (EndProcess end = guest.end.load()) {
        end(code);
        LOG_ERROR(crt, "the guest process's end function returned");
    }
    ExitProcess(static_cast<UINT>(code));
}

// Runs the cleanup under the exit lock and returns.
void clean_up_and_return(Cleanup cleanup) {
    GuestProcess& guest = process();
    const bool took = guest.exit_lock.acquire();
    clean_up(cleanup);
    if (took) {
        guest.exit_lock.release();
    }
}

} // namespace

void start_guest_process(EndProcess end) {
    GuestProcess& guest = process();
    {
        const std::scoped_lock lock(guest.tables_lock);
        guest.atexit_table.clear();
        guest.quick_exit_table.clear();
        guest.thread_local_callback = nullptr;
    }
    guest.end.store(end);
    guest.exit_lock.reset();
}

int _crt_atexit(AtExitFunction function) {
    return add(process().atexit_table, function);
}

int _crt_at_quick_exit(AtExitFunction function) {
    return add(process().quick_exit_table, function);
}

void _register_thread_local_exe_atexit_callback(PIMAGE_TLS_CALLBACK callback) {
    GuestProcess& guest = process();
    {
        const std::scoped_lock lock(guest.tables_lock);
        if (guest.thread_local_callback == nullptr) {
            guest.thread_local_callback = callback;
            return;
        }
    }
    LOG_ERROR(crt, "the guest registered a second thread_local exit callback; ending the process");
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

void exit(int code) {
    leave(Cleanup::Full, code);
}

void quick_exit(int code) {
    leave(Cleanup::Quick, code);
}

void _exit(int code) {
    leave(Cleanup::None, code);
}

void _Exit(int code) {
    leave(Cleanup::None, code);
}

void _cexit() {
    clean_up_and_return(Cleanup::Full);
}

void _c_exit() {
    clean_up_and_return(Cleanup::None);
}

// ---- Registration ----

namespace {

struct Entry {
    const char* name;
    const void* function;
};

template <typename Function>
Entry entry(const char* name, Function* function) {
    return {name, reinterpret_cast<const void*>(function)};
}

} // namespace

void register_exit(loader::ImportRegistry& imports) {
    const Entry functions[] = {
        entry("_crt_atexit", &_crt_atexit),
        entry("_crt_at_quick_exit", &_crt_at_quick_exit),
        entry("_register_thread_local_exe_atexit_callback",
              &_register_thread_local_exe_atexit_callback),
        entry("exit", &ucrtbase::exit),
        entry("quick_exit", &ucrtbase::quick_exit),
        entry("_exit", &ucrtbase::_exit),
        entry("_Exit", &ucrtbase::_Exit),
        entry("_cexit", &_cexit),
        entry("_c_exit", &_c_exit),
    };
    for (const Entry& item : functions) {
        imports.hle("api-ms-win-crt-*", loader::Symbol::by_name(item.name), item.function);
    }
}

} // namespace g6::imports::ucrtbase
// NOLINTEND(bugprone-reserved-identifier)
