#pragma once

// Logging for tools and runtime: LOG_<LEVEL>(category, format, args...).
// `category` is a bare identifier (LOG_WARN(pe, "...")); it is stringified, not declared.
// The minimum level comes from the G6_LOG_LEVEL environment variable (trace, debug, info,
// warn, error) and defaults to info.

#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace g6::log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error };

void set_min_level(Level level);
Level min_level();
inline bool enabled(Level level) {
    return static_cast<int>(level) >= static_cast<int>(min_level());
}

void write(Level level, std::string_view category, std::string_view message);

// Receives every message that passes the level filter, in place of stderr. Tests use it to check
// log output. Passing nullptr restores stderr.
using Sink = void (*)(Level level, std::string_view category, std::string_view message,
                      void* context);
void set_sink(Sink sink, void* context);

template <typename... Args>
void writef(Level level, std::string_view category, fmt::format_string<Args...> format,
            Args&&... args) {
    if (enabled(level)) {
        write(level, category, fmt::format(format, std::forward<Args>(args)...));
    }
}

} // namespace g6::log

#define G6_LOG_AT(level, category, ...) ::g6::log::writef(level, #category, __VA_ARGS__)
#define LOG_TRACE(category, ...) G6_LOG_AT(::g6::log::Level::Trace, category, __VA_ARGS__)
#define LOG_DEBUG(category, ...) G6_LOG_AT(::g6::log::Level::Debug, category, __VA_ARGS__)
#define LOG_INFO(category, ...) G6_LOG_AT(::g6::log::Level::Info, category, __VA_ARGS__)
#define LOG_WARN(category, ...) G6_LOG_AT(::g6::log::Level::Warn, category, __VA_ARGS__)
#define LOG_ERROR(category, ...) G6_LOG_AT(::g6::log::Level::Error, category, __VA_ARGS__)
