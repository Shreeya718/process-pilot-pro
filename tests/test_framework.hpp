#pragma once

// Minimal dependency-free unit-test harness (keeps the project 100% C++20, no third-party code).

#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace pilot::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) { registry().push_back({std::move(name), std::move(fn)}); }
};

inline int runAll() {
    int passed = 0;
    for (const auto& t : registry()) {
        const int before = failures();
        std::cout << "[ RUN  ] " << t.name << '\n';
        try {
            t.fn();
        } catch (const std::exception& ex) {
            ++failures();
            std::cerr << "  uncaught exception: " << ex.what() << '\n';
        }
        if (failures() == before) {
            ++passed;
            std::cout << "[  OK  ] " << t.name << '\n';
        } else {
            std::cout << "[ FAIL ] " << t.name << '\n';
        }
    }
    std::cout << passed << "/" << registry().size() << " tests passed\n";
    return failures() == 0 ? 0 : 1;
}

}  // namespace pilot::test

#define PP_TEST(name)                                                         \
    static void name();                                                       \
    static ::pilot::test::Registrar pp_registrar_##name(#name, name);         \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++::pilot::test::failures();                                                   \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
        }                                                                                  \
    } while (false)

#define CHECK_MSG(cond, msg)                                                               \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++::pilot::test::failures();                                                   \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond " -- " \
                      << (msg) << "\n";                                                    \
        }                                                                                  \
    } while (false)

#define PP_TEST_MAIN() \
    int main() { return ::pilot::test::runAll(); }
