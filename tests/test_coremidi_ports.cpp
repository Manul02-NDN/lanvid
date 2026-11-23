// macOS: LanVid virtual CoreMIDI ports exist system-wide and a burst of messages survives CoreMIDI
// packet coalescing (several messages per MIDIPacket) without losses.
#include "mini_test.hpp"

#if defined(__APPLE__)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "midi/midi_device_manager.hpp"

TEST(coremidi_virtual_ports_and_packet_burst) {
    lv::MidiInputPort vin;
    lv::MidiOutputPort vout;
    REQUIRE(vin.open(-1, "LanVid Virtual MIDI In", [](uint8_t, uint8_t, uint8_t, uint64_t) {}));
    REQUIRE(vout.open(-1, "LanVid Virtual MIDI Out"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bool dest_seen = false, src_seen = false;
    int src_index = -1;
    for (const auto& d : lv::list_midi_outputs()) dest_seen |= (!d.is_virtual && d.name == "LanVid Virtual MIDI In");
    for (const auto& d : lv::list_midi_inputs())
        if (!d.is_virtual && d.name == "LanVid Virtual MIDI Out") { src_seen = true; src_index = d.index; }
    CHECK(dest_seen);
    CHECK(src_seen);
    REQUIRE(src_index >= 0);

    std::atomic<int> note_on{0}, note_off{0}, cc{0};
    std::atomic<int> order_errors{0};
    std::atomic<int> expect_note{0};
    lv::MidiInputPort reader;
    REQUIRE(reader.open(src_index, "LanVid Virtual MIDI Out", [&](uint8_t st, uint8_t d1, uint8_t d2, uint64_t) {
        const uint8_t hi = st & 0xF0;
        if (hi == 0x90 && d2 > 0) {
            if (d1 != uint8_t(36 + (expect_note.load() % 64))) ++order_errors;
            ++expect_note;
            ++note_on;
        } else if (hi == 0x80 || (hi == 0x90 && d2 == 0)) {
            ++note_off;
        } else if (hi == 0xB0) {
            ++cc;
        }
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Back-to-back burst: CoreMIDI coalesces these into multi-message packets.
    for (int i = 0; i < 64; ++i) {
        vout.send_short(0x90, uint8_t(36 + i), 100);
        vout.send_short(0x80, uint8_t(36 + i), 0);
        vout.send_short(0xB0, 7, uint8_t(i));
    }
    for (int t = 0; t < 50 && (note_on < 64 || note_off < 64 || cc < 64); ++t)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::printf("    [coremidi] virtual ports in system: dest=%d src=%d | burst received: note_on=%d note_off=%d cc=%d "
                "order_errors=%d\n", int(dest_seen), int(src_seen), note_on.load(), note_off.load(), cc.load(),
                order_errors.load());
    CHECK(note_on == 64);
    CHECK(note_off == 64);
    CHECK(cc == 64);
    CHECK(order_errors == 0);
}
#endif
