#pragma once
// PersonalMonitorBus — Server-side 64-to-Stereo DSP Mixer & UDP Streaming Engine for up to 8
// smartphone personal monitoring clients ("Personal Monitor Bus").
//
// Each client bus mixes up to 64 matrix channels (gain_db, pan, mute, solo) into 32-bit float
// stereo L/R with 5 ms click-free coefficient smoothing and processes it through:
//   - High-Pass Filter (20..250 Hz, 2nd-order Butterworth)
//   - 3-Band EQ: Low Shelf (100 Hz), Mid Peak (300..5000 Hz, default 1.2 kHz), High Shelf (8 kHz), +-12 dB
//   - Stereo Feed-Forward Compressor (Threshold -40..0 dB, Ratio 1:1..10:1, Attack, Release, Makeup)
//   - Protective Brickwall Limiter (-0.5 dBFS ceiling) + Master Volume
//
// Processed 2-channel stereo audio is encoded into compact AUDIO2 (2ch, ~256-384 kbps) and sent
// directly to each smartphone's UDP endpoint; smartphones control their bus in real time via
// PacketType::kMonitorControl (0xA).

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/audio_tx.hpp"
#include "audio/master_clock.hpp"
#include "net/packet.hpp"
#include "net/udp_socket.hpp"

namespace lv {

constexpr int kMaxMonitorClients = 8;
constexpr int kMonitorMaxInputChannels = 64;
constexpr uint16_t kDefaultMonitorPort = 5020;

struct PersonalMonitorChannelMix {
    float gain_db = 0.0f;  // -60..+12 dB
    float pan = 0.0f;      // -1.0 (Left) .. +1.0 (Right)
    bool mute = false;
    bool solo = false;
};

struct PersonalMonitorDspParams {
    bool hpf_enabled = false;
    float hpf_hz = 80.0f;          // 20..250 Hz

    bool eq_enabled = false;
    float eq_low_db = 0.0f;        // -12..+12 dB @ 100 Hz
    float eq_mid_hz = 1200.0f;     // 300..5000 Hz
    float eq_mid_db = 0.0f;        // -12..+12 dB
    float eq_high_db = 0.0f;       // -12..+12 dB @ 8 kHz

    bool comp_enabled = false;
    float comp_thresh_db = -18.0f; // -40..0 dB
    float comp_ratio = 3.0f;       // 1.0..10.0
    float comp_attack_ms = 5.0f;   // 0.5..50 ms
    float comp_release_ms = 80.0f; // 10..500 ms
    float comp_makeup_db = 0.0f;   // 0..18 dB

    float master_volume_db = 0.0f; // -60..+12 dB
    bool master_mute = false;
};

// Single personal stereo monitor bus (64 inputs -> Stereo L/R + HPF + 3-band EQ + Comp + -0.5 dBFS Limiter).
class PersonalMonitorBus {
public:
    explicit PersonalMonitorBus(uint32_t sample_rate = 48000);

    void set_channel_mix(int ch, const PersonalMonitorChannelMix& mix);
    PersonalMonitorChannelMix channel_mix(int ch) const;

    void set_dsp(const PersonalMonitorDspParams& dsp);
    PersonalMonitorDspParams dsp() const;

    void reset();

    // Mixes `num_in_ch` planar float32 channels (up to 64) into `out_l` / `out_r` (length `frames`)
    // and applies the real-time DSP chain with 5 ms click-free smoothing.
    void process_block(const float* const* in_planes, int num_in_ch,
                       float* out_l, float* out_r, uint32_t frames);

    // Current gain reduction of the compressor + limiter in dB (<= 0 dB, for telemetry/tests).
    float last_comp_gr_db() const { return last_comp_gr_db_.load(std::memory_order_relaxed); }

private:
    struct BiquadCoeffs {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    };
    struct StereoBiquad {
        BiquadCoeffs cur{};
        BiquadCoeffs tgt{};
        float z1_l = 0.0f, z2_l = 0.0f;
        float z1_r = 0.0f, z2_r = 0.0f;

        void reset_state() { z1_l = z2_l = z1_r = z2_r = 0.0f; }
        void snap_to_target() { cur = tgt; }
        inline void step_smooth(float alpha) {
            cur.b0 += (tgt.b0 - cur.b0) * alpha;
            cur.b1 += (tgt.b1 - cur.b1) * alpha;
            cur.b2 += (tgt.b2 - cur.b2) * alpha;
            cur.a1 += (tgt.a1 - cur.a1) * alpha;
            cur.a2 += (tgt.a2 - cur.a2) * alpha;
        }
        inline void process(float& l, float& r) {
            const float yl = cur.b0 * l + z1_l;
            z1_l = cur.b1 * l - cur.a1 * yl + z2_l;
            z2_l = cur.b2 * l - cur.a2 * yl;
            l = yl;

            const float yr = cur.b0 * r + z1_r;
            z1_r = cur.b1 * r - cur.a1 * yr + z2_r;
            z2_r = cur.b2 * r - cur.a2 * yr;
            r = yr;
        }
    };

    void recompute_targets_locked();

    uint32_t sample_rate_ = 48000;
    float smooth_alpha_ = 0.02f;  // ~1.2 ms time constant @ 48 kHz
    mutable std::mutex mu_;

    std::array<PersonalMonitorChannelMix, kMonitorMaxInputChannels> ch_mix_{};
    PersonalMonitorDspParams dsp_{};

    // Smoothed linear per-channel L/R gains (click-free 5 ms ramp)
    std::array<float, kMonitorMaxInputChannels> cur_gain_l_{};
    std::array<float, kMonitorMaxInputChannels> cur_gain_r_{};
    std::array<float, kMonitorMaxInputChannels> tgt_gain_l_{};
    std::array<float, kMonitorMaxInputChannels> tgt_gain_r_{};

    StereoBiquad hpf_{};
    StereoBiquad eq_low_{};
    StereoBiquad eq_mid_{};
    StereoBiquad eq_high_{};

    float comp_env_ = 0.0f;
    float lim_gain_ = 1.0f;
    static constexpr uint32_t kLookahead = 48;     // 1 ms @ 48 kHz
    static constexpr uint32_t kLookaheadBuf = kLookahead;
    std::array<float, kLookaheadBuf> la_l_{}, la_r_{}, la_req_{}, la_env_{};
    float la_env_sum_ = float(kLookahead);
    uint32_t la_pos_ = 0;
    float cur_master_gain_ = 1.0f;
    float tgt_master_gain_ = 1.0f;
    bool initialized_ = false;
    std::atomic<float> last_comp_gr_db_{0.0f};
};

struct MonitorClientInfo {
    int slot = -1;            // 0..7
    uint32_t client_id = 0;
    std::string name;
    std::string address;      // "ip:port"
    uint16_t bitrate_kbps = 256;
    uint16_t latency_ms = 20;
    bool muted = false;
    bool connected = false;
    uint64_t packets_tx = 0;
    uint64_t control_rx = 0;
};

constexpr uint16_t kDefaultMonitorDiscoveryPort = 5021;
constexpr const char* kMonitorDiscoveryReq = "LV_MON_DISC_REQ";
constexpr const char* kMonitorDiscoveryRespPrefix = "LV_MON_DISC_RESP|";

struct DiscoveredMonitorServer {
    std::string name;
    std::string ip;
    uint16_t port = kDefaultMonitorPort;
    uint8_t channels = 8;
};

// Broadcasts LV_MON_DISC_REQ on 255.255.255.255 (+ loopback fallback) to find LanVid Studio servers
// even when mDNS/Bonjour is blocked on Windows or Wi-Fi routers.
std::vector<DiscoveredMonitorServer> discover_monitor_servers(
    int timeout_ms = 350, uint16_t discovery_port = kDefaultMonitorDiscoveryPort,
    bool include_loopback = false);

// Server hosting up to 8 PersonalMonitorBus instances over UDP (control + 2ch AUDIO2 stream).
class PersonalMonitorServer {
public:
    explicit PersonalMonitorServer(uint32_t sample_rate = 48000);
    ~PersonalMonitorServer();

    bool start(uint16_t bind_port = kDefaultMonitorPort, const std::string& bind_ip = "0.0.0.0",
               uint16_t discovery_port = kDefaultMonitorDiscoveryPort);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    uint16_t bound_port() const { return bound_port_; }
    uint16_t discovery_port() const { return disc_port_; }
    void set_studio_name(const std::string& name);

    uint64_t total_control_rx() const { return total_control_rx_.load(std::memory_order_relaxed); }
    uint64_t discovery_replies() const { return discovery_replies_.load(std::memory_order_relaxed); }

    void set_channel_name(int ch, const std::string& name);
    void set_client_mute(int slot, bool muted);
    // bitrate_kbps: total stereo kbps (128..512, i.e. 64..256 per channel) or kMonitorBitrateLossless.
    void set_client_bitrate(int slot, uint16_t bitrate_kbps);
    std::vector<MonitorClientInfo> clients() const;

    // Polls incoming kMonitorControl packets from smartphones (safe to call from audio/control thread).
    void poll_control();

    // Pushes one 48 kHz multi-channel audio block (up to 64 planar channels) from MultiDeviceAudioMatrix,
    // updates VU meters, mixes & DSP-processes each connected client's stereo bus, and sends 2ch AUDIO2!
    void process_and_send(const float* const* in_planes, int num_in_ch, uint32_t frames);

    // Direct access to a client's DSP bus (for inspection/tests).
    PersonalMonitorBus* bus(int slot);

    // Pacer (dedicated real-time send thread) statistics, for tests / diagnostics.
    uint64_t paced_chunks() const { return paced_chunks_.load(std::memory_order_relaxed); }
    uint64_t ring_overflows() const { return ring_overflows_.load(std::memory_order_relaxed); }
    static constexpr uint32_t kPaceTickUs = 2500;   // pacer period
    static constexpr uint32_t kPaceChunk = 120;     // frames per pacer step (2.5 ms @ 48 kHz)
    static constexpr uint32_t kPaceRingFrames = 16384;

private:
    struct ClientSlot {
        bool active = false;
        uint32_t client_id = 0;
        std::string name;
        Endpoint ep{};
        uint16_t bitrate_kbps = 256;
        uint16_t latency_ms = 20;
        bool server_muted = false;
        uint64_t last_seen_us = 0;
        uint64_t packets_tx = 0;
        uint64_t control_rx = 0;
        std::unique_ptr<PersonalMonitorBus> bus;
        std::unique_ptr<AudioTxEngine> tx;
        uint64_t media_sample = 0;
        int8_t mix_peak_dbfs = -120;  // peak of the stereo mix last sent to this phone
        // Paced send ring: the mixer writes processed stereo blocks (any size, e.g. 1024 frames), the
        // pacer thread drains them in 2.5 ms pieces on a steady clock -> one packet every 5 ms, no bursts.
        std::vector<float> ring_l, ring_r;
        uint64_t ring_wr = 0;  // frames written (== media timestamp of the next written frame)
        uint64_t ring_rd = 0;  // frames handed to the AudioTxEngine
        double credit = 0.0;   // frames the pacer may send now
        bool primed = false;   // one mixer block queued before the first send
    };
    void pacer_loop();
    void pace_tick_locked(double dt_us);
    void apply_bitrate_locked(ClientSlot& s, uint16_t bitrate_kbps);

    int find_or_alloc_slot_locked(uint32_t client_id, const Endpoint& ep);
    void handle_control_packet(const PacketHeader& h, const uint8_t* payload, size_t len,
                               const Endpoint& src, uint64_t now);
    void send_meta_response(const Endpoint& dst, uint32_t client_id, int slot);

    uint32_t sample_rate_ = 48000;
    uint16_t bound_port_ = 0;
    uint16_t disc_port_ = 0;
    std::string studio_name_ = "LanVid Studio";
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_control_rx_{0};
    std::atomic<uint64_t> discovery_replies_{0};
    UdpSocket sock_;
    UdpSocket disc_sock_;
    mutable std::mutex mu_;
    std::array<ClientSlot, kMaxMonitorClients> slots_{};
    std::array<std::string, kMonitorMaxInputChannels> ch_names_{};
    std::array<int8_t, kMonitorMaxInputChannels> ch_peaks_dbfs_{};
    uint8_t active_in_channels_ = 0;
    std::vector<float> tmp_l_;
    std::vector<float> tmp_r_;
    std::vector<float> pace_l_ = std::vector<float>(kPaceChunk);
    std::vector<float> pace_r_ = std::vector<float>(kPaceChunk);
    uint32_t in_block_max_ = 0;  // largest mixer block seen recently (slow decay)
    std::thread pacer_;
    std::atomic<bool> pacer_stop_{false};
    std::atomic<uint64_t> paced_chunks_{0};
    std::atomic<uint64_t> ring_overflows_{0};
};

// Smartphone-side C++ client (exposed via C ABI lv_monitor_client_* for Android/iOS/tests).
class PersonalMonitorClient {
public:
    explicit PersonalMonitorClient(uint32_t sample_rate = 48000);
    ~PersonalMonitorClient();

    bool connect(const std::string& server_ip, uint16_t server_port, const std::string& client_name,
                 uint16_t bitrate_kbps = 256, uint16_t latency_ms = 20);
    void disconnect();
    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

    bool request_meta();
    bool set_channel_mix(int ch, const PersonalMonitorChannelMix& mix);
    bool set_dsp(const PersonalMonitorDspParams& dsp);
    // Re-sends kHello with the current name / bitrate / buffer: keepalive (server drops clients unseen for
    // 15 s) and the way to change stream quality or buffer live, without reconnecting.
    bool keepalive();
    bool update_stream(uint16_t bitrate_kbps, uint16_t latency_ms);

    // Polls incoming AUDIO2 stereo packets & kSyncMetaResp from PC server.
    void poll();

    // Pops decoded stereo interleaved float32 audio (2 channels, `frames` stereo frames).
    void pop_stereo(float* out_interleaved, uint32_t frames);

    std::vector<MonitorMetaChannel> last_meta() const;
    MonitorBusTelemetry last_bus_telemetry() const;
    uint64_t audio_packets_rx() const { return audio_rx_pkts_.load(std::memory_order_relaxed); }
    uint64_t meta_packets_rx() const { return meta_rx_pkts_.load(std::memory_order_relaxed); }
    // Real playout delay of the jitter buffer (one-way-clock position minus playout cursor): steady at the
    // chosen buffer (+ any learned cushion), not the burst-to-burst arrival difference.
    double buffer_ms() const { return matrix_.current_effective_latency_ms(); }
    uint64_t underruns() const { return matrix_.total_underruns(); }
    uint64_t prefill_events() const { return matrix_.prefill_events(); }
    uint64_t wake_pings_sent() const { return pings_.load(std::memory_order_relaxed); }
    // Interval of the tiny keep-awake datagram sent while connected (stops Wi-Fi power save from batching
    // incoming audio for 20-100 ms). Default 15 ms; 0 disables.
    static constexpr uint64_t kWakePingUs = 15'000;

private:
    bool send_control(const MonitorControlPayload& mc);
    std::atomic<uint64_t> last_tx_us_{0};
    std::atomic<uint64_t> pings_{0};

    uint32_t sample_rate_ = 48000;
    uint32_t client_id_ = 0;
    std::atomic<bool> connected_{false};
    UdpSocket sock_;
    Endpoint server_ep_{};
    MasterClock mclock_;
    AudioSyncMatrix matrix_;
    AudioRxEngine arx_;
    mutable std::mutex meta_mu_;
    std::vector<MonitorMetaChannel> meta_;
    MonitorBusTelemetry bus_tel_{};
    std::atomic<uint64_t> audio_rx_pkts_{0};
    std::atomic<uint64_t> meta_rx_pkts_{0};
    std::string name_;
    std::atomic<uint16_t> bitrate_kbps_{256};
    std::atomic<uint16_t> latency_ms_{20};
};

}  // namespace lv
