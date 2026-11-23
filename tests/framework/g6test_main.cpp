#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <windows.h>

#include "g6test.h"

namespace g6::test {
namespace {

int g_failures_in_case = 0;

std::vector<std::string>& info_stack() {
    static std::vector<std::string> stack;
    return stack;
}

} // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

void report_failure(const char* file, int line, const std::string& message) {
    ++g_failures_in_case;
    fmt::print(stdout, "    {}({}): {}\n", file, line, message);
    for (const std::string& info : info_stack()) {
        fmt::print(stdout, "      with: {}\n", info);
    }
}

ScopedInfo::ScopedInfo(std::string message) {
    info_stack().push_back(std::move(message));
}

ScopedInfo::~ScopedInfo() {
    info_stack().pop_back();
}

namespace {

int run_all(int argc, char** argv) {
    const std::string_view filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const TestCase& test : registry()) {
        if (!filter.empty() && std::string_view(test.name).find(filter) == std::string_view::npos) {
            continue;
        }
        g_failures_in_case = 0;
        bool skip = false;
        std::string skip_reason;
        try {
            test.function();
        } catch (const SkipTest& skip_test) {
            skip = true;
            skip_reason = skip_test.reason;
        } catch (const RequireFailed&) {
            // Already reported.
        } catch (const std::exception& error) {
            report_failure(test.file, test.line,
                           fmt::format("unexpected exception: {}", error.what()));
        } catch (...) {
            report_failure(test.file, test.line, "unexpected non-standard exception");
        }
        if (g_failures_in_case != 0) {
            ++failed;
            fmt::print(stdout, "FAIL  {}\n", test.name);
        } else if (skip) {
            ++skipped;
            fmt::print(stdout, "SKIP  {} ({})\n", test.name, skip_reason);
        } else {
            ++passed;
            fmt::print(stdout, "ok    {}\n", test.name);
        }
        std::fflush(stdout);
    }
    fmt::print(stdout, "\n{} passed, {} failed, {} skipped\n", passed, failed, skipped);
    if (failed != 0) {
        return 1;
    }
    return passed == 0 && skipped != 0 ? 77 : 0;
}

} // namespace

} // namespace g6::test

int main(int argc, char** argv) {
    // Tests run unattended: debug CRT reports (heap corruption, failed assertions) go to stderr,
    // and crashes end the process, instead of waiting on a dialog nobody will click.
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        return g6::test::run_all(argc, argv);
    } catch (...) {
        // Writing the report failed; there is nowhere left to say so.
        return 1;
    }
}
