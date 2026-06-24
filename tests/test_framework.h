#pragma once

// Minimal zero-dependency test harness. Define tests with TEST(name) and
// assert with CHECK(expr). PLAN.md milestone M1 may swap this for Catch2 if
// it starts to chafe.

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

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

inline int runAll() {
    const auto& tests = registry();

    // Category order, in first-seen order so output is stable and grouped.
    std::vector<std::string> order;
    for (const auto& t : tests) {
        if (std::find(order.begin(), order.end(), t.category) == order.end())
            order.push_back(t.category);
    }

    struct Stat { int total = 0; int failed = 0; };
    std::vector<std::pair<std::string, Stat>> stats;

    for (const auto& category : order) {
        std::printf("[%s]\n", category.c_str());
        Stat st;
        for (const auto& test : tests) {
            if (test.category != category) continue;
            const int failuresBefore = failureCount();
            test.fn();
            const bool ok = failureCount() == failuresBefore;
            std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", test.name.c_str());
            ++st.total;
            if (!ok) ++st.failed;
        }
        stats.push_back({category, st});
    }

    std::size_t width = 0;
    for (const auto& [name, st] : stats)
        width = std::max(width, name.size());

    int total = 0;
    int failed = 0;
    std::printf("\nTested areas:\n");
    for (const auto& [name, st] : stats) {
        std::printf("  %-*s  %3d test(s)%s\n", static_cast<int>(width),
                    name.c_str(), st.total,
                    st.failed ? "   <-- FAILURES" : "");
        total += st.total;
        failed += st.failed;
    }
    std::printf("\n%d test(s) across %zu areas, %d failure(s)\n", total,
                stats.size(), failed);
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
