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
#include <deque>
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
    uint64_t active_channel_mask = ~0ull;  // Receiver-Driven Subscription bitmask
    bool flow_enabled = false;
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

    // Dante-inspired Wi-Fi Redundancy (1+1 Duplicate with +1.5 ms time stagger, zero NACK wait)
    void set_wifi_redundancy(bool enabled, uint32_t stagger_us = 1500);
    bool wifi_redundancy() const { return wifi_redundancy_.load(std::memory_order_acquire); }

    // Dante-style Multi-Channel Monitor Flows (kMonitorAudioFlow: pack 2..16 active subscribed channels
    // into ONE compact UDP datagram) + Receiver-Driven Subscriptions (active_channel_mask).
    void set_multichannel_flow(bool enabled, uint8_t flow_channels = 2);
    bool multichannel_flow() const { return multichannel_flow_.load(std::memory_order_acquire); }
    uint8_t multichannel_flow_channels() const { return flow_channels_.load(std::memory_order_relaxed); }

    uint64_t flow_packets_tx() const { return flow_tx_pkts_.load(std::memory_order_relaxed); }
    uint64_t dup_packets_tx() const { return dup_tx_pkts_.load(std::memory_order_relaxed); }
    uint64_t muted_packets_suppressed() const { return muted_suppressed_.load(std::memory_order_relaxed); }

    // Physical NIC selection & Dual-NIC 1+1 Failover (Primary Ethernet + Secondary Wi-Fi/Ethernet)
    bool set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip);
    bool dual_nic_active() const { return sec_sock_active_.load(std::memory_order_acquire); }
    uint64_t primary_tx_bytes() const { return pri_tx_bytes_.load(std::memory_order_relaxed); }
    uint64_t secondary_tx_bytes() const { return sec_tx_bytes_.load(std::memory_order_relaxed); }
    uint64_t failover_packets() const { return failover_pkts_.load(std::memory_order_relaxed); }
    void set_primary_link_down(bool down) { primary_link_down_.store(down, std::memory_order_release); }
    bool primary_link_down() const { return primary_link_down_.load(std::memory_order_acquire); }

    static constexpr uint32_t kPaceTickUs = 2500;   // pacer period (drops to 1000 us when 1+1 redundancy active)
    static constexpr uint32_t kPaceChunk = 120;     // frames per pacer step (2.5 ms @ 48 kHz)
    static constexpr uint32_t kPaceRingFrames = 16384;

private:
    struct PendingFlowDup {
        uint64_t due_us = 0;
        int slot_idx = -1;
        Endpoint dst{};
        std::vector<uint8_t> dgram;
    };
    struct ClientSlot {
        bool active = false;
        uint32_t client_id = 0;
        std::string name;
        Endpoint ep{};
        uint16_t bitrate_kbps = 256;
        uint16_t latency_ms = 20;
        bool server_muted = false;
        bool client_supports_flow = false;
        bool client_wants_redundancy = false;
        uint64_t active_channel_mask = ~0ull;  // Receiver-Driven Subscription bitmask
        uint32_t flow_seq = 0;
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
        // Optional multi-channel direct flow rings (up to 16 channels when flow_channels > 2)
        std::array<std::vector<float>, kMaxMonitorFlowChannels> flow_rings;
        // Opaque per-channel Opus encoders for compact kMonitorAudioFlow encoding (destroyed in stop/reset)
        std::array<void*, kMaxMonitorFlowChannels> flow_opus{};
        int flow_opus_bps = 0;
        uint64_t ring_wr = 0;  // frames written (== media timestamp of the next written frame)
        uint64_t ring_rd = 0;  // frames handed to the AudioTxEngine / Flow emitter
        double credit = 0.0;   // frames the pacer may send now
        bool primed = false;   // one mixer block queued before the first send
        uint64_t flow_stage_ts = 0;
        uint32_t flow_stage_n = 0;
        std::array<std::vector<float>, kMaxMonitorFlowChannels> flow_stage;
    };
    void pacer_loop();
    void pace_tick_locked(double dt_us);
    void flush_flow_dups_locked(uint64_t now);
    void emit_monitor_flow_locked(ClientSlot& s, int slot_idx, uint8_t flow_ch, uint32_t samples,
                                  uint64_t media_ts, uint64_t now);
    void destroy_slot_flow_codecs_locked(ClientSlot& s);
    void refresh_any_redundancy_locked();
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
    std::atomic<bool> wifi_redundancy_{false};
    std::atomic<bool> any_client_redundancy_{false};
    std::atomic<uint32_t> dup_stagger_us_{1500};
    std::atomic<bool> multichannel_flow_{true};
    std::atomic<uint8_t> flow_channels_{2};
    std::atomic<uint64_t> total_control_rx_{0};
    std::atomic<uint64_t> discovery_replies_{0};
    std::atomic<uint64_t> flow_tx_pkts_{0};
    std::atomic<uint64_t> dup_tx_pkts_{0};
    std::atomic<uint64_t> muted_suppressed_{0};
    std::atomic<uint64_t> pri_tx_bytes_{0};
    std::atomic<uint64_t> sec_tx_bytes_{0};
    std::atomic<uint64_t> failover_pkts_{0};
    std::atomic<bool> sec_sock_active_{false};
    std::atomic<bool> primary_link_down_{false};
    std::string primary_nic_ip_;
    std::string secondary_nic_ip_;
    UdpSocket sock_;
    UdpSocket sec_sock_;
    UdpSocket disc_sock_;
    mutable std::mutex mu_;
    std::array<ClientSlot, kMaxMonitorClients> slots_{};
    std::deque<PendingFlowDup> flow_dups_;
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

struct MonitorDanteTelemetry {
    bool clock_locked = false;
    uint32_t clock_offset_jitter_us = 0;
    uint64_t duplicates_recovered = 0;  // lost primary Wi-Fi packets rescued by +1.5 ms 1+1 copy (0 NACK)
    uint32_t active_flow_channels = 0;
    double effective_playout_ms = 0.0;
    bool wifi_redundancy_enabled = false;
    uint64_t flow_packets_rx = 0;
    uint64_t nacks_sent = 0;
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
    // Pops decoded multi-channel planar float32 audio (up to 16 flow channels).
    void pop_multichannel(float* const* out_planes, uint32_t num_channels, uint32_t frames);

    // Receiver-Driven Subscriptions: explicit bitmask or automatically derived from set_channel_mix Mute/Solo.
    void set_active_channel_mask(uint64_t mask);
    uint64_t active_channel_mask() const { return active_channel_mask_.load(std::memory_order_acquire); }
    uint16_t last_rx_channel_mask() const { return last_rx_flow_mask_.load(std::memory_order_relaxed); }

    // Per-client Dante 1+1 Wi-Fi Redundancy (+1.5 ms backup copy negotiated in kHello + deterministic playout)
    void set_wifi_redundancy(bool enabled);
    bool wifi_redundancy() const { return wifi_redundancy_.load(std::memory_order_acquire); }

    // Physical NIC selection (e.g. Wi-Fi en0/wlan0 + USB-C Ethernet en2/eth0 1+1)
    bool set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip);
    std::string primary_nic_ip() const { return primary_nic_ip_; }
    std::string secondary_nic_ip() const { return secondary_nic_ip_; }

    MonitorDanteTelemetry dante_telemetry() const;
    uint64_t duplicates_recovered() const {
        return flow_dups_recovered_.load(std::memory_order_relaxed) + arx_.total_duplicates_recovered();
    }
    bool clock_locked() const { return clock_locked_.load(std::memory_order_relaxed); }
    uint32_t clock_offset_jitter_us() const { return clock_jitter_us_.load(std::memory_order_relaxed); }

    std::vector<MonitorMetaChannel> last_meta() const;
    MonitorBusTelemetry last_bus_telemetry() const;
    uint64_t audio_packets_rx() const { return audio_rx_pkts_.load(std::memory_order_relaxed); }
    uint64_t flow_packets_rx() const { return flow_rx_pkts_.load(std::memory_order_relaxed); }
    uint64_t flow_duplicates_dropped() const { return flow_dup_drops_.load(std::memory_order_relaxed); }
    uint64_t nacks_sent() const { return arx_.total_nacks_sent(); }
    uint64_t meta_packets_rx() const { return meta_rx_pkts_.load(std::memory_order_relaxed); }
    // Real playout delay of the jitter buffer (one-way-clock position minus playout cursor): steady at the
    // chosen buffer (+ any learned cushion), not the burst-to-burst arrival difference.
    double buffer_ms() const { return matrix_.current_effective_latency_ms(); }
    uint64_t underruns() const { return matrix_.total_underruns(); }
    uint64_t prefill_events() const { return matrix_.prefill_events(); }
    uint64_t wake_pings_sent() const { return pings_.load(std::memory_order_relaxed); }
    // Interval of the compact keep-alive + Rx Subscription datagram sent while connected (15 ms).
    static constexpr uint64_t kWakePingUs = 15'000;

private:
    bool send_control(const MonitorControlPayload& mc);
    bool send_keepalive_ping(uint64_t now);
    void recompute_active_channel_mask();
    void handle_flow_packet(const PacketHeader& h, const uint8_t* payload, size_t len, uint64_t now);
    void decode_flow_frame(const MonitorAudioFlowHeader& fh,
                           const uint8_t* const ch_ptrs[kMaxMonitorFlowChannels],
                           const uint16_t ch_lens[kMaxMonitorFlowChannels]);
    void flush_flow_opus_inorder();
    void service_flow_reorder(uint64_t now);
    void flow_one_way_observe(double media_end_us, uint64_t now);
    void destroy_flow_decoders();
    std::atomic<uint64_t> last_tx_us_{0};
    std::atomic<uint64_t> pings_{0};
    std::atomic<uint64_t> active_channel_mask_{~0ull};
    std::atomic<uint16_t> last_rx_flow_mask_{0};
    std::atomic<bool> wifi_redundancy_{false};
    std::atomic<bool> clock_locked_{false};
    std::atomic<uint32_t> clock_jitter_us_{0};
    std::atomic<uint64_t> flow_rx_pkts_{0};
    std::atomic<uint64_t> flow_dup_drops_{0};
    std::atomic<uint64_t> flow_dups_recovered_{0};
    bool mask_override_ = false;
    std::array<PersonalMonitorChannelMix, kMonitorMaxInputChannels> local_mix_{};
    // Instant seq deduplication ring for kMonitorAudioFlow (4096-bit window)
    static constexpr uint32_t kFlowDupWindow = 4096;
    bool flow_seen_ = false;
    uint32_t flow_highest_ = 0;
    std::array<uint64_t, kFlowDupWindow / 64> flow_recv_bits_{};
    // In-order Opus reorder gate for kMonitorAudioFlow (zero added latency in-order; prevents MDCT clicks
    // when a +1.5 ms redundant backup copy arrives slightly after seq+1).
    struct PendingFlowOpus {
        bool valid = false;
        uint32_t seq = 0;
        uint64_t arrived_us = 0;
        MonitorAudioFlowHeader hdr{};
        std::vector<uint8_t> raw_payload;
    };
    static constexpr uint32_t kFlowOpusReorderSlots = 16;
    bool flow_opus_seq_init_ = false;
    uint32_t flow_opus_next_seq_ = 0;
    std::array<PendingFlowOpus, kFlowOpusReorderSlots> flow_opus_pending_{};
    // One-way clock observer (min-one-way-delay PTP-like offset filter) for kMonitorAudioFlow
    bool flow_ow_have_ = false;
    double flow_ow_applied_ = 0.0;
    double flow_ow_win_[4] = {0, 0, 0, 0};
    double flow_ow_cur_ = -1e300;
    uint64_t flow_ow_win_start_ = 0;
    int flow_ow_win_n_ = 0;
    uint32_t flow_ow_samples_ = 0;
    double flow_ow_jitter_ema_ = 0.0;
    std::array<void*, kMaxMonitorFlowChannels> flow_opus_dec_{};
    std::vector<float> flow_dec_scratch_;

    uint32_t sample_rate_ = 48000;
    uint32_t client_id_ = 0;
    std::atomic<bool> connected_{false};
    std::string primary_nic_ip_;
    std::string secondary_nic_ip_;
    UdpSocket sock_;
    UdpSocket sec_sock_;
    std::atomic<bool> sec_sock_active_{false};
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
