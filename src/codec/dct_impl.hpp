#pragma once
// AAN butterflies, generic over the lane type V (float for scalar, 8-wide vector for SIMD).
// Ported from the IJG jfdctflt.c / jidctflt.c algorithms.
// Wrapped in an anonymous namespace on purpose: this header is included by TUs compiled with
// different ISA flags, and per-TU copies prevent the linker from merging an AVX2-compiled
// instantiation into the scalar path (ODR / illegal-instruction pitfall).

namespace {

template <class V>
inline void aan_fdct8(V& d0, V& d1, V& d2, V& d3, V& d4, V& d5, V& d6, V& d7) {
    V tmp0 = d0 + d7, tmp7 = d0 - d7;
    V tmp1 = d1 + d6, tmp6 = d1 - d6;
    V tmp2 = d2 + d5, tmp5 = d2 - d5;
    V tmp3 = d3 + d4, tmp4 = d3 - d4;

    // Even part
    V tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
    V tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
    d0 = tmp10 + tmp11;
    d4 = tmp10 - tmp11;
    V z1 = (tmp12 + tmp13) * 0.707106781f;
    d2 = tmp13 + z1;
    d6 = tmp13 - z1;

    // Odd part
    tmp10 = tmp4 + tmp5;
    tmp11 = tmp5 + tmp6;
    tmp12 = tmp6 + tmp7;
    V z5 = (tmp10 - tmp12) * 0.382683433f;
    V z2 = tmp10 * 0.541196100f + z5;
    V z4 = tmp12 * 1.306562965f + z5;
    V z3 = tmp11 * 0.707106781f;
    V z11 = tmp7 + z3, z13 = tmp7 - z3;
    d5 = z13 + z2;
    d3 = z13 - z2;
    d1 = z11 + z4;
    d7 = z11 - z4;
}

template <class V>
inline void aan_idct8(V& d0, V& d1, V& d2, V& d3, V& d4, V& d5, V& d6, V& d7) {
    // Even part
    V tmp10 = d0 + d4, tmp11 = d0 - d4;
    V tmp13 = d2 + d6;
    V tmp12 = (d2 - d6) * 1.414213562f - tmp13;
    V e0 = tmp10 + tmp13, e3 = tmp10 - tmp13;
    V e1 = tmp11 + tmp12, e2 = tmp11 - tmp12;

    // Odd part
    V z13 = d5 + d3, z10 = d5 - d3;
    V z11 = d1 + d7, z12 = d1 - d7;
    V o7 = z11 + z13;
    V t11 = (z11 - z13) * 1.414213562f;
    V z5 = (z10 + z12) * 1.847759065f;
    V t10 = z12 * 1.082392200f - z5;
    V t12 = z10 * -2.613125930f + z5;
    V o6 = t12 - o7;
    V o5 = t11 - o6;
    V o4 = t10 + o5;

    d0 = e0 + o7;
    d7 = e0 - o7;
    d1 = e1 + o6;
    d6 = e1 - o6;
    d2 = e2 + o5;
    d5 = e2 - o5;
    d4 = e3 + o4;
    d3 = e3 - o4;
}

}  // namespace
