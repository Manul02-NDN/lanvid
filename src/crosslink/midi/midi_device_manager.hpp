#pragma once
// Cross-platform OS MIDI Device Manager:
//   - Windows: WinMM (midiInOpen / midiOutOpen / midiOutShortMsg with QPC microsecond timestamps)
//   - macOS:   CoreMIDI (MIDIGetNumberOfSources/Destinations + virtual ports
//              "CrossLink Virtual MIDI In" and "CrossLink Virtual MIDI Out")
//   - Linux:   Safe stub (enumeration returns empty).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cl {

struct MidiDeviceInfo {
    int index = 0;
    std::string name;
    bool is_input = true;
    bool is_virtual = false;
};

std::vector<MidiDeviceInfo> list_midi_inputs();
std::vector<MidiDeviceInfo> list_midi_outputs();

class MidiInputPort {
public:
    using Callback = std::function<void(uint8_t status, uint8_t d1, uint8_t d2, uint64_t qpc_us)>;

    MidiInputPort();
    ~MidiInputPort();
    MidiInputPort(const MidiInputPort&) = delete;
    MidiInputPort& operator=(const MidiInputPort&) = delete;

    // Opens OS input port by index or name (on macOS, name=="CrossLink Virtual MIDI In" creates virtual destination).
    bool open(int device_index, const std::string& device_name, Callback cb);
    void close();
    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class MidiOutputPort {
public:
    MidiOutputPort();
    ~MidiOutputPort();
    MidiOutputPort(const MidiOutputPort&) = delete;
    MidiOutputPort& operator=(const MidiOutputPort&) = delete;

    // Opens OS output port by index or name (on macOS, name=="CrossLink Virtual MIDI Out" creates virtual source).
    bool open(int device_index, const std::string& device_name);
    void close();
    bool is_open() const;

    bool send_short(uint8_t status, uint8_t d1, uint8_t d2);
    // Sends CC 123 (All Notes Off) + CC 120 (All Sound Off) on all 16 MIDI channels.
    void send_panic();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cl
