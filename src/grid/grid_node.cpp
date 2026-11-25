#include "grid/grid_node.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/clock.hpp"

namespace lv::grid {

GridNode::GridNode(std::unique_ptr<L2Transport> transport, const GridNodeConfig& cfg)
    : transport_(std::move(transport)), cfg_(cfg) {
    clock_.is_master = cfg_.clock_master;
    clock_.clock_epoch = 1;
    if (cfg_.clock_master && transport_) {
        clock_.master_mac = transport_->mac();
        clock_.locked = true;
    }
    for (uint16_t c = 0; c < kGridMaxChannels; ++c) {
        rx_scratch_[c].resize(128, 0.0f);
        rx_ptrs_[c] = rx_scratch_[c].data();
    }
}

GridNode::~GridNode() = default;

void GridNode::set_vlan_qos(const VlanQosTag& vlan) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.vlan = vlan;
}

void GridNode::set_clock_master(bool master) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.clock_master = master;
    clock_.is_master = master;
    if (master && transport_) {
        clock_.master_mac = transport_->mac();
        clock_.locked = true;
        clock_.drift_ppm = 0.0;
        ++clock_.clock_epoch;
    }
}

void GridNode::set_audio_format(L2AudioFormat fmt) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.audio_fmt = fmt;
}

void GridNode::set_telemetry(float cpu_pct, float buffer_pct) {
    cpu_pct_.store(std::clamp(cpu_pct, 0.0f, 100.0f), std::memory_order_relaxed);
    buf_pct_.store(std::clamp(buffer_pct, 0.0f, 100.0f), std::memory_order_relaxed);
}

void GridNode::set_audio_rx_callback(AudioRxCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_audio_ = std::move(cb);
}

void GridNode::set_dsp_param_callback(DspParamCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_dsp_param_ = std::move(cb);
}

void GridNode::set_heartbeat_callback(HeartbeatCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_heartbeat_ = std::move(cb);
}

bool GridNode::broadcast_discovery_probe() {
    if (!is_open()) return false;
    LvL2Header hdr{};
    hdr.type = L2PacketType::kDiscoveryReq;
    hdr.seq = seq_.fetch_add(1, std::memory_order_relaxed);
    VlanQosTag vlan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        vlan = cfg_.vlan;
    }
    uint8_t frame[128];
    const size_t n = write_l2_frame(frame, sizeof(frame), MacAddress::broadcast(), mac(), vlan, hdr, nullptr, 0);
    return n > 0 && transport_->send_frame(frame, n);
}

bool GridNode::announce_self(const MacAddress& dst) {
    if (!is_open()) return false;
    L2DiscoveryPayload p{};
    VlanQosTag vlan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        p.role = cfg_.role;
        p.flags = uint8_t((cfg_.clock_master ? 0x01u : 0u) | (cfg_.is_backup_dsp ? 0x02u : 0u));
        p.max_channels = cfg_.max_channels;
        p.sample_rate = cfg_.sample_rate;
        p.cpu_load_pct_x10 = uint16_t(std::lround(cpu_pct_.load(std::memory_order_relaxed) * 10.0f));
        p.buffer_fill_pct_x10 = uint16_t(std::lround(buf_pct_.load(std::memory_order_relaxed) * 10.0f));
        std::snprintf(p.node_name, sizeof(p.node_name), "%s", cfg_.node_name.c_str());
        vlan = cfg_.vlan;
    }
    uint8_t body[kL2DiscoverySize];
    const size_t blen = write_l2_discovery(body, sizeof(body), p);
    LvL2Header hdr{};
    hdr.type = L2PacketType::kDiscoveryResp;
    hdr.seq = seq_.fetch_add(1, std::memory_order_relaxed);
    uint8_t frame[256];
    const size_t n = write_l2_frame(frame, sizeof(frame), dst, mac(), vlan, hdr, body, blen);
    return n > 0 && transport_->send_frame(frame, n);
}

std::vector<GridDiscoveredNode> GridNode::discovered_nodes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return peers_;
}

bool GridNode::emit_clock_sync(uint64_t master_sample_pos) {
    if (!is_open()) return false;
    L2SyncClockPayload p{};
    VlanQosTag vlan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfg_.clock_master) return false;
        p.master_sample_pos = master_sample_pos;
        p.master_wallclock_us = now_us();
        p.sample_rate = cfg_.sample_rate;
        p.clock_epoch = clock_.clock_epoch;
        p.clock_priority = cfg_.clock_priority;
        clock_.master_sample_pos = master_sample_pos;
        ++clock_.sync_tx_count;
        vlan = cfg_.vlan;
    }
    uint8_t body[kL2SyncClockSize];
    const size_t blen = write_l2_sync_clock(body, sizeof(body), p);
    LvL2Header hdr{};
    hdr.type = L2PacketType::kSyncClock;
    hdr.flags = 0x01;
    hdr.seq = seq_.fetch_add(1, std::memory_order_relaxed);
    hdr.media_ts_lo = uint32_t(master_sample_pos & 0xFFFFFFFFull);
    uint8_t frame[128];
    const size_t n = write_l2_frame(frame, sizeof(frame), MacAddress::broadcast(), mac(), vlan, hdr, body, blen);
    return n > 0 && transport_->send_frame(frame, n);
}

GridClockStatus GridNode::clock_status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return clock_;
}

bool GridNode::send_route_rule(const MacAddress& target_src_node, const L2RouteRulePayload& rule) {
    if (!is_open()) return false;
    if (target_src_node == mac()) {
        apply_local_route_rule(rule);
        return true;
    }
    uint8_t body[kL2RouteRuleSize];
    const size_t blen = write_l2_route_rule(body, sizeof(body), rule);
    LvL2Header hdr{};
    hdr.type = L2PacketType::kRouteCtrl;
    hdr.seq = seq_.fetch_add(1, std::memory_order_relaxed);
    VlanQosTag vlan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        vlan = cfg_.vlan;
    }
    uint8_t frame[128];
    const size_t n = write_l2_frame(frame, sizeof(frame), target_src_node, mac(), vlan, hdr, body, blen);
    return n > 0 && transport_->send_frame(frame, n);
}

void GridNode::apply_local_route_rule(const L2RouteRulePayload& rule) {
    std::lock_guard<std::mutex> lk(mu_);
    if (rule.op == 0) {
        tx_routes_.clear();
        return;
    }
    if (rule.op == 2) {
        tx_routes_.erase(std::remove_if(tx_routes_.begin(), tx_routes_.end(),
                                        [&](const L2RouteRulePayload& r) { return r.route_id == rule.route_id; }),
                         tx_routes_.end());
        return;
    }
    for (auto& r : tx_routes_) {
        if (r.route_id == rule.route_id) {
            r = rule;
            return;
        }
    }
    tx_routes_.push_back(rule);
}

std::vector<L2RouteRulePayload> GridNode::active_tx_routes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tx_routes_;
}

bool GridNode::send_dsp_param(const MacAddress& dst_dsp_mac, const L2DspParamPayload& param) {
    if (!is_open()) return false;
    uint8_t body[kL2DspParamSize];
    const size_t blen = write_l2_dsp_param(body, sizeof(body), param);
    LvL2Header hdr{};
    hdr.type = L2PacketType::kDspParam;
    hdr.seq = seq_.fetch_add(1, std::memory_order_relaxed);
    VlanQosTag vlan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        vlan = cfg_.vlan;
    }
    uint8_t frame[192];
    const size_t n = write_l2_frame(frame, sizeof(frame), dst_dsp_mac, mac(), vlan, hdr, body, blen);
    return n > 0 && transport_->send_frame(frame, n);
}

bool GridNode::send_heartbeat(const MacAddress& dst_mac, const L2HeartbeatPayload& hb) {
    if (!is_open()) return false;
    uint8_t body[kL2HeartbeatSize];
    const size_t blen = write_l2_heartbeat(body, sizeof(body), hb);
    LvL2Header hdr{};
    hdr.type = L2PacketType::kHeartbeat;
    hdr.seq = seq_.fetch_add(1, std::memory_order_relaxed);
    VlanQosTag vlan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        vlan = cfg_.vlan;
    }
    uint8_t frame[128];
    const size_t n = write_l2_frame(frame, sizeof(frame), dst_mac, mac(), vlan, hdr, body, blen);
    return n > 0 && transport_->send_frame(frame, n);
}

size_t GridNode::send_audio_direct(const MacAddress& dst, uint16_t stream_id,
                                   const float* const* planes, uint16_t src_ch_start, uint16_t dst_ch_start,
                                   uint16_t ch_count, uint16_t samples_per_ch, uint32_t media_ts_lo,
                                   uint8_t flags) {
    if (!is_open() || !planes || ch_count == 0 || samples_per_ch == 0) return 0;
    L2AudioFormat fmt;
    VlanQosTag vlan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fmt = cfg_.audio_fmt;
        vlan = cfg_.vlan;
    }

    // Fit cleanly inside standard 1500-byte Ethernet MTU (max ~1464 bytes audio payload per frame)
    constexpr size_t kMaxPayload = 1440;
    const size_t bytes_per_ch = size_t(samples_per_ch) * ((fmt == L2AudioFormat::kFloat32) ? 4u : 3u);
    const uint16_t max_ch_per_frame =
        uint16_t(std::clamp<size_t>(kMaxPayload / std::max<size_t>(bytes_per_ch, 1u), 1u, 255u));

    uint8_t payload[kMaxPayload];
    uint8_t frame[kMaxEthFrameSize];
    size_t sent_frames = 0;

    uint16_t done = 0;
    while (done < ch_count) {
        const uint16_t take = std::min<uint16_t>(max_ch_per_frame, uint16_t(ch_count - done));
        const size_t plen = pack_l2_audio_planar(fmt, planes, uint16_t(src_ch_start + done), take,
                                                 samples_per_ch, payload, sizeof(payload));
        if (plen == 0) break;

        LvL2Header hdr{};
        hdr.type = L2PacketType::kAudio;
        hdr.stream_id = stream_id;
        hdr.seq = seq_.fetch_add(1, std::memory_order_relaxed);
        hdr.channel_start = uint16_t(dst_ch_start + done);
        hdr.channel_count = uint8_t(take);
        hdr.samples_per_ch = uint8_t(samples_per_ch);
        hdr.audio_fmt = fmt;
        hdr.flags = flags;
        hdr.media_ts_lo = media_ts_lo;

        const size_t flen = write_l2_frame(frame, sizeof(frame), dst, mac(), vlan, hdr, payload, plen);
        if (flen > 0 && transport_->send_frame(frame, flen)) {
            ++sent_frames;
            audio_tx_frames_.fetch_add(1, std::memory_order_relaxed);
        }
        done = uint16_t(done + take);
    }
    return sent_frames;
}

size_t GridNode::push_routed_audio(const float* const* planes, uint16_t total_channels,
                                   uint16_t samples_per_ch, uint32_t media_ts_lo) {
    if (!planes || total_channels == 0 || samples_per_ch == 0) return 0;
    std::vector<L2RouteRulePayload> routes;
    {
        std::lock_guard<std::mutex> lk(mu_);
        routes = tx_routes_;
    }
    size_t total_frames = 0;
    for (const auto& r : routes) {
        if ((r.flags & 0x01u) != 0) continue;  // muted
        if (r.src_ch_start >= total_channels || r.channel_count == 0) continue;
        const uint16_t avail = std::min<uint16_t>(r.channel_count, uint16_t(total_channels - r.src_ch_start));
        total_frames += send_audio_direct(r.dst_mac, r.stream_id, planes, r.src_ch_start, r.dst_ch_start,
                                          avail, samples_per_ch, media_ts_lo,
                                          (r.flags & 0x02u) ? 0x04u : 0x00u);
    }
    return total_frames;
}

int GridNode::poll(int max_frames) {
    if (!is_open()) return 0;
    uint8_t buf[kMaxEthFrameSize];
    int processed = 0;
    const uint64_t now = now_us();
    for (int i = 0; i < max_frames; ++i) {
        const int n = transport_->recv_frame(buf, sizeof(buf));
        if (n <= 0) break;
        ParsedL2Frame f{};
        if (!parse_l2_frame(buf, size_t(n), f)) continue;
        // Ignore frames addressed to another unicast MAC
        if (!f.dst.is_broadcast() && f.dst != mac()) continue;
        handle_frame(f, now);
        ++processed;
    }
    return processed;
}

void GridNode::handle_frame(const ParsedL2Frame& f, uint64_t now_us_val) {
    switch (f.hdr.type) {
        case L2PacketType::kAudio: {
            const uint16_t ch0 = f.hdr.channel_start;
            const uint16_t chn = f.hdr.channel_count;
            const uint16_t ns = f.hdr.samples_per_ch;
            if (chn == 0 || ns == 0 || uint32_t(ch0) + uint32_t(chn) > kGridMaxChannels) return;
            for (uint16_t c = 0; c < chn; ++c) {
                if (rx_scratch_[ch0 + c].size() < ns) {
                    rx_scratch_[ch0 + c].resize(ns, 0.0f);
                    rx_ptrs_[ch0 + c] = rx_scratch_[ch0 + c].data();
                }
            }
            if (!unpack_l2_audio_planar(f.hdr.audio_fmt, f.payload, f.payload_len, chn, ns, rx_ptrs_.data(), ch0)) {
                return;
            }
            audio_rx_frames_.fetch_add(1, std::memory_order_relaxed);
            AudioRxCallback cb;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb = on_audio_;
            }
            if (cb) {
                cb(f.src, f.hdr.stream_id, ch0, chn, ns, f.hdr.media_ts_lo, rx_ptrs_.data(), f.hdr.flags);
            }
            break;
        }
        case L2PacketType::kDiscoveryReq: {
            control_rx_frames_.fetch_add(1, std::memory_order_relaxed);
            announce_self(f.src);
            break;
        }
        case L2PacketType::kDiscoveryResp: {
            L2DiscoveryPayload p{};
            if (!parse_l2_discovery(f.payload, f.payload_len, p)) return;
            control_rx_frames_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(mu_);
            GridDiscoveredNode* slot = nullptr;
            for (auto& existing : peers_) {
                if (existing.mac == f.src) {
                    slot = &existing;
                    break;
                }
            }
            if (!slot) {
                peers_.push_back({});
                slot = &peers_.back();
            }
            slot->mac = f.src;
            slot->name = p.node_name[0] ? std::string(p.node_name) : f.src.to_string();
            slot->role = p.role;
            slot->max_channels = p.max_channels;
            slot->sample_rate = p.sample_rate;
            slot->cpu_load_pct = float(p.cpu_load_pct_x10) * 0.1f;
            slot->buffer_fill_pct = float(p.buffer_fill_pct_x10) * 0.1f;
            slot->is_clock_master = (p.flags & 0x01u) != 0;
            slot->is_backup_dsp = (p.flags & 0x02u) != 0;
            slot->last_seen_us = now_us_val;
            break;
        }
        case L2PacketType::kSyncClock: {
            L2SyncClockPayload p{};
            if (!parse_l2_sync_clock(f.payload, f.payload_len, p)) return;
            control_rx_frames_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(mu_);
            if (cfg_.clock_master) return;  // Master ignores external clocks
            if (prev_sync_local_us_ > 0 && now_us_val > prev_sync_local_us_ &&
                p.master_sample_pos > prev_sync_samples_ && p.sample_rate > 0) {
                const double expected_s = double(now_us_val - prev_sync_local_us_) * double(p.sample_rate) / 1e6;
                const double actual_s = double(p.master_sample_pos - prev_sync_samples_);
                if (expected_s > 10.0) {
                    const double inst_ppm = ((actual_s - expected_s) / expected_s) * 1e6;
                    clock_.drift_ppm = clock_.drift_ppm * 0.85 + std::clamp(inst_ppm, -500.0, 500.0) * 0.15;
                }
            }
            prev_sync_local_us_ = now_us_val;
            prev_sync_samples_ = p.master_sample_pos;
            clock_.master_mac = f.src;
            clock_.master_sample_pos = p.master_sample_pos;
            clock_.clock_epoch = p.clock_epoch;
            clock_.locked = true;
            ++clock_.sync_rx_count;
            break;
        }
        case L2PacketType::kRouteCtrl: {
            L2RouteRulePayload rule{};
            if (!parse_l2_route_rule(f.payload, f.payload_len, rule)) return;
            control_rx_frames_.fetch_add(1, std::memory_order_relaxed);
            apply_local_route_rule(rule);
            break;
        }
        case L2PacketType::kDspParam: {
            L2DspParamPayload param{};
            if (!parse_l2_dsp_param(f.payload, f.payload_len, param)) return;
            control_rx_frames_.fetch_add(1, std::memory_order_relaxed);
            DspParamCallback cb;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb = on_dsp_param_;
            }
            if (cb) cb(f.src, param);
            break;
        }
        case L2PacketType::kHeartbeat: {
            L2HeartbeatPayload hb{};
            if (!parse_l2_heartbeat(f.payload, f.payload_len, hb)) return;
            control_rx_frames_.fetch_add(1, std::memory_order_relaxed);
            HeartbeatCallback cb;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb = on_heartbeat_;
            }
            if (cb) cb(f.src, hb);
            break;
        }
        default:
            break;
    }
}

}  // namespace lv::grid
