// Jitter-buffer latency mode: Fixed (Ethernet / live monitoring) vs Adaptive (Wi-Fi), switched live, and
// the measured playout delay current_effective_latency_ms(). Deterministic virtual time, no sockets.
#include "mini_test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "audio/audio_sync_matrix.hpp"
#include "audio/master_clock.hpp"
#include "lanvid/lanvid_sdk.h"
#include "lanvid_c_api.h"

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlk = 240;  // 5 ms
constexpr uint32_t kCh = 8;
constexpr int64_t kOffsetUs = 3'000'000;  // master = local + 3 s
constexpr uint32_t kTargetMs = 20;

// Sender produces one 5 ms block per step (media ts = master position); each block reaches the
// receiver after a per-block network delay. The receiver reports RFC 3550 style jitter to the matrix
// and renders one block per step, exactly like the real audio thread.
struct LatencySim {
    lv::MasterClock clk;
    lv::AudioSyncMatrix mat{kRate};
    uint64_t now = 1'000'000;  // local us
    uint64_t next_block = 0;
    std::multimap<uint64_t, uint64_t> in_flight;  // arrival us -> media ts
    std::mt19937 rng{0xF1DEDu};
    std::vector<std::vector<float>> out = std::vector<std::vector<float>>(kCh, std::vector<float>(kBlk));
    float* outp[kCh];
    double max_abs_step = 0.0;  // largest sample-to-sample jump in channel 0 (click detector)
    float last = 0.0f;
    bool have_last = false;

    LatencySim() {
        clk.set_fixed_offset(kOffsetUs);
        mat.set_target_latency_ms(kTargetMs);
        for (uint32_t c = 0; c < kCh; ++c) outp[c] = out[c].data();
        // Start the timeline at a block boundary of the master clock.
        next_block = uint64_t(clk.master_samples(now, kRate)) / kBlk;
    }

    uint64_t underruns() const {
        uint64_t u = 0;
        for (uint16_t c = 0; c < kCh; ++c) u += mat.meter(c).underruns;
        return u;
    }

    // One 5 ms step. delay_lo..hi = network delay range (ms), jitter_us = what the RX engine reports.
    void step(double delay_lo_ms, double delay_hi_ms, uint32_t jitter_us) {
        now += 5000;
        const uint64_t master = uint64_t(clk.master_samples(now, kRate));
        while ((next_block + 1) * kBlk <= master) {  // blocks completed by the sender
            std::uniform_real_distribution<double> d(delay_lo_ms, delay_hi_ms);
            const uint64_t sent_us = now;  // within this 5 ms step
            in_flight.insert({sent_us + uint64_t(d(rng) * 1000.0), next_block * kBlk});
            ++next_block;
        }
        std::vector<float> blk(kBlk);
        while (!in_flight.empty() && in_flight.begin()->first <= now) {
            const uint64_t ts = in_flight.begin()->second;
            in_flight.erase(in_flight.begin());
            // Smooth 220 Hz tone: any hard discontinuity at the playout cursor shows up as a large step.
            for (uint32_t i = 0; i < kBlk; ++i) blk[i] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * float(ts + i) / float(kRate));
            for (uint16_t c = 0; c < kCh; ++c) mat.write(c, ts, blk.data(), kBlk);
            mat.observe_network_jitter_us(jitter_us);
            mat.observe_arrival_lag_samples(clk.master_samples(now, kRate) - int64_t(ts + kBlk));
        }
        mat.pop_planar(clk, now, outp, kCh, kBlk);
        for (uint32_t i = 0; i < kBlk; ++i) {
            const float v = out[0][i];
            if (have_last && v != 0.0f && last != 0.0f) max_abs_step = std::max(max_abs_step, double(std::fabs(v - last)));
            last = v;
            have_last = true;
        }
    }
    void run(double seconds, double lo, double hi, uint32_t jitter_us) {
        const int n = int(seconds * 200.0);
        for (int i = 0; i < n; ++i) step(lo, hi, jitter_us);
    }
};

}  // namespace

TEST(audio_latency_mode_fixed_vs_adaptive_wifi) {
    LatencySim s;
    // Default of the matrix itself is Fixed (the C ABI / receivers default to Adaptive, checked below).
    CHECK(!s.mat.adaptive_jitter_enabled());
    CHECK(s.mat.current_effective_latency_ms() == 0.0);  // nothing played yet

    // 1) Fixed, calm wired LAN (1..2 ms delay): actual buffer == target, no underruns.
    s.run(2.0, 1.0, 2.0, 200);
    const double fixed_calm = s.mat.current_effective_latency_ms();
    CHECK(s.mat.effective_latency_ms() == kTargetMs);
    CHECK(std::fabs(fixed_calm - double(kTargetMs)) < 1.0);
    CHECK(s.underruns() == 0);

    // 2) Fixed under a Wi-Fi burst (2..32 ms delay, 12 ms jitter): the buffer must NOT float.
    const uint64_t u0 = s.underruns();
    double fixed_min = 1e9, fixed_max = 0.0;
    for (int i = 0; i < 600; ++i) {  // 3 s
        s.step(2.0, 32.0, 12000);
        fixed_min = std::min(fixed_min, s.mat.current_effective_latency_ms());
        fixed_max = std::max(fixed_max, s.mat.current_effective_latency_ms());
    }
    const uint64_t fixed_burst_underruns = s.underruns() - u0;
    CHECK(s.mat.effective_latency_ms() == kTargetMs);
    CHECK(fixed_min > double(kTargetMs) - 1.0 && fixed_max < double(kTargetMs) + 1.0);
    CHECK(fixed_burst_underruns > 0);  // late packets are dropped, latency stays put (by design)

    // 3) Live switch to Adaptive during the same burst: the buffer grows (no reanchor) and absorbs it.
    const uint64_t resync0 = s.mat.resync_count();
    s.mat.set_adaptive_jitter_enabled(true);
    CHECK(s.mat.adaptive_jitter_enabled());
    s.run(10.0, 2.0, 32.0, 12000);
    const uint32_t grown_target = s.mat.effective_latency_ms();
    const double grown_actual = s.mat.current_effective_latency_ms();
    const uint64_t u1 = s.underruns();
    s.run(3.0, 2.0, 32.0, 12000);  // settled: the grown buffer covers the 32 ms worst case
    const uint64_t adaptive_settled_underruns = s.underruns() - u1;
    CHECK(grown_target > kTargetMs + 10 && grown_target <= lv::AudioSyncMatrix::kAdaptiveMaxMs);
    CHECK(std::fabs(grown_actual - double(grown_target)) < 2.0);
    CHECK(adaptive_settled_underruns == 0);
    // Grown by at most a few crossfaded re-anchors (steps > 10 ms), never by > +/-500 ppm stretch.
    const uint64_t grow_resyncs = s.mat.resync_count() - resync0;
    CHECK(grow_resyncs >= 1 && grow_resyncs <= 3);
    const uint64_t resync1 = s.mat.resync_count();

    // 4) Calm again: holds, then glides back to the target through +/-0.05 % micro-resampling.
    double max_fast_ppm = 0.0, min_ppm = 0.0;
    const uint64_t u2 = s.underruns();
    for (int i = 0; i < 200 * 120; ++i) {  // up to 120 s
        s.step(1.0, 2.0, 200);
        max_fast_ppm = std::max(max_fast_ppm, s.mat.resample_ppm());
        min_ppm = std::min(min_ppm, s.mat.resample_ppm());
        if (s.mat.effective_latency_ms() == kTargetMs && std::fabs(s.mat.current_effective_latency_ms() - kTargetMs) < 1.0)
            break;
    }
    CHECK(s.mat.effective_latency_ms() == kTargetMs);
    CHECK(std::fabs(s.mat.current_effective_latency_ms() - double(kTargetMs)) < 1.0);
    CHECK(max_fast_ppm <= 500.0 + 1e-6);         // never faster than +0.05 %
    CHECK(min_ppm >= -500.0 - 1e-6);             // never slower than -0.05 % (ratio >= 0.9995)
    CHECK(s.mat.resync_count() == resync1);      // smooth return, no reanchor
    CHECK(s.underruns() - u2 == 0);

    // 5) Grow again, then switch to Fixed while grown: one crossfaded re-anchor to exactly the target.
    s.run(8.0, 2.0, 32.0, 12000);
    CHECK(s.mat.effective_latency_ms() > kTargetMs + 10);
    const uint64_t resync2 = s.mat.resync_count();
    s.mat.set_adaptive_jitter_enabled(false);
    s.run(0.1, 1.0, 2.0, 200);
    CHECK(s.mat.effective_latency_ms() == kTargetMs);
    CHECK(std::fabs(s.mat.current_effective_latency_ms() - double(kTargetMs)) < 1.0);
    CHECK(s.mat.resync_count() == resync2 + 1);

    // No clicks anywhere: a 220 Hz tone at 0.5 amplitude moves < 0.015 per sample; crossfades and
    // time-stretch must keep every step well below a hard jump.
    CHECK(s.max_abs_step < 0.1);

    std::printf("    [latency mode] fixed: calm=%.2f ms burst=%.2f..%.2f ms (underruns %llu) | adaptive: target=%u ms "
                "actual=%.2f ms settled_underruns=%llu | return max +%.0f ppm | max step=%.4f\n",
                fixed_calm, fixed_min, fixed_max, (unsigned long long)fixed_burst_underruns, grown_target, grown_actual,
                (unsigned long long)adaptive_settled_underruns, max_fast_ppm, s.max_abs_step);
}

TEST(audio_latency_mode_c_abi) {
    LvReceiverConfig rc;
    lv_receiver_config_default(&rc);
    CHECK(rc.adaptive_jitter == 1);  // Adaptive Wi-Fi by default
    LV_DuplexConfig dc;
    lv_duplex_config_default(&dc);
    CHECK(dc.adaptive_jitter == 1);

    CHECK(lv_receiver_set_adaptive_jitter(nullptr, 0) == LV_ERR_INVALID_ARG);
    CHECK(lv_duplex_set_adaptive_jitter(nullptr, 0) == LV_ERR_INVALID_ARG);
    CHECK(lv_receiver_get_effective_latency_ms(nullptr) == 0.0);

    LvReceiverHandle* rx = lv_receiver_create();
    REQUIRE(rx != nullptr);
    CHECK(lv_receiver_set_adaptive_jitter(rx, 0) == LV_OK);  // before start: kept for the next start
    CHECK(lv_receiver_get_effective_latency_ms(rx) == 0.0);
    LvReceiverStats st{};
    lv_receiver_get_stats(rx, &st);
    CHECK(st.adaptive_jitter == 0);
    CHECK(lv_receiver_set_adaptive_jitter(rx, 1) == LV_OK);
    lv_receiver_get_stats(rx, &st);
    CHECK(st.adaptive_jitter == 1);
    lv_receiver_destroy(rx);

    LvDuplexHandle* dx = lv_duplex_create();
    REQUIRE(dx != nullptr);
    CHECK(lv_duplex_set_adaptive_jitter(dx, 0) == LV_OK);
    lv_duplex_destroy(dx);
}
