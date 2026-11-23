#pragma once

// A minimal test harness, enough for the nogame and game suites without another dependency.
//
//   TEST_CASE("image rejects a missing MZ header") {
//       REQUIRE(condition);          // stops the test case on failure
//       CHECK(condition);            // records the failure and continues
//       CHECK_EQ(actual, expected);  // prints both values on failure
//       CHECK_THROWS_AS(expression, ExceptionType);
//       INFO("context");             // printed with every failure until the end of its scope
//       SKIP("reason");              // skips the rest of the test case
//   }
//
// Each test executable takes an optional substring filter on test names. It exits 0 when every
// test ran and passed, 1 on any failure, and 77 (ctest's SKIP_RETURN_CODE) when nothing failed
// and every test skipped, so a game suite without game files shows as skipped, not passed.

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace g6::test {

struct SkipTest {
    std::string reason;
};
struct RequireFailed {};

using TestFunction = void (*)();

struct TestCase {
    const char* name;
    TestFunction function;
    const char* file;
    int line;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, TestFunction function, const char* file, int line) {
        registry().push_back({name, function, file, line});
    }
};

void report_failure(const char* file, int line, const std::string& message);

// Context for the failures reported while it exists; see INFO.
class ScopedInfo {
public:
    explicit ScopedInfo(std::string message);
    ~ScopedInfo();
    ScopedInfo(const ScopedInfo&) = delete;
    ScopedInfo& operator=(const ScopedInfo&) = delete;
    ScopedInfo(ScopedInfo&&) = delete;
    ScopedInfo& operator=(ScopedInfo&&) = delete;
};

template <typename T>
std::string describe(const T& value) {
    if constexpr (fmt::is_formattable<T>::value) {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
            return fmt::format("{} ({:#x})", value, value);
        } else {
            return fmt::format("{}", value);
        }
    } else {
        return "<unprintable>";
    }
}

template <typename A, typename B>
bool check_eq(const A& actual, const B& expected, const char* actual_text,
              const char* expected_text, const char* file, int line) {
    if (actual == expected) {
        return true;
    }
    report_failure(file, line,
                   fmt::format("CHECK_EQ({}, {})\n      actual:   {}\n      expected: {}",
                               actual_text, expected_text, describe(actual), describe(expected)));
    return false;
}

} // namespace g6::test

#define G6_TEST_CONCAT_(a, b) a##b
#define G6_TEST_CONCAT(a, b) G6_TEST_CONCAT_(a, b)
#define G6_TEST_CASE_IMPL(name, function)                                                          \
    static void function();                                                                        \
    static const ::g6::test::Registrar G6_TEST_CONCAT(function, _registrar)(name, &(function),     \
                                                                            __FILE__, __LINE__);   \
    static void function()
#define TEST_CASE(name) G6_TEST_CASE_IMPL(name, G6_TEST_CONCAT(g6_test_case_, __COUNTER__))

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::g6::test::report_failure(__FILE__, __LINE__, "CHECK(" #expr ")");                    \
        }                                                                                          \
    } while (0)

#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::g6::test::report_failure(__FILE__, __LINE__, "REQUIRE(" #expr ")");                  \
            throw ::g6::test::RequireFailed{};                                                     \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                 \
    ::g6::test::check_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define CHECK_THROWS_AS(expr, type)                                                                \
    do {                                                                                           \
        bool g6_threw_ = false;                                                                    \
        try {                                                                                      \
            (void)(expr);                                                                          \
        } catch (const type&) {                                                                    \
            g6_threw_ = true;                                                                      \
        } catch (...) {                                                                            \
        }                                                                                          \
        if (!g6_threw_) {                                                                          \
            ::g6::test::report_failure(__FILE__, __LINE__,                                         \
                                       "CHECK_THROWS_AS(" #expr ", " #type ") did not throw");     \
        }                                                                                          \
    } while (0)

#define INFO(message)                                                                              \
    const ::g6::test::ScopedInfo G6_TEST_CONCAT(g6_info_, __COUNTER__) {                           \
        message                                                                                    \
    }

#define SKIP(reason)                                                                               \
    throw ::g6::test::SkipTest {                                                                   \
        reason                                                                                     \
    }
