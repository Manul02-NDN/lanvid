#pragma once
// Runtime CPU feature detection for SIMD (x86 AVX2 / SSE4.2, ARM64 NEON) dispatch.

namespace lv {

bool cpu_has_sse42();            // x86 hardware CRC32C instructions
bool cpu_has_avx2();             // x86 CPU + OS support for AVX2 (YMM state enabled)
bool cpu_has_neon();             // ARM64 AdvSIMD / NEON (always true on aarch64)
void set_simd_disabled(bool d);  // force scalar kernels (benchmarks / --no-simd)
bool simd_enabled();             // !disabled && not overridden by env LV_SIMD=scalar

}  // namespace lv
