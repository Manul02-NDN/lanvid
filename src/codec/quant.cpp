#include "codec/quant.hpp"

#include <cmath>

#include "common/platform.hpp"

namespace lv {

namespace {

constexpr uint8_t kStdLuma[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,
    24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99};

constexpr uint8_t kStdChroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99};

double aan_scale(int k) {
    if (k == 0) return 1.0;
    return std::cos(double(k) * 3.14159265358979323846 / 16.0) * std::sqrt(2.0);
}

void fill_derived(QuantTables& out) {
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < 64; ++i) {
            const int q = out.q[t][i];
            const double a = aan_scale(i >> 3) * aan_scale(i & 7);
            out.fwd[t][i] = float(1.0 / (double(q) * a * 8.0));
            out.inv[t][i] = float(double(q) * a / 8.0);

            // 2-step mulhrs_epi16 reciprocal for 16 * q (exact 15-bit mantissa across q=1..255):
            //   target = 1 / (16 * q) = (recip / 32768) * (shift / 32768)
            //   Pick k in [1..14] so f = (1 << k) / (16 * q) lies in [0.5, 1.0).
            const double target = 1.0 / (16.0 * double(q));
            int k = 0;
            double f = target;
            while (f * 2.0 < 1.0 && k < 14) {
                f *= 2.0;
                ++k;
            }
            int recip = int(std::llround(f * 32768.0));
            if (recip > 32767) recip = 32767;
            out.llm_recip[t][i] = int16_t(recip);
            out.llm_shift[t][i] = int16_t(k == 0 ? 32767 : (1 << (15 - k)));
            out.llm_deq[t][i] = int16_t(q * 2);
        }
    }
}

}  // namespace

void build_quant_tables(int quality, QuantTables& out) {
    quality = clamp_val(quality, 1, 100);
    out.quality = quality;
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    const uint8_t* base[2] = {kStdLuma, kStdChroma};
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < 64; ++i) {
            out.q[t][i] = uint16_t(clamp_val((int(base[t][i]) * scale + 50) / 100, 1, 255));
        }
    }
    fill_derived(out);
}

void build_unit_quant_tables(QuantTables& out) {
    out.quality = 100;
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < 64; ++i) out.q[t][i] = 1;
    }
    fill_derived(out);
}

}  // namespace lv
