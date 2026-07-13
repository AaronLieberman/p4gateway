// SPDX-License-Identifier: MIT

#pragma once

// Minimal zero-dependency test harness. Define tests with TEST(name) and
// assert with CHECK(expr). PLAN.md milestone M1 may swap this for Catch2 if
// it starts to chafe.
//
// Running the binary directly prints a per-area summary (one line per source
// file: "running N tests ... Passed"). Pass -v / --verbose to also list every
// individual test. Output is colorized when stdout is a terminal; set NO_COLOR
// to force plain text.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // keep <windows.h> from clobbering std::min/std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace testfw {

struct TestCase {
    std::string category;
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

// True when the NO_COLOR environment variable is present (https://no-color.org).
inline bool noColorRequested() {
#ifdef _WIN32
    std::size_t len = 0;  // getenv_s avoids MSVC's C4996 on getenv
    return getenv_s(&len, nullptr, 0, "NO_COLOR") == 0 && len > 0;
#else
    return std::getenv("NO_COLOR") != nullptr;
#endif
}

// Color is enabled only for an interactive terminal and when NO_COLOR is unset.
// On Windows we also flip the console into ANSI mode.
inline bool colorEnabled() {
    static const bool enabled = [] {
        if (noColorRequested()) return false;
#ifdef _WIN32
        if (!_isatty(_fileno(stdout))) return false;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        return true;
#else
        return isatty(fileno(stdout)) != 0;
#endif
    }();
    return enabled;
}

inline const char* green() { return colorEnabled() ? "\033[32m" : ""; }
inline const char* red() { return colorEnabled() ? "\033[31m" : ""; }
inline const char* bold() { return colorEnabled() ? "\033[1m" : ""; }
inline const char* dim() { return colorEnabled() ? "\033[2m" : ""; }
inline const char* reset() { return colorEnabled() ? "\033[0m" : ""; }

// Derive a short category name from the __FILE__ of the test: strip the
// directory, the leading "test_" prefix, and the ".cpp" suffix, so that
// tests/test_config.cpp groups under "config".
inline std::string categoryFromFile(const char* file) {
    std::string path(file);
    const auto slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".cpp") == 0)
        base.resize(base.size() - 4);
    if (base.rfind("test_", 0) == 0)
        base.erase(0, 5);
    return base;
}

struct Registrar {
    Registrar(std::string category, const char* name, std::function<void()> fn) {
        registry().push_back({std::move(category), name, std::move(fn)});
    }
};

// runAll prints a per-area summary by default; verbose also lists each test.
inline int runAll(bool verbose = false) {
    const auto& tests = registry();

    // Category order, in first-seen order so output is stable and grouped.
    std::vector<std::string> order;
    for (const auto& t : tests) {
        if (std::find(order.begin(), order.end(), t.category) == order.end())
            order.push_back(t.category);
    }

    std::size_t width = 0;
    for (const auto& name : order)
        width = std::max(width, name.size());

    // Header: the leading blank line separates the run from any build output
    // above it, and the counts mirror the closing summary line.
    std::printf("\n%sRunning %zu test(s) across %zu areas...%s\n\n", bold(),
                tests.size(), order.size(), reset());

    struct Stat { int total = 0; int failed = 0; };
    std::vector<std::pair<std::string, Stat>> stats;

    for (const auto& category : order) {
        Stat st;
        for (const auto& t : tests)
            if (t.category == category) ++st.total;

        if (verbose) {
            std::printf("%s[%s]%s\n", bold(), category.c_str(), reset());
        } else {
            std::printf("  %-*s  running %2d test%s ... ",
                        static_cast<int>(width), category.c_str(), st.total,
                        st.total == 1 ? " " : "s");
            std::fflush(stdout);
        }

        for (const auto& test : tests) {
            if (test.category != category) continue;
            const int failuresBefore = failureCount();
            test.fn();
            const bool ok = failureCount() == failuresBefore;
            if (!ok) ++st.failed;
            if (verbose) {
                std::printf("  %s%s%s  %s\n", ok ? green() : red(),
                            ok ? "ok  " : "FAIL", reset(), test.name.c_str());
            }
        }

        if (!verbose) {
            if (st.failed == 0) {
                std::printf("%sPassed%s\n", green(), reset());
            } else {
                // A failed CHECK has already printed detail lines, so restate
                // the area on a fresh line alongside the failure count.
                std::printf("  %s%-*s  Failed%s (%d failed)\n", red(),
                            static_cast<int>(width), category.c_str(), reset(),
                            st.failed);
            }
        }
        stats.push_back({category, st});
    }

    int total = 0;
    int failed = 0;

    // The per-area table only adds value in verbose mode; in summary mode it
    // just repeats the per-area lines printed above.
    if (verbose) {
        std::printf("\nTested areas:\n");
        for (const auto& [name, st] : stats) {
            std::printf("  %-*s  %3d test(s)%s%s%s\n", static_cast<int>(width),
                        name.c_str(), st.total, st.failed ? red() : "",
                        st.failed ? "   <-- FAILURES" : "",
                        st.failed ? reset() : "");
        }
    }
    for (const auto& [name, st] : stats) {
        total += st.total;
        failed += st.failed;
    }

    const char* tone = failed == 0 ? green() : red();
    std::printf("\n%s%d test(s) across %zu areas, %d failure(s)%s\n", tone, total,
                stats.size(), failed, reset());
    return failed == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                                     \
    static void test_##name();                                         \
    static testfw::Registrar registrar_##name(                         \
        testfw::categoryFromFile(__FILE__), #name, test_##name);       \
    static void test_##name()

#define CHECK(expr)                                                    \
    do {                                                               \
        if (!(expr)) {                                                 \
            std::printf("  CHECK failed at %s:%d: %s\n", __FILE__,     \
                        __LINE__, #expr);                              \
            ++testfw::failureCount();                                  \
        }                                                              \
    } while (0)