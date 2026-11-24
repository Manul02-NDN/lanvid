#pragma once
// MultiDeviceAudioMatrix — 48 kHz lock-free multi-device audio router & summing mixer (up to 8 slots,
// 2..64 channels per slot):
//   - Slot Virtual Bus (LanVidAudioBus_v1 SHM v2: 64 In / 64 Out to DAW)
//   - Slot Network AoIP (64 In / 64 Out from/to Sender, Receiver or Duplex node)
//   - Slot System Audio (Windows WASAPI Shared Mode / macOS CoreAudio HAL render/capture with
//     44.1/88.2/96 -> 48 kHz resampler + drift guard)
//   - Slot External ASIO (Windows 64-bit COM IASIO host for any installed third-party ASIO driver)
//
// Supports 1-to-many fan-out, many-to-1 summing with per-crosspoint gain_db & mute,
// Feedback Loop Guard, real-time VU meters, and deterministic synchronous block processing for tests.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lv {

constexpr int kMatrixMaxSlots = 8;
constexpr int kMatrixMaxMidiSlots = 16;
constexpr int kMatrixMaxChannels = 64;
constexpr uint32_t kMatrixSampleRate = 48000;

enum class MidiSlotType : int {
    kMidiInput = 0,       // OS physical/virtual MIDI input (keyboard, e-drums, controller)
    kMidiOutput = 1,      // OS physical/virtual MIDI output (DAW, hardware synth)
    kNetworkMidiTx = 2,   // MIDI2 over IP outgoing bus
    kNetworkMidiRx = 3,   // MIDI2 over IP incoming bus
    kVirtualDawMidi = 4   // System virtual MIDI device ("LanVid MIDI" In/Out for DAWs)
};

struct MidiSlotConfig {
    int id = -1;  // 0..kMatrixMaxMidiSlots-1
    MidiSlotType type = MidiSlotType::kMidiInput;
    std::string name;
    std::string device_name;
    int device_index = -1;
    uint8_t port_index = 0;  // 0..15 for MIDI2 wire port_index
    bool active = true;
};

struct MidiCrosspoint {
    int src_slot = 0;
    int dst_slot = 0;
    int channel_filter = 0;  // 0 = All channels, 1..16 = only MIDI channel 1..16
    bool mute = false;
};

struct MidiSlotRuntimeStatus {
    int id = 0;
    bool active = false;
    bool connected = false;
    uint64_t events_count = 0;
    uint64_t last_activity_us = 0;  // for blinking MIDI ACTIVITY LED in GUI
    std::string backend_name;
    std::string status_text;
};

enum class MatrixSlotType : int {
    kVirtualBus = 0,   // LanVidAudioBus_v1 SHM v2 (DAW 64ch)
    kNetworkAoip = 1,  // Network AoIP 64ch stream
    kSystemAudio = 2,  // OS audio device (WASAPI / CoreAudio render/capture)
    kExternalAsio = 3  // Third-party Windows ASIO driver
};

enum class MatrixSlotRole : int {
    kDuplex = 0,  // both input (source to matrix) and output (sink from matrix)
    kInput = 1,   // source only (e.g., Mic / Line-In)
    kOutput = 2   // sink only (e.g., Headphones / Speakers)
};

struct MatrixSlotConfig {
    int id = 0;  // 0..kMatrixMaxSlots-1
    MatrixSlotType type = MatrixSlotType::kVirtualBus;
    MatrixSlotRole role = MatrixSlotRole::kDuplex;
    std::string name;
    std::string device_id;  // OS device name or ASIO driver name / CLSID
    int device_index = -1;  // -1 = system default (for WASAPI)
    int channels = 2;       // 2..64
    int buffer_size = 256;  // 128 / 256 / 512
    bool active = true;
};

struct MatrixCrosspoint {
    int src_slot = 0;
    int src_ch = 0;     // 0..63
    int dst_slot = 0;
    int dst_ch = 0;     // 0..63
    float gain_db = 0.0f;
    bool mute = false;
};

// Aggregated Visual Patchbay cable derived live from the 64x64 crosspoint table.
struct SlotPatchLink {
    int src_slot = -1;
    int dst_slot = -1;
    int channel_count = 0;
    float gain_db = 0.0f;
    bool any_muted = false;
    bool all_muted = false;
};

struct MatrixSlotRuntimeStatus {
    int id = 0;
    bool active = false;
    bool connected = false;
    uint32_t device_sample_rate = kMatrixSampleRate;
    uint32_t buffer_size = 256;
    float peak_in_dbfs = -120.0f;
    float peak_out_dbfs = -120.0f;
    uint64_t xruns = 0;
    std::string status_text;
};

struct AsioDriverEntry {
    std::string name;
    std::string clsid;
};

enum class CrosspointResult {
    kOk = 0,
    kInvalidSlotOrChannel = 1,
    kBlockedFeedbackLoop = 2
};

// Physical OS audio device for kSystemAudio slots (macOS CoreAudio; empty elsewhere — Windows uses
// the WASAPI endpoint lists). `uid` goes into MatrixSlotConfig::device_id ("" = system default).
struct SystemAudioDeviceEntry {
    std::string name;
    std::string uid;
    int channels = 0;           // channels in the requested direction
    uint32_t sample_rate = 0;   // nominal device rate (converted to/from 48 kHz by the matrix)
    bool is_default = false;
};

// Lists CoreAudio output (output=true) or input devices. Excludes "LanVid Virtual 64ch" by default:
// it is already reachable through the Virtual Bus (SHM) slot and opening it twice would loop.
std::vector<SystemAudioDeviceEntry> list_system_audio_devices(bool output, bool exclude_lanvid = true);

// Enumerates installed 64-bit ASIO drivers from HKLM\SOFTWARE\ASIO (Windows; empty on UNIX).
// Excludes "LanVid Virtual ASIO" by default to prevent recursive self-hosting.
std::vector<AsioDriverEntry> list_external_asio_drivers(bool exclude_lanvid = true);

class MultiDeviceAudioMatrix {
public:
    using NetworkPushFn = std::function<void(const float* const* planes, uint32_t channels, uint32_t frames)>;
    using NetworkPopFn = std::function<void(float* const* planes, uint32_t channels, uint32_t frames)>;

    MultiDeviceAudioMatrix();
    ~MultiDeviceAudioMatrix();

    MultiDeviceAudioMatrix(const MultiDeviceAudioMatrix&) = delete;
    MultiDeviceAudioMatrix& operator=(const MultiDeviceAudioMatrix&) = delete;

    // ---- Slot management (up to kMatrixMaxSlots) ----
    // Adds or updates a slot; returns slot id (0..7) or -1 if full.
    int add_slot(const MatrixSlotConfig& cfg);
    bool update_slot(int slot_id, const MatrixSlotConfig& cfg);
    bool remove_slot(int slot_id);
    void clear_slots();
    std::vector<MatrixSlotConfig> slots() const;

    // ---- Crosspoint routing (1-to-many & many-to-1 summing) ----
    // Blocks direct src_slot == dst_slot unless allow_feedback is true.
    CrosspointResult set_crosspoint(int src_slot, int src_ch, int dst_slot, int dst_ch,
                                    float gain_db = 0.0f, bool mute = false, bool allow_feedback = false);
    bool remove_crosspoint(int src_slot, int src_ch, int dst_slot, int dst_ch);
    bool has_crosspoint(int src_slot, int src_ch, int dst_slot, int dst_ch, MatrixCrosspoint* out = nullptr) const;
    void clear_crosspoints();
    std::vector<MatrixCrosspoint> crosspoints() const;

    // Visual Patchbay block operations (directly synchronized with the 64x64 crosspoint table).
    CrosspointResult connect_slot_block(int src_slot, int dst_slot, int max_channels,
                                        float gain_db = 0.0f, bool mute = false, bool allow_feedback = false);
    bool disconnect_slot_block(int src_slot, int dst_slot);
    bool set_slot_block_gain_mute(int src_slot, int dst_slot, float gain_db, bool mute);
    std::vector<SlotPatchLink> slot_patch_links() const;

    // Global Feedback Loop Guard toggle (default: true = guard enabled, blocks self-loop unless confirmed).
    void set_feedback_guard(bool enabled);
    bool feedback_guard() const;

    // ---- Built-in 1-click presets ----
    // 0: "Mac/Net -> DAW (64ch) + Headphones (Ch 1/2)"
    // 1: "Full-Duplex 64x64 (DAW <-> Net) + Local Monitor"
    // 2: "1:1 Direct Patch"
    void apply_preset(int preset_index, int net_slot_id, int daw_slot_id, int monitor_slot_id);

    // ---- Network AoIP bridge hooks ----
    void set_network_io(NetworkPushFn push_tx, NetworkPopFn pop_rx);

    // ---- Lifecycle ----
    void set_buffer_size(int frames);  // 128 / 256 / 512
    int buffer_size() const;
    bool start();
    void stop();
    bool is_running() const;

    // ---- Live telemetry ----
    MatrixSlotRuntimeStatus slot_status(int slot_id) const;

    // ---- Direct programmatic ring access (for unit tests & custom integrations) ----
    // Pushes planar float32 audio into a slot's input buffer (source -> matrix).
    uint32_t inject_slot_input(int slot_id, const float* const* planes, uint32_t channels, uint32_t frames);
    // Pops planar float32 audio from a slot's output buffer (matrix -> sink).
    uint32_t extract_slot_output(int slot_id, float* const* planes, uint32_t channels, uint32_t frames);
    // Runs one deterministic mixing block synchronously (does not require start()).
    void process_block_sync(uint32_t frames);
    // Frames rendered by the real-time mix thread since construction (exactly 48000 per second of wall time).
    uint64_t frames_mixed() const;

    // ---- MIDI Slots & Routing (up to 16 MIDI devices, many-to-1 merge & 1-to-many split) ----
    using MidiOutSinkFn = std::function<void(int dst_slot_id, uint8_t status, uint8_t d1, uint8_t d2,
                                             uint8_t port_index, uint64_t ts_us)>;
    int add_midi_slot(const MidiSlotConfig& cfg);
    bool update_midi_slot(int slot_id, const MidiSlotConfig& cfg);
    bool remove_midi_slot(int slot_id);
    void clear_midi_slots();
    std::vector<MidiSlotConfig> midi_slots() const;

    bool set_midi_crosspoint(int src_slot, int dst_slot, int channel_filter = 0, bool mute = false);
    bool remove_midi_crosspoint(int src_slot, int dst_slot);
    bool has_midi_crosspoint(int src_slot, int dst_slot, MidiCrosspoint* out = nullptr) const;
    void clear_midi_crosspoints();
    std::vector<MidiCrosspoint> midi_crosspoints() const;

    void set_midi_sink(MidiOutSinkFn sink);
    // Injects an incoming MIDI event into `src_slot_id` and routes it through all active MIDI crosspoints.
    void inject_midi_event(int src_slot_id, uint8_t status, uint8_t d1, uint8_t d2, uint64_t ts_us);
    // Sends CC 123 (All Notes Off) + CC 120 (All Sound Off) on all 16 channels to `slot_id` (or all if -1).
    void send_midi_panic(int slot_id = -1);
    MidiSlotRuntimeStatus midi_slot_status(int slot_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lv
