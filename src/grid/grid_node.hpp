#pragma once
// grid_node.hpp — LanVid Grid Layer 2 Node (256ch Audio, L2 Broadcast Discovery, Clock Master/Slave,
// and Decentralized Peer-to-Peer Patchbay Control Plane).

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "grid/l2_protocol.hpp"
#include "grid/l2_transport.hpp"

namespace lv::grid {

struct GridDiscoveredNode {
    MacAddress mac{};
    std::string name;
    GridNodeRole role = GridNodeRole::kStudioHost;
    uint16_t max_channels = kGridMaxChannels;
    uint32_t sample_rate = 48000;
    float cpu_load_pct = 0.0f;
    float buffer_fill_pct = 0.0f;
    bool is_clock_master = false;
    bool is_backup_dsp = false;
    uint64_t last_seen_us = 0;
};

struct GridClockStatus {
    bool is_master = false;
    bool locked = false;
    MacAddress master_mac{};
    uint64_t master_sample_pos = 0;
    uint32_t clock_epoch = 0;
    double drift_ppm = 0.0;
    uint64_t sync_rx_count = 0;
    uint64_t sync_tx_count = 0;
};

struct GridNodeConfig {
    std::string node_name = "LanVid Grid Node";
    GridNodeRole role = GridNodeRole::kStudioHost;
    uint16_t max_channels = kGridMaxChannels;  // up to 256 channels!
    uint32_t sample_rate = 48000;
    L2AudioFormat audio_fmt = L2AudioFormat::kPcm24Packed;
    VlanQosTag vlan{false, 0, 6};              // 802.1Q / 802.1p PCP=6 (Voice/Audio)
    bool clock_master = false;
    uint8_t clock_priority = 128;
    bool is_backup_dsp = false;
};

class GridNode {
public:
    using AudioRxCallback = std::function<void(const MacAddress& src, uint16_t stream_id,
                                               uint16_t ch_start, uint16_t ch_count,
                                               uint16_t samples_per_ch, uint32_t media_ts_lo,
                                               const float* const* planes, uint8_t flags)>;
    using DspParamCallback = std::function<void(const MacAddress& src, const L2DspParamPayload& param)>;
    using HeartbeatCallback = std::function<void(const MacAddress& src, const L2HeartbeatPayload& hb)>;

    explicit GridNode(std::unique_ptr<L2Transport> transport, const GridNodeConfig& cfg = {});
    ~GridNode();

    GridNode(const GridNode&) = delete;
    GridNode& operator=(const GridNode&) = delete;

    bool is_open() const { return transport_ && transport_->is_open(); }
    MacAddress mac() const { return transport_ ? transport_->mac() : MacAddress{}; }
    std::string backend_name() const { return transport_ ? transport_->backend_name() : "Closed"; }
    const GridNodeConfig& config() const { return cfg_; }

    void set_vlan_qos(const VlanQosTag& vlan);
    void set_clock_master(bool master);
    void set_audio_format(L2AudioFormat fmt);
    void set_telemetry(float cpu_pct, float buffer_pct);

    // ---- Callbacks ----
    void set_audio_rx_callback(AudioRxCallback cb);
    void set_dsp_param_callback(DspParamCallback cb);
    void set_heartbeat_callback(HeartbeatCallback cb);

    // ---- Layer 2 Broadcast Discovery (Zero IP!) ----
    bool broadcast_discovery_probe();
    bool announce_self(const MacAddress& dst = MacAddress::broadcast());
    std::vector<GridDiscoveredNode> discovered_nodes() const;

    // ---- Clock Master Sync ----
    bool emit_clock_sync(uint64_t master_sample_pos);
    GridClockStatus clock_status() const;

    // ---- Decentralized Peer-to-Peer Patchbay Control Plane ----
    // Instructs `target_src_node` to route its channels directly to `rule.dst_mac`!
    bool send_route_rule(const MacAddress& target_src_node, const L2RouteRulePayload& rule);
    void apply_local_route_rule(const L2RouteRulePayload& rule);
    std::vector<L2RouteRulePayload> active_tx_routes() const;

    // ---- DSP Control & Heartbeat ----
    bool send_dsp_param(const MacAddress& dst_dsp_mac, const L2DspParamPayload& param);
    bool send_heartbeat(const MacAddress& dst_mac, const L2HeartbeatPayload& hb);

    // ---- 256-Channel Direct Layer 2 Audio TX ----
    // Sends audio directly to one MAC (used by Stageboxes / DSP Server returns).
    size_t send_audio_direct(const MacAddress& dst, uint16_t stream_id,
                             const float* const* planes, uint16_t src_ch_start, uint16_t dst_ch_start,
                             uint16_t ch_count, uint16_t samples_per_ch, uint32_t media_ts_lo,
                             uint8_t flags = 0);

    // Executes ALL active local P2P TX routes (`active_tx_routes()`) for one audio block (up to 256ch).
    size_t push_routed_audio(const float* const* planes, uint16_t total_channels,
                             uint16_t samples_per_ch, uint32_t media_ts_lo);

    // Non-blocking poll: drains incoming L2 Ethernet frames and dispatches Discovery, Clock, Routes, Audio & DSP.
    int poll(int max_frames = 256);

    // Counters
    uint64_t audio_tx_frames() const { return audio_tx_frames_.load(std::memory_order_relaxed); }
    uint64_t audio_rx_frames() const { return audio_rx_frames_.load(std::memory_order_relaxed); }
    uint64_t control_rx_frames() const { return control_rx_frames_.load(std::memory_order_relaxed); }

private:
    void handle_frame(const ParsedL2Frame& f, uint64_t now_us_val);

    std::unique_ptr<L2Transport> transport_;
    GridNodeConfig cfg_{};
    mutable std::mutex mu_;

    std::vector<GridDiscoveredNode> peers_;
    std::vector<L2RouteRulePayload> tx_routes_;
    GridClockStatus clock_{};
    uint64_t prev_sync_local_us_ = 0;
    uint64_t prev_sync_samples_ = 0;

    AudioRxCallback on_audio_;
    DspParamCallback on_dsp_param_;
    HeartbeatCallback on_heartbeat_;

    std::atomic<uint16_t> seq_{0};
    std::atomic<float> cpu_pct_{0.0f};
    std::atomic<float> buf_pct_{0.0f};
    std::atomic<uint64_t> audio_tx_frames_{0};
    std::atomic<uint64_t> audio_rx_frames_{0};
    std::atomic<uint64_t> control_rx_frames_{0};

    // Scratch buffers for zero-allocation 256ch audio TX/RX
    std::array<std::vector<float>, kGridMaxChannels> rx_scratch_;
    std::array<float*, kGridMaxChannels> rx_ptrs_{};
};

}  // namespace lv::grid
