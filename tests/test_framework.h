#pragma once

// Minimal zero-dependency test harness. Define tests with TEST(name) and
// assert with CHECK(expr). PLAN.md milestone M1 may swap this for Catch2 if
// it starts to chafe.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
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

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int runAll() {
    for (const auto& test : registry()) {
        const int failuresBefore = failureCount();
        test.fn();
        std::printf("%s  %s\n",
                    failureCount() == failuresBefore ? "ok  " : "FAIL",
                    test.name.c_str());
    }
    std::printf("\n%zu test(s), %d failure(s)\n", registry().size(),
                failureCount());
    return failureCount() == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                                     \
    static void test_##name();                                         \
    static testfw::Registrar registrar_##name(#name, test_##name);     \
    static void test_##name()

#define CHECK(expr)                                                    \
    do {                                                               \
        if (!(expr)) {                                                 \
            std::printf("  CHECK failed at %s:%d: %s\n", __FILE__,     \
                        __LINE__, #expr);                              \
            ++testfw::failureCount();                                  \
        }                                                              \
    } while (0)
