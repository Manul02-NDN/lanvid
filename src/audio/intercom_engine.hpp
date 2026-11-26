#pragma once
// TallyIntercomEngine — Isolated full-duplex Director <-> Camera Operator Tally & Voice Intercom.
//
// - Tally (PacketType::kTallyState = 0xB):
//     Studio -> Camera: Standby (0) / Preview (1, Green) / Program ON AIR (2, Red) + remote REC trigger,
//     sent with 3x Wi-Fi redundancy on every state change and refreshed via 250 ms heartbeat.
//     Camera -> Studio: periodic registration + battery % + device temperature telemetry.
//
// - Duplex Intercom (PacketType::kIntercomAudio = 0xC):
//     48 kHz mono Opus VOIP (~48 kbps, 10 ms = 480 samples, fallback PCM16), completely isolated from
//     the main program audio bus (AUDIO2):
//       * Downlink (Studio -> Camera IFB): Direct Call to selected camera(s) or All Call (Partyline),
//         with automatic smooth program ducking (`ducking_gain()`) when the director speaks.
//       * Uplink (Camera -> Studio): PTT or Latch hands-free from operator headset to director output.

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/packet.hpp"
#include "net/udp_socket.hpp"

namespace lv {

constexpr int kMaxStudioCameras = 8;
constexpr uint16_t kDefaultTallyIntercomPort = 5022;
constexpr int kIntercomFrameSamples = 480;  // 10 ms @ 48 kHz mono

struct StudioCameraInfo {
    uint32_t camera_id = 0;
    std::string label;
    std::string address;
    uint8_t tally_state = kTallyStandby;
    bool rec_trigger = false;
    bool talk_to_cam = false;
    bool listen_from_cam = true;
    uint8_t battery_pct = 100;
    int8_t temp_c = 30;
    bool operator_speaking = false;
    float voice_peak_dbfs = -120.0f;
    bool connected = false;
    uint64_t tally_tx_count = 0;
    uint64_t intercom_rx_pkts = 0;
    uint64_t intercom_tx_pkts = 0;
};

class TallyIntercomEngine {
public:
    using TallyCallback = std::function<void(const TallyStatePayload&)>;

    TallyIntercomEngine();
    ~TallyIntercomEngine();

    TallyIntercomEngine(const TallyIntercomEngine&) = delete;
    TallyIntercomEngine& operator=(const TallyIntercomEngine&) = delete;

    // ---- Lifecycle ----
    // Starts in Studio Director Server mode (listens on UDP `bind_port` for up to 8 cameras).
    bool start_studio(uint16_t bind_port = kDefaultTallyIntercomPort, const std::string& bind_ip = "0.0.0.0");

    // Starts in Mobile Camera Operator Client mode (connects to Studio IP:port).
    bool start_camera(const std::string& studio_ip, uint16_t studio_port, uint32_t camera_id,
                      const std::string& label);

    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    bool is_studio_mode() const { return studio_mode_.load(std::memory_order_relaxed); }
    uint16_t bound_port() const { return bound_port_; }

    // ---- Tally API ----
    // Studio side: sets Tally state (0=Standby, 1=Preview, 2=Program) + label + remote REC trigger.
    // Sends 3x redundant UDP packets immediately to the target camera.
    bool studio_set_camera_tally(uint32_t camera_id, uint8_t tally_state, const std::string& label,
                                 bool rec_trigger);

    // Camera side: registers callback invoked immediately on incoming Tally state changes.
    void camera_set_tally_callback(TallyCallback cb);
    // Remote camera control (lens / focus / quality preset), Studio -> Camera.
    using CameraControlCallback = std::function<void(const CameraControlPayload&)>;
    bool studio_set_camera_control(uint32_t camera_id, const CameraControlPayload& ctl);
    void camera_set_control_callback(CameraControlCallback cb);
    CameraControlPayload camera_last_control() const;
    TallyStatePayload camera_last_tally() const;
    void camera_update_telemetry(uint8_t battery_pct, int8_t temp_c);

    // ---- Duplex Intercom Voice API ----
    // Pushes 48 kHz mono float32 mic samples (Director mic in Studio, Operator mic in Camera).
    // Encodes 10 ms (480-sample) Opus VOIP frames and transmits immediately when talk_active is true.
    void push_mic_pcm(const float* samples, int frames);

    // Pulls 48 kHz mono float32 received intercom voice (Operator IFB or Director headphones).
    int pull_rx_pcm(float* out_samples, int frames);

    // Local talkback state (PTT/Latch, listen mute, RX volume, sidetone gain).
    void set_intercom_state(bool talk_active, bool listen_muted, float rx_gain, float sidetone_gain);

    // Studio side: per-camera talkback routing (Direct Call / Listen / All Call Partyline).
    void studio_set_camera_intercom(uint32_t camera_id, bool talk_to_cam, bool listen_from_cam, bool all_call);
    bool studio_all_call() const { return all_call_.load(std::memory_order_relaxed); }

    // Camera side: true while Director IFB audio is actively arriving; `ducking_gain()` smoothly
    // attenuates program monitoring to 0.25 (-12 dB) during director speech and releases to 1.0.
    bool is_director_speaking() const;
    float ducking_gain() const { return ducking_gain_.load(std::memory_order_relaxed); }
    float rx_peak_dbfs() const { return rx_peak_dbfs_.load(std::memory_order_relaxed); }
    float tx_peak_dbfs() const { return tx_peak_dbfs_.load(std::memory_order_relaxed); }

    // Polls incoming UDP kTallyState / kIntercomAudio packets and runs 250 ms Tally heartbeats.
    void poll();

    // Studio telemetry list of connected cameras (for LanVid Studio UI).
    std::vector<StudioCameraInfo> studio_cameras() const;

    uint64_t intercom_tx_packets() const { return tx_pkts_.load(std::memory_order_relaxed); }
    uint64_t intercom_rx_packets() const { return rx_pkts_.load(std::memory_order_relaxed); }
    uint64_t intercom_duplicates_dropped() const { return dup_drops_.load(std::memory_order_relaxed); }

    // Wi-Fi Redundant Flow (1+1 Duplicate): queues an identical +1.5 ms staggered backup datagram
    // for every kTallyState and kIntercomAudio packet; receiver deduplicates instantly by `seq`.
    void set_wifi_redundancy(bool enabled, uint32_t stagger_us = 1500);
    bool wifi_redundancy() const { return wifi_redundancy_.load(std::memory_order_acquire); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> running_{false};
    std::atomic<bool> studio_mode_{false};
    uint16_t bound_port_ = 0;
    std::atomic<bool> all_call_{false};
    std::atomic<float> ducking_gain_{1.0f};
    std::atomic<float> rx_peak_dbfs_{-120.0f};
    std::atomic<float> tx_peak_dbfs_{-120.0f};
    std::atomic<uint64_t> tx_pkts_{0};
    std::atomic<uint64_t> rx_pkts_{0};
    std::atomic<uint64_t> dup_drops_{0};
    std::atomic<bool> wifi_redundancy_{false};
    std::atomic<uint32_t> dup_stagger_us_{1500};
};

}  // namespace lv
