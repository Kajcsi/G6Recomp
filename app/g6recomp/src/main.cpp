// g6recomp.exe runs the game's main executable natively (docs/execution-model.md, phase 5, "The
// runner"). The work is in g6/app/runner.h.

#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "g6/app/runner.h"
#include "g6/log.h"

int wmain(int argc, wchar_t** argv) { // NOLINT(misc-use-internal-linkage): the entry point
    const std::span<wchar_t*> all(argv, argc > 0 ? static_cast<size_t>(argc) : 0);
    const std::vector<std::wstring> arguments(all.empty() ? all.end() : all.begin() + 1, all.end());
    try {
        const g6::app::RunOptions options = g6::app::parse_arguments(arguments);
        if (options.help) {
            fmt::print("{}", g6::app::usage());
            return 0;
        }
        g6::app::run(options);
    } catch (const g6::app::UsageError& error) {
        fmt::print(stderr, "g6recomp: {}\n\n{}", error.what(), g6::app::usage());
        return 2;
    } catch (const std::exception& error) {
        LOG_ERROR(runner, "{}", error.what());
        return 1;
    }
}
