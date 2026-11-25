#pragma once
// Minimal dependency-free unit test runner (~70 lines).

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace lv_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& fail_count() {
    static int c = 0;
    return c;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void record_failure(const char* file, int line, const char* expr, const std::string& msg = {}) {
    ++fail_count();
    if (msg.empty()) {
        std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
    } else {
        std::fprintf(stderr, "  FAIL %s:%d: %s (%s)\n", file, line, expr, msg.c_str());
    }
}

// Filter: one substring, or several separated by ',' (any of them matches), e.g. "udp_,audio2_".
inline bool name_matches(const char* name, const char* filter) {
    const char* p = filter;
    while (*p) {
        const char* e = std::strchr(p, ',');
        const size_t n = e ? size_t(e - p) : std::strlen(p);
        if (n > 0 && std::string(name).find(std::string(p, n)) != std::string::npos) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

inline int run_all(const char* filter = nullptr) {
    int passed = 0, failed = 0, ran = 0;
    for (const auto& tc : registry()) {
        if (filter && !name_matches(tc.name, filter)) continue;
        ++ran;
        const int before = fail_count();
        try {
            tc.fn();
        } catch (const std::exception& e) {
            record_failure(__FILE__, __LINE__, "uncaught exception", e.what());
        } catch (...) {
            record_failure(__FILE__, __LINE__, "uncaught unknown exception");
        }
        if (fail_count() == before) {
            ++passed;
            std::printf("[PASS] %s\n", tc.name);
        } else {
            ++failed;
            std::printf("[FAIL] %s\n", tc.name);
        }
    }
    std::printf("\nSummary: %d/%d passed (%d failed)\n", passed, ran, failed);
    return failed ? 1 : 0;
}

}  // namespace lv_test

#define LV_TEST_CONCAT2(a, b) a##b
#define LV_TEST_CONCAT(a, b) LV_TEST_CONCAT2(a, b)

#define TEST(name)                                                           \
    static void LV_TEST_CONCAT(test_fn_, name)();                            \
    static lv_test::Registrar LV_TEST_CONCAT(test_reg_, name)(#name,         \
                                                              &LV_TEST_CONCAT(test_fn_, name)); \
    static void LV_TEST_CONCAT(test_fn_, name)()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) lv_test::record_failure(__FILE__, __LINE__, #cond);     \
    } while (0)

#define CHECK_MSG(cond, msg)                                                 \
    do {                                                                     \
        if (!(cond)) lv_test::record_failure(__FILE__, __LINE__, #cond, (msg)); \
    } while (0)

#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            lv_test::record_failure(__FILE__, __LINE__, #cond);              \
            return;                                                          \
        }                                                                    \
    } while (0)
