#include "grid/dsp_server.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/clock.hpp"

namespace lv::grid {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kBrickwallCeiling = 0.94406088f;  // -0.5 dBFS

inline float db_to_lin(float db) {
    if (!std::isfinite(db) || db <= -60.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

inline float finite_clamp(float v, float lo, float hi, float def) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
}

}  // namespace

// =============================================================================
// GridChannelDspStrip
// =============================================================================

GridChannelDspStrip::GridChannelDspStrip(uint32_t sample_rate)
    : sample_rate_(sample_rate ? sample_rate : 48000) {
    rebuild_filters();
}

void GridChannelDspStrip::set_sample_rate(uint32_t sample_rate) {
    sample_rate_ = sample_rate ? sample_rate : 48000;
    rebuild_filters();
}

void GridChannelDspStrip::set_params(const L2DspParamPayload& p) {
    params_ = p;
    params_.hpf_hz = finite_clamp(p.hpf_hz, 20.0f, 300.0f, 80.0f);
    params_.eq_low_db = finite_clamp(p.eq_low_db, -15.0f, 15.0f, 0.0f);
    params_.eq_mid_hz = finite_clamp(p.eq_mid_hz, 200.0f, 8000.0f, 1200.0f);
    params_.eq_mid_db = finite_clamp(p.eq_mid_db, -15.0f, 15.0f, 0.0f);
    params_.eq_high_db = finite_clamp(p.eq_high_db, -15.0f, 15.0f, 0.0f);
    params_.comp_thresh_db = finite_clamp(p.comp_thresh_db, -48.0f, 0.0f, -18.0f);
    params_.comp_ratio = finite_clamp(p.comp_ratio, 1.0f, 20.0f, 3.0f);
    params_.comp_makeup_db = finite_clamp(p.comp_makeup_db, 0.0f, 24.0f, 0.0f);
    params_.gate_thresh_db = finite_clamp(p.gate_thresh_db, -80.0f, -10.0f, -50.0f);
    params_.output_gain_db = finite_clamp(p.output_gain_db, -60.0f, 12.0f, 0.0f);
    rebuild_filters();
}

L2DspParamPayload GridChannelDspStrip::params() const {
    return params_;
}

void GridChannelDspStrip::rebuild_filters() {
    const float fs = float(sample_rate_);
    // HPF Butterworth 2nd order
    if (params_.flags & 0x02u) {
        const float w0 = 2.0f * kPi * std::min(params_.hpf_hz, fs * 0.45f) / fs;
        const float cs = std::cos(w0), sn = std::sin(w0);
        const float alpha = sn / (2.0f * 0.70710678f);
        const float a0 = 1.0f + alpha;
        hpf_.b0 = ((1.0f + cs) * 0.5f) / a0;
        hpf_.b1 = (-(1.0f + cs)) / a0;
        hpf_.b2 = ((1.0f + cs) * 0.5f) / a0;
        hpf_.a1 = (-2.0f * cs) / a0;
        hpf_.a2 = (1.0f - alpha) / a0;
    } else {
        hpf_.b0 = 1.0f; hpf_.b1 = hpf_.b2 = hpf_.a1 = hpf_.a2 = 0.0f;
    }

    // 3-band EQ (Low Shelf 100 Hz, Mid Peak, High Shelf 8 kHz)
    if (params_.flags & 0x04u) {
        // Low shelf
        {
            const float A = std::pow(10.0f, params_.eq_low_db / 40.0f);
            const float w0 = 2.0f * kPi * std::min(100.0f, fs * 0.45f) / fs;
            const float cs = std::cos(w0), sn = std::sin(w0);
            const float alpha = sn * 0.5f * std::sqrt(2.0f);
            const float two = 2.0f * std::sqrt(A) * alpha;
            const float a0 = (A + 1.0f) + (A - 1.0f) * cs + two;
            eq_low_.b0 = (A * ((A + 1.0f) - (A - 1.0f) * cs + two)) / a0;
            eq_low_.b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs)) / a0;
            eq_low_.b2 = (A * ((A + 1.0f) - (A - 1.0f) * cs - two)) / a0;
            eq_low_.a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cs)) / a0;
            eq_low_.a2 = ((A + 1.0f) + (A - 1.0f) * cs - two) / a0;
        }
        // Mid peaking
        {
            const float A = std::pow(10.0f, params_.eq_mid_db / 40.0f);
            const float w0 = 2.0f * kPi * std::min(params_.eq_mid_hz, fs * 0.45f) / fs;
            const float cs = std::cos(w0), sn = std::sin(w0);
            const float alpha = sn / 2.0f;
            const float a0 = 1.0f + alpha / A;
            eq_mid_.b0 = (1.0f + alpha * A) / a0;
            eq_mid_.b1 = (-2.0f * cs) / a0;
            eq_mid_.b2 = (1.0f - alpha * A) / a0;
            eq_mid_.a1 = (-2.0f * cs) / a0;
            eq_mid_.a2 = (1.0f - alpha / A) / a0;
        }
        // High shelf
        {
            const float A = std::pow(10.0f, params_.eq_high_db / 40.0f);
            const float w0 = 2.0f * kPi * std::min(8000.0f, fs * 0.45f) / fs;
            const float cs = std::cos(w0), sn = std::sin(w0);
            const float alpha = sn * 0.5f * std::sqrt(2.0f);
            const float two = 2.0f * std::sqrt(A) * alpha;
            const float a0 = (A + 1.0f) - (A - 1.0f) * cs + two;
            eq_high_.b0 = (A * ((A + 1.0f) + (A - 1.0f) * cs + two)) / a0;
            eq_high_.b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs)) / a0;
            eq_high_.b2 = (A * ((A + 1.0f) + (A - 1.0f) * cs - two)) / a0;
            eq_high_.a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cs)) / a0;
            eq_high_.a2 = ((A + 1.0f) - (A - 1.0f) * cs - two) / a0;
        }
    } else {
        eq_low_.b0 = eq_mid_.b0 = eq_high_.b0 = 1.0f;
        eq_low_.b1 = eq_low_.b2 = eq_low_.a1 = eq_low_.a2 = 0.0f;
        eq_mid_.b1 = eq_mid_.b2 = eq_mid_.a1 = eq_mid_.a2 = 0.0f;
        eq_high_.b1 = eq_high_.b2 = eq_high_.a1 = eq_high_.a2 = 0.0f;
    }
}

void GridChannelDspStrip::process_inplace(float* buf, uint32_t frames) {
    if (!buf || frames == 0 || (params_.flags & 0x01u) != 0) return;  // bypass

    const float fs = float(sample_rate_);
    const bool hpf_on = (params_.flags & 0x02u) != 0;
    const bool eq_on = (params_.flags & 0x04u) != 0;
    const bool comp_on = (params_.flags & 0x08u) != 0 && params_.comp_ratio > 1.01f;
    const bool gate_on = (params_.flags & 0x10u) != 0;

    const float comp_att = std::exp(-1.0f / std::max(1.0f, 0.005f * fs));
    const float comp_rel = std::exp(-1.0f / std::max(1.0f, 0.080f * fs));
    const float gate_att = std::exp(-1.0f / std::max(1.0f, 0.0002f * fs));
    const float gate_rel = std::exp(-1.0f / std::max(1.0f, 0.004f * fs));
    const float gate_smooth = 1.0f - std::exp(-1.0f / std::max(1.0f, 0.0004f * fs));
    const float comp_makeup = db_to_lin(params_.comp_makeup_db);
    const float out_gain = db_to_lin(params_.output_gain_db);
    const float lim_rel = std::exp(-1.0f / std::max(1.0f, 0.030f * fs));

    float worst_gr = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float x = std::isfinite(buf[i]) ? buf[i] : 0.0f;
        if (hpf_on) x = hpf_.process(x);
        if (eq_on) {
            x = eq_low_.process(x);
            x = eq_mid_.process(x);
            x = eq_high_.process(x);
        }
        if (gate_on) {
            const float a = std::fabs(x);
            const float gc = (a > gate_env_) ? gate_att : gate_rel;
            gate_env_ = gc * gate_env_ + (1.0f - gc) * a;
            const float db = (gate_env_ > 1e-6f) ? (20.0f * std::log10(gate_env_)) : -120.0f;
            const float tgt = (db >= params_.gate_thresh_db) ? 1.0f : 0.0f;
            gate_gain_ += (tgt - gate_gain_) * gate_smooth;
            x *= gate_gain_;
        }
        if (comp_on) {
            const float a = std::fabs(x);
            const float c = (a > comp_env_) ? comp_att : comp_rel;
            comp_env_ = c * comp_env_ + (1.0f - c) * a;
            const float env_db = (comp_env_ > 1e-6f) ? (20.0f * std::log10(comp_env_)) : -120.0f;
            float gr = 0.0f;
            if (env_db > params_.comp_thresh_db) {
                gr = (env_db - params_.comp_thresh_db) * (1.0f / params_.comp_ratio - 1.0f);
            }
            if (gr < worst_gr) worst_gr = gr;
            x *= db_to_lin(gr) * comp_makeup;
        }
        x *= out_gain;
        const float pk = std::fabs(x);
        const float req = (pk > kBrickwallCeiling) ? (kBrickwallCeiling / pk) : 1.0f;
        lim_gain_ = (req < lim_gain_) ? req : (lim_rel * lim_gain_ + (1.0f - lim_rel));
        buf[i] = std::clamp(x * lim_gain_, -kBrickwallCeiling, kBrickwallCeiling);
    }
    last_gr_db_ = worst_gr;
}

// =============================================================================
// GridDspServer
// =============================================================================

GridDspServer::GridDspServer(std::unique_ptr<L2Transport> transport, const std::string& server_name,
                             bool is_backup, uint32_t sample_rate)
    : node_(std::move(transport),
            GridNodeConfig{server_name, GridNodeRole::kDspServer, kGridMaxChannels,
                           sample_rate ? sample_rate : 48000, L2AudioFormat::kPcm24Packed,
                           VlanQosTag{false, 0, 6}, false, 200, is_backup}),
      is_backup_(is_backup),
      sample_rate_(sample_rate ? sample_rate : 48000),
      start_us_(now_us()) {
    for (uint16_t c = 0; c < kGridMaxChannels; ++c) {
        strips_[c].set_sample_rate(sample_rate_);
        out_bufs_[c].resize(128, 0.0f);
        out_ptrs_[c] = out_bufs_[c].data();
    }
    node_.set_audio_rx_callback([this](const MacAddress& src, uint16_t stream_id, uint16_t ch0, uint16_t chn,
                                       uint16_t ns, uint32_t ts, const float* const* planes, uint8_t flags) {
        on_audio_rx(src, stream_id, ch0, chn, ns, ts, planes, flags);
    });
    node_.set_dsp_param_callback([this](const MacAddress& src, const L2DspParamPayload& p) {
        on_dsp_param_rx(src, p);
    });
}

GridDspServer::~GridDspServer() = default;

void GridDspServer::set_return_mac_override(const MacAddress& dst) {
    std::lock_guard<std::mutex> lk(mu_);
    return_override_ = dst;
}

void GridDspServer::set_channel_params(uint16_t channel, const L2DspParamPayload& p) {
    std::lock_guard<std::mutex> lk(mu_);
    if (channel == 0xFFFFu) {
        for (auto& s : strips_) s.set_params(p);
    } else if (channel < kGridMaxChannels) {
        strips_[channel].set_params(p);
    }
}

void GridDspServer::on_dsp_param_rx(const MacAddress&, const L2DspParamPayload& p) {
    set_channel_params(p.channel, p);
}

void GridDspServer::on_audio_rx(const MacAddress& src, uint16_t stream_id, uint16_t ch_start, uint16_t ch_count,
                                uint16_t samples_per_ch, uint32_t media_ts_lo, const float* const* planes,
                                uint8_t flags) {
    if (flags & 0x02u) return;  // Ignore DSP return frames to prevent loops
    const uint64_t t0 = now_us();

    MacAddress dst = src;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!return_override_.is_zero()) dst = return_override_;
        for (uint16_t i = 0; i < ch_count; ++i) {
            const uint16_t ch = uint16_t(ch_start + i);
            if (ch >= kGridMaxChannels) break;
            if (out_bufs_[ch].size() < samples_per_ch) {
                out_bufs_[ch].resize(samples_per_ch, 0.0f);
                out_ptrs_[ch] = out_bufs_[ch].data();
            }
            if (planes[ch]) {
                std::memcpy(out_bufs_[ch].data(), planes[ch], size_t(samples_per_ch) * sizeof(float));
            } else {
                std::memset(out_bufs_[ch].data(), 0, size_t(samples_per_ch) * sizeof(float));
            }
            strips_[ch].process_inplace(out_bufs_[ch].data(), samples_per_ch);
        }
    }

    const uint64_t proc_us = now_us() - t0;
    const double budget_us = double(samples_per_ch) * 1e6 / double(sample_rate_);
    const float inst_cpu = float(std::clamp((double(proc_us) / std::max(budget_us, 1.0)) * 100.0, 0.0, 100.0));
    cpu_load_pct_.store(cpu_load_pct_.load(std::memory_order_relaxed) * 0.85f + inst_cpu * 0.15f,
                        std::memory_order_relaxed);
    node_.set_telemetry(cpu_load_pct_.load(std::memory_order_relaxed), 5.0f);
    active_ch_.store(std::max<uint16_t>(active_ch_.load(std::memory_order_relaxed), uint16_t(ch_start + ch_count)),
                     std::memory_order_relaxed);

    const uint8_t ret_flags = uint8_t(0x02u | (is_backup_ ? 0x04u : 0x00u));
    node_.send_audio_direct(dst, stream_id, out_ptrs_.data(), ch_start, ch_start, ch_count, samples_per_ch,
                            media_ts_lo, ret_flags);
    frames_processed_.fetch_add(1, std::memory_order_relaxed);
}

int GridDspServer::service(int max_frames) {
    return node_.poll(max_frames);
}

bool GridDspServer::emit_heartbeat() {
    L2HeartbeatPayload hb{};
    hb.uptime_ms = uint32_t((now_us() - start_us_) / 1000ull);
    hb.cpu_load_pct_x10 = uint16_t(std::lround(cpu_load_pct_.load(std::memory_order_relaxed) * 10.0f));
    hb.buffer_fill_pct_x10 = 50;  // 5.0%
    hb.active_channels = active_ch_.load(std::memory_order_relaxed);
    hb.is_backup = is_backup_ ? 1 : 0;
    hb.health_ok = 1;
    hb.frames_processed = uint32_t(frames_processed_.load(std::memory_order_relaxed));
    return node_.send_heartbeat(MacAddress::broadcast(), hb);
}

// =============================================================================
// GridFailoverController
// =============================================================================

GridFailoverController::GridFailoverController(GridNode& host_node, const MacAddress& primary_mac,
                                               const MacAddress& backup_mac, uint64_t failover_timeout_us)
    : host_(host_node),
      primary_mac_(primary_mac),
      backup_mac_(backup_mac),
      timeout_us_(failover_timeout_us ? failover_timeout_us : 8000),
      start_us_(now_us()) {}

void GridFailoverController::send_dry_mirrored(uint16_t stream_id, const float* const* planes, uint16_t ch_start,
                                               uint16_t ch_count, uint16_t samples_per_ch, uint32_t media_ts_lo) {
    if (!primary_mac_.is_zero()) {
        host_.send_audio_direct(primary_mac_, stream_id, planes, ch_start, ch_start, ch_count, samples_per_ch,
                                media_ts_lo, 0x00);
    }
    if (!backup_mac_.is_zero()) {
        host_.send_audio_direct(backup_mac_, stream_id, planes, ch_start, ch_start, ch_count, samples_per_ch,
                                media_ts_lo, 0x00);
    }
}

void GridFailoverController::send_param_mirrored(const L2DspParamPayload& param) {
    if (!primary_mac_.is_zero()) host_.send_dsp_param(primary_mac_, param);
    if (!backup_mac_.is_zero()) host_.send_dsp_param(backup_mac_, param);
}

bool GridFailoverController::on_processed_return(const MacAddress& src_mac, uint64_t now_us_val) {
    std::lock_guard<std::mutex> lk(mu_);
    if (start_us_ == 0 || now_us_val < start_us_) start_us_ = now_us_val;
    if (src_mac == primary_mac_) {
        last_primary_us_ = now_us_val;
        if (using_backup_ && last_backup_us_ > 0 && now_us_val > last_backup_us_ &&
            (now_us_val - last_backup_us_) > timeout_us_) {
            using_backup_ = false;
            ++failover_count_;
        }
        return !using_backup_;
    }
    if (src_mac == backup_mac_) {
        last_backup_us_ = now_us_val;
        const uint64_t ref_p = last_primary_us_ ? last_primary_us_ : start_us_;
        if (!using_backup_ && now_us_val > ref_p && (now_us_val - ref_p) > timeout_us_) {
            using_backup_ = true;
            ++failover_count_;
        }
        return using_backup_;
    }
    return false;
}

void GridFailoverController::on_heartbeat(const MacAddress& src_mac, const L2HeartbeatPayload& hb,
                                          uint64_t now_us_val) {
    std::lock_guard<std::mutex> lk(mu_);
    if (start_us_ == 0 || now_us_val < start_us_) start_us_ = now_us_val;
    if (src_mac == primary_mac_) {
        last_primary_us_ = now_us_val;
        primary_cpu_ = float(hb.cpu_load_pct_x10) * 0.1f;
    } else if (src_mac == backup_mac_) {
        last_backup_us_ = now_us_val;
        backup_cpu_ = float(hb.cpu_load_pct_x10) * 0.1f;
    }
}

void GridFailoverController::check_failover(uint64_t now_us_val) {
    std::lock_guard<std::mutex> lk(mu_);
    if (start_us_ == 0 || now_us_val < start_us_) start_us_ = now_us_val;
    const uint64_t ref_p = last_primary_us_ ? last_primary_us_ : start_us_;
    const bool primary_timed_out = (now_us_val > ref_p && (now_us_val - ref_p) > timeout_us_);
    const uint64_t backup_age = (now_us_val > last_backup_us_) ? (now_us_val - last_backup_us_) : 0;
    const uint64_t primary_age = (now_us_val > last_primary_us_) ? (now_us_val - last_primary_us_) : 0;
    if (!using_backup_ && primary_timed_out && last_backup_us_ > 0 && backup_age <= timeout_us_ * 2) {
        using_backup_ = true;
        ++failover_count_;
    } else if (using_backup_ && last_backup_us_ > 0 && backup_age > timeout_us_ &&
               last_primary_us_ > 0 && primary_age <= timeout_us_ * 2) {
        using_backup_ = false;
        ++failover_count_;
    }
}

FailoverTelemetry GridFailoverController::telemetry() const {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t now = now_us();
    const uint64_t p_age = (now > last_primary_us_) ? (now - last_primary_us_) : 0;
    const uint64_t b_age = (now > last_backup_us_) ? (now - last_backup_us_) : 0;
    FailoverTelemetry t{};
    t.primary_mac = primary_mac_;
    t.backup_mac = backup_mac_;
    t.using_backup = using_backup_;
    t.primary_alive = (last_primary_us_ > 0 && p_age <= timeout_us_ * 4);
    t.backup_alive = (last_backup_us_ > 0 && b_age <= timeout_us_ * 4);
    t.primary_cpu_pct = primary_cpu_;
    t.backup_cpu_pct = backup_cpu_;
    t.failover_count = failover_count_;
    return t;
}

}  // namespace lv::grid
