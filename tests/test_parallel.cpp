#include "mini_test.hpp"

#include <atomic>
#include <cstring>
#include <vector>

#include "app/test_pattern.hpp"
#include "codec/frame_codec.hpp"
#include "parallel/thread_pool.hpp"

TEST(thread_pool_parallel_for_and_async) {
    lv::ThreadPool pool(8);

    // 1. parallel_for visits every element exactly once across many iterations.
    constexpr uint32_t kN = 1000;
    std::vector<std::atomic<int>> hits(kN);
    for (int iter = 0; iter < 50; ++iter) {
        for (auto& h : hits) h.store(0, std::memory_order_relaxed);
        pool.parallel_for(kN, [&](uint32_t i, uint32_t slot) {
            CHECK(slot < pool.slot_count());
            hits[i].fetch_add(1, std::memory_order_relaxed);
        });
        for (uint32_t i = 0; i < kN; ++i) CHECK(hits[i].load() == 1);
    }

    // 2. Async enqueue + wait_idle runs all tasks with valid worker slots.
    std::atomic<int> sum{0};
    for (int i = 1; i <= 500; ++i) {
        pool.enqueue([&, i](uint32_t slot) {
            CHECK(slot < pool.num_threads());
            sum.fetch_add(i, std::memory_order_relaxed);
        });
    }
    pool.wait_idle();
    CHECK(sum.load() == 500 * 501 / 2);
}

TEST(frame_codec_parallel_matches_sequential_bit_exact) {
    lv::Image src;
    src.allocate(1920, 1080);
    lv::generate_test_frame(src, 123);

    lv::CodecParams p;
    p.width = 1920;
    p.height = 1080;
    p.chroma = lv::ChromaFormat::k420;
    p.quality = 80;
    p.slice_height = 16;

    lv::FrameCodec seq_codec, par_codec;
    lv::EncodedFrame enc_seq, enc_par;
    REQUIRE(seq_codec.encode(src, p, enc_seq, nullptr));

    lv::ThreadPool pool(8);
    REQUIRE(par_codec.encode(src, p, enc_par, &pool));
    REQUIRE(enc_seq.slices.size() == enc_par.slices.size());
    for (size_t i = 0; i < enc_seq.slices.size(); ++i) {
        CHECK(enc_seq.slices[i] == enc_par.slices[i]);
    }

    // Streaming encode callback must produce identical bytes for every slice.
    std::vector<std::vector<uint8_t>> streamed(enc_seq.slices.size());
    REQUIRE(par_codec.encode_streaming(
        src, p,
        [&](int slice_id, int total_slices, const uint8_t* data, size_t size) {
            CHECK(total_slices == int(enc_seq.slices.size()));
            streamed[size_t(slice_id)].assign(data, data + size);
        },
        &pool));
    for (size_t i = 0; i < enc_seq.slices.size(); ++i) {
        CHECK(streamed[i] == enc_seq.slices[i]);
    }

    // Parallel decode must match sequential decode bit-for-bit.
    lv::Image dec_seq, dec_par;
    const auto r_seq = seq_codec.decode(enc_seq, dec_seq, nullptr);
    const auto r_par = par_codec.decode(enc_par, dec_par, &pool);
    CHECK(r_seq.slices_failed == 0 && r_par.slices_failed == 0);
    CHECK(dec_seq.data == dec_par.data);
}
