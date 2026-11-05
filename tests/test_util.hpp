#pragma once

#include <cstdio>
#include <string>

inline int g_checks = 0;
inline int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        auto va = (a);                                                       \
        auto vb = (b);                                                       \
        if (!(va == vb)) {                                                   \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s == %s  (got %s vs %s)\n", __FILE__,  \
                        __LINE__, #a, #b, test_detail::show(va).c_str(),     \
                        test_detail::show(vb).c_str());                      \
        }                                                                    \
    } while (0)

namespace test_detail {
inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return show(std::string(s)); }
inline std::string show(bool v) { return v ? "true" : "false"; }
template <typename T>
inline std::string show(const T& v) {
    return std::to_string(v);
}
}

inline int test_summary(const char* name) {
    std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
