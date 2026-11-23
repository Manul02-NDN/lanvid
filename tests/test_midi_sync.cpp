#include "mini_test.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <vector>

#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/audio_tx.hpp"
#include "audio/master_clock.hpp"
#include "audio/multi_device_matrix.hpp"
#include "gui/app_config.hpp"
#include "midi/midi_sync_engine.hpp"
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
