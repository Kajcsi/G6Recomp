// Crash reports for guest code (g6/loader/crash_report.h, docs/execution-model.md, "Crash
// reports"). Guest modules are found through the active Loader's lock-free module list, host
// modules through Windows. The walk follows the function tables Windows knows, the guest's
// included, so it gets through guest frames where RtlCaptureStackBackTrace stops.

#include "g6/loader/crash_report.h"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>

#include <fmt/format.h>

#include "g6/log.h"
#include "loader_impl.h"
#include "text.h"

namespace g6::loader {
namespace {

// What the C++ runtime raises for a throw ('msc' with the customer bit).
constexpr DWORD kCxxExceptionCode = 0xE06D7363;

struct StackRange {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;

    bool holds(uint64_t address, uint64_t size) const {
        return address >= low && address <= high && size <= high - address;
    }
};

StackRange current_stack() {
    StackRange stack;
    GetCurrentThreadStackLimits(&stack.low, &stack.high);
    return stack;
}

// Reads memory a fault may have left unmapped.
bool read_memory(uint64_t address, void* buffer, size_t size) {
    __try {
        std::memcpy(buffer, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// RtlVirtualUnwind reads the saved registers from the stack, which the fault may have damaged.
bool virtual_unwind(DWORD64 image_base, PRUNTIME_FUNCTION function, CONTEXT& context) {
    __try {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Moves `context` to the caller's frame. False when the walk can't go on.
bool unwind_one(CONTEXT& context, const StackRange& stack) {
    const uint64_t sp = context.Rsp;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (function == nullptr) {
        // A leaf function, or no code at all (a call through a bad pointer): the return address
        // is at the stack pointer.
        uint64_t return_address = 0;
        if (!stack.holds(sp, sizeof(return_address)) ||
            !read_memory(sp, &return_address, sizeof(return_address))) {
            return false;
        }
        context.Rip = return_address;
        context.Rsp = sp + sizeof(return_address);
    } else if (!virtual_unwind(image_base, function, context)) {
        return false;
    }
    return context.Rip != 0 && context.Rsp > sp && stack.holds(context.Rsp, 0);
}

std::vector<CodeAddress> walk(const CONTEXT& start, size_t max_frames, const StackRange& stack) {
    std::vector<CodeAddress> frames;
    CONTEXT context = start;
    while (frames.size() < max_frames) {
        frames.push_back(locate_code(context.Rip));
        if (!unwind_one(context, stack)) {
            break;
        }
    }
    return frames;
}

std::string_view exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_SINGLE_STEP: return "single step";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_STACK_CHECK:
    case STATUS_FLOAT_MULTIPLE_FAULTS:
    case STATUS_FLOAT_MULTIPLE_TRAPS: return "floating-point exception";
    case kCxxExceptionCode: return "C++ exception";
    default: return "exception";
    }
}

// What an access violation or in-page error touched: "writing 0x...".
std::string access_line(const EXCEPTION_RECORD& record) {
    if ((record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
         record.ExceptionCode != EXCEPTION_IN_PAGE_ERROR) ||
        record.NumberParameters < 2) {
        return {};
    }
    std::string_view access = "touching";
    switch (record.ExceptionInformation[0]) {
    case 0: access = "reading"; break;
    case 1: access = "writing"; break;
    case 8: access = "executing"; break;
    default: break;
    }
    std::string line = fmt::format("  {} {:#018x}", access, record.ExceptionInformation[1]);
    if (record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR && record.NumberParameters >= 3) {
        line += fmt::format(" (status {:#010x})", record.ExceptionInformation[2]);
    }
    return line;
}

// The decorated name of a C++ exception's type, from the throw information the compiler emits
// (the public layout in the C++ runtime's ehdata.h; on x64 its pointers are offsets from the
// throwing image's base). Empty when the record isn't a C++ throw or the tables can't be read.
std::string cxx_type_name(const EXCEPTION_RECORD& record) {
    constexpr ULONG_PTR kMagicFirst = 0x19930520;
    constexpr ULONG_PTR kMagicLast = 0x19930522;
    if (record.ExceptionCode != kCxxExceptionCode || record.NumberParameters < 4 ||
        record.ExceptionInformation[0] < kMagicFirst ||
        record.ExceptionInformation[0] > kMagicLast) {
        return {};
    }
    const uint64_t throw_info = record.ExceptionInformation[2];
    const uint64_t image_base = record.ExceptionInformation[3];
    // ThrowInfo: attributes, unwind, forward compatibility, then the catchable type array.
    int32_t array_offset = 0;
    if (throw_info == 0 || !read_memory(throw_info + 12, &array_offset, sizeof(array_offset))) {
        return {};
    }
    // CatchableTypeArray: a count, then the catchable types, the thrown type first.
    std::array<int32_t, 2> array{};
    if (!read_memory(image_base + static_cast<uint32_t>(array_offset), array.data(),
                     sizeof(array)) ||
        array[0] < 1) {
        return {};
    }
    // CatchableType: properties, then the type descriptor.
    int32_t type_offset = 0;
    if (!read_memory(image_base + static_cast<uint32_t>(array[1]) + 4, &type_offset,
                     sizeof(type_offset))) {
        return {};
    }
    // TypeDescriptor: the vtable pointer and a spare pointer, then the name.
    const uint64_t name = image_base + static_cast<uint32_t>(type_offset) + 16;
    std::string text;
    for (size_t i = 0; i < 256; ++i) {
        char c = 0;
        if (!read_memory(name + i, &c, 1)) {
            return {};
        }
        if (c == '\0') {
            return text;
        }
        text += c;
    }
    return text;
}

std::string registers(const CONTEXT& context) {
    const std::array<std::pair<std::string_view, DWORD64>, 18> values = {{
        {"rax", context.Rax},
        {"rbx", context.Rbx},
        {"rcx", context.Rcx},
        {"rdx", context.Rdx},
        {"rsi", context.Rsi},
        {"rdi", context.Rdi},
        {"rbp", context.Rbp},
        {"rsp", context.Rsp},
        {"r8", context.R8},
        {"r9", context.R9},
        {"r10", context.R10},
        {"r11", context.R11},
        {"r12", context.R12},
        {"r13", context.R13},
        {"r14", context.R14},
        {"r15", context.R15},
        {"rip", context.Rip},
        {"efl", context.EFlags},
    }};
    std::string text;
    for (size_t i = 0; i < values.size(); ++i) {
        text += i % 4 == 0 ? "\n  " : "  ";
        text += fmt::format("{:<3} {:016x}", values[i].first, values[i].second);
    }
    return text;
}

std::string report(const EXCEPTION_POINTERS& exception, const StackRange& stack) {
    const EXCEPTION_RECORD& record = *exception.ExceptionRecord;
    const CONTEXT& context = *exception.ContextRecord;
    std::string text = fmt::format(
        "unhandled {} {:#010x} on thread {}\n  at {}", exception_name(record.ExceptionCode),
        record.ExceptionCode, GetCurrentThreadId(),
        describe_code(locate_code(reinterpret_cast<uint64_t>(record.ExceptionAddress))));
    if (const std::string access = access_line(record); !access.empty()) {
        text += '\n';
        text += access;
    }
    if (const std::string type = cxx_type_name(record); !type.empty()) {
        text += fmt::format("\n  type {}", type);
    }
    text += registers(context);
    text += "\n  stack:";
    const std::vector<CodeAddress> frames = walk(context, 64, stack);
    for (size_t i = 0; i < frames.size(); ++i) {
        text += fmt::format("\n    #{:<2} {}", i, describe_code(frames[i]));
    }
    return text;
}

// ---- The top-level filter ----

// The host's filter from before ours, and the guest's, which the guest's
// SetUnhandledExceptionFilter keeps here instead of installing.
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_guest_filter{nullptr};
std::mutex g_report_mutex;
thread_local bool t_reporting = false;

void log_report(const std::string& text) {
    LOG_ERROR(crash, "{}", text);
}

struct HelperJob {
    const EXCEPTION_POINTERS* exception = nullptr;
    StackRange stack;
};

DWORD WINAPI report_from_helper(void* parameter) {
    const std::unique_ptr<HelperJob> job(static_cast<HelperJob*>(parameter));
    try {
        log_report(report(*job->exception, job->stack));
    } catch (...) { // NOLINT(bugprone-empty-catch): nothing is left to report to
    }
    return 0;
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* exception) {
    if (!t_reporting && exception != nullptr) {
        t_reporting = true;
        try {
            const std::scoped_lock lock(g_report_mutex);
            if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
                // Little of this thread's stack is left. The helper walks it while this thread
                // waits here, so the frames stay put; a helper that hangs, perhaps on a lock
                // this thread holds, is given up on and keeps its job.
                auto* job = new HelperJob{exception, current_stack()};
                HANDLE helper = CreateThread(nullptr, 0, &report_from_helper, job, 0, nullptr);
                if (helper == nullptr) {
                    delete job; // NOLINT(cppcoreguidelines-owning-memory)
                } else {
                    WaitForSingleObject(helper, 10'000);
                    CloseHandle(helper);
                }
            } else {
                log_report(report(*exception, current_stack()));
            }
        } catch (...) { // NOLINT(bugprone-empty-catch): the exception goes on as it would have
        }
        t_reporting = false;
    }
    if (const LPTOP_LEVEL_EXCEPTION_FILTER guest = g_guest_filter.load()) {
        const LONG result = guest(exception);
        if (result != EXCEPTION_CONTINUE_SEARCH) {
            return result;
        }
    }
    return g_previous != nullptr ? g_previous(exception) : EXCEPTION_CONTINUE_SEARCH;
}

// Named like the Win32 function it stands in for; only register_crash_report hands it out.
namespace hle {

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    install_crash_report();
    return g_guest_filter.exchange(filter);
}

} // namespace hle
} // namespace

CodeAddress locate_code(uint64_t address) {
    CodeAddress where;
    where.address = address;
    if (const Loader::Impl* loader = active_loader()) {
        if (const Module::State* module = loader->containing(address)) {
            where.module = narrow(module->path.filename().wstring());
            where.offset = address - module->base();
            where.guest = true;
            where.preferred = module->image.image_base() + where.offset;
            return where;
        }
    }
    HMODULE host = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &host)) {
        std::array<wchar_t, MAX_PATH> path{};
        const DWORD length = GetModuleFileNameW(host, path.data(), static_cast<DWORD>(path.size()));
        const std::wstring_view full(path.data(), length);
        const size_t slash = full.find_last_of(L"\\/");
        where.module = narrow(slash == std::wstring_view::npos ? full : full.substr(slash + 1));
        where.offset = address - reinterpret_cast<uint64_t>(host);
    }
    return where;
}

std::string describe_code(const CodeAddress& where) {
    if (where.module.empty()) {
        return fmt::format("{:#018x}", where.address);
    }
    if (where.guest) {
        return fmt::format("{}+{:#x} [guest {:#x}]", where.module, where.offset, where.preferred);
    }
    return fmt::format("{}+{:#x}", where.module, where.offset);
}

std::vector<CodeAddress> walk_stack(const CONTEXT& context, size_t max_frames) {
    return walk(context, max_frames, current_stack());
}

std::string crash_report(const EXCEPTION_POINTERS& exception) {
    return report(exception, current_stack());
}

void install_crash_report() {
    static std::once_flag once;
    std::call_once(once,
                   [] { g_previous = ::SetUnhandledExceptionFilter(&on_unhandled_exception); });
}

void register_crash_report(ImportRegistry& imports) {
    for (const char* dll : {"api-ms-win-core-errorhandling-*", "kernel32.dll", "kernelbase.dll"}) {
        imports.hle(dll, Symbol::by_name("SetUnhandledExceptionFilter"),
                    reinterpret_cast<const void*>(&hle::SetUnhandledExceptionFilter));
    }
}

} // namespace g6::loader
