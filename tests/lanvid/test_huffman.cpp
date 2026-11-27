#include "mini_test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "codec/dct.hpp"
#include "codec/entropy.hpp"
#include "codec/huffman.hpp"
#include "common/bitio.hpp"

namespace {

uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

}  // namespace

TEST(huffman_all_symbols_roundtrip) {
    const lv::StdHuffman& H = lv::std_huffman();

    for (int t = 0; t < 2; ++t) {
        // Test every DC symbol (0..11) and every AC symbol (162 symbols), including 16-bit slow-path codes.
        std::vector<uint8_t> buf(4096);
        lv::BitWriter bw(buf.data(), buf.size());
        for (int i = 0; i < H.dc_spec[t].count; ++i) {
            const uint8_t sym = H.dc_spec[t].vals[i];
            REQUIRE(H.enc_dc[t].len[sym] > 0);
            bw.put(H.enc_dc[t].code[sym], H.enc_dc[t].len[sym]);
        }
        for (int i = 0; i < H.ac_spec[t].count; ++i) {
            const uint8_t sym = H.ac_spec[t].vals[i];
            REQUIRE(H.enc_ac[t].len[sym] > 0);
            bw.put(H.enc_ac[t].code[sym], H.enc_ac[t].len[sym]);
        }
        bw.flush();

        lv::BitReader br(buf.data(), bw.size());
        for (int i = 0; i < H.dc_spec[t].count; ++i) {
            br.refill();
            CHECK(lv::huff_decode(br, H.dec_dc[t]) == H.dc_spec[t].vals[i]);
        }
        for (int i = 0; i < H.ac_spec[t].count; ++i) {
            br.refill();
            CHECK(lv::huff_decode(br, H.dec_ac[t]) == H.ac_spec[t].vals[i]);
        }
        CHECK(!br.overrun());
    }
}

TEST(entropy_block_roundtrip_edge_cases) {
    const lv::StdHuffman& H = lv::std_huffman();

    // Build a suite of blocks: all-zero, max DC swings, multi-ZRL, last-coeff-at-63, dense max amplitude, random.
    std::vector<std::array<int16_t, 64>> blocks;
    auto push_zero = [&]() -> std::array<int16_t, 64>& {
        blocks.push_back({});
        return blocks.back();
    };

    push_zero();                                 // 0: all zeros
    push_zero()[0] = lv::kMaxCoef;               // 1: +1023 DC
    push_zero()[0] = -lv::kMaxCoef;              // 2: -1023 DC (diff = -2046, size = 11!)
    push_zero()[0] = lv::kMaxCoef;               // 3: +1023 DC (diff = +2046, size = 11!)
    {
        auto& b = push_zero();
        b[63] = -1;                              // 4: 3 ZRLs + run 14 + coeff at zigzag[63] (no EOB!)
    }
    {
        auto& b = push_zero();
        for (int i = 0; i < 64; ++i) b[i] = int16_t((i & 1) ? lv::kMaxCoef : -lv::kMaxCoef);
    }

    uint32_t rng = 0xCAFEBABEu;
    for (int n = 0; n < 300; ++n) {
        auto& b = push_zero();
        b[0] = int16_t(int(xorshift32(rng) % 2047u) - 1023);
        const int density = (n % 5) + 1;  // sparse to dense
        for (int i = 1; i < 64; ++i) {
            if (int(xorshift32(rng) % 10u) < density) {
                b[i] = int16_t(int(xorshift32(rng) % 2047u) - 1023);
            }
        }
    }

    for (int t = 0; t < 2; ++t) {
        std::vector<uint8_t> buf(blocks.size() * 256);
        lv::BitWriter bw(buf.data(), buf.size());
        int enc_pred = 0;
        for (const auto& b : blocks) {
            lv::encode_block(bw, b.data(), enc_pred, H.enc_dc[t], H.enc_ac[t]);
        }
        bw.flush();

        lv::BitReader br(buf.data(), bw.size());
        int dec_pred = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            int16_t out[64]{};
            REQUIRE(lv::decode_block(br, out, dec_pred, H.dec_dc[t], H.dec_ac[t]));
            CHECK(std::memcmp(out, blocks[i].data(), sizeof(out)) == 0);
        }
        CHECK(!br.overrun());
    }
}

TEST(entropy_rejects_corrupt_streams) {
    const lv::StdHuffman& H = lv::std_huffman();

    // 1. All-ones stream -> invalid 16-bit Huffman code (0xFFFF is unused in JPEG Annex K).
    const uint8_t all_ones[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    {
        lv::BitReader br(all_ones, sizeof(all_ones));
        int pred = 0;
        int16_t out[64]{};
        CHECK(!lv::decode_block(br, out, pred, H.dec_dc[0], H.dec_ac[0]));
    }

    // 2. Four consecutive ZRLs in one block (k would exceed 63) -> must fail.
    {
        uint8_t buf[64];
        lv::BitWriter bw(buf, sizeof(buf));
        bw.put(H.enc_dc[0].code[0], H.enc_dc[0].len[0]);
        for (int i = 0; i < 4; ++i) bw.put(H.enc_ac[0].code[0xF0], H.enc_ac[0].len[0xF0]);
        bw.flush();

        lv::BitReader br(buf, bw.size());
        int pred = 0;
        int16_t out[64]{};
        CHECK(!lv::decode_block(br, out, pred, H.dec_dc[0], H.dec_ac[0]));
    }
}
