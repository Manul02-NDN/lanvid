// 64-channel AoIP tests: virtual-time network simulation (deterministic, no sockets, no sleeps).
#include "mini_test.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "audio/audio_reliability.hpp"
#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/audio_tx.hpp"
#include "audio/master_clock.hpp"
#include "net/packet.hpp"

namespace {

constexpr uint32_t kRate = 48000;
constexpr int64_t kMasterOffsetUs = 5'000'000;  // master clock = local clock + 5 s

struct InFlight {
    int dir;  // 0 sender->receiver, 1 receiver->sender
    std::vector<uint8_t> data;
};

struct Sim {
    uint64_t now = 1'000'000;  // local (receiver) time in us
    uint64_t step_us = 1000;
    std::multimap<uint64_t, InFlight> q;
    std::mt19937 rng{12345};
    double loss_fwd = 0.0, loss_rev = 0.0;
    std::function<uint64_t(uint16_t stream, uint64_t t)> delay_fwd = [](uint16_t, uint64_t) { return 500u; };
    uint64_t rev_delay_us = 500;
    uint64_t spike_start = 0, spike_end = 0;  // packets sent inside are held until spike_end

    lv::MasterClock clk;
    lv::AudioSyncMatrix mat{kRate};
    lv::AudioTxEngine tx;
    lv::AudioRxEngine rx;
    uint64_t sent_fwd = 0, dropped_fwd = 0;

    Sim()
        : tx(kRate, lv::kDefaultMtu,
             [this](const uint8_t* d, size_t n) { return fwd(d, n); }),
          rx(mat, clk, [this](const uint8_t* d, size_t n) { return rev(d, n); }) {
        clk.set_fixed_offset(kMasterOffsetUs);
    }

    bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }

    bool fwd(const uint8_t* d, size_t n) {
        ++sent_fwd;
        if (chance(loss_fwd)) { ++dropped_fwd; return true; }
        lv::PacketHeader h;
        const uint8_t* pl = nullptr;
        if (lv::parse_packet(d, n, h, &pl) != lv::PacketParseStatus::kOk) return false;
        uint64_t arrive = now + delay_fwd(h.slice_id, now);
        if (spike_end && now >= spike_start && now < spike_end) arrive = spike_end + delay_fwd(h.slice_id, now);
        q.insert({arrive, {0, std::vector<uint8_t>(d, d + n)}});
        return true;
    }
    bool rev(const uint8_t* d, size_t n) {
        if (chance(loss_rev)) return true;
        q.insert({now + rev_delay_us, {1, std::vector<uint8_t>(d, d + n)}});
        return true;
    }

    int64_t master_samples_now() const { return (int64_t(now) + kMasterOffsetUs) * kRate / 1'000'000; }

    void deliver() {
        while (!q.empty() && q.begin()->first <= now) {
            InFlight f = std::move(q.begin()->second);
            q.erase(q.begin());
            lv::PacketHeader h;
            const uint8_t* pl = nullptr;
            if (lv::parse_packet(f.data.data(), f.data.size(), h, &pl) != lv::PacketParseStatus::kOk) continue;
            if (f.dir == 0 && h.type == lv::PacketType::kAudio2) rx.on_packet(h, pl, h.payload_size, now);
            if (f.dir == 1 && h.type == lv::PacketType::kAudioNack) tx.on_nack(pl, h.payload_size);
        }
    }
};

// Sample value encodes its own media position so alignment can be verified bit-exactly.
inline float encode_pos(uint64_t ts) { return float(ts & 0xFFFFF); }

void push_block(Sim& s, uint32_t channels, uint32_t frames, lv::AudioFormat2 fmt = lv::AudioFormat2::kFloat32) {
    const uint64_t ts0 = uint64_t(s.master_samples_now()) - frames;  // samples completed up to "now"
    std::vector<float> buf(frames);
    for (uint32_t i = 0; i < frames; ++i) buf[i] = encode_pos(ts0 + i);
    std::vector<int16_t> b16(frames);
    for (uint32_t c = 0; c < channels; ++c) {
        if (fmt == lv::AudioFormat2::kFloat32) {
            s.tx.push_channel(uint16_t(c), buf.data(), frames, fmt, ts0, uint32_t(s.now));
        } else {
            for (uint32_t i = 0; i < frames; ++i) b16[i] = int16_t(((ts0 + i) % 20000) - 10000);
            s.tx.push_channel(uint16_t(c), b16.data(), frames, fmt, ts0, uint32_t(s.now));
        }
    }
}

}  // namespace

TEST(audio_64ch_independent_streams_phase_sync) {
    Sim s;
    constexpr uint32_t kCh = 64;
    constexpr uint32_t kFrames = 48;  // 1 ms
    s.rx.set_latency_ms(50);
    s.tx.set_latency_ms(50);

    // Independent network delay for every channel: 1 .. 40 ms (+ up to 2 ms jitter).
    std::vector<uint64_t> base(kCh);
    std::mt19937 r(777);
    for (auto& b : base) b = 1000 + (r() % 39001);
    std::mt19937 jr(4242);
    s.delay_fwd = [&](uint16_t st, uint64_t) { return base[st] + (jr() % 2000); };

    uint64_t checked_blocks = 0, divergent_samples = 0, wrong_samples = 0;
    std::vector<float> out(size_t(kCh) * kFrames);

    const int steps = 2500;  // 2.5 s
    for (int step = 0; step < steps; ++step) {
        s.now += s.step_us;
        s.deliver();
        push_block(s, kCh, kFrames);
        s.rx.service(s.now);

        const int64_t lat = int64_t(50) * kRate / 1000;
        const int64_t expect0 = s.master_samples_now() - lat;
        s.mat.pop_interleaved(s.clk, s.now, out.data(), kCh, kFrames);
        if (s.now > 1'000'000 + 400'000) {  // skip pipeline warm-up (longest path 42 ms + chunk)
            ++checked_blocks;
            for (uint32_t i = 0; i < kFrames; ++i) {
                const float ref = out[size_t(i) * kCh + 0];
                for (uint32_t c = 1; c < kCh; ++c) {
                    if (out[size_t(i) * kCh + c] != ref) ++divergent_samples;
                }
                if (ref != encode_pos(uint64_t(expect0) + i)) ++wrong_samples;
            }
        }
    }
    uint64_t underruns = 0, lost = 0;
    for (uint16_t c = 0; c < kCh; ++c) {
        underruns += s.mat.meter(c).underruns;
        lost += s.rx.channel_stats(c).packets_lost;
        CHECK(s.mat.meter(c).active);
    }
    std::printf("    [64ch sync] blocks=%llu divergent_samples=%llu wrong=%llu underruns=%llu lost=%llu\n",
                (unsigned long long)checked_blocks, (unsigned long long)divergent_samples,
                (unsigned long long)wrong_samples, (unsigned long long)underruns, (unsigned long long)lost);
    CHECK(checked_blocks > 1000);
    CHECK(divergent_samples == 0);  // all 64 channels aligned to the sample
    CHECK(wrong_samples == 0);      // and exactly at T_master_now - latency
    CHECK(underruns == 0);
    CHECK(lost == 0);
}

TEST(audio_latency_scaling_reliability_2ms_to_2000ms) {
    // --- Phase A: 1000 ms budget, 15 % loss both ways, 300 ms network spike -----------------
    {
        Sim s;
        constexpr uint32_t kCh = 16, kFrames = 48;
        s.loss_fwd = 0.15;
        s.loss_rev = 0.15;
        s.rx.set_latency_ms(1000);
        s.tx.set_latency_ms(1000);
        std::mt19937 jr(99);
        s.delay_fwd = [&](uint16_t, uint64_t) { return 1500 + (jr() % 2000); };
        s.spike_start = 1'000'000 + 3'000'000;
        s.spike_end = s.spike_start + 300'000;

        std::vector<float> out(size_t(kCh) * kFrames);
        uint64_t wrong = 0, blocks = 0;
        for (int step = 0; step < 8000; ++step) {  // 8 s
            s.now += s.step_us;
            s.deliver();
            push_block(s, kCh, kFrames);
            s.rx.service(s.now);
            const int64_t expect0 = s.master_samples_now() - int64_t(1000) * kRate / 1000;
            s.mat.pop_interleaved(s.clk, s.now, out.data(), kCh, kFrames);
            if (s.now > 1'000'000 + 1'200'000) {
                ++blocks;
                for (uint32_t i = 0; i < kFrames; ++i)
                    for (uint32_t c = 0; c < kCh; ++c)
                        if (out[size_t(i) * kCh + c] != encode_pos(uint64_t(expect0) + i)) ++wrong;
            }
        }
        uint64_t underruns = 0, lost = 0, fec = 0, nack = 0;
        for (uint16_t c = 0; c < kCh; ++c) {
            underruns += s.mat.meter(c).underruns;
            const auto st = s.rx.channel_stats(c);
            lost += st.packets_lost;
            fec += st.fec_recovered;
            nack += st.nack_recovered;
        }
        std::printf("    [1000ms/15%% loss/300ms spike] dropped=%llu/%llu fec_rec=%llu nack_rec=%llu lost=%llu "
                    "underruns=%llu wrong_samples=%llu\n",
                    (unsigned long long)s.dropped_fwd, (unsigned long long)s.sent_fwd,
                    (unsigned long long)fec, (unsigned long long)nack, (unsigned long long)lost,
                    (unsigned long long)underruns, (unsigned long long)wrong);
        CHECK(s.dropped_fwd > 1000);  // the loss simulation really happened
        CHECK(fec > 0);
        CHECK(nack > 0);
        CHECK(blocks > 5000);
        CHECK(wrong == 0);  // 100 % of samples bit-exact
        CHECK(underruns == 0);
        CHECK(lost == 0);
    }

    // --- Phase B: live latency changes 2 ms -> 500 ms -> 5 ms (no reconnect) -----------------
    {
        Sim s;
        s.step_us = 500;
        constexpr uint32_t kCh = 8, kFrames = 24;
        s.delay_fwd = [](uint16_t, uint64_t) { return 250u; };
        s.rev_delay_us = 250;
        s.rx.set_latency_ms(2);
        s.tx.set_latency_ms(2);
        std::vector<float> out(size_t(kCh) * kFrames);
        uint64_t base_underruns = 0;
        bool baseline_taken = false;
        uint64_t resyncs_before = 0;
        for (int step = 0; step < 6000; ++step) {  // 3 s
            const uint64_t t_rel = s.now - 1'000'000;
            s.now += s.step_us;
            if (t_rel == 1'000'000) { s.rx.set_latency_ms(500); s.tx.set_latency_ms(500); }
            if (t_rel == 2'000'000) { s.rx.set_latency_ms(5); s.tx.set_latency_ms(5); }
            s.deliver();
            push_block(s, kCh, kFrames);
            s.rx.service(s.now);
            s.mat.pop_interleaved(s.clk, s.now, out.data(), kCh, kFrames);
            if (!baseline_taken && t_rel > 300'000) {
                baseline_taken = true;
                for (uint16_t c = 0; c < kCh; ++c) base_underruns += s.mat.meter(c).underruns;
                resyncs_before = s.mat.resync_count();
            }
            if (t_rel == 2'800'000) {
                // Playout lag behind newest data must now be ~5 ms (+ chunk/network).
                const double lag_ms =
                    (double(s.mat.newest_media_ts()) - s.mat.playout_cursor()) * 1000.0 / kRate;
                std::printf("    [latency 2->500->5] final lag=%.2f ms\n", lag_ms);
                CHECK(lag_ms > 3.0 && lag_ms < 12.0);
            }
        }
        uint64_t u = 0;
        for (uint16_t c = 0; c < kCh; ++c) u += s.mat.meter(c).underruns;
        std::printf("    [latency 2->500->5] underruns after settle=%llu resyncs=%llu\n",
                    (unsigned long long)(u - base_underruns),
                    (unsigned long long)(s.mat.resync_count() - resyncs_before));
        CHECK(u == base_underruns);
        CHECK(s.mat.resync_count() - resyncs_before >= 2);  // two live latency switches took effect
        CHECK(s.mat.target_latency_ms() == 5);
    }
}

TEST(audio_master_clock_ptp_filter_converges) {
    lv::MasterClock clk;
    std::mt19937 r(5);
    const double skew = 50e-6;  // master runs 50 ppm fast vs local
    const double off0 = 7'300'000.0;
    auto master_of = [&](double local) { return local + off0 + skew * (local - 1e6); };
    double max_err = 0;
    for (int i = 0; i < 150; ++i) {
        const double t1 = 1e6 + i * 200'000.0;
        double d1 = 150 + r() % 500, d2 = 150 + r() % 500;
        if (r() % 10 == 0) d1 += 4000;  // queueing spike
        const double t2m = master_of(t1 + d1);
        const double t3m = t2m + 40;
        const double t4 = t1 + d1 + 40 + d2;
        clk.on_exchange(uint64_t(t1), uint64_t(t2m), uint64_t(t3m), uint64_t(t4));
        if (i > 40) {
            const double probe = t4 + 50'000;
            max_err = std::max(max_err, std::fabs(double(clk.master_us(uint64_t(probe))) - master_of(probe)));
        }
    }
    std::printf("    [master clock] max error=%.1f us skew=%.1f ppm min_rtt=%u us\n", max_err,
                clk.skew_ppm(), clk.min_rtt_us());
    CHECK(clk.synced());
    CHECK(max_err < 150.0);  // << 1 sample would be 20.8 us at 48 kHz; < 7 samples worst case
    CHECK(std::fabs(clk.skew_ppm() - 50.0) < 25.0);
}

TEST(audio_reliability_profiles_follow_latency) {
    using lv::AudioReliabilityMode;
    const auto u = lv::audio_profile_for_latency(2, kRate, 1400, lv::AudioFormat2::kPcm16);
    CHECK(u.mode == AudioReliabilityMode::kUltraLow && u.chunk_samples == 24 && u.nack_attempts == 0);
    const auto b = lv::audio_profile_for_latency(50, kRate, 1400, lv::AudioFormat2::kPcm16);
    CHECK(b.mode == AudioReliabilityMode::kBalanced && b.chunk_samples == 240 && b.fec_k >= 2 && b.fec_k <= 4 &&
          b.nack_attempts == 1);
    const auto h = lv::audio_profile_for_latency(2000, kRate, 1400, lv::AudioFormat2::kPcm16);
    CHECK(h.mode == AudioReliabilityMode::kHighReliability && h.nack_attempts >= 5 &&
          h.fec_interleave > 1 && h.allow_duplication);
    CHECK(h.chunk_samples * 2 + 56 <= 1400);  // fits the MTU
    CHECK(lv::audio_profile_for_latency(1, kRate, 1400, lv::AudioFormat2::kPcm16).latency_ms == 2);
}

TEST(audio_v2_wire_roundtrip) {
    lv::AudioHeader2 a;
    a.stream_id = 63; a.channel_index = 17; a.sample_rate = 96000; a.num_samples = 48;
    a.sample_format = lv::AudioFormat2::kPcm24Packed; a.flags = lv::kAudioFlagRetransmit;
    a.media_timestamp_samples = 0x0123456789ABCDEFull; a.wallclock_us = 0xCAFEBABE;
    uint8_t buf[lv::kAudioHeader2Size + 48 * 3] = {};
    lv::write_audio_header2(buf, a);
    lv::AudioHeader2 b;
    CHECK(lv::parse_audio_header2(buf, sizeof(buf), b));
    CHECK(b.stream_id == 63 && b.channel_index == 17 && b.sample_rate == 96000 && b.num_samples == 48);
    CHECK(b.media_timestamp_samples == a.media_timestamp_samples && b.wallclock_us == a.wallclock_us);
    CHECK(!lv::parse_audio_header2(buf, sizeof(buf) - 1, b));  // truncated payload rejected
    buf[0] = 64;  // stream id out of range
    CHECK(!lv::parse_audio_header2(buf, sizeof(buf), b));

    lv::AudioNack n; n.stream_id = 5; n.base_seq = 0xFFFFFFF0u; n.mask = 0x8000000000000001ull;
    uint8_t nb[lv::kAudioNackSize];
    lv::write_audio_nack(nb, n);
    lv::AudioNack m;
    CHECK(lv::parse_audio_nack(nb, sizeof(nb), m) && m.base_seq == n.base_seq && m.mask == n.mask);
}

#include <thread>
#include "lanvid/lanvid_sdk.h"

TEST(audio_64ch_c_api_udp_loopback) {
    // Real sockets: 8-channel interleaved push -> master-clock sync over PING/PONG -> aligned pull.
    LvReceiverHandle* rx = lv_receiver_create();
    LvSenderHandle* tx = lv_sender_create();
    REQUIRE(rx && tx);

    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    // 50 ms: this loop paces itself with sleep_until(5 ms), which lasts ~15.6 ms at the default Windows
    // timer resolution, so pushes arrive in bursts; 20 ms left single-channel packets late on CI runners.
    bus.latency_ms = 50;
    REQUIRE(lv_receiver_configure_audio_bus(rx, &bus) == LV_OK);
    REQUIRE(lv_sender_configure_audio_bus(tx, &bus) == LV_OK);

    LV_ReceiverConfig rc;
    lv_sdk_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "127.0.0.1");
    rc.bind_port = 0;
    rc.enable_discovery = 0;
    REQUIRE(lv_sdk_receiver_start(rx, &rc) == LV_OK);

    LV_SenderConfig sc;
    lv_sdk_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "127.0.0.1");
    sc.dest_port = lv_receiver_get_local_port(rx);
    REQUIRE(lv_sdk_sender_start(tx, &sc) == LV_OK);

    constexpr int kCh = 8, kFrames = 240;
    std::vector<float> in(size_t(kCh) * kFrames), out(size_t(kCh) * kFrames);
    int divergent = 0, nonsilent = 0, blocks = 0;
    uint64_t pos = 0;
    auto next_tick = std::chrono::steady_clock::now();
    for (int b = 0; b < 400; ++b) {  // 2 s real time, 5 ms blocks, absolute-deadline pacing
        next_tick += std::chrono::milliseconds(5);
        std::this_thread::sleep_until(next_tick);
        for (int i = 0; i < kFrames; ++i)
            for (int c = 0; c < kCh; ++c) in[size_t(i) * kCh + c] = 0.5f * std::sin(0.05f * float(pos + i));
        pos += kFrames;
        REQUIRE(lv_sender_push_audio_multichannel(tx, in.data(), kCh, kFrames, LV_AUDIO_FORMAT_F32,
                                                  LV_AUDIO_LAYOUT_INTERLEAVED, LV_MEDIA_TS_AUTO) == LV_OK);
        REQUIRE(lv_receiver_pop_audio_multichannel(rx, out.data(), kCh, kFrames, LV_AUDIO_LAYOUT_INTERLEAVED) ==
                kFrames);
        if (b > 300) {
            ++blocks;
            bool any = false;
            for (int i = 0; i < kFrames; ++i) {
                for (int c = 1; c < kCh; ++c)
                    // A one-sample skew of this tone is ~2.5e-2; 2e-3 only tolerates the resampler's
                    // interpolation rounding at the newest-data edge on loaded CI runners.
                    if (std::fabs(out[size_t(i) * kCh + c] - out[size_t(i) * kCh]) > 2e-3f) ++divergent;
                if (std::fabs(out[size_t(i) * kCh]) > 0.01f) any = true;
            }
            if (any) ++nonsilent;
        }
    }
    LV_AudioChannelStats st{};
    CHECK(lv_receiver_get_channel_stats(rx, 3, &st) == LV_OK);
    std::printf("    [C API 8ch loopback] blocks=%d nonsilent=%d divergent=%d rx=%llu lost=%llu level=%.1f dBFS\n",
                blocks, nonsilent, divergent, (unsigned long long)st.packets_rx,
                (unsigned long long)st.packets_lost, st.level_dbfs);
    CHECK(st.packets_rx > 100);
    CHECK(st.active == 1);
    CHECK(st.level_dbfs > -20.0f);
    CHECK(blocks > 50 && nonsilent > blocks / 2);
    CHECK(divergent == 0);
    // Input validation & live latency change.
    CHECK(lv_receiver_set_audio_latency_ms(rx, 1) == LV_ERR_INVALID_ARG);
    CHECK(lv_receiver_set_audio_latency_ms(rx, 2000) == LV_OK);
    CHECK(lv_sender_push_audio_channel(tx, 64, in.data(), 8, LV_AUDIO_FORMAT_F32, LV_MEDIA_TS_AUTO) ==
          LV_ERR_INVALID_ARG);
    lv_sender_destroy(tx);
    lv_receiver_destroy(rx);
}

TEST(audio_64ch_wifi_jitter_phase_lock) {
    using namespace lv;
    constexpr uint32_t kCh = 64;
    constexpr uint32_t kBlk = 240;         // 5 ms blocks
    constexpr uint32_t kTotalBlocks = 400; // 2.0 s of virtual time

    // 1. Verify PCM24 wire packing saves ~25% bandwidth vs Float32 across 64 channels
    uint64_t bytes_f32 = 0, bytes_pcm24 = 0;
    {
        AudioTxEngine tx_f32(kRate, kDefaultMtu, [&](const uint8_t*, size_t len) { bytes_f32 += len; return true; });
        AudioTxEngine tx_p24(kRate, kDefaultMtu, [&](const uint8_t*, size_t len) { bytes_pcm24 += len; return true; });
        tx_f32.set_latency_ms(120);
        tx_p24.set_latency_ms(120);
        tx_p24.set_wire_pcm24(true);
        std::vector<float> blk(kBlk, 0.25f);
        for (uint32_t b = 0; b < 20; ++b) {
            for (uint16_t c = 0; c < kCh; ++c) {
                tx_f32.push_channel(c, blk.data(), kBlk, AudioFormat2::kFloat32, uint64_t(b) * kBlk, b * 5000);
                tx_p24.push_channel(c, blk.data(), kBlk, AudioFormat2::kFloat32, uint64_t(b) * kBlk, b * 5000);
            }
        }
        tx_f32.flush(100000);
        tx_p24.flush(100000);
    }
    const double bw_ratio = double(bytes_pcm24) / double(std::max<uint64_t>(bytes_f32, 1));
    CHECK(bw_ratio < 0.82);

    // 2. 64ch Wi-Fi 6 simulation: 10..120 ms per-packet jitter + 5% packet loss + PCM24
    Sim sim;
    std::mt19937 jrng(0x64C4F1F1u);
    std::uniform_int_distribution<uint64_t> jdist(10000, 120000);  // 10..120 ms Wi-Fi jitter
    sim.loss_fwd = 0.05;
    sim.delay_fwd = [&](uint16_t, uint64_t) { return jdist(jrng); };
    sim.tx.set_latency_ms(150);
    sim.tx.set_wire_pcm24(true);
    sim.rx.set_latency_ms(150);
    sim.mat.set_adaptive_jitter(true, 5, 200);
    sim.mat.set_atomic_frame_lock(true);

    std::vector<float> in_blk(kBlk);
    std::vector<std::vector<float>> out_planes(kCh, std::vector<float>(kBlk, 0.0f));
    float* out_ptrs[kCh];
    for (uint32_t c = 0; c < kCh; ++c) out_ptrs[c] = out_planes[c].data();

    uint64_t divergent_ch1_ch64 = 0;
    uint64_t nonsilent_samples = 0;
    double max_diff = 0.0;

    for (uint32_t b = 0; b < kTotalBlocks; ++b) {
        const uint64_t media_ts = uint64_t(sim.master_samples_now());
        for (uint32_t i = 0; i < kBlk; ++i) {
            in_blk[i] = 0.5f * std::sin(0.06f * float(media_ts + i));
        }
        for (uint16_t c = 0; c < kCh; ++c) {
            sim.tx.push_channel(c, in_blk.data(), kBlk, AudioFormat2::kFloat32, media_ts, uint32_t(sim.now));
        }
        sim.now += 5000;  // advance 5 ms
        sim.deliver();
        sim.rx.service(sim.now);

        sim.mat.pop_planar(sim.clk, sim.now, out_ptrs, kCh, kBlk);
        if (b >= 80) {  // after initial 400 ms warmup
            for (uint32_t i = 0; i < kBlk; ++i) {
                const float ch1 = out_planes[0][i];
                const float ch64 = out_planes[kCh - 1][i];
                const double diff = std::fabs(double(ch1) - double(ch64));
                if (diff > max_diff) max_diff = diff;
                if (diff > 1e-4) ++divergent_ch1_ch64;
                if (std::fabs(ch1) > 0.01f && std::fabs(ch64) > 0.01f) ++nonsilent_samples;
            }
        }
    }

    std::printf("    [64ch Wi-Fi 10-120ms jitter / 5%% loss / PCM24] bw_ratio=%.3f eff_lat=%ums resyncs=%llu "
                "nonsilent=%llu max_diff_ch1_ch64=%.2e divergent=%llu (dropped=%llu/%llu)\n",
                bw_ratio, sim.mat.effective_latency_ms(), (unsigned long long)sim.mat.resync_count(),
                (unsigned long long)nonsilent_samples, max_diff, (unsigned long long)divergent_ch1_ch64,
                (unsigned long long)sim.dropped_fwd, (unsigned long long)sim.sent_fwd);

    CHECK(nonsilent_samples > 10000);
    CHECK(divergent_ch1_ch64 == 0);  // Strictly 0-sample phase skew between Channel 1 and Channel 64!
    CHECK(sim.mat.effective_latency_ms() >= 5 && sim.mat.effective_latency_ms() <= 200);
}
