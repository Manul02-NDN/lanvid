#pragma once
// VirtualMidiPort — Cross-platform system-level virtual MIDI interface ("LanVid MIDI" In / Out)
// visible to DAWs (REAPER, Ableton, Logic, Cubase, etc.) as a native hardware MIDI device:
//   - Windows 10/11:
//       1) Native Windows MIDI Services Loopback Endpoint Pair ("LanVid MIDI Internal" <-> "LanVid MIDI")
//          — zero unsigned kernel drivers, immediately visible to all WinMM/WinRT DAWs.
//       2) Dynamic teVirtualMIDI64.dll (if Tobias Erichsen's virtual MIDI driver is installed).
//       3) Existing WinMM virtual loopback port ("LanVid MIDI" / "loopMIDI") fallback.
//   - macOS:
//       CoreMIDI virtual client + MIDISourceCreateWithProtocol (DAW In) +
//       MIDIDestinationCreateWithProtocol (DAW Out) with stable UniqueID, Manufacturer, Model,
//       DisplayName, and kMIDIPropertyOffline = 0.
//   - Linux / Other:
//       Safe no-op stub reporting unavailable status.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lv {

enum class VirtualMidiBackend : int {
    kNone = 0,
    kWindowsMidiServices = 1,  // Native Windows 11 Windows MIDI Services Loopback Pair
    kTeVirtualMidi = 2,        // Tobias Erichsen teVirtualMIDI64.dll
    kWinMmLoopback = 3,        // Existing WinMM loopback port (loopMIDI / persistent LanVid MIDI)
    kCoreMidi = 4              // macOS CoreMIDI virtual source + destination
};

struct VirtualMidiStatus {
    bool open = false;
    VirtualMidiBackend backend = VirtualMidiBackend::kNone;
    std::string daw_port_name;       // Name the user selects in DAW (e.g. "LanVid MIDI")
    std::string internal_port_name;  // Internal peer endpoint name on Windows loopback
    std::string backend_name;        // Human-readable backend description
    std::string status_text;         // UI-ready status summary
};

class VirtualMidiPort {
public:
    // Called when the DAW sends MIDI data TO LanVid MIDI Out (status, data1, data2, timestamp_us).
    using Callback = std::function<void(uint8_t status, uint8_t d1, uint8_t d2, uint64_t ts_us)>;

    VirtualMidiPort();
    ~VirtualMidiPort();

    VirtualMidiPort(const VirtualMidiPort&) = delete;
    VirtualMidiPort& operator=(const VirtualMidiPort&) = delete;

    // Creates/opens the system virtual MIDI device named `daw_port_name` (default: "LanVid MIDI").
    // Returns true if the DAW-visible virtual MIDI port is active.
    bool open(const std::string& daw_port_name = "LanVid MIDI", Callback on_from_daw = nullptr);
    void close();
    bool is_open() const;

    // Sends a MIDI 1.0 short message TO the DAW (appears on "LanVid MIDI" input in REAPER/Logic/etc.).
    bool send_to_daw(uint8_t status, uint8_t d1, uint8_t d2);

    // Sends All Notes Off (CC 123) + All Sound Off (CC 120) on all 16 channels to the DAW.
    void send_panic_to_daw();

    VirtualMidiStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lv
