#pragma once
// Quantization tables (JPEG Annex K base tables, IJG quality scaling) with:
//  - float AAN scale factors folded in (scalar path);
//  - 16-bit integer LLM reciprocal multipliers & dequant factors (AVX2 __m256i path).

#include <cstdint>

namespace lv {

struct QuantTables {
    int quality = 0;
    uint16_t q[2][64];          // [0]=luma, [1]=chroma; natural order
    float fwd[2][64];           // 1 / (q * aan_r * aan_c * 8) -> scalar float AAN encoder
    float inv[2][64];           // q * aan_r * aan_c / 8       -> scalar float AAN decoder
    alignas(32) int16_t llm_recip[2][64];  // Q15 mantissa in [16384, 32767] for 2-step mulhrs quant
    alignas(32) int16_t llm_shift[2][64];  // 1 << (15 - k) second mulhrs factor
    alignas(32) int16_t llm_deq[2][64];    // 2 * q[t][i] for int16 LLM IDCT
};

// quality is clamped to [1, 100]
void build_quant_tables(int quality, QuantTables& out);

// Builds q==1 unit tables (unquantized FDCT -> IDCT testing).
void build_unit_quant_tables(QuantTables& out);

}  // namespace lv
