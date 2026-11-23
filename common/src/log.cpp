#include "g6/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace g6::log {
namespace {

Level level_from_env() {
    std::string value;
#ifdef _WIN32
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, "G6_LOG_LEVEL") == 0 && buffer != nullptr) {
        value = buffer;
        std::free(buffer);
    }
#else
    if (const char* env = std::getenv("G6_LOG_LEVEL")) {
        value = env;
    }
#endif
    if (value == "trace") {
        return Level::Trace;
    }
    if (value == "debug") {
        return Level::Debug;
    }
    if (value == "warn") {
        return Level::Warn;
    }
    if (value == "error") {
        return Level::Error;
    }
    return Level::Info;
}

std::atomic<int>& level_storage() {
    static std::atomic<int> level{static_cast<int>(level_from_env())};
    return level;
}

std::mutex& output_mutex() {
    static std::mutex mutex;
    return mutex;
}

// Guarded by output_mutex().
Sink g_sink = nullptr;
void* g_sink_context = nullptr;

char level_letter(Level level) {
    switch (level) {
    case Level::Trace: return 'T';
    case Level::Debug: return 'D';
    case Level::Info: return 'I';
    case Level::Warn: return 'W';
    case Level::Error: return 'E';
    }
    return '?';
}

} // namespace

void set_min_level(Level level) {
    level_storage().store(static_cast<int>(level), std::memory_order_relaxed);
}

Level min_level() {
    return static_cast<Level>(level_storage().load(std::memory_order_relaxed));
}

void write(Level level, std::string_view category, std::string_view message) {
    const std::scoped_lock lock(output_mutex());
    if (g_sink != nullptr) {
        g_sink(level, category, message, g_sink_context);
        return;
    }
    fmt::print(stderr, "[{}] {}: {}\n", level_letter(level), category, message);
}

void set_sink(Sink sink, void* context) {
    const std::scoped_lock lock(output_mutex());
    g_sink = sink;
    g_sink_context = context;
}

} // namespace g6::log
