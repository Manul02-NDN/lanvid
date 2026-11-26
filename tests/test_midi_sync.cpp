#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/audio_tx.hpp"
#include "audio/intercom_engine.hpp"
#include "audio/master_clock.hpp"
#include "lanvid/lanvid_sdk.h"
#include "audio/multi_device_matrix.hpp"
#include "audio/personal_monitor_bus.hpp"
#include "midi/midi_dejitter_queue.hpp"
#include "midi/midi_device_manager.hpp"
#include "midi/midi_sync_engine.hpp"
#include "midi/virtual_midi.hpp"
#include "net/packet.hpp"
#include "net/receiver.hpp"
#include "net/sender.hpp"

TEST(midi_over_ip_audio_sync_and_matrix_routing) {
    using namespace lv;
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kStepFrames = 48;  // 1 ms steps
    constexpr int64_t kMasterOffUs = 2'000'000;

    // ------------------------------------------------------------------
    // Part 1: Audio impulse + MIDI Note On / Note Off under 10..50 ms jitter + packet loss
    // ------------------------------------------------------------------
    MasterClock clk;
    clk.set_fixed_offset(kMasterOffUs);

    AudioSyncMatrix audio_mat(kRate);
    audio_mat.set_target_latency_ms(80);

    std::multimap<uint64_t, std::vector<uint8_t>> net_q;
    std::mt19937 rng(0x4D494449u);
    std::uniform_int_distribution<uint64_t> jitter_us(10000, 50000);  // 10..50 ms jitter

    uint64_t now = 1'000'000;
    AudioRxEngine audio_rx(audio_mat, clk, [](const uint8_t*, size_t) { return true; });
    audio_rx.set_latency_ms(80);

    AudioTxEngine audio_tx(kRate, kDefaultMtu, [&](const uint8_t* d, size_t n) {
        net_q.insert({now + jitter_us(rng), std::vector<uint8_t>(d, d + n)});
        return true;
    });
    audio_tx.set_latency_ms(80);

    MidiRxEngine midi_rx;
    uint32_t midi_pkt_idx = 0;
    MidiTxEngine midi_tx([&](const uint8_t* d, size_t n) {
        ++midi_pkt_idx;
        // Drop the 2nd MIDI packet (carrying initial Note Off!) to verify 16-event redundancy recovery
        if (midi_pkt_idx == 2) return true;
        // Inject a duplicate copy of the 1st packet to verify event_seq deduplication
        net_q.insert({now + jitter_us(rng), std::vector<uint8_t>(d, d + n)});
        if (midi_pkt_idx == 1) {
            net_q.insert({now + jitter_us(rng) + 3000, std::vector<uint8_t>(d, d + n)});
        }
        return true;
    });

    constexpr uint32_t kPulseStep = 100;  // 100 ms into stream
    constexpr uint32_t kNoteOffStep = 150;
    uint64_t audio_pulse_playout_us = 0;
    uint64_t midi_note_on_playout_us = 0;
    uint64_t midi_note_off_playout_us = 0;
    int note_on_count = 0, note_off_count = 0;

    std::vector<float> in_blk(kStepFrames, 0.0f);
    std::vector<float> out_blk(kStepFrames, 0.0f);
    float* out_ptrs[1] = {out_blk.data()};

    for (uint32_t step = 0; step < 350; ++step) {
        const uint64_t master_us = uint64_t(int64_t(now) + kMasterOffUs);
        const uint64_t media_sample = master_us * kRate / 1'000'000;

        std::fill(in_blk.begin(), in_blk.end(), 0.0f);
        if (step == kPulseStep) {
            in_blk[0] = 0.9f;  // sharp audio impulse at exact sample `media_sample`
            midi_tx.push_event(0x90, 60, 127, 0, master_us);  // Note On C4 at exact same MasterClock timestamp
            midi_tx.flush(master_us, uint32_t(now));
        } else if (step == kNoteOffStep) {
            midi_tx.push_event(0x80, 60, 0, 0, master_us);    // Note Off (packet #2 will be dropped!)
            midi_tx.flush(master_us, uint32_t(now));
        } else if (step == kNoteOffStep + 10) {
            // Heartbeat carries recent history -> recovers dropped Note Off cleanly!
            midi_tx.flush(master_us, uint32_t(now), true);
        }

        audio_tx.push_channel(0, in_blk.data(), kStepFrames, AudioFormat2::kFloat32, media_sample, uint32_t(now));

        // Deliver due audio & MIDI datagrams
        while (!net_q.empty() && net_q.begin()->first <= now) {
            auto pkt = std::move(net_q.begin()->second);
            net_q.erase(net_q.begin());
            PacketHeader ph{};
            const uint8_t* pl = nullptr;
            if (parse_packet(pkt.data(), pkt.size(), ph, &pl) != PacketParseStatus::kOk) continue;
            if (ph.type == PacketType::kAudio2) audio_rx.on_packet(ph, pl, ph.payload_size, now);
            else if (ph.type == PacketType::kMidiFrame) midi_rx.on_packet(ph, pl, ph.payload_size);
        }

        // Render audio block and dispatch MIDI synchronized to AudioSyncMatrix playout cursor
        audio_mat.pop_planar(clk, now, out_ptrs, 1, kStepFrames);
        for (uint32_t i = 0; i < kStepFrames; ++i) {
            if (audio_pulse_playout_us == 0 && out_blk[i] > 0.4f) {
                audio_pulse_playout_us = now + (uint64_t(i) * 1'000'000 / kRate);
            }
        }
        const uint64_t playout_media_us = uint64_t(std::max(0.0, audio_mat.playout_cursor()) * 1'000'000.0 / kRate);
        midi_rx.pop_due_events(playout_media_us, [&](const TimedMidiEvent& ev) {
            if (ev.status == 0x90 && ev.data1 == 60) {
                ++note_on_count;
                midi_note_on_playout_us = now;
            } else if (ev.status == 0x80 && ev.data1 == 60) {
                ++note_off_count;
                midi_note_off_playout_us = now;
            }
        });

        now += 1000;  // advance 1 ms
    }

    const int64_t av_midi_delta_us = std::llabs(int64_t(midi_note_on_playout_us) - int64_t(audio_pulse_playout_us));
    const auto mrx_st = midi_rx.stats();
    std::printf("    [MIDI+Audio Sync 10-50ms jitter] delta=%.3f ms | NoteOn=%d NoteOff=%d | deduped=%llu\n",
                double(av_midi_delta_us) / 1000.0, note_on_count, note_off_count,
                (unsigned long long)mrx_st.events_deduped);

    CHECK(audio_pulse_playout_us > 0);
    CHECK(note_on_count == 1);     // deduplicated (sent twice, delivered once)
    CHECK(note_off_count == 1);    // recovered from dropped packet via redundancy ring (no stuck notes!)
    CHECK(mrx_st.events_deduped >= 1);
    CHECK(av_midi_delta_us <= 1000);  // < 1.0 ms MIDI <-> Audio sync even under 50 ms jitter!

    // ------------------------------------------------------------------
    // Part 2: MultiDeviceAudioMatrix MIDI Merge (2 inputs -> 1 dest), Channel Filter & Panic
    // ------------------------------------------------------------------
    MultiDeviceAudioMatrix mx;
    const int kbd = mx.add_midi_slot({0, MidiSlotType::kMidiInput, "Keyboard", "", -1, 0, true});
    const int drums = mx.add_midi_slot({1, MidiSlotType::kMidiInput, "E-Drums", "", -1, 1, true});
    const int net_tx = mx.add_midi_slot({2, MidiSlotType::kNetworkMidiTx, "Net MIDI TX", "", -1, 0, true});
    const int synth = mx.add_midi_slot({3, MidiSlotType::kMidiOutput, "Synth Out", "", -1, 2, true});
    CHECK(kbd == 0 && drums == 1 && net_tx == 2 && synth == 3);

    // Many-to-1 merge: Keyboard + E-Drums -> Net MIDI TX (all channels)
    // Plus 1-to-many split: E-Drums -> Synth Out (channel filter = Ch 10 only)
    CHECK(mx.set_midi_crosspoint(kbd, net_tx, 0, false));
    CHECK(mx.set_midi_crosspoint(drums, net_tx, 0, false));
    CHECK(mx.set_midi_crosspoint(drums, synth, 10, false));

    int net_tx_evs = 0, synth_evs = 0, panic_cc123 = 0;
    mx.set_midi_sink([&](int dst, uint8_t st, uint8_t d1, uint8_t, uint8_t, uint64_t) {
        if (dst == net_tx) ++net_tx_evs;
        if (dst == synth) ++synth_evs;
        if ((st & 0xF0u) == 0xB0u && d1 == 123) ++panic_cc123;
    });

    mx.inject_midi_event(kbd, 0x90, 60, 100, 1000);    // Ch 1 -> net_tx
    mx.inject_midi_event(drums, 0x99, 36, 120, 1005);  // Ch 10 -> net_tx + synth
    mx.inject_midi_event(drums, 0x91, 40, 90, 1010);   // Ch 2 -> net_tx only (filtered out of synth)
    CHECK(net_tx_evs == 3);
    CHECK(synth_evs == 1);

    mx.send_midi_panic(net_tx);
    CHECK(panic_cc123 == 16);  // All 16 channels received CC 123 All Notes Off

}

TEST(virtual_midi_daw_port_visibility_and_loopback) {
    using namespace lv;

    struct Msg {
        uint8_t st, d1, d2;
    };
    std::mutex mu;
    std::vector<Msg> rx_from_daw;
    std::vector<Msg> rx_in_daw;

    VirtualMidiPort vport;
    const bool opened = vport.open("LanVid MIDI", [&](uint8_t st, uint8_t d1, uint8_t d2, uint64_t) {
        std::lock_guard<std::mutex> lk(mu);
        rx_from_daw.push_back({st, d1, d2});
    });

    const auto vst = vport.status();
    std::printf("    [VirtualMIDI] open=%d backend='%s' port='%s'\n",
                opened ? 1 : 0, vst.backend_name.c_str(), vst.daw_port_name.c_str());

    if (opened) {
        CHECK(vport.is_open());
        CHECK(!vst.daw_port_name.empty());
#if defined(__APPLE__)
        // Wait for CoreMIDI MIDIServer to publish the newly created virtual endpoints.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif

        // 1. Verify "LanVid MIDI" is visible in OS MIDI enumeration (same API REAPER / Logic use).
        const auto ins = list_midi_inputs();
        const auto outs = list_midi_outputs();
        int daw_in_idx = -1;
        int daw_out_idx = -1;
        for (const auto& d : ins) {
            if (d.name == vst.daw_port_name) daw_in_idx = d.index;
        }
        for (const auto& d : outs) {
            if (d.name == vst.daw_port_name) daw_out_idx = d.index;
        }
        CHECK(daw_in_idx >= 0);
        CHECK(daw_out_idx >= 0);

        // 2. Simulate a DAW opening "LanVid MIDI" (Input + Output) and test bidirectional Note On/Off/CC!
        MidiInputPort daw_in;
        MidiOutputPort daw_out;
        const bool daw_in_ok = daw_in.open(daw_in_idx, vst.daw_port_name,
                                           [&](uint8_t st, uint8_t d1, uint8_t d2, uint64_t) {
                                               std::lock_guard<std::mutex> lk(mu);
                                               rx_in_daw.push_back({st, d1, d2});
                                           });
        const bool daw_out_ok = daw_out.open(daw_out_idx, vst.daw_port_name);
        CHECK(daw_in_ok);
        CHECK(daw_out_ok);

#if defined(__APPLE__)
        // CoreMIDI registers virtual endpoints & MIDIPortConnectSource asynchronously with MIDIServer.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif

        // LanVid Studio -> DAW ("LanVid MIDI" Input)
        CHECK(vport.send_to_daw(0x90, 60, 100));  // Note On C4
        CHECK(vport.send_to_daw(0xB0, 74, 64));   // CC 74 Filter Cutoff
        CHECK(vport.send_to_daw(0x80, 60, 0));    // Note Off C4

        // DAW ("LanVid MIDI" Output) -> LanVid Studio
        CHECK(daw_out.send_short(0x91, 64, 110));  // Note On E4 Ch 2
        CHECK(daw_out.send_short(0x81, 64, 0));    // Note Off E4 Ch 2

        // Wait up to 500 ms for OS MIDI delivery
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (rx_in_daw.size() >= 3 && rx_from_daw.size() >= 2) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        daw_in.close();
        daw_out.close();

        {
            std::lock_guard<std::mutex> lk(mu);
            std::printf("    [VirtualMIDI loopback] LanVid->DAW=%zu msgs | DAW->LanVid=%zu msgs\n",
                        rx_in_daw.size(), rx_from_daw.size());
            CHECK(rx_in_daw.size() >= 3);
            if (rx_in_daw.size() >= 3) {
                CHECK(rx_in_daw[0].st == 0x90 && rx_in_daw[0].d1 == 60 && rx_in_daw[0].d2 == 100);
                CHECK(rx_in_daw[1].st == 0xB0 && rx_in_daw[1].d1 == 74 && rx_in_daw[1].d2 == 64);
                CHECK(rx_in_daw[2].st == 0x80 && rx_in_daw[2].d1 == 60 && rx_in_daw[2].d2 == 0);
            }

            CHECK(rx_from_daw.size() >= 2);
            if (rx_from_daw.size() >= 2) {
                CHECK(rx_from_daw[0].st == 0x91 && rx_from_daw[0].d1 == 64 && rx_from_daw[0].d2 == 110);
                CHECK(rx_from_daw[1].st == 0x81 && rx_from_daw[1].d1 == 64 && rx_from_daw[1].d2 == 0);
            }
        }
    }

    vport.close();
    CHECK(!vport.is_open());

    // 3. Verify MultiDeviceAudioMatrix kVirtualDawMidi slot integration
    MultiDeviceAudioMatrix mx;
    const int net_rx = mx.add_midi_slot({0, MidiSlotType::kNetworkMidiRx, "Net MIDI RX", "", -1, 0, true});
    const int daw_slot = mx.add_midi_slot(
        {1, MidiSlotType::kVirtualDawMidi, "LanVid MIDI (DAW)", "LanVid MIDI", -1, 0, true});
    CHECK(net_rx == 0 && daw_slot == 1);
    CHECK(mx.set_midi_crosspoint(net_rx, daw_slot, 0, false));
    mx.inject_midi_event(net_rx, 0x90, 60, 100, 5000);
    const auto mst = mx.midi_slot_status(daw_slot);
    CHECK(mst.active);
    CHECK(mst.events_count == 1);
    if (opened) {
        CHECK(mst.connected);
        CHECK(!mst.status_text.empty());
    }
}

TEST(midi_static_playout_delay_absorbs_wifi_jitter) {
    using namespace lv;
    constexpr uint32_t kRate = 48000;
    constexpr double kFixedMs = 15.0;          // 15 ms Fixed delay = 720 samples @ 48 kHz
    constexpr uint64_t kStepUs = 125'000;      // 16th-note grid @ 120 BPM (125.0 ms)
    constexpr int kNumNotes = 16;
    constexpr int64_t kMasterOffsetUs = 5'000'000;  // arbitrary sender/receiver clock offset

    // ------------------------------------------------------------------
    // Part 1: Deterministic 16th-note grid with random 0..12 ms Wi-Fi jitter at Fixed 15 ms
    // ------------------------------------------------------------------
    MidiDejitterQueue dj(kRate);
    dj.set_latency(kFixedMs, /*adaptive=*/false);
    CHECK(dj.target_buffer_samples() == 720);
    CHECK(dj.is_fixed_mode());

    std::mt19937 rng(0x57494649u);
    std::uniform_int_distribution<uint64_t> wifi_jitter_us(0, 12'000);  // 0..12 ms random Wi-Fi jitter

    struct Arrival {
        uint64_t arrival_local_us = 0;
        uint64_t send_local_us = 0;
        TimedMidiEvent ev{};
    };
    std::vector<Arrival> arrivals;
    const uint64_t t0_local = 1'000'000;
    for (int i = 0; i < kNumNotes; ++i) {
        const uint64_t send_local = t0_local + uint64_t(i) * kStepUs;
        const uint64_t jitter = (i == 0) ? 0 : wifi_jitter_us(rng);  // first note establishes baseline
        TimedMidiEvent ev{};
        ev.event_seq = uint32_t(i + 1);
        ev.media_ts_us = uint64_t(int64_t(send_local) + kMasterOffsetUs);
        ev.status = 0x90;
        ev.data1 = uint8_t(60 + (i % 12));
        ev.data2 = 100;
        arrivals.push_back({send_local + jitter, send_local, ev});
    }
    // Also inject a late Note Off arriving at +22 ms (> 15 ms Fixed buffer) to verify zero stuck notes!
    {
        const uint64_t send_local = t0_local + uint64_t(kNumNotes) * kStepUs;
        TimedMidiEvent off{};
        off.event_seq = uint32_t(kNumNotes + 1);
        off.media_ts_us = uint64_t(int64_t(send_local) + kMasterOffsetUs);
        off.status = 0x80;
        off.data1 = 60;
        off.data2 = 0;
        arrivals.push_back({send_local + 22'000, send_local, off});
    }

    // Sort by network arrival time and simulate a 0.5 ms (24-sample) receiver playout tick
    std::sort(arrivals.begin(), arrivals.end(),
              [](const Arrival& a, const Arrival& b) { return a.arrival_local_us < b.arrival_local_us; });

    std::vector<uint64_t> note_playout_local_us(kNumNotes, 0);
    bool late_note_off_emitted = false;
    size_t next_arr = 0;
    const uint64_t end_local = t0_local + uint64_t(kNumNotes + 1) * kStepUs + 40'000;

    for (uint64_t now = t0_local; now <= end_local; now += 500) {
        while (next_arr < arrivals.size() && arrivals[next_arr].arrival_local_us <= now) {
            dj.push(arrivals[next_arr].ev, arrivals[next_arr].arrival_local_us,
                    /*master_synced=*/true, kMasterOffsetUs);
            ++next_arr;
        }
        dj.pop_due_local(now, [&](const TimedMidiEvent& ev, bool was_late) {
            if (ev.status == 0x90 && ev.event_seq >= 1 && ev.event_seq <= uint32_t(kNumNotes)) {
                note_playout_local_us[ev.event_seq - 1] = now;
            } else if (ev.status == 0x80 && ev.event_seq == uint32_t(kNumNotes + 1)) {
                CHECK(was_late);
                late_note_off_emitted = true;
            }
        });
    }

    double min_delay_ms = 1e9, max_delay_ms = 0.0;
    double max_interval_err_ms = 0.0;
    for (int i = 0; i < kNumNotes; ++i) {
        CHECK(note_playout_local_us[i] > 0);
        const uint64_t send_local = t0_local + uint64_t(i) * kStepUs;
        const double delay_ms = double(note_playout_local_us[i] - send_local) / 1000.0;
        min_delay_ms = std::min(min_delay_ms, delay_ms);
        max_delay_ms = std::max(max_delay_ms, delay_ms);
        CHECK(std::fabs(delay_ms - kFixedMs) <= 1.5);
        if (i > 0) {
            const double interval_ms =
                double(note_playout_local_us[i] - note_playout_local_us[i - 1]) / 1000.0;
            const double err_ms = std::fabs(interval_ms - double(kStepUs) / 1000.0);
            max_interval_err_ms = std::max(max_interval_err_ms, err_ms);
            CHECK(err_ms < 1.5);
        }
    }
    const auto dj_st = dj.stats();
    CHECK(late_note_off_emitted);
    CHECK(dj_st.late_events == 1);
    CHECK(dj_st.total_dispatched == uint64_t(kNumNotes + 1));

    // Verify live on-the-fly latency update (e.g. from UI or kControlSync)
    dj.set_latency(30.0, false);
    CHECK(dj.target_buffer_samples() == 1440);

    // ------------------------------------------------------------------
    // Part 2: Audio impulse + MIDI note locked to 0 ms skew at Fixed 15 ms under 0..12 ms jitter
    // ------------------------------------------------------------------
    MasterClock clk;
    clk.set_fixed_offset(kMasterOffsetUs);
    AudioSyncMatrix audio_mat(kRate);
    audio_mat.set_target_latency_ms(uint32_t(kFixedMs));
    audio_mat.set_adaptive_jitter(false);

    AudioRxEngine audio_rx(audio_mat, clk, [](const uint8_t*, size_t) { return true; });
    audio_rx.set_latency_ms(uint32_t(kFixedMs));
    audio_rx.set_adaptive_jitter_enabled(false);

    std::multimap<uint64_t, std::vector<uint8_t>> net_q;
    uint64_t sim_now = t0_local;
    AudioTxEngine audio_tx(kRate, kDefaultMtu, [&](const uint8_t* d, size_t n) {
        net_q.insert({sim_now + wifi_jitter_us(rng), std::vector<uint8_t>(d, d + n)});
        return true;
    });
    audio_tx.set_latency_ms(uint32_t(kFixedMs));

    MidiDejitterQueue synced_dj(kRate);
    synced_dj.set_latency(kFixedMs, false);
    synced_dj.set_effective_buffer_ms(kFixedMs, true);

    constexpr uint32_t kStepFrames = 48;  // 1 ms blocks
    std::vector<float> in_blk(kStepFrames, 0.0f);
    std::vector<float> out_blk(kStepFrames, 0.0f);
    float* out_ptrs[1] = {out_blk.data()};
    uint64_t audio_imp_us = 0;
    uint64_t midi_imp_us = 0;

    for (uint32_t step = 0; step < 120; ++step) {
        const uint64_t master_us = uint64_t(int64_t(sim_now) + kMasterOffsetUs);
        const uint64_t media_sample = master_us * kRate / 1'000'000;
        std::fill(in_blk.begin(), in_blk.end(), 0.0f);
        if (step == 40) {
            in_blk[0] = 0.9f;
            TimedMidiEvent ev{};
            ev.event_seq = 1;
            ev.media_ts_us = master_us;
            ev.status = 0x90;
            ev.data1 = 60;
            ev.data2 = 127;
            // Deliver MIDI with +10 ms Wi-Fi jitter
            synced_dj.push(ev, sim_now + 10'000, true, kMasterOffsetUs);
        }
        audio_tx.push_channel(0, in_blk.data(), kStepFrames, AudioFormat2::kFloat32, media_sample, uint32_t(sim_now));

        while (!net_q.empty() && net_q.begin()->first <= sim_now) {
            auto pkt = std::move(net_q.begin()->second);
            net_q.erase(net_q.begin());
            PacketHeader ph{};
            const uint8_t* pl = nullptr;
            if (parse_packet(pkt.data(), pkt.size(), ph, &pl) == PacketParseStatus::kOk &&
                ph.type == PacketType::kAudio2) {
                audio_rx.on_packet(ph, pl, ph.payload_size, sim_now);
            }
        }
        audio_mat.pop_planar(clk, sim_now, out_ptrs, 1, kStepFrames);
        for (uint32_t i = 0; i < kStepFrames; ++i) {
            if (audio_imp_us == 0 && out_blk[i] > 0.4f) {
                audio_imp_us = sim_now + (uint64_t(i) * 1'000'000 / kRate);
            }
        }
        if (audio_mat.playout_cursor() > 0.0) {
            synced_dj.pop_due_media(audio_mat.playout_media_ts_us(), [&](const TimedMidiEvent& ev, bool) {
                if (ev.status == 0x90 && midi_imp_us == 0) midi_imp_us = sim_now;
            });
        }
        sim_now += 1000;
    }

    const int64_t av_delta_us = std::llabs(int64_t(midi_imp_us) - int64_t(audio_imp_us));
    std::printf("    [MIDI Static Playout 15ms Fixed | 0-12ms Wi-Fi jitter] delay=%.2f..%.2f ms | "
                "max_interval_err=%.2f ms | audio_midi_delta=%.2f ms | late=%llu\n",
                min_delay_ms, max_delay_ms, max_interval_err_ms, double(av_delta_us) / 1000.0,
                static_cast<unsigned long long>(dj_st.late_events));

    CHECK(audio_imp_us > 0 && midi_imp_us > 0);
    CHECK(av_delta_us <= 1000);  // 0 ms skew (within 1 ms block quantization)
}

TEST(midi_only_mode_ultra_low_latency_no_audio_traffic) {
    using namespace lv;

    // 1. Start real UDP loopback Receiver + Sender in MIDI Only mode
    ReceiverConfig rcfg{};
    rcfg.bind_ip = "127.0.0.1";
    rcfg.bind_port = 0;  // ephemeral port
    rcfg.audio_latency_ms = 25;
    rcfg.audio_adaptive_jitter = false;
    rcfg.recv_timeout_ms = 1;
    UdpReceiver rx(rcfg, nullptr);
    CHECK(rx.init());
    rx.set_midi_only(true);
    CHECK(rx.midi_only());

    const auto dj_st = rx.midi_dejitter_stats();
    CHECK(dj_st.midi_only_mode);
    CHECK(dj_st.playout_delay_ms <= 2.0);  // Ultra-Low Latency (<= 2.0 ms instead of 25 ms audio buffer!)

    std::atomic<int> midi_rx_count{0};
    std::atomic<uint64_t> first_rx_us{0};
    rx.set_midi_callback([&](const TimedMidiEvent& ev, int32_t) {
        if (ev.status == 0x90 && ev.data1 == 64) {
            if (first_rx_us.load() == 0) first_rx_us.store(now_us());
            midi_rx_count.fetch_add(1);
        }
    });

    SenderConfig scfg{};
    scfg.dest_ip = "127.0.0.1";
    scfg.dest_port = rx.socket().local_port();
    scfg.audio_latency_ms = 25;
    UdpSender tx(scfg);
    CHECK(tx.init());
    tx.set_midi_only(true);
    CHECK(tx.midi_only());

    // Attempt to push audio while in MIDI Only mode -> MUST send 0 audio packets!
    std::vector<float> dummy_audio(256, 0.5f);
    for (int i = 0; i < 10; ++i) {
        tx.push_audio_channel(0, dummy_audio.data(), 256, AudioFormat2::kFloat32, UdpSender::kAutoMediaTs);
        tx.send_audio_raw(uint32_t(i + 1), now_us32(), dummy_audio.data(), 128, 48000, 2,
                          AudioSampleFormat::kFloat32);
    }
    CHECK(tx.audio2_stats().packets_sent == 0);
    CHECK(tx.stats().audio_packets_sent == 0);

    // Send MIDI Note On and verify ultra-low latency delivery (<= 2 ms playout delay + tiny loopback RTT)
    const uint64_t t_send = now_us();
    CHECK(tx.push_midi(0x90, 64, 110, 0, 0));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (midi_rx_count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        rx.step_once();
    }
    CHECK(midi_rx_count.load() == 1);
    CHECK(rx.audio2_packets_rx() == 0);

    const double measured_ms = double(first_rx_us.load() - t_send) / 1000.0;
    std::printf("    [MIDI Only Ultra-Low Latency] playout_cfg=%.1f ms | measured=%.2f ms | "
                "audio2_tx=%llu audio2_rx=%llu\n",
                dj_st.playout_delay_ms, measured_ms,
                static_cast<unsigned long long>(tx.audio2_stats().packets_sent),
                static_cast<unsigned long long>(rx.audio2_packets_rx()));
    CHECK(dj_st.playout_delay_ms <= 2.0);
    CHECK(measured_ms <= 60.0);  // allows Windows VM 15.6 ms timer granularity while proving ultra-low latency path
}

TEST(personal_monitor_bus_64ch_to_stereo_dsp_and_remote_control) {
    using namespace lv;
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kFrames = 256;

    // ------------------------------------------------------------------
    // Part 1: Direct 64ch -> Stereo DSP verification (Pan, HPF, 3-band EQ, Comp, -0.5 dBFS Limiter)
    // ------------------------------------------------------------------
    PersonalMonitorBus bus(kRate);
    // Mute all channels except Ch 0 (1 kHz sine, panned hard Left) and Ch 63 (200 Hz sine, panned hard Right)
    for (int c = 0; c < kMonitorMaxInputChannels; ++c) {
        bus.set_channel_mix(c, {0.0f, 0.0f, true, false});
    }
    bus.set_channel_mix(0, {0.0f, -1.0f, false, false});   // Ch 0 -> Left only
    bus.set_channel_mix(63, {0.0f, +1.0f, false, false});  // Ch 63 -> Right only

    std::vector<std::vector<float>> planes(kMonitorMaxInputChannels, std::vector<float>(kFrames, 0.0f));
    std::vector<const float*> ptrs(kMonitorMaxInputChannels);
    for (int c = 0; c < kMonitorMaxInputChannels; ++c) ptrs[c] = planes[c].data();

    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);
    double phase0 = 0.0, phase63 = 0.0;
    auto fill_tones = [&](float amp0, float amp63) {
        for (uint32_t i = 0; i < kFrames; ++i) {
            planes[0][i] = amp0 * float(std::sin(phase0));
            planes[63][i] = amp63 * float(std::sin(phase63));
            phase0 += 2.0 * 3.141592653589793 * 1000.0 / double(kRate);
            phase63 += 2.0 * 3.141592653589793 * 200.0 / double(kRate);
        }
    };

    // Settle 5 ms smoother
    for (int b = 0; b < 4; ++b) {
        fill_tones(0.4f, 0.2f);
        bus.process_block(ptrs.data(), kMonitorMaxInputChannels, out_l.data(), out_r.data(), kFrames);
    }
    auto rms_of = [](const std::vector<float>& v) {
        double s = 0.0;
        for (float x : v) s += double(x) * double(x);
        return std::sqrt(s / double(v.size()));
    };
    const double base_rms_l = rms_of(out_l);
    const double base_rms_r = rms_of(out_r);
    CHECK(base_rms_l > 0.14);  // bus carries -3 dB of mix headroom
    CHECK(base_rms_r > 0.07);

    // Enable 3-band EQ (+12 dB Mid @ 1000 Hz) + HPF (250 Hz) -> 1 kHz Left boosts, 200 Hz Right attenuates!
    PersonalMonitorDspParams dsp{};
    dsp.hpf_enabled = true;
    dsp.hpf_hz = 250.0f;
    dsp.eq_enabled = true;
    dsp.eq_mid_hz = 1000.0f;
    dsp.eq_mid_db = 12.0f;
    bus.set_dsp(dsp);
    for (int b = 0; b < 6; ++b) {
        fill_tones(0.2f, 0.2f);
        bus.process_block(ptrs.data(), kMonitorMaxInputChannels, out_l.data(), out_r.data(), kFrames);
    }
    const double eq_rms_l = rms_of(out_l);
    const double eq_rms_r = rms_of(out_r);
    CHECK(eq_rms_l > eq_rms_r * 2.0);  // +12 dB 1 kHz boost on L vs 250 Hz HPF cut on 200 Hz R!

    // Enable Compressor (-24 dB threshold, 6:1 ratio) + verify -0.5 dBFS Brickwall Limiter ceiling!
    dsp.comp_enabled = true;
    dsp.comp_thresh_db = -24.0f;
    dsp.comp_ratio = 6.0f;
    dsp.comp_attack_ms = 2.0f;
    dsp.comp_release_ms = 50.0f;
    dsp.master_volume_db = 12.0f;  // drive hard into brickwall limiter
    bus.set_dsp(dsp);
    float max_peak = 0.0f;
    for (int b = 0; b < 6; ++b) {
        fill_tones(0.9f, 0.9f);
        bus.process_block(ptrs.data(), kMonitorMaxInputChannels, out_l.data(), out_r.data(), kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            max_peak = std::max(max_peak, std::max(std::fabs(out_l[i]), std::fabs(out_r[i])));
        }
    }
    CHECK(bus.last_comp_gr_db() < -3.0f);  // compressor actively reducing gain
    CHECK(max_peak <= 0.945f);             // brickwall ceiling strictly <= -0.5 dBFS (0.9441)

    // ------------------------------------------------------------------
    // Part 2: End-to-End UDP Server <-> Smartphone Client (kMonitorControl + 2ch AUDIO2)
    // ------------------------------------------------------------------
    PersonalMonitorServer srv(kRate);
    CHECK(srv.start(0, "127.0.0.1"));  // ephemeral port
    srv.set_channel_name(0, "Kick");
    srv.set_channel_name(1, "Lead Vox");

    PersonalMonitorClient cli(kRate);
    CHECK(cli.connect("127.0.0.1", srv.bound_port(), "iPhone Singer", 256, 20));

    // Pump server & client for hello + meta response
    for (int i = 0; i < 10; ++i) {
        srv.poll_control();
        cli.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto clients = srv.clients();
    CHECK(clients.size() == 1);
    CHECK(clients[0].name == "iPhone Singer");
    CHECK(clients[0].bitrate_kbps == 256);

    const auto meta = cli.last_meta();
    CHECK(meta.size() >= 2);
    CHECK(std::string(meta[0].name) == "Kick");
    CHECK(std::string(meta[1].name) == "Lead Vox");

    // Smartphone updates Ch 0 gain & DSP remotely over UDP kMonitorControl!
    CHECK(cli.set_channel_mix(0, {-6.0f, -0.5f, false, true}));
    PersonalMonitorDspParams remote_dsp{};
    remote_dsp.hpf_enabled = true;
    remote_dsp.hpf_hz = 120.0f;
    remote_dsp.eq_enabled = true;
    remote_dsp.eq_high_db = 4.5f;
    remote_dsp.comp_enabled = true;
    remote_dsp.comp_thresh_db = -20.0f;
    remote_dsp.comp_ratio = 4.0f;
    CHECK(cli.set_dsp(remote_dsp));

    for (int i = 0; i < 12; ++i) {
        fill_tones(0.5f, 0.3f);
        srv.process_and_send(ptrs.data(), 8, kFrames);
        cli.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto* srv_bus = srv.bus(clients[0].slot);
    CHECK(srv_bus != nullptr);
    const auto applied_mix = srv_bus->channel_mix(0);
    const auto applied_dsp = srv_bus->dsp();
    CHECK(std::fabs(applied_mix.gain_db - (-6.0f)) < 0.01f);
    CHECK(std::fabs(applied_mix.pan - (-0.5f)) < 0.01f);
    CHECK(applied_mix.solo);
    CHECK(applied_dsp.hpf_enabled && std::fabs(applied_dsp.hpf_hz - 120.0f) < 0.1f);
    CHECK(applied_dsp.eq_enabled && std::fabs(applied_dsp.eq_high_db - 4.5f) < 0.01f);
    CHECK(applied_dsp.comp_enabled && std::fabs(applied_dsp.comp_ratio - 4.0f) < 0.01f);
    CHECK(cli.audio_packets_rx() > 0);

    std::printf("    [PersonalMonitorBus] 64ch->2ch EQ_L=%.3f HPF_R=%.3f comp_GR=%.1f dB lim_peak=%.4f | "
                "UDP client='%s' audio2_rx=%llu\n",
                eq_rms_l, eq_rms_r, bus.last_comp_gr_db(), max_peak,
                clients[0].name.c_str(), static_cast<unsigned long long>(cli.audio_packets_rx()));

    cli.disconnect();
    srv.stop();
}

TEST(tally_and_duplex_intercom_e2e_udp) {
    using namespace lv;

    // 1. Packet serialization round-trip for kTallyState and kIntercomAudio
    {
        TallyStatePayload in{};
        in.camera_id = 3;
        in.tally_state = kTallyProgram;
        in.flags = kTallyFlagRecTrigger | kTallyFlagHighBright;
        in.revision = 42;
        in.battery_pct = 87;
        in.temp_c = 38;
        std::snprintf(in.camera_label, sizeof(in.camera_label), "CAM 3 A-ROLL");

        std::vector<uint8_t> buf(kTallyStateSize);
        write_tally_state(buf.data(), in);
        TallyStatePayload out{};
        CHECK(parse_tally_state(buf.data(), buf.size(), out));
        CHECK(out.camera_id == 3);
        CHECK(out.tally_state == kTallyProgram);
        CHECK((out.flags & kTallyFlagRecTrigger) != 0);
        CHECK(out.revision == 42);
        CHECK(std::string(out.camera_label) == "CAM 3 A-ROLL");

        IntercomAudioHeader ih{};
        ih.camera_id = 3;
        ih.codec = 1;
        ih.flags = kIntercomFlagStudioToCam | kIntercomFlagPttActive | kIntercomFlagDuckProgram;
        ih.num_samples = 480;
        ih.seq = 77;
        ih.sample_rate = 48000;
        std::vector<uint8_t> ibuf(kIntercomHeaderSize);
        write_intercom_header(ibuf.data(), ih);
        IntercomAudioHeader ih_out{};
        CHECK(parse_intercom_header(ibuf.data(), ibuf.size(), ih_out));
        CHECK(ih_out.camera_id == 3 && ih_out.seq == 77 && ih_out.num_samples == 480 && ih_out.sample_rate == 48000);
    }

    // 2. Live UDP Studio <-> Camera Tally transitions + Duplex Intercom E2E
    TallyIntercomEngine studio;
    TallyIntercomEngine camera;
    CHECK(studio.start_studio(0));
    const uint16_t studio_port = studio.bound_port();
    CHECK(studio_port != 0);

    std::atomic<int> tally_events{0};
    std::atomic<uint8_t> last_tally{kTallyStandby};
    std::atomic<bool> last_rec{false};
    camera.camera_set_tally_callback([&](const TallyStatePayload& t) {
        if (t.camera_id == 1) {
            last_tally.store(t.tally_state);
            last_rec.store((t.flags & kTallyFlagRecTrigger) != 0);
            tally_events.fetch_add(1);
        }
    });
    camera.camera_update_telemetry(92, 36);
    CHECK(camera.start_camera("127.0.0.1", studio_port, 1, "CAM 1 MAIN"));

    // Wait for Camera hello -> Studio registration
    for (int i = 0; i < 40; ++i) {
        camera.poll();
        studio.poll();
        if (!studio.studio_cameras().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto cams = studio.studio_cameras();
    CHECK(cams.size() == 1);
    CHECK(cams[0].camera_id == 1);
    CHECK(cams[0].battery_pct == 92);

    // Standby -> Preview -> Program + Remote REC
    CHECK(studio.studio_set_camera_tally(1, kTallyPreview, "CAM 1 PVW", false));
    for (int i = 0; i < 20 && last_tally.load() != kTallyPreview; ++i) {
        studio.poll();
        camera.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(last_tally.load() == kTallyPreview);
    CHECK(!last_rec.load());

    CHECK(studio.studio_set_camera_tally(1, kTallyProgram, "CAM 1 ON AIR", true));
    for (int i = 0; i < 20 && (last_tally.load() != kTallyProgram || !last_rec.load()); ++i) {
        studio.poll();
        camera.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(last_tally.load() == kTallyProgram);
    CHECK(last_rec.load());
    CHECK(tally_events.load() >= 2);

    // 3. Duplex Intercom: Director -> Camera IFB (with ducking) + Camera PTT -> Studio
    studio.set_intercom_state(/*talk_active=*/true, /*listen_muted=*/false, /*rx_gain=*/1.0f, /*sidetone=*/0.0f);
    studio.studio_set_camera_intercom(1, /*talk_to_cam=*/true, /*listen=*/true, /*all_call=*/false);
    camera.set_intercom_state(/*talk_active=*/true, /*listen_muted=*/false, /*rx_gain=*/1.0f, /*sidetone=*/0.0f);

    std::vector<float> dir_tone(480), op_tone(480);
    for (size_t i = 0; i < 480; ++i) {
        dir_tone[i] = float(0.45 * std::sin(2.0 * 3.1415926535 * 600.0 * double(i) / 48000.0));
        op_tone[i] = float(0.40 * std::sin(2.0 * 3.1415926535 * 950.0 * double(i) / 48000.0));
    }

    const auto t_start = std::chrono::steady_clock::now();
    std::vector<float> cam_rx(480, 0.0f), studio_rx(480, 0.0f);
    int cam_got = 0, studio_got = 0;
    double cam_rms = 0.0, studio_rms = 0.0;

    for (int step = 0; step < 15; ++step) {
        studio.push_mic_pcm(dir_tone.data(), int(dir_tone.size()));
        camera.push_mic_pcm(op_tone.data(), int(op_tone.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        camera.poll();
        studio.poll();
        if (cam_rms < 0.05) {
            const int n = camera.pull_rx_pcm(cam_rx.data(), int(cam_rx.size()));
            if (n > 0) {
                cam_got = n;
                double e = 0.0;
                for (int k = 0; k < n; ++k) e += double(cam_rx[k]) * double(cam_rx[k]);
                cam_rms = std::sqrt(e / double(n));
            }
        }
        if (studio_rms < 0.05) {
            const int n = studio.pull_rx_pcm(studio_rx.data(), int(studio_rx.size()));
            if (n > 0) {
                studio_got = n;
                double e = 0.0;
                for (int k = 0; k < n; ++k) e += double(studio_rx[k]) * double(studio_rx[k]);
                studio_rms = std::sqrt(e / double(n));
            }
        }
        if (cam_got > 0 && studio_got > 0 && cam_rms > 0.05 && studio_rms > 0.05) break;
    }
    const double e2e_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();

    CHECK(cam_got > 0);
    CHECK(studio_got > 0);
    CHECK(cam_rms > 0.05);
    CHECK(studio_rms > 0.05);
    CHECK(camera.is_director_speaking());
    CHECK(camera.ducking_gain() < 0.9f);
    CHECK(e2e_ms < 50.0);

    // 4. C ABI smoke test
    LvTallyIntercomHandle* h = lv_tally_intercom_create_studio(0);
    CHECK(h != nullptr);
    CHECK(lv_tally_intercom_port(h) != 0);
    CHECK(lv_tally_intercom_set_camera_tally(h, 2, kTallyProgram, "CAM 2", 1) == 1 ||
          lv_tally_intercom_set_camera_tally(h, 2, kTallyProgram, "CAM 2", 1) == 0);
    lv_tally_intercom_destroy(h);

    std::printf("    [Tally+Intercom E2E] tally_events=%d cam_ifb_rms=%.3f studio_rx_rms=%.3f duck=%.2f e2e=%.1f ms\n",
                tally_events.load(), cam_rms, studio_rms, camera.ducking_gain(), e2e_ms);

    camera.stop();
    studio.stop();
}
