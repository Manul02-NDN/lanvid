// Multi-threaded 1080p & 4K (3840x2160) codec benchmark: sweeps 1..N threads, scalar vs AVX2.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "app/test_pattern.hpp"
#include "codec/dct.hpp"
#include "codec/frame_codec.hpp"
#include "common/clock.hpp"
#include "common/cpu_features.hpp"
#include "common/image.hpp"
#include "parallel/thread_pool.hpp"

namespace {

struct BenchResult {
    double enc_avg_ms = 0;
    double enc_p95_ms = 0;
    double dec_avg_ms = 0;
    double dec_p95_ms = 0;
    size_t bytes = 0;
    double psnr_y = 0;
};

BenchResult measure(const lv::Image& src, int quality, uint32_t threads, int iters) {
    lv::CodecParams p;
    p.width = src.width;
    p.height = src.height;
    p.chroma = lv::ChromaFormat::k420;
    p.quality = quality;
    p.slice_height = 16;

    lv::FrameCodec codec;
    lv::ThreadPool pool(threads);
    lv::ThreadPool* pp = threads > 1 ? &pool : nullptr;

    lv::EncodedFrame enc;
    lv::Image dst;
    dst.allocate(src.width, src.height, src.order);

    for (int i = 0; i < 4; ++i) {
        codec.encode(src, p, enc, pp);
        codec.decode(enc, dst, pp);
    }

    std::vector<double> enc_t(static_cast<size_t>(iters), 0.0);
    std::vector<double> dec_t(static_cast<size_t>(iters), 0.0);
    double enc_sum = 0, dec_sum = 0;
    for (int i = 0; i < iters; ++i) {
        const uint64_t t0 = lv::now_us();
        codec.encode(src, p, enc, pp);
        const uint64_t t1 = lv::now_us();
        codec.decode(enc, dst, pp);
        const uint64_t t2 = lv::now_us();
        enc_t[size_t(i)] = double(t1 - t0) / 1000.0;
        dec_t[size_t(i)] = double(t2 - t1) / 1000.0;
        enc_sum += enc_t[size_t(i)];
        dec_sum += dec_t[size_t(i)];
    }

    std::sort(enc_t.begin(), enc_t.end());
    std::sort(dec_t.begin(), dec_t.end());
    const size_t p95_idx = size_t(iters * 95 / 100);

    BenchResult r;
    r.enc_avg_ms = enc_sum / double(iters);
    r.enc_p95_ms = enc_t[std::min(p95_idx, enc_t.size() - 1)];
    r.dec_avg_ms = dec_sum / double(iters);
    r.dec_p95_ms = dec_t[std::min(p95_idx, dec_t.size() - 1)];
    r.bytes = enc.total_bytes();
    r.psnr_y = lv::psnr_luma(src, dst);
    return r;
}

void bench_resolution(const char* name, int w, int h, int quality, int iters, bool force_no_simd) {
    lv::Image src;
    src.allocate(w, h, lv::PixelOrder::kRGBA);
    lv::generate_test_frame(src, 42);
    lv::swap_rb_inplace(src);  // BGRA native order

    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    std::printf("\n=== %s (%dx%d BGRA 4:2:0, Q=%d, %d slices), HW threads=%u, AVX2=%s ===\n",
                name, w, h, quality, (h + 15) / 16, hw, lv::cpu_has_avx2() ? "yes" : "no");
    std::printf("%-7s %-7s | %-18s | %-18s | %-8s | %-7s | %-7s\n", "Kernel", "Threads",
                "Encode avg (p95)", "Decode avg (p95)", "Enc FPS", "Speedup", "Size");
    std::printf("------------------------------------------------------------------------------------------\n");

    std::vector<uint32_t> counts = {1, 4, 8};
    if (hw > 8) counts.push_back(hw);
    counts.erase(std::remove_if(counts.begin(), counts.end(), [&](uint32_t c) { return c > hw; }),
                 counts.end());

    double scalar_1t = 0;
    for (bool use_simd : {false, true}) {
        if (use_simd && (force_no_simd || !lv::cpu_has_avx2())) continue;
        lv::set_simd_disabled(!use_simd);
        for (uint32_t t : counts) {
            const auto r = measure(src, quality, t, iters);
            if (!use_simd && t == 1) scalar_1t = r.enc_avg_ms;
            std::printf("%-7s %-7u | %6.2f ms (%5.2f) | %6.2f ms (%5.2f) | %7.0f | %5.2fx  | %5.0f KB\n",
                        lv::dct_kernels_best().name, t, r.enc_avg_ms, r.enc_p95_ms,
                        r.dec_avg_ms, r.dec_p95_ms, 1000.0 / r.enc_avg_ms, scalar_1t / r.enc_avg_ms,
                        double(r.bytes) / 1024.0);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool force_no_simd = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-simd") == 0) force_no_simd = true;
    }
    bench_resolution("1080p", 1920, 1080, 75, 40, force_no_simd);
    bench_resolution("4K UHD", 3840, 2160, 75, 25, force_no_simd);
    return 0;
}
