#include "common/cpu_features.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define LV_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace lv {

namespace {

#if LV_X86
void cpuid(int leaf, int sub, int out[4]) {
#if defined(_MSC_VER)
    __cpuidex(out, leaf, sub);
#else
    unsigned a, b, c, d;
    __cpuid_count(leaf, sub, a, b, c, d);
    out[0] = int(a); out[1] = int(b); out[2] = int(c); out[3] = int(d);
#endif
}

unsigned long long xgetbv0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
}

bool detect_sse42() {
    int r[4];
    cpuid(0, 0, r);
    if (r[0] < 1) return false;
    cpuid(1, 0, r);
    return (r[2] & (1 << 20)) != 0;
}

bool detect_avx2() {
    int r[4];
    cpuid(0, 0, r);
    if (r[0] < 7) return false;
    cpuid(1, 0, r);
    const bool osxsave = (r[2] & (1 << 27)) != 0;
    const bool avx = (r[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) return false;
    if ((xgetbv0() & 0x6) != 0x6) return false;
    cpuid(7, 0, r);
    return (r[1] & (1 << 5)) != 0;
}
#else
bool detect_sse42() { return false; }
bool detect_avx2() { return false; }
#endif

bool env_forces_scalar() {
    const char* v = std::getenv("LV_SIMD");
    return v && std::strcmp(v, "scalar") == 0;
}

std::atomic<bool> g_disabled{false};

}  // namespace

bool cpu_has_sse42() {
    static const bool has = detect_sse42();
    return has;
}

bool cpu_has_avx2() {
    static const bool has = detect_avx2();
    return has;
}

bool cpu_has_neon() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return false;
#endif
}

void set_simd_disabled(bool d) { g_disabled.store(d, std::memory_order_relaxed); }

bool simd_enabled() {
    static const bool env_scalar = env_forces_scalar();
    return !env_scalar && !g_disabled.load(std::memory_order_relaxed);
}

}  // namespace lv
