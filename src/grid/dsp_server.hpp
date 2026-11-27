#pragma once
// dsp_server.hpp — LanVid Grid Distributed DSP Server (up to 256 channels, zero-latency LV-DSP Rack)
// + Hot Standby Redundancy Failover Controller (Primary + Backup mirroring with < 10 ms switchover).

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "grid/grid_node.hpp"

namespace lv::grid {

// Per-channel zero-latency studio DSP strip (HPF + 3-band EQ + Compressor + Gate + Brickwall Limiter).
class GridChannelDspStrip {
public:
    explicit GridChannelDspStrip(uint32_t sample_rate = 48000);

    void set_sample_rate(uint32_t sample_rate);
    void set_params(const L2DspParamPayload& p);
    L2DspParamPayload params() const;
    void process_inplace(float* buf, uint32_t frames);
    float last_gr_db() const { return last_gr_db_; }

private:
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        inline float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    void rebuild_filters();

    uint32_t sample_rate_ = 48000;
    L2DspParamPayload params_{};
    Biquad hpf_{};
    Biquad eq_low_{};
    Biquad eq_mid_{};
    Biquad eq_high_{};
    float comp_env_ = 0.0f;
    float gate_env_ = 0.0f;
    float gate_gain_ = 1.0f;
    float lim_gain_ = 1.0f;
    float last_gr_db_ = 0.0f;
};

// Distributed Layer 2 DSP Offload Server (runs on Linux RT / Windows / macOS).
// Receives dry kL2Audio frames + kL2DspParam updates, processes through LV-DSP strips,
// and immediately returns processed kL2Audio directly to the caller's (or configured return) MAC!
class GridDspServer {
public:
    explicit GridDspServer(std::unique_ptr<L2Transport> transport, const std::string& server_name = "LanVid DSP Server",
                           bool is_backup = false, uint32_t sample_rate = 48000);
    ~GridDspServer();

    GridNode& node() { return node_; }
    const GridNode& node() const { return node_; }
    MacAddress mac() const { return node_.mac(); }
    bool is_backup() const { return is_backup_; }

    // Optional override: return processed audio to a specific destination MAC instead of echoing to sender.
    void set_return_mac_override(const MacAddress& dst);
    void set_channel_params(uint16_t channel, const L2DspParamPayload& p);

    // Polls incoming L2 frames, processes audio blocks in-place, returns processed frames, and emits heartbeat.
    int service(int max_frames = 256);
    bool emit_heartbeat();

    float cpu_load_pct() const { return cpu_load_pct_.load(std::memory_order_relaxed); }
    uint64_t frames_processed() const { return frames_processed_.load(std::memory_order_relaxed); }

private:
    void on_audio_rx(const MacAddress& src, uint16_t stream_id, uint16_t ch_start, uint16_t ch_count,
                     uint16_t samples_per_ch, uint32_t media_ts_lo, const float* const* planes, uint8_t flags);
    void on_dsp_param_rx(const MacAddress& src, const L2DspParamPayload& p);

    GridNode node_;
    bool is_backup_ = false;
    uint32_t sample_rate_ = 48000;
    uint64_t start_us_ = 0;

    mutable std::mutex mu_;
    MacAddress return_override_{};
    std::array<GridChannelDspStrip, kGridMaxChannels> strips_;
    std::array<std::vector<float>, kGridMaxChannels> out_bufs_;
    std::array<float*, kGridMaxChannels> out_ptrs_{};

    std::atomic<float> cpu_load_pct_{0.0f};
    std::atomic<uint64_t> frames_processed_{0};
    std::atomic<uint16_t> active_ch_{0};
};

// Hot Standby Redundancy Controller (mirrors dry audio & DSP params to Primary + Backup DSP servers
// and automatically switches output selection to Backup within < 10 ms if Primary fails).
struct FailoverTelemetry {
    MacAddress primary_mac{};
    MacAddress backup_mac{};
    bool using_backup = false;
    bool primary_alive = false;
    bool backup_alive = false;
    float primary_cpu_pct = 0.0f;
    float backup_cpu_pct = 0.0f;
    uint32_t failover_count = 0;
};

class GridFailoverController {
public:
    GridFailoverController(GridNode& host_node, const MacAddress& primary_mac, const MacAddress& backup_mac,
                           uint64_t failover_timeout_us = 8000);

    // Mirrors dry audio block to BOTH Primary and Backup DSP servers simultaneously.
    void send_dry_mirrored(uint16_t stream_id, const float* const* planes, uint16_t ch_start,
                           uint16_t ch_count, uint16_t samples_per_ch, uint32_t media_ts_lo);

    // Mirrors a DSP plugin parameter update to BOTH Primary and Backup servers.
    void send_param_mirrored(const L2DspParamPayload& param);

    // Feeds a returned processed audio frame (from Primary or Backup).
    // Returns true if this frame belongs to the currently selected active server (or triggers instant failover).
    bool on_processed_return(const MacAddress& src_mac, uint64_t now_us_val);

    // Feeds a server heartbeat and evaluates timeout-based failover.
    void on_heartbeat(const MacAddress& src_mac, const L2HeartbeatPayload& hb, uint64_t now_us_val);
    void check_failover(uint64_t now_us_val);

    FailoverTelemetry telemetry() const;

private:
    GridNode& host_;
    MacAddress primary_mac_{};
    MacAddress backup_mac_{};
    uint64_t timeout_us_ = 8000;

    mutable std::mutex mu_;
    bool using_backup_ = false;
    uint64_t start_us_ = 0;
    uint64_t last_primary_us_ = 0;
    uint64_t last_backup_us_ = 0;
    float primary_cpu_ = 0.0f;
    float backup_cpu_ = 0.0f;
    uint32_t failover_count_ = 0;
};

}  // namespace lv::grid
