// Rate integrity at 48 kHz: the stream neither slows down nor doubles, and a sine survives every stage
// without discontinuities. Guards against the classic causes of "crackling AND slowed-down" audio:
//   * 48 kHz samples played at a 44.1 kHz device rate (8.8 % slow, periodic dropouts),
//   * per-block resampling that restarts its phase / discards frames every callback,
//   * zeros pushed into a resampler on underrun (gap + permanent delay),
//   * a mixer timer that truncates its period or skips time,
//   * two consumers advancing one playout cursor (double speed + constant re-anchoring).
#include "mini_test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/device_rate_adapter.hpp"
#include "audio/multi_device_matrix.hpp"
#include "audio/planar_ring.hpp"
#include "common/clock.hpp"
#include "lanvid_c_api.h"
#include "lanvid/lanvid_sdk.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 997.0;  // co-prime with every block size: no lucky alignment

// Frequency from rising zero crossings (linear interpolation), over the whole buffer.
double estimate_freq(const std::vector<float>& x, double rate) {
    double first = -1.0, last = -1.0;
    int n = 0;
    for (size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]);
            if (first < 0.0) first = t;
            last = t;
            ++n;
        }
    }
    return n > 1 ? double(n - 1) * rate / (last - first) : 0.0;
}

// THD+N (dB) of a sine: joint least-squares fit of sin + cos + DC at the best frequency near `f0`
// (0.01 Hz grid), residual energy vs fitted-sine energy.
double thd_n_db(const std::vector<float>& x, double rate, double f0) {
    double best = 1e9;
    const double n = double(x.size());
    for (int k = -100; k <= 100; ++k) {
        const double f = f0 + 0.01 * double(k);
        const double w = 2.0 * kPi * f / rate;
        // Normal equations for basis [s, c, 1].
        double ss = 0, sc = 0, s1 = 0, cc = 0, c1 = 0, xs = 0, xc = 0, x1 = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double s = std::sin(w * double(i)), c = std::cos(w * double(i));
            ss += s * s; sc += s * c; s1 += s; cc += c * c; c1 += c;
            xs += x[i] * s; xc += x[i] * c; x1 += x[i];
        }
        const double M[3][3] = {{ss, sc, s1}, {sc, cc, c1}, {s1, c1, n}};
        const double r[3] = {xs, xc, x1};
        auto det3 = [](const double m[3][3]) {
            return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        };
        const double D = det3(M);
        if (std::fabs(D) < 1e-12) continue;
        double coef[3];
        for (int j = 0; j < 3; ++j) {  // Cramer's rule
            double Mj[3][3];
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) Mj[a][b] = (b == j) ? r[a] : M[a][b];
            coef[j] = det3(Mj) / D;
        }
        double res = 0, sig = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double tone = coef[0] * std::sin(w * double(i)) + coef[1] * std::cos(w * double(i));
            const double e = x[i] - tone - coef[2];
            res += e * e;
            sig += tone * tone;
        }
        best = std::min(best, 10.0 * std::log10(std::max(res, 1e-30) / std::max(sig, 1e-30)));
    }
    return best;
}

// Worst THD+N over consecutive windows (a slow, legitimate drift correction of a few ppm moves the phase
// over a whole second; short windows isolate real distortion: dropouts, doubled or missing blocks).
double worst_windowed_thd_n_db(const std::vector<float>& x, double rate, double f0, size_t win) {
    double worst = -200.0;
    for (size_t off = 0; off + win <= x.size(); off += win) {
        std::vector<float> w(x.begin() + long(off), x.begin() + long(off + win));
        worst = std::max(worst, thd_n_db(w, rate, f0));
    }
    return worst;
}

double max_step(const std::vector<float>& x) {
    double m = 0.0;
    for (size_t i = 1; i < x.size(); ++i) m = std::max(m, double(std::fabs(x[i] - x[i - 1])));
    return m;
}

}  // namespace

// ---- Part 1: real time, whole chain (sender -> UDP -> receiver -> MultiDeviceAudioMatrix -> sink) ----
TEST(audio_realtime_rate_integrity_48khz) {
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kBlk = 240;  // 5 ms "device" blocks on the sender side
    constexpr int kCh = 2;

    // Receiver: Fixed 30 ms, popped ONLY by the matrix network slot (no meter pump, no other puller).
    LvReceiverHandle* rx = lv_receiver_create();
    REQUIRE(rx);
    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.latency_ms = 30;
    bus.num_channels = kCh;
    REQUIRE(lv_receiver_configure_audio_bus(rx, &bus) == LV_OK);
    REQUIRE(lv_receiver_set_adaptive_jitter(rx, 0) == LV_OK);
    LV_ReceiverConfig rc;
    lv_sdk_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "127.0.0.1");
    rc.bind_port = 0;
    rc.enable_discovery = 0;
    REQUIRE(lv_sdk_receiver_start(rx, &rc) == LV_OK);

    LvSenderHandle* tx = lv_sender_create();
    REQUIRE(tx);
    lv_sender_configure_audio_bus(tx, &bus);
    LV_SenderConfig sc;
    lv_sdk_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "127.0.0.1");
    sc.dest_port = lv_receiver_get_local_port(rx);
    REQUIRE(lv_sdk_sender_start(tx, &sc) == LV_OK);

    // Matrix: slot 0 = network AoIP input, slot 1 = a device-less sink (its out ring is read below).
    lv::MultiDeviceAudioMatrix mx;
    lv::MatrixSlotConfig net;
    net.id = 0;
    net.type = lv::MatrixSlotType::kNetworkAoip;
    net.role = lv::MatrixSlotRole::kInput;
    net.channels = kCh;
    net.active = true;
    REQUIRE(mx.add_slot(net) == 0);
    lv::MatrixSlotConfig sink;
    sink.id = 1;
    sink.type = lv::MatrixSlotType::kExternalAsio;  // no driver named: the matrix just fills the out ring
    sink.role = lv::MatrixSlotRole::kOutput;
    sink.channels = kCh;
    sink.active = true;
    REQUIRE(mx.add_slot(sink) == 1);
    CHECK(mx.set_crosspoint(0, 0, 1, 0, 0.0f) == lv::CrosspointResult::kOk);
    CHECK(mx.set_crosspoint(0, 1, 1, 1, 0.0f) == lv::CrosspointResult::kOk);
    mx.set_buffer_size(256);
    std::atomic<uint64_t> pops{0};
    mx.set_network_io(nullptr, [&](float* const* planes, uint32_t ch, uint32_t frames) {
        std::vector<float> tmp(size_t(ch) * frames);
        if (lv_receiver_pop_audio_multichannel(rx, tmp.data(), uint16_t(ch), frames, LV_AUDIO_LAYOUT_PLANAR) < 0)
            std::fill(tmp.begin(), tmp.end(), 0.0f);
        for (uint32_t c = 0; c < ch; ++c) std::copy(tmp.begin() + c * frames, tmp.begin() + (c + 1) * frames, planes[c]);
        pops.fetch_add(frames);
    });

    // Producer: a 997 Hz sine at exactly 48 kHz of wall time, 5 ms blocks, LV_MEDIA_TS_AUTO.
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        std::vector<std::vector<float>> pl(kCh, std::vector<float>(kBlk));
        const float* pp[kCh] = {pl[0].data(), pl[1].data()};
        uint64_t n = 0;
        double next = double(lv::now_us());
        while (!stop.load()) {
            for (uint32_t i = 0; i < kBlk; ++i) {
                const float v = 0.5f * float(std::sin(2.0 * kPi * kToneHz * double(n + i) / kRate));
                pl[0][i] = v;
                pl[1][i] = v;
            }
            n += kBlk;
            lv_sender_push_audio_multichannel(tx, pp, kCh, kBlk, LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR,
                                              LV_MEDIA_TS_AUTO);
            next += double(kBlk) * 1e6 / kRate;
            lv::sleep_until_us(uint64_t(next), [] {});
        }
    });

    REQUIRE(mx.start());
    // Drain the sink continuously; keep everything after a 2.5 s warm-up (MasterClock skew converged,
    // 30 ms buffer filled).
    std::vector<float> out;
    out.reserve(kRate * 3);
    std::vector<std::vector<float>> rd(kCh, std::vector<float>(1024));
    float* rp[kCh] = {rd[0].data(), rd[1].data()};
    const uint64_t t_start = lv::now_us();
    // Rate measurement on the mixer's own block boundaries: (time, frames) at the first block completed
    // after warm-up and at the first one completed >= 1.0 s later -> frames per exactly 1.0 s.
    uint64_t f_a = 0, t_a = 0, f_b = 0, t_b = 0, last_f = mx.frames_mixed();
    int ch_mismatch = 0;
    while (lv::now_us() - t_start < 4'000'000) {
        const uint32_t got = mx.extract_slot_output(1, rp, kCh, 1024);
        const bool warm = lv::now_us() - t_start > 2'500'000;
        if (warm)
            for (uint32_t i = 0; i < got; ++i) {
                out.push_back(rd[0][i]);
                if (rd[0][i] != rd[1][i]) ++ch_mismatch;
            }
        const uint64_t f = mx.frames_mixed();
        if (f != last_f) {
            const uint64_t t = lv::now_us();
            if (warm && t_a == 0) { t_a = t; f_a = f; }
            else if (t_a && !t_b && t - t_a >= 1'000'000) { t_b = t; f_b = f; }
            last_f = f;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    mx.stop();
    stop.store(true);
    producer.join();
    uint64_t epoch_resets = 0;
    lv_receiver_get_stream_epoch_info(rx, &epoch_resets, nullptr, nullptr);
    lv_sender_destroy(tx);
    lv_receiver_destroy(rx);

    REQUIRE(t_b > t_a && t_a > 0);
    const double frames_per_sec = double(f_b - f_a) * 1e6 / double(t_b - t_a);
    // Only real audio: trim leading silence (before the 30 ms buffer filled).
    size_t first = 0;
    while (first < out.size() && std::fabs(out[first]) < 1e-4f) ++first;
    std::vector<float> sig(out.begin() + long(std::min(first + 480, out.size())), out.end());
    if (sig.size() > kRate) sig.resize(kRate);  // analyse exactly 1.0 s of output
    const double f_est = estimate_freq(sig, kRate);
    const double thdn = sig.size() >= kRate / 2 ? worst_windowed_thd_n_db(sig, kRate, kToneHz, kRate / 10) : 0.0;
    const double step = max_step(sig);
    size_t zero_run = 0, cur = 0;
    for (float v : sig) { cur = std::fabs(v) < 1e-6f ? cur + 1 : 0; zero_run = std::max(zero_run, cur); }

    std::printf("    [rate 48k] mixer %.1f frames/1.0 s | output %zu samples: tone %.2f Hz (sent %.0f) THD+N %.1f dB "
                "max step %.4f longest zero run %zu | ch mismatch %d | matrix pulls %llu\n",
                frames_per_sec, sig.size(), f_est, kToneHz, thdn, step, zero_run, ch_mismatch,
                (unsigned long long)pops.load());
    CHECK(std::fabs(frames_per_sec - 48000.0) <= 50.0);  // exactly 48 kHz of wall time
    CHECK(sig.size() >= kRate - 512);                    // 1.0 s of real audio was delivered
    CHECK(std::fabs(f_est - kToneHz) < 0.5);             // no slow-down (44.1 mismatch would read 916 Hz)
    CHECK(thdn < -55.0);                                 // clean sine (a gap / doubled block reads ~ -20 dB)
    CHECK(step < 0.08);                                  // 997 Hz @ 0.5 moves <= 0.066 per sample
    CHECK(zero_run < 4);                                 // no gaps
    CHECK(ch_mismatch == 0);
    CHECK(epoch_resets == 0);
}

// ---- Part 2: device side at 44.1 kHz (+ crystal drift), deterministic virtual time ----
TEST(audio_device_rate_adapter_44k1_render_capture) {
    constexpr uint32_t kMix = 256;      // matrix block @ 48 kHz
    constexpr uint32_t kDev = 512;      // device callback @ 44.1 kHz
    constexpr double kDevNominal = 44100.0;
    constexpr double kDevActual = 44100.0 * (1.0 + 120e-6);  // device crystal runs 120 ppm fast

    // Render: matrix writes exact 48 kHz blocks, device pulls 512-frame callbacks at its own clock.
    lv::LockFreePlanarRing ring;
    ring.configure(2);
    lv::DeviceRateAdapter ad;
    ad.configure(2, 4096, kMix);
    std::vector<float> m0(kMix), d0(kDev), d1(kDev);
    float* dp[2] = {d0.data(), d1.data()};
    std::vector<float> dev_out;
    uint64_t mix_frames = 0, dev_frames = 0, real_frames = 0;
    double t_mix = 0.0, t_dev = 0.0;
    double max_trim = 0.0;
    const double T = 20.0;  // seconds of virtual time
    while (t_mix < T || t_dev < T) {
        if (t_mix <= t_dev) {
            for (uint32_t i = 0; i < kMix; ++i)
                m0[i] = 0.5f * float(std::sin(2.0 * kPi * kToneHz * double(mix_frames + i) / 48000.0));
            const float* mp[2] = {m0.data(), m0.data()};
            ring.write(mp, 2, kMix);
            mix_frames += kMix;
            t_mix = double(mix_frames) / 48000.0;
        } else {
            real_frames += ad.render(ring, dp, 2, kDev, kDevNominal);
            if (t_dev > 2.0) dev_out.insert(dev_out.end(), d0.begin(), d0.end());  // after priming/settling
            if (t_dev > 2.0) max_trim = std::max(max_trim, std::fabs(ad.trim_ppm()));
            dev_frames += kDev;
            t_dev = double(dev_frames) / kDevActual;
        }
    }
    std::vector<float> one_sec(dev_out.begin(), dev_out.begin() + long(std::min<size_t>(44100, dev_out.size())));
    const double f_r = estimate_freq(dev_out, kDevNominal);
    const double thd_r = worst_windowed_thd_n_db(one_sec, kDevNominal, kToneHz, 4410);
    const double step_r = max_step(dev_out);
    const uint64_t under_r = ad.underruns();

    // Capture: device delivers 44.1 kHz (+120 ppm) callbacks, the matrix pulls exact 48 kHz blocks.
    lv::LockFreePlanarRing cring;
    cring.configure(2);
    lv::DeviceRateAdapter ac;
    ac.configure(2, 4096, kMix);
    std::vector<float> cap48;
    uint64_t dframes = 0, mframes = 0, mreal = 0;
    double td = 0.0, tm = 0.0;
    std::vector<float> c0(kDev), r0(kMix), r1(kMix);
    float* rp[2] = {r0.data(), r1.data()};
    uint64_t f_at_2 = 0, f_at_12 = 0;
    while (td < T || tm < T) {
        if (td <= tm) {
            for (uint32_t i = 0; i < kDev; ++i)
                c0[i] = 0.5f * float(std::sin(2.0 * kPi * kToneHz * double(dframes + i) / kDevActual));
            const float* cp[2] = {c0.data(), c0.data()};
            ac.capture(cring, cp, 2, kDev, kDevNominal);
            dframes += kDev;
            td = double(dframes) / kDevActual;
        } else {
            const uint32_t got = cring.read_primed(rp, 2, kMix, ac.target_fill(kDevNominal));
            mreal += got;
            if (tm > 2.0) cap48.insert(cap48.end(), r0.begin(), r0.end());
            mframes += kMix;
            tm = double(mframes) / 48000.0;
            if (!f_at_2 && tm >= 2.0) f_at_2 = mreal;
            if (!f_at_12 && tm >= 12.0) f_at_12 = mreal;
        }
    }
    std::vector<float> cap_sec(cap48.begin(), cap48.begin() + long(std::min<size_t>(48000, cap48.size())));
    const double f_c = estimate_freq(cap48, 48000.0);
    const double thd_c = worst_windowed_thd_n_db(cap_sec, 48000.0, kToneHz, 4800);
    const double per_sec = double(f_at_12 - f_at_2) / 10.0;  // average over 10 s (256-frame granularity)

    std::printf("    [44.1k render] tone %.2f Hz THD+N %.1f dB max step %.4f underruns %llu trim max %.0f ppm | "
                "[44.1k capture] tone %.2f Hz THD+N %.1f dB, %.0f real frames/s @48k, reprimes %llu\n",
                f_r, thd_r, step_r, (unsigned long long)under_r, max_trim, f_c, thd_c, per_sec,
                (unsigned long long)cring.reprimes());
    // Render: a 48 kHz tone played on a 44.1 kHz device keeps its pitch (8.8 % slow would read 916 Hz);
    // the device crystal is 120 ppm fast, so in device-nominal time the tone reads 120 ppm high.
    CHECK(std::fabs(f_r - kToneHz * (1.0 + 120e-6)) < 0.3);
    CHECK(thd_r < -45.0);       // linear-interpolation SRC, no discontinuities
    CHECK(step_r < 0.08);
    CHECK(under_r <= 1);        // at most the initial priming
    CHECK(max_trim <= 500.0 + 1e-9);
    // Capture: 48000 +/- 50 real frames per second into the matrix, pitch kept.
    CHECK(std::fabs(per_sec - 48000.0) <= 50.0);
    CHECK(std::fabs(f_c - kToneHz) < 0.3);
    CHECK(thd_c < -45.0);
    CHECK(cring.reprimes() == 0);
}

TEST(udp_sender_receiver_audio_only_bitrate_and_routing) {
    // Verifies real Audio-Only UDP flow:
    //   TX MultiDeviceAudioMatrix -> Network Audio Tx -> lv_sender_push_audio_multichannel -> UDP ->
    //   lv_receiver_pop_audio_multichannel -> RX MultiDeviceAudioMatrix
    // and checks that BOTH TX and RX bitrate/audio_kbps telemetry counters report > 0 Mbps and audio arrives.
    constexpr uint16_t kPort = 56920;
    constexpr int kCh = 8;
    constexpr uint32_t kBlk = 256;

    LvReceiverHandle* rx = lv_receiver_create();
    LvSenderHandle* tx = lv_sender_create();
    CHECK(rx != nullptr && tx != nullptr);

    LvReceiverConfig rcfg{};
    lv_receiver_config_default(&rcfg);
    rcfg.bind_port = kPort;
    rcfg.enable_audio = 1;
    rcfg.enable_discovery = 0;
    rcfg.audio_only = 1;

    LV_AudioBusConfig bus{};
    lv_audio_bus_config_default(&bus);
    bus.num_channels = kCh;
    bus.latency_ms = 20;
    lv_receiver_configure_audio_bus(rx, &bus);
    CHECK(lv_receiver_start(rx, &rcfg) == 1);

    LvSenderConfig scfg{};
    lv_sender_config_default(&scfg);
    std::snprintf(scfg.dest_ip, sizeof(scfg.dest_ip), "127.0.0.1");
    scfg.dest_port = kPort;
    scfg.audio_only = 1;
    scfg.audio_codec = 0;  // PCM32F (~12.3 Mbps for 8ch @ 48 kHz)
    lv_sender_configure_audio_bus(tx, &bus);
    CHECK(lv_sender_start(tx, &scfg) == 1);

    // TX Matrix: slot 0 (virtual source) -> crosspoints 1:1 -> slot 1 (Network AoIP TX)
    lv::MultiDeviceAudioMatrix tx_mx;
    tx_mx.set_buffer_size(kBlk);
    tx_mx.add_slot({0, lv::MatrixSlotType::kNetworkAoip, lv::MatrixSlotRole::kInput, "Test Src", "", -1, kCh, kBlk, true});
    tx_mx.add_slot({1, lv::MatrixSlotType::kNetworkAoip, lv::MatrixSlotRole::kOutput, "Net TX", "", -1, kCh, kBlk, true});
    for (int c = 0; c < kCh; ++c) tx_mx.set_crosspoint(0, c, 1, c, 0.0f, false, true);

    std::atomic<uint64_t> src_phase{0};
    std::atomic<uint64_t> tx_pushes{0};
    tx_mx.set_network_io(
        [&](const float* const* planes, uint32_t ch, uint32_t frames) {
            lv_sender_push_audio_multichannel(tx, planes, uint16_t(std::min<uint32_t>(ch, kCh)), frames,
                                              LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR, LV_MEDIA_TS_AUTO);
            tx_pushes.fetch_add(1, std::memory_order_relaxed);
        },
        [&](float* const* planes, uint32_t ch, uint32_t frames) {
            const uint64_t p0 = src_phase.fetch_add(frames, std::memory_order_relaxed);
            for (uint32_t c = 0; c < ch; ++c) {
                for (uint32_t i = 0; i < frames; ++i) {
                    planes[c][i] = 0.35f * float(std::sin(2.0 * kPi * 440.0 * double(p0 + i) / 48000.0));
                }
            }
        });

    // RX Matrix: slot 0 (Network AoIP RX) -> crosspoints 1:1 -> slot 1 (Monitor Out)
    lv::MultiDeviceAudioMatrix rx_mx;
    rx_mx.set_buffer_size(kBlk);
    rx_mx.add_slot({0, lv::MatrixSlotType::kNetworkAoip, lv::MatrixSlotRole::kInput, "Net RX", "", -1, kCh, kBlk, true});
    rx_mx.add_slot({1, lv::MatrixSlotType::kNetworkAoip, lv::MatrixSlotRole::kOutput, "Mon Out", "", -1, kCh, kBlk, true});
    for (int c = 0; c < kCh; ++c) rx_mx.set_crosspoint(0, c, 1, c, 0.0f, false, true);

    std::atomic<float> rx_peak{0.0f};
    rx_mx.set_network_io(
        [&](const float* const* planes, uint32_t ch, uint32_t frames) {
            float pk = 0.0f;
            for (uint32_t c = 0; c < std::min<uint32_t>(ch, kCh); ++c)
                for (uint32_t i = 0; i < frames; ++i) pk = std::max(pk, std::fabs(planes[c][i]));
            float prev = rx_peak.load(std::memory_order_relaxed);
            while (pk > prev && !rx_peak.compare_exchange_weak(prev, pk, std::memory_order_relaxed)) {}
        },
        [&](float* const* planes, uint32_t ch, uint32_t frames) {
            static thread_local std::vector<float> tmp;
            const uint32_t nch = std::min<uint32_t>(ch, kCh);
            tmp.resize(size_t(nch) * frames);
            if (lv_receiver_pop_audio_multichannel(rx, tmp.data(), uint16_t(nch), frames, LV_AUDIO_LAYOUT_PLANAR) < 0)
                std::fill(tmp.begin(), tmp.end(), 0.0f);
            for (uint32_t c = 0; c < nch; ++c)
                std::copy(tmp.begin() + size_t(c) * frames, tmp.begin() + size_t(c + 1) * frames, planes[c]);
        });

    CHECK(tx_mx.start());
    CHECK(rx_mx.start());

    LvSenderStats tx_st{};
    LvReceiverStats rx_st{};
    for (int i = 0; i < 14; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        lv_sender_get_stats(tx, &tx_st);
        lv_receiver_get_stats(rx, &rx_st);
    }

    tx_mx.stop();
    rx_mx.stop();
    lv_sender_stop(tx);
    lv_receiver_stop(rx);
    lv_sender_destroy(tx);
    lv_receiver_destroy(rx);

    std::printf("    [audio-only bitrate] TX=%.2f Mbps (%.0f kbps, pkts=%llu) | RX=%.2f Mbps (%.0f kbps, pkts=%llu) | rx_peak=%.3f\n",
                tx_st.tx_mbps, tx_st.audio_kbps, (unsigned long long)tx_st.audio_packets_sent,
                rx_st.rx_mbps, rx_st.audio_kbps, (unsigned long long)rx_st.audio_packets_rx,
                double(rx_peak.load()));

    CHECK(tx_pushes.load() > 50);
    CHECK(tx_st.tx_mbps > 1.0);
    CHECK(tx_st.audio_kbps > 1000.0);
    CHECK(tx_st.audio_packets_sent > 50);
    CHECK(rx_st.rx_mbps > 1.0);
    CHECK(rx_st.audio_kbps > 1000.0);
    CHECK(rx_st.audio_packets_rx > 50);
    CHECK(rx_peak.load() > 0.10f);
}
