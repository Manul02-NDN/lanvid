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
#include "audio/master_clock.hpp"
#include "audio/multi_device_matrix.hpp"
#include "gui/app_config.hpp"
#include "midi/midi_dejitter_queue.hpp"
#include "midi/midi_device_manager.hpp"
#include "midi/midi_sync_engine.hpp"
#include "midi/virtual_midi.hpp"
#include "net/packet.hpp"

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

    // Verify AppConfig persistence for MIDI slots & crosspoints (backup/restore user config)
    namespace fs = std::filesystem;
    const fs::path cfg_path(lv::gui::config_file_path());
    std::string backup;
    bool had_existing = false;
    if (fs::exists(cfg_path)) {
        std::ifstream in(cfg_path, std::ios::binary);
        backup.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        had_existing = true;
    }
    lv::gui::AppConfig saved{};
    saved.midi_slots = mx.midi_slots();
    saved.midi_crosspoints = mx.midi_crosspoints();
    CHECK(lv::gui::save_app_config(saved));
    lv::gui::AppConfig loaded{};
    CHECK(lv::gui::load_app_config(loaded));
    CHECK(loaded.midi_slots.size() == size_t(4));
    CHECK(loaded.midi_crosspoints.size() == size_t(3));
    CHECK(loaded.midi_crosspoints[2].channel_filter == 10);
    if (had_existing) {
        std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
        out.write(backup.data(), std::streamsize(backup.size()));
    } else {
        std::error_code ec;
        fs::remove(cfg_path, ec);
    }
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
            CHECK(rx_in_daw[0].st == 0x90 && rx_in_daw[0].d1 == 60 && rx_in_daw[0].d2 == 100);
            CHECK(rx_in_daw[1].st == 0xB0 && rx_in_daw[1].d1 == 74 && rx_in_daw[1].d2 == 64);
            CHECK(rx_in_daw[2].st == 0x80 && rx_in_daw[2].d1 == 60 && rx_in_daw[2].d2 == 0);

            CHECK(rx_from_daw.size() >= 2);
            CHECK(rx_from_daw[0].st == 0x91 && rx_from_daw[0].d1 == 64 && rx_from_daw[0].d2 == 110);
            CHECK(rx_from_daw[1].st == 0x81 && rx_from_daw[1].d1 == 64 && rx_from_daw[1].d2 == 0);
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
