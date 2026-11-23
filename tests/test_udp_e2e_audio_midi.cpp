// End-to-end over a REAL local UDP socket: 64 channels of audio + MIDI2 Note On/Off in one stream,
// a live codec change, and a sender restart (new stream epoch) while the receiver keeps running.
// Checks MIDI-to-audio sync (< 1.5 ms), 64-channel phase alignment, no stuck notes, the epoch reset,
// plus the hardening knobs (DSCP EF, >= 2 MB socket buffers, real-time thread policy).
#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "common/rt_thread.hpp"
#include "lanvid/lanvid_sdk.h"
#include "lanvid_c_api.h"
#include "net/receiver.hpp"
#include "net/sender.hpp"

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlk = 64;           // 1.33 ms pull / push blocks
constexpr uint64_t kImpulseEvery = 24000;  // every 0.5 s of media time
constexpr uint64_t kNoteLen = 6000;        // Note Off 125 ms after Note On
constexpr int kCh = 64;

struct MidiRec {
    uint8_t status, d1;
    uint32_t seq;
    int32_t off;
    int64_t g_pos;  // output-stream frame index (block base + offset), -1 if not inside a pop
};

struct Ctx {
    std::mutex mu;
    std::vector<MidiRec> ev;
    int64_t block_base = 0;  // set by the pulling thread right before each pop
};

void on_midi(const LV_MidiEvent* e, void* ud) {
    auto* c = static_cast<Ctx*>(ud);
    std::lock_guard<std::mutex> lk(c->mu);
    c->ev.push_back({e->status, e->data1, e->event_seq, e->block_offset_frames,
                     e->block_offset_frames >= 0 ? c->block_base + e->block_offset_frames : -1});
}

}  // namespace

TEST(udp_end_to_end_64ch_audio_and_midi_sync) {
    // ---- 0. Hardening knobs: DSCP EF, socket buffers, real-time thread policy ----
    {
        lv::ReceiverConfig rcfg;
        rcfg.bind_ip = "127.0.0.1";
        rcfg.bind_port = 0;
        lv::UdpReceiver r(rcfg, nullptr);
        REQUIRE(r.init());
        lv::SenderConfig scfg;
        scfg.dest_ip = "127.0.0.1";
        scfg.dest_port = r.local_port();
        lv::UdpSender s(scfg);
        REQUIRE(s.init());
        std::printf("    [qos] sender tos=0x%02X snd=%d rcv=%d | receiver tos=0x%02X snd=%d rcv=%d | epoch=%u\n",
                    s.socket().tos(), s.socket().send_buffer_bytes(), s.socket().recv_buffer_bytes(), r.socket().tos(),
                    r.socket().send_buffer_bytes(), r.socket().recv_buffer_bytes(), unsigned(s.stream_epoch()));
#if !defined(_WIN32)
        CHECK(s.socket().tos() == 0xB8);  // DSCP 46 / EF (Windows only honours it under QoS policy)
        CHECK(r.socket().tos() == 0xB8);
#endif
#if defined(__APPLE__) || defined(_WIN32)
        CHECK(s.socket().send_buffer_bytes() >= 2 * 1024 * 1024);
        CHECK(r.socket().recv_buffer_bytes() >= 2 * 1024 * 1024);
#endif
        CHECK(s.stream_epoch() != 0);
    }
#if defined(__APPLE__) || defined(_WIN32)
    {
        bool rt = false;
        std::thread t([&] {
            lv::RealtimeThreadScope scope(2667);  // 128 frames @ 48 kHz
            rt = scope.active() && lv::current_thread_is_realtime();
        });
        t.join();
        std::printf("    [rt] time-constraint / MMCSS policy on a 2.67 ms audio thread: %s\n", rt ? "active" : "NOT active");
        CHECK(rt);
    }
#endif

    // ---- 1. Receiver (keeps running across the sender restart) ----
    Ctx ctx;
    LvReceiverHandle* rx = lv_receiver_create();
    REQUIRE(rx);
    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.latency_ms = 50;
    REQUIRE(lv_receiver_configure_audio_bus(rx, &bus) == LV_OK);
    lv_receiver_set_midi_callback(rx, &on_midi, &ctx);
    LV_ReceiverConfig rc;
    lv_sdk_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "127.0.0.1");
    rc.bind_port = 0;
    rc.enable_discovery = 0;
    REQUIRE(lv_sdk_receiver_start(rx, &rc) == LV_OK);
    const uint16_t port = lv_receiver_get_local_port(rx);

    auto make_sender = [&]() {
        LvSenderHandle* tx = lv_sender_create();
        lv_sender_configure_audio_bus(tx, &bus);
        LV_SenderConfig sc;
        lv_sdk_sender_config_default(&sc);
        std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "127.0.0.1");
        sc.dest_port = port;
        return (lv_sdk_sender_start(tx, &sc) == LV_OK) ? tx : nullptr;
    };
    LvSenderHandle* tx = make_sender();
    REQUIRE(tx);

    // ---- 2. Stream: 64 ch audio + MIDI notes on the impulses, codec change, restart ----
    std::vector<std::vector<float>> in(kCh, std::vector<float>(kBlk)), out(kCh, std::vector<float>(kBlk));
    std::vector<const float*> ip(kCh);
    std::vector<float*> op(kCh);
    for (int c = 0; c < kCh; ++c) { ip[c] = in[c].data(); op[c] = out[c].data(); }

    uint64_t ts0 = lv_sender_get_master_clock_samples(tx);
    ts0 -= ts0 % kBlk;
    uint64_t k = 0;  // block index on the current sender timeline
    std::multimap<uint64_t, uint8_t> pending_off;  // media sample -> note
    std::vector<int64_t> impulses;                 // output frame index of every received impulse
    int64_t g = 0;                                 // output frames pulled so far
    int note_seq = 0, restarts = 0, blocks = 0, live = 0, gaps = 0;
    double worst = 0.0;
    int64_t restart_g = -1;

    const int total = int(5.0 * kRate / kBlk);
    const int codec24_at = int(1.5 * kRate / kBlk), codecf_at = int(2.2 * kRate / kBlk);
    const int restart_at = int(3.0 * kRate / kBlk);  // (note 99 hangs from 2.8 s)
    auto next = std::chrono::steady_clock::now();
    for (int b = 0; b < total; ++b) {
        next += std::chrono::nanoseconds(1000000000ull * kBlk / kRate);
        std::this_thread::sleep_until(next);

        if (b == codec24_at) CHECK(lv_sender_set_audio_codec(tx, 1) == LV_OK);  // PCM24 on the fly
        if (b == codecf_at) CHECK(lv_sender_set_audio_codec(tx, 0) == LV_OK);   // back to Float32
        // 200 ms before the restart: a Note On whose Note Off will never be sent (it is sounding when
        // the sender dies). The receiver must release it when the new stream epoch arrives.
        if (b == restart_at - int(0.2 * kRate / kBlk)) lv_sender_push_midi(tx, 0x90, 99, 100, 0, 0);
        if (b == restart_at) {
            lv_sender_destroy(tx);
            tx = make_sender();
            REQUIRE(tx);
            ts0 = lv_sender_get_master_clock_samples(tx);
            ts0 -= ts0 % kBlk;
            k = 0;
            pending_off.clear();
            ++restarts;
            restart_g = g;
        }

        const uint64_t ts = ts0 + k * kBlk;
        for (uint32_t i = 0; i < kBlk; ++i) {
            const float v = 0.4f * std::sin(0.037f * float(ts + i));
            for (int c = 0; c < 63; ++c) in[c][i] = v * float(c + 1) / 64.0f;
            in[63][i] = ((ts + i) % kImpulseEvery == 0 && ts + i > ts0) ? 0.9f : 0.0f;
        }
        lv_sender_push_audio_multichannel(tx, ip.data(), uint16_t(kCh), kBlk, LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR, ts);
        for (uint32_t i = 0; i < kBlk; ++i) {
            if ((ts + i) % kImpulseEvery == 0 && ts + i > ts0) {
                const uint8_t note = uint8_t(60 + (note_seq++ % 12));
                const uint64_t s = ts + i;
                lv_sender_push_midi(tx, 0x90, note, 100, 0, s * 1000000ull / kRate);
                pending_off.insert({s + kNoteLen, note});
            }
        }
        for (auto it = pending_off.begin(); it != pending_off.end() && it->first < ts + kBlk;) {
            lv_sender_push_midi(tx, 0x80, it->second, 0, 0, it->first * 1000000ull / kRate);
            it = pending_off.erase(it);
        }
        ++k;

        {
            std::lock_guard<std::mutex> lk(ctx.mu);
            ctx.block_base = g;
        }
        lv_receiver_pop_audio_multichannel(rx, nullptr, 0, 0, LV_AUDIO_LAYOUT_PLANAR);  // arg check (ignored)
        std::vector<float> flat(size_t(kCh) * kBlk);
        REQUIRE(lv_receiver_pop_audio_multichannel(rx, flat.data(), uint16_t(kCh), kBlk, LV_AUDIO_LAYOUT_PLANAR) == int(kBlk));
        for (int c = 0; c < kCh; ++c) std::copy(flat.begin() + c * kBlk, flat.begin() + (c + 1) * kBlk, out[c].begin());
        for (uint32_t i = 0; i < kBlk; ++i)
            if (out[63][i] > 0.5f) impulses.push_back(g + i);

        const bool settling = b < int(0.4 * kRate / kBlk) || (restart_g >= 0 && g - restart_g < int64_t(0.4 * kRate));
        if (!settling) {
            ++blocks;
            bool any = false, gap = false;
            double bw = 0.0;
            for (uint32_t i = 0; i < kBlk; ++i) {
                const float ref = out[62][i] * 64.0f / 63.0f;
                if (std::fabs(ref) > 0.02f) any = true;
                for (int c = 0; c < 62; ++c) {
                    const float sc = out[c][i] * 64.0f / float(c + 1);
                    if ((std::fabs(ref) > 0.05f && out[c][i] == 0.0f) || (ref == 0.0f && std::fabs(sc) > 0.05f)) gap = true;
                    bw = std::max(bw, double(std::fabs(sc - ref)));
                }
            }
            if (any) ++live;
            if (gap) ++gaps;
            else worst = std::max(worst, bw);
        }
        g += kBlk;
    }
    // Drain: pull ~0.3 s more so the last Note Offs are dispatched.
    for (int b = 0; b < int(0.3 * kRate / kBlk); ++b) {
        next += std::chrono::nanoseconds(1000000000ull * kBlk / kRate);  // paced like an audio callback
        std::this_thread::sleep_until(next);
        {
            std::lock_guard<std::mutex> lk(ctx.mu);
            ctx.block_base = g;
        }
        std::vector<float> flat(size_t(kCh) * kBlk);
        lv_receiver_pop_audio_multichannel(rx, flat.data(), uint16_t(kCh), kBlk, LV_AUDIO_LAYOUT_PLANAR);
        g += kBlk;
    }

    // ---- 3. Evaluate ----
    uint64_t epoch_resets = 0, midi_rx_events = 0;
    uint32_t active = 99;
    CHECK(lv_receiver_get_stream_epoch_info(rx, &epoch_resets, &active, &midi_rx_events) == LV_OK);
    std::vector<MidiRec> ev;
    {
        std::lock_guard<std::mutex> lk(ctx.mu);
        ev = ctx.ev;
    }
    std::map<int, int> balance;  // note -> on - off
    int matched = 0, unmatched_on = 0, synth_off = 0;
    double max_sync_ms = 0.0;
    for (const auto& e : ev) {
        const uint8_t hi = e.status & 0xF0;
        if (hi == 0x90 && e.d1 != 99) {
            ++balance[e.d1];
            int64_t best = INT64_MAX;
            for (int64_t imp : impulses) best = std::min<int64_t>(best, int64_t(std::llabs(imp - e.g_pos)));
            if (e.g_pos >= 0 && best != INT64_MAX && best < int64_t(kRate / 10)) {
                ++matched;
                max_sync_ms = std::max(max_sync_ms, double(best) * 1000.0 / kRate);
            } else {
                ++unmatched_on;
            }
        } else if (hi == 0x90 && e.d1 == 99) {
            ++balance[99];
        } else if (hi == 0x80) {
            --balance[e.d1];
            if (e.seq == 0) ++synth_off;
        }
    }
    int stuck = 0;
    for (const auto& kv : balance) stuck += std::abs(kv.second);

    std::printf("    [e2e udp] blocks=%d live=%d gaps=%d worst_ch_mismatch=%.2e | impulses=%zu midi_events=%zu (rx unique %llu)\n",
                blocks, live, gaps, worst, impulses.size(), ev.size(), (unsigned long long)midi_rx_events);
    std::printf("    [e2e udp] MIDI<->audio: matched=%d unmatched_note_on=%d max_sync=%.3f ms | restarts=%d epoch_resets=%llu "
                "synthesized_note_off=%d stuck=%d active_after=%u\n",
                matched, unmatched_on, max_sync_ms, restarts, (unsigned long long)epoch_resets, synth_off, stuck, active);

    CHECK(impulses.size() >= 6);
    CHECK(matched >= 6);
    CHECK(max_sync_ms < 1.5);          // sample-aligned MIDI vs audio through real UDP
    CHECK(live > blocks * 8 / 10);
    CHECK(gaps <= blocks / 20);
    CHECK(worst < 2e-3);               // 64 channels phase-aligned across the codec change and restart
    CHECK(epoch_resets >= 1);          // sender restart detected
    CHECK(synth_off >= 1);             // the hanging note 99 was released by the epoch reset
    CHECK(stuck == 0);                 // every Note On got its Note Off
    CHECK(active == 0);

    // Real UDP, default Adaptive mode: the measured buffer sits at the 50 ms target (loopback = no
    // jitter), and a live switch to Fixed keeps it there and is reported in the stats.
    const double eff_adaptive = lv_receiver_get_effective_latency_ms(rx);
    CHECK(lv_receiver_set_adaptive_jitter(rx, 0) == LV_OK);
    std::vector<float> tail(size_t(kCh) * kBlk);
    for (int i = 0; i < 40; ++i) {  // paced like an audio callback
        next += std::chrono::nanoseconds(1000000000ull * kBlk / kRate);
        std::this_thread::sleep_until(next);
        lv_receiver_pop_audio_multichannel(rx, tail.data(), kCh, kBlk, LV_AUDIO_LAYOUT_PLANAR);
    }
    LvReceiverStats rs{};
    lv_receiver_get_stats(rx, &rs);
    std::printf("    [e2e udp] actual buffer: adaptive=%.2f ms -> fixed=%.2f ms (target %u ms, mode=%s)\n", eff_adaptive,
                rs.effective_latency_ms, rs.effective_target_ms, rs.adaptive_jitter ? "AUTO" : "FIXED");
    // (upper margin: the measurement includes how late the OS woke this "audio callback" thread)
    CHECK(eff_adaptive > 45.0 && eff_adaptive < 75.0);
    CHECK(rs.adaptive_jitter == 0);
    CHECK(rs.effective_target_ms == 50);
    CHECK(rs.effective_latency_ms > 45.0 && rs.effective_latency_ms < 75.0);

    lv_sender_destroy(tx);
    lv_receiver_destroy(rx);
}
