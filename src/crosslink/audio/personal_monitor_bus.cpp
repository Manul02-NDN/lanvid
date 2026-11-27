#include "audio/personal_monitor_bus.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef CL_HAVE_OPUS
#include <opus.h>
#endif

#include "common/clock.hpp"
#include "common/rt_thread.hpp"
#include "common/network_interfaces.hpp"

namespace cl {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// -0.5 dBFS protective brickwall ceiling = 10^(-0.5/20)
constexpr float kBrickwallCeiling = 0.94406088f;
constexpr float kBusHeadroom = 0.70794578f;   // -3 dB: a sum of several 0 dBFS channels stays inside the chain
constexpr float kSatKnee = 0.80f;             // soft saturator: transparent below, tanh-rounded above

inline float soft_sat(float x) {
    const float a = std::fabs(x);
    if (a <= kSatKnee) return x;
    const float y = kSatKnee + (1.0f - kSatKnee) * std::tanh((a - kSatKnee) / (1.0f - kSatKnee));
    return x < 0.0f ? -y : y;
}

inline float db_to_lin(float db) {
    if (!std::isfinite(db) || db <= -60.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

inline float finite_clamp(float v, float lo, float hi, float def) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
}

inline int8_t lin_to_dbfs_i8(float peak) {
    if (peak <= 1e-6f) return -120;
    const float db = 20.0f * std::log10(peak);
    return int8_t(std::clamp(int(std::lround(db)), -120, 0));
}

// RBJ Audio EQ Cookbook biquad coefficient generators (normalized so a0 == 1).
void calc_hpf_butterworth(float fs, float fc, float& b0, float& b1, float& b2, float& a1, float& a2) {
    fc = std::clamp(fc, 20.0f, std::min(250.0f, fs * 0.45f));
    const float w0 = 2.0f * kPi * fc / fs;
    const float cs = std::cos(w0);
    const float sn = std::sin(w0);
    const float alpha = sn / (2.0f * 0.70710678f);  // Q = 1/sqrt(2)
    const float a0 = 1.0f + alpha;
    b0 = ((1.0f + cs) * 0.5f) / a0;
    b1 = (-(1.0f + cs)) / a0;
    b2 = ((1.0f + cs) * 0.5f) / a0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
}

void calc_low_shelf(float fs, float fc, float gain_db, float& b0, float& b1, float& b2, float& a1, float& a2) {
    const float A = std::pow(10.0f, std::clamp(gain_db, -12.0f, 12.0f) / 40.0f);
    const float w0 = 2.0f * kPi * fc / fs;
    const float cs = std::cos(w0);
    const float sn = std::sin(w0);
    const float alpha = sn * 0.5f * std::sqrt(2.0f);  // shelf slope S = 1
    const float two_sqrtA_alpha = 2.0f * std::sqrt(A) * alpha;
    const float a0 = (A + 1.0f) + (A - 1.0f) * cs + two_sqrtA_alpha;
    b0 = (A * ((A + 1.0f) - (A - 1.0f) * cs + two_sqrtA_alpha)) / a0;
    b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs)) / a0;
    b2 = (A * ((A + 1.0f) - (A - 1.0f) * cs - two_sqrtA_alpha)) / a0;
    a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cs)) / a0;
    a2 = ((A + 1.0f) + (A - 1.0f) * cs - two_sqrtA_alpha) / a0;
}

void calc_peaking_eq(float fs, float fc, float q, float gain_db,
                     float& b0, float& b1, float& b2, float& a1, float& a2) {
    fc = std::clamp(fc, 300.0f, std::min(5000.0f, fs * 0.45f));
    const float A = std::pow(10.0f, std::clamp(gain_db, -12.0f, 12.0f) / 40.0f);
    const float w0 = 2.0f * kPi * fc / fs;
    const float cs = std::cos(w0);
    const float sn = std::sin(w0);
    const float alpha = sn / (2.0f * q);
    const float a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0;
    b1 = (-2.0f * cs) / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha / A) / a0;
}

void calc_high_shelf(float fs, float fc, float gain_db, float& b0, float& b1, float& b2, float& a1, float& a2) {
    const float A = std::pow(10.0f, std::clamp(gain_db, -12.0f, 12.0f) / 40.0f);
    const float w0 = 2.0f * kPi * fc / fs;
    const float cs = std::cos(w0);
    const float sn = std::sin(w0);
    const float alpha = sn * 0.5f * std::sqrt(2.0f);
    const float two_sqrtA_alpha = 2.0f * std::sqrt(A) * alpha;
    const float a0 = (A + 1.0f) - (A - 1.0f) * cs + two_sqrtA_alpha;
    b0 = (A * ((A + 1.0f) + (A - 1.0f) * cs + two_sqrtA_alpha)) / a0;
    b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs)) / a0;
    b2 = (A * ((A + 1.0f) + (A - 1.0f) * cs - two_sqrtA_alpha)) / a0;
    a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cs)) / a0;
    a2 = ((A + 1.0f) - (A - 1.0f) * cs - two_sqrtA_alpha) / a0;
}

}  // namespace

// =========================================================================
// PersonalMonitorBus
// =========================================================================

PersonalMonitorBus::PersonalMonitorBus(uint32_t sample_rate)
    : sample_rate_(sample_rate ? sample_rate : 48000) {
    // 1.2 ms smooth time constant (~5 ms to settle): click-free parameter changes.
    const float tau_samples = std::max(1.0f, float(sample_rate_) * 0.0012f);
    smooth_alpha_ = 1.0f - std::exp(-1.0f / tau_samples);
    reset();
}

void PersonalMonitorBus::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    hpf_.reset_state();
    eq_low_.reset_state();
    eq_mid_.reset_state();
    eq_high_.reset_state();
    comp_env_ = 0.0f;
    lim_gain_ = 1.0f;
    la_l_.fill(0.0f); la_r_.fill(0.0f); la_req_.fill(1.0f); la_env_.fill(1.0f);
    la_env_sum_ = double(kLookahead);
    la_recalc_cnt_ = 0;
    la_pos_ = 0;
    initialized_ = false;
    recompute_targets_locked();
    cur_gain_l_ = tgt_gain_l_;
    cur_gain_r_ = tgt_gain_r_;
    cur_master_gain_ = tgt_master_gain_;
    hpf_.snap_to_target();
    eq_low_.snap_to_target();
    eq_mid_.snap_to_target();
    eq_high_.snap_to_target();
    initialized_ = true;
}

void PersonalMonitorBus::recompute_targets_locked() {
    bool any_solo = false;
    for (const auto& m : ch_mix_) {
        if (m.solo) { any_solo = true; break; }
    }
    for (int i = 0; i < kMonitorMaxInputChannels; ++i) {
        const auto& m = ch_mix_[i];
        const bool audible = any_solo ? (m.solo && !m.mute) : !m.mute;
        if (!audible) {
            tgt_gain_l_[i] = 0.0f;
            tgt_gain_r_[i] = 0.0f;
        } else {
            const float g = db_to_lin(std::clamp(m.gain_db, -60.0f, 12.0f));
            const float pan = std::clamp(m.pan, -1.0f, 1.0f);
            // Constant-power pan law (-3 dB center)
            const float angle = (pan + 1.0f) * 0.25f * kPi;
            tgt_gain_l_[i] = g * std::cos(angle);
            tgt_gain_r_[i] = g * std::sin(angle);
        }
    }

    tgt_master_gain_ = dsp_.master_mute ? 0.0f : db_to_lin(std::clamp(dsp_.master_volume_db, -60.0f, 12.0f));

    const float fs = float(sample_rate_);
    if (dsp_.hpf_enabled) {
        calc_hpf_butterworth(fs, dsp_.hpf_hz,
                             hpf_.tgt.b0, hpf_.tgt.b1, hpf_.tgt.b2, hpf_.tgt.a1, hpf_.tgt.a2);
    } else {
        hpf_.tgt = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }

    if (dsp_.eq_enabled) {
        calc_low_shelf(fs, 100.0f, dsp_.eq_low_db,
                       eq_low_.tgt.b0, eq_low_.tgt.b1, eq_low_.tgt.b2, eq_low_.tgt.a1, eq_low_.tgt.a2);
        calc_peaking_eq(fs, dsp_.eq_mid_hz, 1.0f, dsp_.eq_mid_db,
                        eq_mid_.tgt.b0, eq_mid_.tgt.b1, eq_mid_.tgt.b2, eq_mid_.tgt.a1, eq_mid_.tgt.a2);
        calc_high_shelf(fs, 8000.0f, dsp_.eq_high_db,
                        eq_high_.tgt.b0, eq_high_.tgt.b1, eq_high_.tgt.b2, eq_high_.tgt.a1, eq_high_.tgt.a2);
    } else {
        eq_low_.tgt = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        eq_mid_.tgt = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        eq_high_.tgt = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }
}

void PersonalMonitorBus::set_channel_mix(int ch, const PersonalMonitorChannelMix& mix) {
    if (ch < 0 || ch >= kMonitorMaxInputChannels) return;
    std::lock_guard<std::mutex> lk(mu_);
    ch_mix_[ch] = mix;
    ch_mix_[ch].gain_db = finite_clamp(mix.gain_db, -60.0f, 12.0f, 0.0f);
    ch_mix_[ch].pan = finite_clamp(mix.pan, -1.0f, 1.0f, 0.0f);
    recompute_targets_locked();
}

PersonalMonitorChannelMix PersonalMonitorBus::channel_mix(int ch) const {
    if (ch < 0 || ch >= kMonitorMaxInputChannels) return {};
    std::lock_guard<std::mutex> lk(mu_);
    return ch_mix_[ch];
}

void PersonalMonitorBus::set_dsp(const PersonalMonitorDspParams& dsp) {
    std::lock_guard<std::mutex> lk(mu_);
    dsp_ = dsp;
    dsp_.hpf_hz = finite_clamp(dsp.hpf_hz, 20.0f, 250.0f, 80.0f);
    dsp_.eq_low_db = finite_clamp(dsp.eq_low_db, -12.0f, 12.0f, 0.0f);
    dsp_.eq_mid_hz = finite_clamp(dsp.eq_mid_hz, 300.0f, 5000.0f, 1200.0f);
    dsp_.eq_mid_db = finite_clamp(dsp.eq_mid_db, -12.0f, 12.0f, 0.0f);
    dsp_.eq_high_db = finite_clamp(dsp.eq_high_db, -12.0f, 12.0f, 0.0f);
    dsp_.comp_thresh_db = finite_clamp(dsp.comp_thresh_db, -40.0f, 0.0f, -18.0f);
    dsp_.comp_ratio = finite_clamp(dsp.comp_ratio, 1.0f, 10.0f, 3.0f);
    dsp_.comp_attack_ms = finite_clamp(dsp.comp_attack_ms, 0.5f, 50.0f, 5.0f);
    dsp_.comp_release_ms = finite_clamp(dsp.comp_release_ms, 10.0f, 500.0f, 80.0f);
    dsp_.comp_makeup_db = finite_clamp(dsp.comp_makeup_db, 0.0f, 18.0f, 0.0f);
    dsp_.master_volume_db = finite_clamp(dsp.master_volume_db, -60.0f, 12.0f, 0.0f);
    recompute_targets_locked();
}

PersonalMonitorDspParams PersonalMonitorBus::dsp() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dsp_;
}

void PersonalMonitorBus::process_block(const float* const* in_planes, int num_in_ch,
                                       float* out_l, float* out_r, uint32_t frames) {
    if (!out_l || !out_r || frames == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!initialized_) {
        recompute_targets_locked();
        cur_gain_l_ = tgt_gain_l_;
        cur_gain_r_ = tgt_gain_r_;
        cur_master_gain_ = tgt_master_gain_;
        hpf_.snap_to_target();
        eq_low_.snap_to_target();
        eq_mid_.snap_to_target();
        eq_high_.snap_to_target();
        initialized_ = true;
    }

    const int ch_n = std::clamp(num_in_ch, 0, kMonitorMaxInputChannels);
    const float alpha = smooth_alpha_;

    const float fs = float(sample_rate_);
    const float comp_att = std::exp(-1.0f / std::max(1.0f, dsp_.comp_attack_ms * 0.001f * fs));
    const float comp_rel = std::exp(-1.0f / std::max(1.0f, dsp_.comp_release_ms * 0.001f * fs));
    const float comp_makeup = db_to_lin(dsp_.comp_makeup_db);
    const float lim_rel = std::exp(-1.0f / std::max(1.0f, 0.080f * fs));  // 80 ms limiter release (no pumping)

    float worst_gr_db = 0.0f;

    for (uint32_t i = 0; i < frames; ++i) {
        // 1. Mix up to 64 channels into Stereo L/R with 5 ms click-free gain smoothing
        float l = 0.0f;
        float r = 0.0f;
        if (in_planes) {
            for (int c = 0; c < ch_n; ++c) {
                cur_gain_l_[c] += (tgt_gain_l_[c] - cur_gain_l_[c]) * alpha;
                cur_gain_r_[c] += (tgt_gain_r_[c] - cur_gain_r_[c]) * alpha;
                const float* p = in_planes[c];
                if (!p) continue;
                const float s = p[i];
                l += s * cur_gain_l_[c];
                r += s * cur_gain_r_[c];
            }
        }

        l *= kBusHeadroom;
        r *= kBusHeadroom;

        // 2. Smooth biquad coefficients & run HPF + 3-band EQ
        hpf_.step_smooth(alpha);
        eq_low_.step_smooth(alpha);
        eq_mid_.step_smooth(alpha);
        eq_high_.step_smooth(alpha);

        hpf_.process(l, r);
        eq_low_.process(l, r);
        eq_mid_.process(l, r);
        eq_high_.process(l, r);

        // 3. Stereo feed-forward Compressor
        if (dsp_.comp_enabled && dsp_.comp_ratio > 1.01f) {
            const float det = std::max(std::fabs(l), std::fabs(r));
            const float coeff = (det > comp_env_) ? comp_att : comp_rel;
            comp_env_ = coeff * comp_env_ + (1.0f - coeff) * det;
            const float env_db = (comp_env_ > 1e-6f) ? (20.0f * std::log10(comp_env_)) : -120.0f;
            float gr_db = 0.0f;
            if (env_db > dsp_.comp_thresh_db) {
                const float over = env_db - dsp_.comp_thresh_db;
                gr_db = over * (1.0f / dsp_.comp_ratio - 1.0f);  // negative dB
            }
            if (gr_db < worst_gr_db) worst_gr_db = gr_db;
            const float cg = db_to_lin(gr_db) * comp_makeup;
            l *= cg;
            r *= cg;
        }

        // 4. Master Volume (smoothed)
        cur_master_gain_ += (tgt_master_gain_ - cur_master_gain_) * alpha;
        l *= cur_master_gain_;
        r *= cur_master_gain_;

        // 5. Soft saturator (tanh knee): rounds residual overs instead of hard-clipping them.
        l = soft_sat(l);
        r = soft_sat(r);

        // 6. Lookahead peak limiter (~1 ms): gain is computed on the undelayed signal and applied to the
        //    delayed one, so the attack is a smooth ramp that is already complete when the peak arrives.
        const uint32_t wp = la_pos_;
        la_l_[wp] = l;
        la_r_[wp] = r;
        const float pk = std::max(std::fabs(l), std::fabs(r));
        la_req_[wp] = (pk > kBrickwallCeiling) ? (kBrickwallCeiling / pk) : 1.0f;
        float m = 1.0f;  // sliding minimum over the last kLookahead samples
        for (uint32_t k = 0; k < kLookahead; ++k) m = std::min(m, la_req_[(wp + kLookaheadBuf - k) % kLookaheadBuf]);
        // Instant-down / slow-up release envelope, never above the sliding minimum.
        const float rise = lim_rel * lim_gain_ + (1.0f - lim_rel);
        lim_gain_ = std::min(m, rise);
        // Boxcar smoothing of the envelope (running sum in double + periodic resync to prevent drift).
        la_env_sum_ += double(lim_gain_) - double(la_env_[wp]);
        la_env_[wp] = lim_gain_;
        if (++la_recalc_cnt_ >= 4096u) {
            la_recalc_cnt_ = 0;
            double ssum = 0.0;
            for (float ev : la_env_) ssum += double(ev);
            la_env_sum_ = ssum;
        }
        const float g = std::clamp(float(la_env_sum_ / double(kLookahead)), 0.0f, 1.0f);
        const uint32_t rp = (wp + kLookaheadBuf - (kLookahead - 1)) % kLookaheadBuf;
        l = std::clamp(la_l_[rp] * g, -kBrickwallCeiling, kBrickwallCeiling);
        r = std::clamp(la_r_[rp] * g, -kBrickwallCeiling, kBrickwallCeiling);
        la_pos_ = (wp + 1) % kLookaheadBuf;

        out_l[i] = l;
        out_r[i] = r;
    }

    last_comp_gr_db_.store(worst_gr_db, std::memory_order_relaxed);
}

// =========================================================================
// PersonalMonitorServer
// =========================================================================

PersonalMonitorServer::PersonalMonitorServer(uint32_t sample_rate)
    : sample_rate_(sample_rate ? sample_rate : 48000) {
    for (int i = 0; i < kMonitorMaxInputChannels; ++i) {
        ch_names_[i] = "Ch " + std::to_string(i + 1);
        ch_peaks_dbfs_[i] = -120;
    }
}

PersonalMonitorServer::~PersonalMonitorServer() { stop(); }

bool PersonalMonitorServer::start(uint16_t bind_port, const std::string& bind_ip, uint16_t discovery_port) {
    stop();
    if (!sock_.open()) return false;
    const std::string eff_ip =
        ((bind_ip.empty() || bind_ip == "0.0.0.0") && !primary_nic_ip_.empty()) ? primary_nic_ip_ : bind_ip;
    bool bound = false;
    if (!eff_ip.empty() && eff_ip != "0.0.0.0") {
        NetworkInterfaceInfo info{};
        const bool found = find_interface_by_ip(eff_ip, info);
        bound = sock_.bind_to_interface(eff_ip, bind_port, found ? info.if_index : 0u,
                                        found ? info.system_name : "");
        if (!bound && (bind_ip.empty() || bind_ip == "0.0.0.0")) {
            bound = sock_.bind("0.0.0.0", bind_port);
        }
    } else {
        bound = sock_.bind("0.0.0.0", bind_port);
    }
    if (!bound) {
        sock_.close();
        return false;
    }
    sock_.set_low_latency_qos();
    sock_.set_nonblocking(true);  // non-blocking poll (SO_RCVTIMEO=0 blocks forever on Winsock!)
    bound_port_ = sock_.local_port();

    // Optional Secondary NIC socket for physical Dual-NIC 1+1 failover
    if (!secondary_nic_ip_.empty() && secondary_nic_ip_ != "0.0.0.0") {
        if (sec_sock_.open()) {
            NetworkInterfaceInfo sinfo{};
            const bool sf = find_interface_by_ip(secondary_nic_ip_, sinfo);
            if (sec_sock_.bind_to_interface(secondary_nic_ip_, 0, sf ? sinfo.if_index : 0u,
                                            sf ? sinfo.system_name : "")) {
                sec_sock_.set_low_latency_qos();
                sec_sock_.set_nonblocking(true);
                sec_sock_active_.store(true, std::memory_order_release);
            } else {
                sec_sock_.close();
                sec_sock_active_.store(false, std::memory_order_release);
            }
        }
    }

    // Optional LAN UDP Broadcast Discovery responder (guarantees discovery even when mDNS is blocked)
    disc_port_ = 0;
    if (disc_sock_.open()) {
        disc_sock_.set_broadcast(true);
        if (disc_sock_.bind("0.0.0.0", discovery_port)) {
            disc_sock_.set_nonblocking(true);
            disc_port_ = disc_sock_.local_port();
        } else {
            disc_sock_.close();
        }
    }

    total_control_rx_.store(0, std::memory_order_relaxed);
    discovery_replies_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    pacer_stop_.store(false, std::memory_order_release);
    pacer_ = std::thread([this] { pacer_loop(); });
    return true;
}

bool PersonalMonitorServer::set_network_interfaces(const std::string& primary_ip,
                                                   const std::string& secondary_ip) {
    std::lock_guard<std::mutex> lk(mu_);
    primary_nic_ip_ = (primary_ip == "auto" || primary_ip == "0.0.0.0") ? std::string() : primary_ip;
    secondary_nic_ip_ = (secondary_ip == "auto" || secondary_ip == "0.0.0.0") ? std::string() : secondary_ip;

    sec_sock_.close();
    sec_sock_active_.store(false, std::memory_order_release);
    bool sec_ok = true;
    if (!secondary_nic_ip_.empty()) {
        sec_ok = false;
        if (sec_sock_.open()) {
            NetworkInterfaceInfo sinfo{};
            const bool sf = find_interface_by_ip(secondary_nic_ip_, sinfo);
            if (sec_sock_.bind_to_interface(secondary_nic_ip_, 0, sf ? sinfo.if_index : 0u,
                                            sf ? sinfo.system_name : "")) {
                sec_sock_.set_low_latency_qos();
                sec_sock_.set_nonblocking(true);
                sec_sock_active_.store(true, std::memory_order_release);
                sec_ok = true;
            } else {
                sec_sock_.close();
            }
        }
    }

    for (int i = 0; i < kMaxMonitorClients; ++i) {
        auto& s = slots_[i];
        if (!s.active || !s.tx) continue;
        s.tx->set_network_interfaces(primary_nic_ip_, secondary_nic_ip_);
        if (sec_sock_active_.load(std::memory_order_acquire)) {
            s.tx->set_secondary_send([this, i](const uint8_t* d, size_t n) {
                const Endpoint dst = slots_[i].ep;
                const bool ok = sec_sock_.is_open() &&
                                sec_sock_.send_to_qos(d, n, dst, QosTrafficClass::kAudio) == int(n);
                if (ok) {
                    sec_tx_bytes_.fetch_add(n, std::memory_order_relaxed);
                    ++slots_[i].packets_tx;
                }
                return ok;
            });
        } else {
            s.tx->set_secondary_send(nullptr);
        }
    }
    return sec_ok;
}

// Dedicated send thread: the mixer may run 512..1024-frame blocks (10.7..21.3 ms) and would otherwise put
// a whole block on the air at once. Here every client's stereo bus leaves in 2.5 ms steps on an absolute
// steady-clock deadline (real-time / QoS USER_INTERACTIVE / MMCSS priority) -> one Opus/PCM packet per
// 5 ms per channel, evenly spaced, which is what Wi-Fi (and the phone's jitter buffer) handles best.
void PersonalMonitorServer::pacer_loop() {
    RealtimeThreadScope rt(kPaceTickUs);
    uint64_t next = now_us();
    uint64_t last = next;
    while (!pacer_stop_.load(std::memory_order_acquire)) {
        const bool fast_tick =
            wifi_redundancy_.load(std::memory_order_relaxed) || any_client_redundancy_.load(std::memory_order_relaxed);
        const uint32_t tick_us = fast_tick ? 1000u : kPaceTickUs;
        next += tick_us;
        sleep_until_us(next, [] {});
        const uint64_t now = now_us();
        if (now > next + 100000) next = now;  // > 100 ms stall (suspend): re-anchor, don't burst
        const double dt = double(now - last);
        last = now;
        poll_control();
        std::lock_guard<std::mutex> lk(mu_);
        flush_flow_dups_locked(now);
        const uint32_t wc = uint32_t(now);
        for (auto& s : slots_) {
            if (s.active && s.tx) s.tx->service_redundancy(wc);
        }
        pace_tick_locked(dt);
    }
}

void PersonalMonitorServer::refresh_any_redundancy_locked() {
    bool any = false;
    for (const auto& s : slots_) {
        if (s.active && s.client_wants_redundancy) {
            any = true;
            break;
        }
    }
    any_client_redundancy_.store(any, std::memory_order_release);
}

void PersonalMonitorServer::set_wifi_redundancy(bool enabled, uint32_t stagger_us) {
    dup_stagger_us_.store(std::clamp<uint32_t>(stagger_us, 0, 10000), std::memory_order_relaxed);
    wifi_redundancy_.store(enabled, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : slots_) {
        if (s.active && s.tx) s.tx->set_wifi_redundancy(enabled || s.client_wants_redundancy, stagger_us);
    }
    if (!enabled && !any_client_redundancy_.load(std::memory_order_relaxed)) flow_dups_.clear();
}

void PersonalMonitorServer::set_multichannel_flow(bool enabled, uint8_t flow_channels) {
    flow_channels_.store(std::clamp<uint8_t>(flow_channels, 2, kMaxMonitorFlowChannels),
                         std::memory_order_relaxed);
    multichannel_flow_.store(enabled, std::memory_order_release);
}

void PersonalMonitorServer::destroy_slot_flow_codecs_locked(ClientSlot& s) {
#ifdef CL_HAVE_OPUS
    for (auto& enc : s.flow_opus) {
        if (enc) {
            opus_encoder_destroy(reinterpret_cast<OpusEncoder*>(enc));
            enc = nullptr;
        }
    }
#else
    s.flow_opus.fill(nullptr);
#endif
    s.flow_opus_bps = 0;
}

void PersonalMonitorServer::flush_flow_dups_locked(uint64_t now) {
    while (!flow_dups_.empty() && now >= flow_dups_.front().due_us) {
        const auto& pd = flow_dups_.front();
        if (sock_.send_to_qos(pd.dgram.data(), pd.dgram.size(), pd.dst, QosTrafficClass::kAudio) ==
            int(pd.dgram.size())) {
            dup_tx_pkts_.fetch_add(1, std::memory_order_relaxed);
            if (pd.slot_idx >= 0 && pd.slot_idx < kMaxMonitorClients && slots_[pd.slot_idx].active) {
                ++slots_[pd.slot_idx].packets_tx;
            }
        }
        flow_dups_.pop_front();
    }
}

void PersonalMonitorServer::emit_monitor_flow_locked(ClientSlot& s, int slot_idx, uint8_t flow_ch,
                                                     uint32_t samples, uint64_t media_ts, uint64_t now) {
    if (samples == 0 || flow_ch == 0 || flow_ch > kMaxMonitorFlowChannels) return;

    // Receiver-Driven Subscriptions:
    //   - Stereo personal mix mode (flow_ch == 2): if client muted ALL input channels (mask == 0) or
    //     server muted client, suppress Wi-Fi packets completely.
    //   - Multi-channel flow mode (flow_ch > 2): pack ONLY subscribed channels (bit c set in active_channel_mask).
    uint16_t send_mask = 0;
    if (s.server_muted || s.active_channel_mask == 0) {
        muted_suppressed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (flow_ch == 2) {
        send_mask = 0x0003u;
    } else {
        const uint16_t avail_mask = uint16_t((flow_ch >= 16) ? 0xFFFFu : ((1u << flow_ch) - 1u));
        send_mask = uint16_t(s.active_channel_mask & avail_mask);
        if (send_mask == 0) {
            muted_suppressed_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    const bool lossless = (s.bitrate_kbps == kMonitorBitrateLossless);
    AudioFormat2 wire_fmt = lossless ? AudioFormat2::kPcm24Packed : AudioFormat2::kPcm16;
    const int target_bps_per_ch =
        lossless ? 0 : std::clamp<int>(int(s.bitrate_kbps) * 1000 / std::max<int>(2, flow_ch), 48000, 256000);
    bool use_opus = false;
#ifdef CL_HAVE_OPUS
    if (!lossless && sample_rate_ == 48000 && (samples == 120 || samples == 240)) {
        use_opus = true;
        wire_fmt = AudioFormat2::kOpus;
    }
#endif

    std::array<std::vector<uint8_t>, kMaxMonitorFlowChannels> enc_bufs;
    const uint8_t* ch_ptrs[kMaxMonitorFlowChannels]{};
    uint16_t ch_lens[kMaxMonitorFlowChannels]{};
    uint8_t packed_count = 0;

    // Encode uniformly across all active channels in this packet so wire_fmt is never mixed mid-packet.
    bool all_opus_ok = false;
#ifdef CL_HAVE_OPUS
    if (use_opus) {
        all_opus_ok = true;
        for (uint8_t c = 0; c < flow_ch; ++c) {
            if (((send_mask >> c) & 1u) == 0u) continue;
            const float* src = s.flow_stage[c].data();
            auto& out = enc_bufs[packed_count];
            auto*& enc = s.flow_opus[c];
            if (!enc) {
                int err = OPUS_OK;
                OpusEncoder* oe = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
                if (oe && err == OPUS_OK) {
                    opus_encoder_ctl(oe, OPUS_SET_COMPLEXITY(5));
                    opus_encoder_ctl(oe, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
                    opus_encoder_ctl(oe, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
                    opus_encoder_ctl(oe, OPUS_SET_LSB_DEPTH(24));
                    opus_encoder_ctl(oe, OPUS_SET_VBR(1));
                    opus_encoder_ctl(oe, OPUS_SET_VBR_CONSTRAINT(1));
                    opus_encoder_ctl(oe, OPUS_SET_BITRATE(target_bps_per_ch));
                    enc = oe;
                }
            }
            if (!enc) {
                all_opus_ok = false;
                break;
            }
            auto* oe = reinterpret_cast<OpusEncoder*>(enc);
            if (s.flow_opus_bps != target_bps_per_ch) {
                for (uint8_t j = 0; j < kMaxMonitorFlowChannels; ++j) {
                    if (s.flow_opus[j])
                        opus_encoder_ctl(reinterpret_cast<OpusEncoder*>(s.flow_opus[j]),
                                         OPUS_SET_BITRATE(target_bps_per_ch));
                }
                s.flow_opus_bps = target_bps_per_ch;
            }
            out.resize(256);
            const int nb = opus_encode_float(oe, src, int(samples), out.data(), int(out.size()));
            if (nb <= 0) {
                all_opus_ok = false;
                break;
            }
            out.resize(size_t(nb));
            ch_ptrs[packed_count] = out.data();
            ch_lens[packed_count] = uint16_t(out.size());
            ++packed_count;
        }
        if (all_opus_ok) {
            wire_fmt = AudioFormat2::kOpus;
        } else {
            packed_count = 0;
        }
    }
#endif
    if (!all_opus_ok) {
        wire_fmt = lossless ? AudioFormat2::kPcm24Packed : AudioFormat2::kPcm16;
        for (uint8_t c = 0; c < flow_ch; ++c) {
            if (((send_mask >> c) & 1u) == 0u) continue;
            const float* src = s.flow_stage[c].data();
            auto& out = enc_bufs[packed_count];
            if (lossless) {
                out.resize(size_t(samples) * 3u);
                for (uint32_t i = 0; i < samples; ++i) {
                    const float cl = std::clamp(src[i], -1.0f, 1.0f);
                    const int32_t q = std::clamp<int32_t>(int32_t(std::lrint(double(cl) * 8388607.0)),
                                                          -8388608, 8388607);
                    const uint32_t u = uint32_t(q) & 0xFFFFFFu;
                    out[3 * i + 0] = uint8_t(u & 0xFFu);
                    out[3 * i + 1] = uint8_t((u >> 8) & 0xFFu);
                    out[3 * i + 2] = uint8_t((u >> 16) & 0xFFu);
                }
            } else {
                out.resize(size_t(samples) * 2u);
                for (uint32_t i = 0; i < samples; ++i) {
                    const float cl = std::clamp(src[i], -1.0f, 1.0f);
                    const int16_t q = int16_t(std::clamp<int>(int(std::lrint(double(cl) * 32767.0)),
                                                              -32768, 32767));
                    out[2 * i + 0] = uint8_t(q & 0xFF);
                    out[2 * i + 1] = uint8_t((q >> 8) & 0xFF);
                }
            }
            ch_ptrs[packed_count] = out.data();
            ch_lens[packed_count] = uint16_t(out.size());
            ++packed_count;
        }
    }

    if (packed_count == 0) return;

    MonitorAudioFlowHeader fh{};
    fh.flow_seq = s.flow_seq++;
    fh.media_timestamp_samples = media_ts;
    fh.sample_rate = sample_rate_;
    fh.num_samples = uint16_t(samples);
    fh.channel_mask = send_mask;
    fh.sample_format = wire_fmt;
    fh.flags = (wire_fmt == AudioFormat2::kOpus) ? kMonitorFlowFlagVarLen : 0u;
    fh.channel_count = packed_count;

    uint8_t body[4096];
    const size_t blen = write_monitor_audio_flow(body, sizeof(body), fh, ch_ptrs, ch_lens);
    if (blen == 0) return;

    PacketHeader ph{};
    ph.type = PacketType::kMonitorAudioFlow;
    ph.flags = kFlagHasCrc;
    ph.seq_id = fh.flow_seq;
    ph.timestamp_us = uint32_t(now);
    ph.seq16 = uint16_t(fh.flow_seq);
    uint8_t dgram[kPacketHeaderSize + sizeof(body)];
    const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, blen);
    if (dlen == 0) return;

    bool pri_ok = false;
    if (!primary_link_down_.load(std::memory_order_acquire) &&
        sock_.send_to_qos(dgram, dlen, s.ep, QosTrafficClass::kAudio) == int(dlen)) {
        pri_ok = true;
        ++s.packets_tx;
        flow_tx_pkts_.fetch_add(1, std::memory_order_relaxed);
        pri_tx_bytes_.fetch_add(dlen, std::memory_order_relaxed);
    }

    // Physical Dual-NIC 1+1 Failover OR single-NIC Wi-Fi 1+1 Redundancy (+1.5 ms):
    const bool dual_nic = sec_sock_active_.load(std::memory_order_acquire) && sec_sock_.is_open();
    if (dual_nic || wifi_redundancy_.load(std::memory_order_acquire) || s.client_wants_redundancy) {
        body[21] |= kFlagRedundantCopy;
        uint8_t dup_dg[kPacketHeaderSize + sizeof(body)];
        const size_t dup_len = write_packet(dup_dg, sizeof(dup_dg), ph, body, blen);
        if (dup_len > 0) {
            if (dual_nic) {
                // Physical Secondary NIC transmits backup copy immediately -> 0 ms failover if Primary NIC drops!
                if (sec_sock_.send_to_qos(dup_dg, dup_len, s.ep, QosTrafficClass::kAudio) == int(dup_len)) {
                    ++s.packets_tx;
                    dup_tx_pkts_.fetch_add(1, std::memory_order_relaxed);
                    sec_tx_bytes_.fetch_add(dup_len, std::memory_order_relaxed);
                    if (!pri_ok) {
                        flow_tx_pkts_.fetch_add(1, std::memory_order_relaxed);
                        failover_pkts_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else {
                const uint32_t stag = dup_stagger_us_.load(std::memory_order_relaxed);
                if (stag == 0) {
                    if (sock_.send_to_qos(dup_dg, dup_len, s.ep, QosTrafficClass::kAudio) == int(dup_len)) {
                        ++s.packets_tx;
                        dup_tx_pkts_.fetch_add(1, std::memory_order_relaxed);
                        pri_tx_bytes_.fetch_add(dup_len, std::memory_order_relaxed);
                    }
                } else {
                    if (flow_dups_.size() >= 1024) flow_dups_.pop_front();
                    PendingFlowDup pd;
                    pd.due_us = now + uint64_t(stag);
                    pd.slot_idx = slot_idx;
                    pd.dst = s.ep;
                    pd.dgram.assign(dup_dg, dup_dg + dup_len);
                    flow_dups_.push_back(std::move(pd));
                }
            }
        }
    }
}

void PersonalMonitorServer::pace_tick_locked(double dt_us) {
    const double per_us = double(sample_rate_) / 1e6;
    const uint64_t block = std::max<uint64_t>(in_block_max_, kPaceChunk);
    const uint64_t high_water = 2 * block + kPaceChunk;
    const uint64_t now = now_us();
    const uint32_t wc_us = uint32_t(now);
    const bool use_flow_global = multichannel_flow_.load(std::memory_order_acquire);
    const uint8_t cfg_flow_ch = flow_channels_.load(std::memory_order_relaxed);

    for (int slot_idx = 0; slot_idx < kMaxMonitorClients; ++slot_idx) {
        auto& s = slots_[slot_idx];
        if (!s.active) continue;
        if (s.last_seen_us != 0 && now > s.last_seen_us && (now - s.last_seen_us) >= 15'000'000ull) {
            destroy_slot_flow_codecs_locked(s);
            s.active = false;
            s.client_wants_redundancy = false;
            s.tx.reset();
            s.bus.reset();
            refresh_any_redundancy_locked();
            continue;
        }
        if (!s.tx || s.ring_l.empty()) continue;
        uint64_t fill = s.ring_wr - s.ring_rd;
        if (!s.primed) {
            if (fill < block) continue;
            s.primed = true;
            s.credit = 0.0;
        }
        // Above the high-water mark (the pacer was starved) drain 2 % faster instead of firing an extra packet
        // ahead of its slot: a packet that leaves early looks like a shorter network delay to the phone's
        // minimum-delay clock filter and would pull its playout deadline towards the data edge.
        const double speed = (fill > high_water) ? 1.02 : 1.0;
        s.credit = std::min(s.credit + dt_us * per_us * speed, double(4 * kPaceChunk));
        uint32_t steps = 0;
        while (s.credit >= double(kPaceChunk) && fill >= kPaceChunk) {
            s.credit -= double(kPaceChunk);
            fill -= kPaceChunk;
            ++steps;
        }

        const bool use_flow = use_flow_global && s.client_supports_flow;
        bool flow_rings_ready = cfg_flow_ch > 2;
        if (flow_rings_ready) {
            for (uint8_t c = 0; c < cfg_flow_ch; ++c) {
                if (s.flow_rings[c].size() != kPaceRingFrames) {
                    flow_rings_ready = false;
                    break;
                }
            }
        }
        const uint8_t fch = flow_rings_ready ? cfg_flow_ch : uint8_t(2);
        const uint32_t flow_target = (s.bitrate_kbps == kMonitorBitrateLossless || s.latency_ms <= 10) ? 120u : 240u;

        for (uint32_t k = 0; k < steps; ++k) {
            const uint64_t ts = s.ring_rd;
            if (use_flow) {
                if (s.flow_stage_n == 0) s.flow_stage_ts = ts;
                const uint32_t need_sz = std::max(flow_target, s.flow_stage_n + kPaceChunk);
                for (uint8_t c = 0; c < fch; ++c) {
                    if (s.flow_stage[c].size() < need_sz) s.flow_stage[c].resize(need_sz, 0.0f);
                    const float* ring_src =
                        (fch == 2) ? (c == 0 ? s.ring_l.data() : s.ring_r.data()) : s.flow_rings[c].data();
                    for (uint32_t i = 0; i < kPaceChunk; ++i) {
                        const size_t idx = size_t((ts + i) % kPaceRingFrames);
                        s.flow_stage[c][s.flow_stage_n + i] = ring_src[idx];
                    }
                }
                s.flow_stage_n += kPaceChunk;
                if (s.flow_stage_n >= flow_target) {
                    emit_monitor_flow_locked(s, slot_idx, fch, s.flow_stage_n, s.flow_stage_ts, now);
                    s.flow_stage_n = 0;
                }
            } else {
                for (uint32_t i = 0; i < kPaceChunk; ++i) {
                    const size_t idx = size_t((ts + i) % kPaceRingFrames);
                    pace_l_[i] = s.ring_l[idx];
                    pace_r_[i] = s.ring_r[idx];
                }
                s.tx->push_channel(0, pace_l_.data(), kPaceChunk, AudioFormat2::kFloat32, ts, wc_us);
                s.tx->push_channel(1, pace_r_.data(), kPaceChunk, AudioFormat2::kFloat32, ts, wc_us);
            }
            s.ring_rd += kPaceChunk;
            paced_chunks_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void PersonalMonitorServer::stop() {
    pacer_stop_.store(true, std::memory_order_release);
    if (pacer_.joinable()) pacer_.join();
    running_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : slots_) {
        destroy_slot_flow_codecs_locked(s);
        s.active = false;
        s.tx.reset();
        s.bus.reset();
    }
    flow_dups_.clear();
    disc_sock_.close();
    sec_sock_.close();
    sec_sock_active_.store(false, std::memory_order_release);
    sock_.close();
    bound_port_ = 0;
    disc_port_ = 0;
}

void PersonalMonitorServer::set_studio_name(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    studio_name_ = name.empty() ? "CrossLink Studio" : name.substr(0, 63);
}

void PersonalMonitorServer::set_channel_name(int ch, const std::string& name) {
    if (ch < 0 || ch >= kMonitorMaxInputChannels) return;
    std::lock_guard<std::mutex> lk(mu_);
    ch_names_[ch] = name.empty() ? ("Ch " + std::to_string(ch + 1)) : name.substr(0, 15);
}

void PersonalMonitorServer::set_client_mute(int slot, bool muted) {
    if (slot < 0 || slot >= kMaxMonitorClients) return;
    std::lock_guard<std::mutex> lk(mu_);
    slots_[slot].server_muted = muted;
}

void PersonalMonitorServer::apply_bitrate_locked(ClientSlot& s, uint16_t bitrate_kbps) {
    if (bitrate_kbps == kMonitorBitrateLossless) {
        s.bitrate_kbps = kMonitorBitrateLossless;
        if (s.tx) s.tx->set_codec_profile(AudioCodecProfile::kPcm24);  // lossless 24-bit, ~2.3 Mbps stereo
        return;
    }
    s.bitrate_kbps = std::clamp<uint16_t>(bitrate_kbps, 128, 512);
    if (s.tx) s.tx->set_codec_profile(audio_codec_profile_from_bitrate(int(s.bitrate_kbps) / 2));
}

void PersonalMonitorServer::set_client_bitrate(int slot, uint16_t bitrate_kbps) {
    if (slot < 0 || slot >= kMaxMonitorClients) return;
    std::lock_guard<std::mutex> lk(mu_);
    apply_bitrate_locked(slots_[slot], bitrate_kbps);
}

std::vector<MonitorClientInfo> PersonalMonitorServer::clients() const {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t now = now_us();
    std::vector<MonitorClientInfo> out;
    for (int i = 0; i < kMaxMonitorClients; ++i) {
        const auto& s = slots_[i];
        if (!s.active) continue;
        MonitorClientInfo info{};
        info.slot = i;
        info.client_id = s.client_id;
        info.name = s.name;
        info.address = s.ep.to_string();
        info.bitrate_kbps = s.bitrate_kbps;
        info.latency_ms = s.latency_ms;
        info.muted = s.server_muted;
        info.connected = (now - s.last_seen_us) < 15'000'000ull;  // 15 s timeout
        info.packets_tx = s.packets_tx;
        info.control_rx = s.control_rx;
        info.active_channel_mask = s.active_channel_mask;
        info.flow_enabled = s.client_supports_flow && multichannel_flow_.load(std::memory_order_relaxed);
        out.push_back(std::move(info));
    }
    return out;
}

PersonalMonitorBus* PersonalMonitorServer::bus(int slot) {
    if (slot < 0 || slot >= kMaxMonitorClients) return nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    return slots_[slot].bus.get();
}

int PersonalMonitorServer::find_or_alloc_slot_locked(uint32_t client_id, const Endpoint& ep, uint64_t now) {
    for (int i = 0; i < kMaxMonitorClients; ++i) {
        if (slots_[i].active &&
            ((client_id != 0 && slots_[i].client_id == client_id) || (slots_[i].ep == ep))) {
            return i;
        }
    }
    int free_idx = -1;
    for (int i = 0; i < kMaxMonitorClients; ++i) {
        if (!slots_[i].active) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        for (int i = 0; i < kMaxMonitorClients; ++i) {
            if (slots_[i].last_seen_us != 0 && now > slots_[i].last_seen_us &&
                (now - slots_[i].last_seen_us) >= 15'000'000ull) {
                destroy_slot_flow_codecs_locked(slots_[i]);
                slots_[i].active = false;
                slots_[i].client_wants_redundancy = false;
                slots_[i].tx.reset();
                slots_[i].bus.reset();
                free_idx = i;
                break;
            }
        }
    }
    if (free_idx >= 0) {
        const int i = free_idx;
        auto& s = slots_[i];
        s.active = true;
        s.client_id = client_id ? client_id : uint32_t(i + 1);
        s.name = "Phone " + std::to_string(i + 1);
        s.ep = ep;
        s.bitrate_kbps = 256;
        s.latency_ms = 20;
        s.server_muted = false;
        s.client_supports_flow = false;
        s.client_wants_redundancy = false;
        s.active_channel_mask = ~0ull;
        s.flow_seq = 0;
        s.packets_tx = 0;
        s.control_rx = 0;
        s.media_sample = 0;
        s.ring_l.assign(kPaceRingFrames, 0.0f);
        s.ring_r.assign(kPaceRingFrames, 0.0f);
        for (auto& fr : s.flow_rings) fr.clear();
        destroy_slot_flow_codecs_locked(s);
        s.ring_wr = s.ring_rd = 0;
        s.credit = 0.0;
        s.primed = false;
        s.flow_stage_n = 0;
        s.bus = std::make_unique<PersonalMonitorBus>(sample_rate_);
        // Always read slots_[i].ep dynamically so phone reconnects on a new UDP port keep working!
        s.tx = std::make_unique<AudioTxEngine>(
            sample_rate_, kDefaultMtu,
            [this, i](const uint8_t* d, size_t n) {
                if (primary_link_down_.load(std::memory_order_acquire)) return false;
                const Endpoint dst = slots_[i].ep;
                const bool ok = sock_.send_to_qos(d, n, dst, QosTrafficClass::kAudio) == int(n);
                if (ok) {
                    ++slots_[i].packets_tx;
                    pri_tx_bytes_.fetch_add(n, std::memory_order_relaxed);
                }
                return ok;
            });
        s.tx->set_latency_ms(20);
        s.tx->set_codec_profile(audio_codec_profile_from_bitrate(128));  // 128 kbps/ch = 256 kbps stereo
        s.tx->set_wifi_redundancy(wifi_redundancy_.load(std::memory_order_acquire),
                                  dup_stagger_us_.load(std::memory_order_relaxed));
        s.tx->set_network_interfaces(primary_nic_ip_, secondary_nic_ip_);
        if (sec_sock_active_.load(std::memory_order_acquire)) {
            s.tx->set_secondary_send([this, i](const uint8_t* d, size_t n) {
                const Endpoint dst = slots_[i].ep;
                const bool ok = sec_sock_.is_open() &&
                                sec_sock_.send_to_qos(d, n, dst, QosTrafficClass::kAudio) == int(n);
                if (ok) {
                    sec_tx_bytes_.fetch_add(n, std::memory_order_relaxed);
                    ++slots_[i].packets_tx;
                }
                return ok;
            });
        }
        return i;
    }
    return -1;
}

void PersonalMonitorServer::send_meta_response(const Endpoint& dst, uint32_t client_id, int slot) {
    MonitorMetaChannel meta[kMonitorMaxInputChannels]{};
    uint8_t n = active_in_channels_ ? active_in_channels_ : 8;
    for (uint8_t i = 0; i < n; ++i) {
        std::snprintf(meta[i].name, sizeof(meta[i].name), "%s", ch_names_[i].c_str());
        meta[i].peak_dbfs = ch_peaks_dbfs_[i];
    }
    MonitorBusTelemetry tel{};
    if (slot >= 0 && slot < kMaxMonitorClients) {
        const auto& s = slots_[slot];
        if (s.bus) tel.gain_reduction_db = s.bus->last_comp_gr_db();
        tel.mix_peak_dbfs = s.mix_peak_dbfs;
    }
    uint8_t body[8 + kMonitorMaxInputChannels * 17];
    const size_t blen = write_monitor_meta(body, sizeof(body), client_id, n, meta, &tel);
    if (blen == 0) return;
    PacketHeader ph{};
    ph.type = PacketType::kMonitorControl;
    ph.flags = kFlagHasCrc;
    ph.timestamp_us = now_us32();
    uint8_t dgram[kPacketHeaderSize + sizeof(body)];
    const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, blen);
    if (dlen > 0) sock_.send_to_qos(dgram, dlen, dst, QosTrafficClass::kSyncControl);
}

void PersonalMonitorServer::handle_control_packet(const PacketHeader&, const uint8_t* payload, size_t len,
                                                  const Endpoint& src, uint64_t now) {
    MonitorControlPayload mc{};
    if (!parse_monitor_control(payload, len, mc)) return;

    std::lock_guard<std::mutex> lk(mu_);
    const int idx = find_or_alloc_slot_locked(mc.client_id, src, now);
    if (idx < 0) return;
    auto& s = slots_[idx];
    s.ep = src;
    s.last_seen_us = now;
    ++s.control_rx;
    total_control_rx_.fetch_add(1, std::memory_order_relaxed);

    switch (mc.op) {
        case MonitorControlOp::kHello:
            if (mc.client_name[0] != '\0') s.name = mc.client_name;
            if ((mc.reserved & kMonitorCapFlowV1) != 0) s.client_supports_flow = true;
            if ((mc.reserved & kMonitorCapRxSubMask) != 0) s.active_channel_mask = mc.active_channel_mask;
            s.client_wants_redundancy = (mc.reserved & kMonitorCapRedundancy1Plus1) != 0;
            refresh_any_redundancy_locked();
            if (s.tx) {
                s.tx->set_wifi_redundancy(wifi_redundancy_.load(std::memory_order_acquire) || s.client_wants_redundancy,
                                          dup_stagger_us_.load(std::memory_order_relaxed));
            }
            if ((mc.bitrate_kbps >= 128 && mc.bitrate_kbps <= 512) || mc.bitrate_kbps == kMonitorBitrateLossless)
                apply_bitrate_locked(s, mc.bitrate_kbps);
            if (mc.target_latency_ms >= 2 && mc.target_latency_ms <= 500) {
                s.latency_ms = mc.target_latency_ms;
                if (s.tx) s.tx->set_latency_ms(mc.target_latency_ms);
            }
            send_meta_response(src, s.client_id, idx);
            break;
        case MonitorControlOp::kSyncMetaReq:
            send_meta_response(src, s.client_id, idx);
            break;
        case MonitorControlOp::kSetChannelMix:
            if (s.bus && mc.channel_index < kMonitorMaxInputChannels) {
                PersonalMonitorChannelMix mix{};
                mix.gain_db = mc.gain_db;
                mix.pan = mc.pan;
                mix.mute = (mc.flags & 0x01u) != 0;
                mix.solo = (mc.flags & 0x02u) != 0;
                s.bus->set_channel_mix(mc.channel_index, mix);
            }
            break;
        case MonitorControlOp::kSetDsp:
            if (s.bus) {
                PersonalMonitorDspParams dsp{};
                dsp.hpf_enabled = (mc.flags & 0x04u) != 0;
                dsp.eq_enabled = (mc.flags & 0x08u) != 0;
                dsp.comp_enabled = (mc.flags & 0x10u) != 0;
                dsp.master_mute = (mc.flags & 0x20u) != 0;
                dsp.hpf_hz = mc.hpf_hz;
                dsp.eq_low_db = mc.eq_low_db;
                dsp.eq_mid_hz = mc.eq_mid_hz;
                dsp.eq_mid_db = mc.eq_mid_db;
                dsp.eq_high_db = mc.eq_high_db;
                dsp.comp_thresh_db = mc.comp_thresh_db;
                dsp.comp_ratio = mc.comp_ratio;
                dsp.comp_attack_ms = mc.comp_attack_ms;
                dsp.comp_release_ms = mc.comp_release_ms;
                dsp.comp_makeup_db = mc.comp_makeup_db;
                dsp.master_volume_db = mc.master_volume_db;
                s.bus->set_dsp(dsp);
            }
            break;
        case MonitorControlOp::kDisconnect:
            destroy_slot_flow_codecs_locked(s);
            s.active = false;
            s.client_wants_redundancy = false;
            refresh_any_redundancy_locked();
            s.tx.reset();
            s.bus.reset();
            break;
        default:
            break;
    }
}

void PersonalMonitorServer::poll_control() {
    if (!running_.load(std::memory_order_acquire)) return;
    uint8_t buf[2048];
    Endpoint src{};
    const uint64_t now = now_us();
    const size_t req_len = std::strlen(kMonitorDiscoveryReq);

    auto maybe_reply_discovery = [&](UdpSocket& s, const uint8_t* data, int n, const Endpoint& from) -> bool {
        if (n < int(req_len) || std::memcmp(data, kMonitorDiscoveryReq, req_len) != 0) return false;
        std::string name;
        uint8_t ch = 8;
        {
            std::lock_guard<std::mutex> lk(mu_);
            name = studio_name_;
            ch = active_in_channels_ ? active_in_channels_ : 8;
        }
        char resp[192];
        const int rlen = std::snprintf(resp, sizeof(resp), "%s%u|%u|%s",
                                       kMonitorDiscoveryRespPrefix, unsigned(bound_port_), unsigned(ch), name.c_str());
        if (rlen > 0) {
            s.send_to(reinterpret_cast<const uint8_t*>(resp), size_t(rlen), from);
            discovery_replies_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    };

    // 1. Poll dedicated LAN UDP broadcast discovery socket (UDP :5021)
    if (disc_port_ != 0) {
        for (int i = 0; i < 16; ++i) {
            const int n = disc_sock_.recv_from(buf, sizeof(buf), src);
            if (n <= 0) break;
            maybe_reply_discovery(disc_sock_, buf, n, src);
        }
    }

    // 2. Poll main monitor control socket (UDP :5020)
    for (int i = 0; i < 32; ++i) {
        const int n = sock_.recv_from(buf, sizeof(buf), src);
        if (n <= 0) break;
        if (maybe_reply_discovery(sock_, buf, n, src)) continue;
        // Handle 15-ms keep-alive + Receiver-Driven Subscription mask ("CLKA")
        MonitorKeepAlivePayload ka{};
        if (parse_monitor_keepalive(buf, size_t(n), ka)) {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& s : slots_) {
                if (s.active && ((ka.client_id != 0 && s.client_id == ka.client_id) || (s.ep == src))) {
                    s.ep = src;
                    s.last_seen_us = now;
                    if (n >= int(kMonitorKeepAliveSize)) {
                        s.active_channel_mask = ka.active_channel_mask;
                    }
                    break;
                }
            }
            continue;
        }
        PacketHeader h{};
        const uint8_t* payload = nullptr;
        if (parse_packet(buf, size_t(n), h, &payload) != PacketParseStatus::kOk) continue;
        if (h.type == PacketType::kMonitorControl) {
            handle_control_packet(h, payload, h.payload_size, src, now);
        } else if (h.type == PacketType::kAudioNack) {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& s : slots_) {
                if (s.active && s.tx && s.ep == src) {
                    s.tx->on_nack(payload, h.payload_size);
                    break;
                }
            }
        }
    }
}

std::vector<DiscoveredMonitorServer> discover_monitor_servers(int timeout_ms, uint16_t discovery_port,
                                                              bool include_loopback) {
    std::vector<DiscoveredMonitorServer> out;
    UdpSocket s;
    if (!s.open()) return out;
    s.set_broadcast(true);
    if (!s.bind("0.0.0.0", 0)) return out;
    s.set_nonblocking(true);

    const uint8_t* req = reinterpret_cast<const uint8_t*>(kMonitorDiscoveryReq);
    const size_t req_len = std::strlen(kMonitorDiscoveryReq);

    auto send_probe = [&](const char* ip) {
        Endpoint ep{};
        if (Endpoint::parse(ip, discovery_port, ep)) {
            s.send_to(req, req_len, ep);
        }
    };
    send_probe("255.255.255.255");
    if (include_loopback) send_probe("127.0.0.1");

    const size_t pfx_len = std::strlen(kMonitorDiscoveryRespPrefix);
    const uint64_t deadline = now_us() + uint64_t(std::clamp(timeout_ms, 50, 3000)) * 1000ull;
    uint8_t buf[512];
    Endpoint from{};
    while (now_us() < deadline) {
        const int n = s.recv_from(buf, sizeof(buf) - 1, from);
        if (n > int(pfx_len) && std::memcmp(buf, kMonitorDiscoveryRespPrefix, pfx_len) == 0) {
            buf[n] = '\0';
            const char* body = reinterpret_cast<const char*>(buf + pfx_len);
            unsigned port = 0, ch = 8;
            char name[96] = {};
            if (std::sscanf(body, "%u|%u|%95[^\r\n]", &port, &ch, name) >= 2 && port > 0 && port <= 65535) {
                std::string ip_full = from.to_string();
                const auto colon = ip_full.rfind(':');
                const std::string ip = (colon != std::string::npos) ? ip_full.substr(0, colon) : ip_full;
                bool dup = false;
                for (const auto& existing : out) {
                    if (existing.ip == ip && existing.port == uint16_t(port)) { dup = true; break; }
                }
                if (!dup) {
                    out.push_back({name[0] ? std::string(name) : "CrossLink Studio", ip, uint16_t(port), uint8_t(ch)});
                }
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return out;
}

void PersonalMonitorServer::process_and_send(const float* const* in_planes, int num_in_ch, uint32_t frames) {
    if (!running_.load(std::memory_order_acquire) || frames == 0) return;

    std::lock_guard<std::mutex> lk(mu_);
    const int ch_n = std::clamp(num_in_ch, 0, kMonitorMaxInputChannels);
    active_in_channels_ = uint8_t(ch_n);

    // Update live per-channel VU peak meters for smartphone UI
    if (in_planes) {
        for (int c = 0; c < ch_n; ++c) {
            float pk = 0.0f;
            const float* p = in_planes[c];
            if (p) {
                for (uint32_t i = 0; i < frames; ++i) {
                    const float a = std::fabs(p[i]);
                    if (a > pk) pk = a;
                }
            }
            ch_peaks_dbfs_[c] = lin_to_dbfs_i8(pk);
        }
    }

    if (tmp_l_.size() < frames) {
        tmp_l_.resize(frames, 0.0f);
        tmp_r_.resize(frames, 0.0f);
    }

    // Slowly decaying maximum of the mixer block size (sets how much the pacer may keep queued).
    in_block_max_ = std::max<uint32_t>(frames, in_block_max_ - (in_block_max_ >> 9));

    for (int i = 0; i < kMaxMonitorClients; ++i) {
        auto& s = slots_[i];
        if (!s.active || !s.bus || !s.tx || s.ring_l.empty()) continue;
        if (s.server_muted) {
            std::memset(tmp_l_.data(), 0, frames * sizeof(float));
            std::memset(tmp_r_.data(), 0, frames * sizeof(float));
        } else {
            s.bus->process_block(in_planes, ch_n, tmp_l_.data(), tmp_r_.data(), frames);
        }
        float pk = 0.0f;
        for (uint32_t k = 0; k < frames; ++k) pk = std::max(pk, std::max(std::fabs(tmp_l_[k]), std::fabs(tmp_r_[k])));
        s.mix_peak_dbfs = lin_to_dbfs_i8(pk);
        // Queue for the pacer thread (timestamps stay contiguous: ring_wr is the media position).
        const uint8_t cfg_flow_ch = flow_channels_.load(std::memory_order_relaxed);
        if (cfg_flow_ch > 2) {
            for (uint8_t c = 0; c < cfg_flow_ch; ++c) {
                if (s.flow_rings[c].size() != kPaceRingFrames) s.flow_rings[c].assign(kPaceRingFrames, 0.0f);
                const float* src_ch = (in_planes && int(c) < ch_n) ? in_planes[c] : nullptr;
                for (uint32_t k = 0; k < frames; ++k) {
                    const size_t idx = size_t((s.ring_wr + k) % kPaceRingFrames);
                    s.flow_rings[c][idx] = src_ch ? src_ch[k] : 0.0f;
                }
            }
        }
        for (uint32_t k = 0; k < frames; ++k) {
            const size_t idx = size_t((s.ring_wr + k) % kPaceRingFrames);
            s.ring_l[idx] = tmp_l_[k];
            s.ring_r[idx] = tmp_r_[k];
        }
        s.ring_wr += frames;
        if (s.ring_wr - s.ring_rd > kPaceRingFrames) {  // pacer starved of CPU for > 340 ms: drop oldest
            s.ring_rd = s.ring_wr - kPaceRingFrames / 2;
            ring_overflows_.fetch_add(1, std::memory_order_relaxed);
        }
        s.media_sample = s.ring_wr;
    }
}

// =========================================================================
// PersonalMonitorClient
// =========================================================================

PersonalMonitorClient::PersonalMonitorClient(uint32_t sample_rate)
    : sample_rate_(sample_rate ? sample_rate : 48000),
      matrix_(sample_rate_),
      arx_(matrix_, mclock_, [this](const uint8_t* data, size_t len) {
          if (!connected_.load(std::memory_order_acquire)) return false;
          std::lock_guard<std::mutex> lk(sock_mu_);
          return sock_.is_open() &&
                 sock_.send_to_qos(data, len, server_ep_, QosTrafficClass::kSyncControl) == int(len);
      }) {
    static std::atomic<uint32_t> s_client_seq{1};
    const uint32_t seq = s_client_seq.fetch_add(1, std::memory_order_relaxed);
    client_id_ = uint32_t(((now_us() ^ 0xA5A55A5Aull) ^ (uint64_t(seq) * 2654435761ull)) & 0xFFFFFFFFu);
    if (client_id_ == 0) client_id_ = seq ? seq : 1u;
}

PersonalMonitorClient::~PersonalMonitorClient() {
    disconnect();
    destroy_flow_decoders();
}

std::string PersonalMonitorClient::primary_nic_ip() const {
    std::lock_guard<std::mutex> lk(sock_mu_);
    return primary_nic_ip_;
}

std::string PersonalMonitorClient::secondary_nic_ip() const {
    std::lock_guard<std::mutex> lk(sock_mu_);
    return secondary_nic_ip_;
}

void PersonalMonitorClient::destroy_flow_decoders() {
#ifdef CL_HAVE_OPUS
    for (auto*& d : flow_opus_dec_) {
        if (d) {
            opus_decoder_destroy(reinterpret_cast<OpusDecoder*>(d));
            d = nullptr;
        }
    }
#else
    flow_opus_dec_.fill(nullptr);
#endif
}

bool PersonalMonitorClient::connect(const std::string& server_ip, uint16_t server_port,
                                    const std::string& client_name, uint16_t bitrate_kbps,
                                    uint16_t latency_ms) {
    disconnect();
    destroy_flow_decoders();
    flow_seen_ = false;
    flow_highest_ = 0;
    flow_recv_bits_.fill(0);
    flow_opus_seq_init_ = false;
    flow_opus_next_seq_ = 0;
    for (auto& po : flow_opus_pending_) po.valid = false;
    flow_ow_have_ = false;
    flow_ow_applied_ = 0.0;
    flow_ow_cur_ = -1e300;
    flow_ow_win_start_ = 0;
    flow_ow_win_n_ = 0;
    flow_ow_samples_ = 0;
    flow_ow_jitter_ema_ = 0.0;
    last_primary_flow_us_ = 0;
    clock_locked_.store(false, std::memory_order_relaxed);
    clock_jitter_us_.store(0, std::memory_order_relaxed);
    flow_dups_recovered_.store(0, std::memory_order_relaxed);
    last_rx_flow_mask_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(sock_mu_);
        if (!sock_.open()) return false;
        bool bound = false;
        if (!primary_nic_ip_.empty() && primary_nic_ip_ != "0.0.0.0") {
            NetworkInterfaceInfo info{};
            const bool found = find_interface_by_ip(primary_nic_ip_, info);
            bound = sock_.bind_to_interface(primary_nic_ip_, 0, found ? info.if_index : 0u,
                                            found ? info.system_name : "");
        }
        if (!bound && !sock_.bind("0.0.0.0", 0)) {
            sock_.close();
            return false;
        }
        sock_.set_low_latency_qos();
        sock_.set_nonblocking(true);
        if (!secondary_nic_ip_.empty() && secondary_nic_ip_ != "0.0.0.0") {
            if (sec_sock_.open()) {
                NetworkInterfaceInfo sinfo{};
                const bool sf = find_interface_by_ip(secondary_nic_ip_, sinfo);
                if (sec_sock_.bind_to_interface(secondary_nic_ip_, 0, sf ? sinfo.if_index : 0u,
                                                sf ? sinfo.system_name : "")) {
                    sec_sock_.set_low_latency_qos();
                    sec_sock_.set_nonblocking(true);
                    sec_sock_active_.store(true, std::memory_order_release);
                } else {
                    sec_sock_.close();
                    sec_sock_active_.store(false, std::memory_order_release);
                }
            }
        }
        if (!Endpoint::parse(server_ip, server_port, server_ep_)) {
            sec_sock_.close();
            sec_sock_active_.store(false, std::memory_order_release);
            sock_.close();
            return false;
        }
    }
    arx_.set_latency_ms(latency_ms);
    // In-ear monitoring: the buffer the musician picks (5 / 10 / 15 / 20 ms) is the buffer they get.
    // Adaptive growth (up to 200 ms on Wi-Fi bursts) would put a slap-back echo on their own instrument.
    arx_.set_adaptive_jitter_enabled(false);
    arx_.set_one_way_clock_sync(true);          // steady playout position from the packet timestamps
    matrix_.set_underrun_prefill(true, 75);     // refill to 75 % of the buffer after an underrun
    if (latency_ms <= 10 || wifi_redundancy_.load(std::memory_order_acquire)) {
        matrix_.set_deterministic_playout(true, 1000.0);  // strict Playout Deadline, +/-0.1% micro-trim
    } else {
        matrix_.set_deterministic_playout(false);
        matrix_.set_max_resample_ppm(2000.0);   // follow phone / PC crystal drift within +/-0.2 %
    }
    name_ = client_name.empty() ? "Smartphone" : client_name;
    bitrate_kbps_.store(bitrate_kbps, std::memory_order_relaxed);
    latency_ms_.store(latency_ms, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_release);
    return keepalive();
}

bool PersonalMonitorClient::keepalive() {
    MonitorControlPayload hello{};
    hello.op = MonitorControlOp::kHello;
    hello.reserved = uint8_t(kMonitorCapFlowV1 | kMonitorCapRxSubMask |
                             (wifi_redundancy_.load(std::memory_order_acquire) ? kMonitorCapRedundancy1Plus1 : 0u));
    hello.client_id = client_id_;
    hello.bitrate_kbps = bitrate_kbps_.load(std::memory_order_relaxed);
    hello.target_latency_ms = latency_ms_.load(std::memory_order_relaxed);
    hello.active_channel_mask = active_channel_mask_.load(std::memory_order_acquire);
    std::snprintf(hello.client_name, sizeof(hello.client_name), "%s", name_.c_str());
    return send_control(hello);
}

bool PersonalMonitorClient::update_stream(uint16_t bitrate_kbps, uint16_t latency_ms) {
    bitrate_kbps_.store(bitrate_kbps, std::memory_order_relaxed);
    latency_ms_.store(latency_ms, std::memory_order_relaxed);
    const uint16_t clamped = std::clamp<uint16_t>(latency_ms, 2, 2000);
    arx_.set_latency_ms(clamped);  // local playout buffer, live
    if (clamped <= 10 || wifi_redundancy_.load(std::memory_order_acquire)) {
        matrix_.set_deterministic_playout(true, 1000.0);
    } else {
        matrix_.set_deterministic_playout(false);
        matrix_.set_max_resample_ppm(2000.0);
    }
    return keepalive();  // server: codec + its jitter profile
}

void PersonalMonitorClient::set_wifi_redundancy(bool enabled) {
    wifi_redundancy_.store(enabled, std::memory_order_release);
    const uint16_t lat = latency_ms_.load(std::memory_order_relaxed);
    if (enabled || lat <= 10) {
        matrix_.set_deterministic_playout(true, 1000.0);
    } else {
        matrix_.set_deterministic_playout(false);
        matrix_.set_max_resample_ppm(2000.0);
    }
    if (connected_.load(std::memory_order_acquire)) {
        keepalive();
    }
}

bool PersonalMonitorClient::set_network_interfaces(const std::string& primary_ip,
                                                   const std::string& secondary_ip) {
    std::lock_guard<std::mutex> lk(sock_mu_);
    primary_nic_ip_ = (primary_ip == "auto" || primary_ip == "0.0.0.0") ? std::string() : primary_ip;
    secondary_nic_ip_ = (secondary_ip == "auto" || secondary_ip == "0.0.0.0") ? std::string() : secondary_ip;
    if (!connected_.load(std::memory_order_acquire)) return true;

    sec_sock_.close();
    sec_sock_active_.store(false, std::memory_order_release);
    if (!secondary_nic_ip_.empty()) {
        if (sec_sock_.open()) {
            NetworkInterfaceInfo sinfo{};
            const bool sf = find_interface_by_ip(secondary_nic_ip_, sinfo);
            if (sec_sock_.bind_to_interface(secondary_nic_ip_, 0, sf ? sinfo.if_index : 0u,
                                            sf ? sinfo.system_name : "")) {
                sec_sock_.set_low_latency_qos();
                sec_sock_.set_nonblocking(true);
                sec_sock_active_.store(true, std::memory_order_release);
                return true;
            }
            sec_sock_.close();
            return false;
        }
        return false;
    }
    return true;
}

MonitorDanteTelemetry PersonalMonitorClient::dante_telemetry() const {
    MonitorDanteTelemetry t{};
    t.clock_locked = clock_locked_.load(std::memory_order_relaxed);
    t.clock_offset_jitter_us = clock_jitter_us_.load(std::memory_order_relaxed);
    t.duplicates_recovered = duplicates_recovered();
    const uint16_t mask = last_rx_flow_mask_.load(std::memory_order_relaxed);
    uint32_t ch = 0;
    for (uint8_t b = 0; b < kMaxMonitorFlowChannels; ++b) {
        if ((mask >> b) & 1u) ++ch;
    }
    if (ch == 0 && audio_rx_pkts_.load(std::memory_order_relaxed) > 0) ch = 2;
    t.active_flow_channels = ch;
    t.effective_playout_ms = matrix_.current_effective_latency_ms();
    t.wifi_redundancy_enabled = wifi_redundancy_.load(std::memory_order_acquire);
    t.flow_packets_rx = flow_rx_pkts_.load(std::memory_order_relaxed);
    t.nacks_sent = arx_.total_nacks_sent();
    return t;
}

void PersonalMonitorClient::disconnect() {
    std::lock_guard<std::mutex> lk(sock_mu_);
    if (connected_.exchange(false, std::memory_order_acq_rel)) {
        MonitorControlPayload bye{};
        bye.op = MonitorControlOp::kDisconnect;
        bye.client_id = client_id_;
        // Temporarily send kDisconnect directly while socket is still open
        uint8_t body[kMonitorControlSize];
        write_monitor_control(body, bye);
        PacketHeader ph{};
        ph.type = PacketType::kMonitorControl;
        ph.flags = kFlagHasCrc;
        ph.timestamp_us = now_us32();
        uint8_t dgram[kPacketHeaderSize + kMonitorControlSize];
        const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, sizeof(body));
        if (dlen > 0 && sock_.is_open()) {
            sock_.send_to_qos(dgram, dlen, server_ep_, QosTrafficClass::kSyncControl);
        }
    }
    sec_sock_.close();
    sec_sock_active_.store(false, std::memory_order_release);
    sock_.close();
}

bool PersonalMonitorClient::send_control(const MonitorControlPayload& mc) {
    if (!connected_.load(std::memory_order_acquire)) return false;
    uint8_t body[kMonitorControlSize];
    write_monitor_control(body, mc);
    PacketHeader ph{};
    ph.type = PacketType::kMonitorControl;
    ph.flags = kFlagHasCrc;
    ph.timestamp_us = now_us32();
    uint8_t dgram[kPacketHeaderSize + kMonitorControlSize];
    const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, sizeof(body));
    last_tx_us_.store(now_us(), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(sock_mu_);
    return dlen > 0 && sock_.is_open() &&
           sock_.send_to_qos(dgram, dlen, server_ep_, QosTrafficClass::kSyncControl) == int(dlen);
}

bool PersonalMonitorClient::send_keepalive_ping(uint64_t now) {
    if (!connected_.load(std::memory_order_acquire)) return false;
    MonitorKeepAlivePayload ka{};
    ka.client_id = client_id_;
    ka.active_channel_mask = active_channel_mask_.load(std::memory_order_acquire);
    uint8_t ka_buf[kMonitorKeepAliveSize];
    const size_t ka_len = write_monitor_keepalive(ka_buf, sizeof(ka_buf), ka);
    if (ka_len == 0) return false;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(sock_mu_);
        ok = sock_.is_open() &&
             sock_.send_to_qos(ka_buf, ka_len, server_ep_, QosTrafficClass::kSyncControl) == int(ka_len);
    }
    last_tx_us_.store(now, std::memory_order_relaxed);
    pings_.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void PersonalMonitorClient::recompute_active_channel_mask() {
    bool any_solo = false;
    for (int c = 0; c < kMonitorMaxInputChannels; ++c) {
        if (local_mix_[c].solo) {
            any_solo = true;
            break;
        }
    }
    uint64_t mask = 0;
    for (int c = 0; c < kMonitorMaxInputChannels; ++c) {
        const bool active = any_solo ? (local_mix_[c].solo && !local_mix_[c].mute) : !local_mix_[c].mute;
        if (active) mask |= (1ull << c);
    }
    active_channel_mask_.store(mask, std::memory_order_release);
}

void PersonalMonitorClient::set_active_channel_mask(uint64_t mask) {
    mask_override_ = true;
    active_channel_mask_.store(mask, std::memory_order_release);
    if (connected_.load(std::memory_order_acquire)) {
        send_keepalive_ping(now_us());
    }
}

bool PersonalMonitorClient::request_meta() {
    MonitorControlPayload req{};
    req.op = MonitorControlOp::kSyncMetaReq;
    req.client_id = client_id_;
    return send_control(req);
}

bool PersonalMonitorClient::set_channel_mix(int ch, const PersonalMonitorChannelMix& mix) {
    if (ch < 0 || ch >= kMonitorMaxInputChannels) return false;
    local_mix_[ch] = mix;
    mask_override_ = false;
    recompute_active_channel_mask();
    MonitorControlPayload mc{};
    mc.op = MonitorControlOp::kSetChannelMix;
    mc.client_id = client_id_;
    mc.channel_index = uint8_t(ch);
    mc.flags = uint8_t((mix.mute ? 0x01u : 0u) | (mix.solo ? 0x02u : 0u));
    mc.gain_db = mix.gain_db;
    mc.pan = mix.pan;
    const bool ok = send_control(mc);
    if (ok) send_keepalive_ping(now_us());
    return ok;
}

bool PersonalMonitorClient::set_dsp(const PersonalMonitorDspParams& dsp) {
    MonitorControlPayload mc{};
    mc.op = MonitorControlOp::kSetDsp;
    mc.client_id = client_id_;
    mc.flags = uint8_t((dsp.hpf_enabled ? 0x04u : 0u) |
                       (dsp.eq_enabled ? 0x08u : 0u) |
                       (dsp.comp_enabled ? 0x10u : 0u) |
                       (dsp.master_mute ? 0x20u : 0u));
    mc.hpf_hz = dsp.hpf_hz;
    mc.eq_low_db = dsp.eq_low_db;
    mc.eq_mid_hz = dsp.eq_mid_hz;
    mc.eq_mid_db = dsp.eq_mid_db;
    mc.eq_high_db = dsp.eq_high_db;
    mc.comp_thresh_db = dsp.comp_thresh_db;
    mc.comp_ratio = dsp.comp_ratio;
    mc.comp_attack_ms = dsp.comp_attack_ms;
    mc.comp_release_ms = dsp.comp_release_ms;
    mc.comp_makeup_db = dsp.comp_makeup_db;
    mc.master_volume_db = dsp.master_volume_db;
    return send_control(mc);
}

void PersonalMonitorClient::flow_one_way_observe(double media_end_us, uint64_t now) {
    constexpr uint64_t kWinUs = 500'000;  // 4 windows x 0.5 s: min one-way delay filter (immune to Wi-Fi spikes)
    const double d = media_end_us - double(now);
    if (!flow_ow_have_ || std::fabs(d - flow_ow_applied_) > 500'000.0) {
        flow_ow_have_ = true;
        flow_ow_cur_ = d;
        flow_ow_win_n_ = 0;
        flow_ow_win_start_ = now;
        flow_ow_applied_ = d;
        flow_ow_slewed_ = d;
        flow_ow_last_us_ = now;
        flow_ow_samples_ = 1;
        flow_ow_jitter_ema_ = 0.0;
        clock_locked_.store(false, std::memory_order_relaxed);
        clock_jitter_us_.store(0, std::memory_order_relaxed);
        mclock_.set_fixed_offset(int64_t(std::llround(d)));
        return;
    }
    ++flow_ow_samples_;
    flow_ow_cur_ = std::max(flow_ow_cur_, d);
    if (now - flow_ow_win_start_ >= kWinUs) {
        for (int i = 3; i > 0; --i) flow_ow_win_[i] = flow_ow_win_[i - 1];
        flow_ow_win_[0] = flow_ow_cur_;
        flow_ow_win_n_ = std::min(flow_ow_win_n_ + 1, 4);
        flow_ow_cur_ = d;
        flow_ow_win_start_ = now;
    }
    double est = flow_ow_cur_;
    for (int i = 0; i < flow_ow_win_n_; ++i) est = std::max(est, flow_ow_win_[i]);
    const double resid = std::fabs(est - d);  // one-way Wi-Fi delay above the minimum envelope (us)
    flow_ow_jitter_ema_ += (resid - flow_ow_jitter_ema_) * 0.125;
    clock_jitter_us_.store(uint32_t(std::clamp(flow_ow_jitter_ema_, 0.0, 500000.0)), std::memory_order_relaxed);
    if (flow_ow_samples_ >= 8) {
        clock_locked_.store(true, std::memory_order_relaxed);
    }
    // Asymmetric: later arrivals move the clock back at once (no dropouts); a luckier (earlier) packet only
    // advances it at <= 500 ppm, so one fast packet never jerks the playout deadline towards the data edge.
    const double dt = double(now - flow_ow_last_us_);
    flow_ow_last_us_ = now;
    const double target = (est > flow_ow_slewed_) ? std::min(est, flow_ow_slewed_ + dt * 500e-6) : est;
    flow_ow_slewed_ = target;
    if (std::fabs(target - flow_ow_applied_) >= 20.0) {
        flow_ow_applied_ = target;
        mclock_.set_fixed_offset(int64_t(std::llround(target)));
    }
}

void PersonalMonitorClient::decode_flow_frame(const MonitorAudioFlowHeader& fh,
                                              const uint8_t* const ch_ptrs[kMaxMonitorFlowChannels],
                                              const uint16_t ch_lens[kMaxMonitorFlowChannels]) {
    if (fh.num_samples == 0) return;
    if (fh.sample_format == AudioFormat2::kOpus && fh.num_samples > 2880) return;
    if (flow_dec_scratch_.size() < fh.num_samples) flow_dec_scratch_.resize(fh.num_samples);
    uint8_t slot = 0;
    for (uint8_t c = 0; c < kMaxMonitorFlowChannels && slot < fh.channel_count; ++c) {
        if (((fh.channel_mask >> c) & 1u) == 0u) continue;
        const uint8_t* src = ch_ptrs[slot];
        const uint16_t slen = ch_lens[slot];
        ++slot;

        bool decoded = false;
        if (fh.sample_format == AudioFormat2::kOpus) {
#ifdef CL_HAVE_OPUS
            auto*& dec_ptr = flow_opus_dec_[c];
            if (!dec_ptr) {
                int err = OPUS_OK;
                OpusDecoder* od = opus_decoder_create(48000, 1, &err);
                if (od && err == OPUS_OK) dec_ptr = od;
            }
            if (dec_ptr) {
                const int n = opus_decode_float(reinterpret_cast<OpusDecoder*>(dec_ptr), src, int(slen),
                                                flow_dec_scratch_.data(), int(fh.num_samples), 0);
                if (n > 0) {
                    if (uint32_t(n) < fh.num_samples) {
                        std::fill(flow_dec_scratch_.begin() + n,
                                  flow_dec_scratch_.begin() + fh.num_samples, 0.0f);
                    }
                    decoded = true;
                }
            }
#endif
        } else if (fh.sample_format == AudioFormat2::kPcm24Packed && slen >= fh.num_samples * 3u) {
            for (uint32_t i = 0; i < fh.num_samples; ++i) {
                int32_t v = int32_t(uint32_t(src[3 * i + 0]) | (uint32_t(src[3 * i + 1]) << 8) |
                                    (uint32_t(src[3 * i + 2]) << 16));
                if (v & 0x800000) v -= 0x1000000;
                flow_dec_scratch_[i] = float(v) * (1.0f / 8388608.0f);
            }
            decoded = true;
        } else if (fh.sample_format == AudioFormat2::kPcm16 && slen >= fh.num_samples * 2u) {
            for (uint32_t i = 0; i < fh.num_samples; ++i) {
                const int16_t v = int16_t(uint16_t(src[2 * i + 0]) | (uint16_t(src[2 * i + 1]) << 8));
                flow_dec_scratch_[i] = float(v) * (1.0f / 32768.0f);
            }
            decoded = true;
        } else if (fh.sample_format == AudioFormat2::kFloat32 && slen >= fh.num_samples * 4u) {
            std::memcpy(flow_dec_scratch_.data(), src, size_t(fh.num_samples) * 4u);
            decoded = true;
        }

        if (decoded) {
            matrix_.write(c, fh.media_timestamp_samples, flow_dec_scratch_.data(), fh.num_samples);
        }
    }
}

void PersonalMonitorClient::flush_flow_opus_inorder() {
    while (flow_opus_seq_init_) {
        auto& po = flow_opus_pending_[flow_opus_next_seq_ & (kFlowOpusReorderSlots - 1u)];
        if (!po.valid || po.seq != flow_opus_next_seq_) break;
        MonitorAudioFlowHeader pfh{};
        const uint8_t* ptrs[kMaxMonitorFlowChannels]{};
        uint16_t lens[kMaxMonitorFlowChannels]{};
        if (parse_monitor_audio_flow(po.raw_payload.data(), po.raw_payload.size(), pfh, ptrs, lens)) {
            decode_flow_frame(pfh, ptrs, lens);
        }
        po.valid = false;
        ++flow_opus_next_seq_;
    }
}

void PersonalMonitorClient::service_flow_reorder(uint64_t now) {
    if (!flow_opus_seq_init_) return;
    // If a buffered packet sits behind a missing flow_opus_next_seq_ for > 4.5 ms (both primary + 1.5 ms
    // duplicate were lost), generate Opus PLC for the missing frame and flush consecutive buffered packets!
    for (uint32_t off = 1; off < kFlowOpusReorderSlots; ++off) {
        const uint32_t s = flow_opus_next_seq_ + off;
        const auto& po = flow_opus_pending_[s & (kFlowOpusReorderSlots - 1u)];
        if (!po.valid || po.seq != s) continue;
        const uint32_t n_samp = po.hdr.num_samples ? po.hdr.num_samples : 240u;
        // Wait for the missing frame (its +1.5 ms backup copy, or a late primary) until its samples are about to
        // be played - not a fixed 4.5 ms: on a busy phone the copy can land a few ms later, and concealing it
        // early throws the real audio away (several in a row fade to silence).
        const double cursor = matrix_.playout_cursor();
        const uint32_t dist = s - flow_opus_next_seq_;
        const double miss_end = double(po.hdr.media_timestamp_samples) - double(dist - 1u) * double(n_samp);
        const double guard = double(sample_rate_) * 0.005;  // conceal 5 ms before the cursor needs it
        if (cursor > 0.0) {
            if (miss_end > cursor + guard && now - po.arrived_us < 100'000ull) return;
        } else if (now - po.arrived_us < 4500ull) {
            return;  // playout not started yet: short fixed wait
        }
        const uint16_t mask = po.hdr.channel_mask;
        if (flow_dec_scratch_.size() < n_samp) flow_dec_scratch_.resize(n_samp);
        // Conceal every missing frame before `s` (wrap-safe), then decode the buffered run in order.
        // (Flushing inside this loop could move flow_opus_next_seq_ past `s`; a `!= s` condition then never
        // ended - a ~4e9-iteration hang of the phone's network thread.)
        while (int32_t(s - flow_opus_next_seq_) > 0) {
            const uint32_t missing_dist = s - flow_opus_next_seq_;
            const uint64_t est_ts =
                (po.hdr.media_timestamp_samples >= uint64_t(missing_dist) * n_samp)
                    ? (po.hdr.media_timestamp_samples - uint64_t(missing_dist) * n_samp)
                    : 0ull;
#ifdef CL_HAVE_OPUS
            for (uint8_t c = 0; c < kMaxMonitorFlowChannels; ++c) {
                if (((mask >> c) & 1u) == 0u) continue;
                if (flow_opus_dec_[c]) {
                    const int n = opus_decode_float(reinterpret_cast<OpusDecoder*>(flow_opus_dec_[c]),
                                                    nullptr, 0, flow_dec_scratch_.data(), int(n_samp), 0);
                    if (n > 0) matrix_.write(c, est_ts, flow_dec_scratch_.data(), uint32_t(n));
                }
            }
#endif
            ++flow_opus_next_seq_;
        }
        flush_flow_opus_inorder();
        return;
    }
}

void PersonalMonitorClient::handle_flow_packet(const PacketHeader&, const uint8_t* payload, size_t len,
                                               uint64_t now) {
    MonitorAudioFlowHeader fh{};
    const uint8_t* ch_ptrs[kMaxMonitorFlowChannels]{};
    uint16_t ch_lens[kMaxMonitorFlowChannels]{};
    if (!parse_monitor_audio_flow(payload, len, fh, ch_ptrs, ch_lens)) return;
    if (fh.sample_rate != sample_rate_) return;

    // Instant seq deduplication (1+1 Glitch-Free Wi-Fi Redundancy):
    // First copy (primary OR +1.5 ms duplicate) goes straight to playback; duplicate copy is dropped immediately.
    const uint32_t seq = fh.flow_seq;
    auto bit_get = [&](uint32_t s) {
        const uint32_t idx = s & (kFlowDupWindow - 1u);
        return ((flow_recv_bits_[idx >> 6] >> (idx & 63u)) & 1ull) != 0ull;
    };
    auto bit_set = [&](uint32_t s) {
        const uint32_t idx = s & (kFlowDupWindow - 1u);
        flow_recv_bits_[idx >> 6] |= (1ull << (idx & 63u));
    };
    auto bit_clr = [&](uint32_t s) {
        const uint32_t idx = s & (kFlowDupWindow - 1u);
        flow_recv_bits_[idx >> 6] &= ~(1ull << (idx & 63u));
    };

    if (!flow_seen_) {
        flow_seen_ = true;
        flow_highest_ = seq;
        flow_recv_bits_.fill(0);
        bit_set(seq);
    } else {
        const int32_t diff = int32_t(seq - flow_highest_);
        if (diff > 0) {
            if (uint32_t(diff) >= kFlowDupWindow) {
                flow_recv_bits_.fill(0);
            } else {
                for (uint32_t s = flow_highest_ + 1u; s != seq; ++s) bit_clr(s);
            }
            flow_highest_ = seq;
            bit_set(seq);
        } else {
            const uint32_t age = uint32_t(-diff);
            if (age >= kFlowDupWindow) {
                if (seq < 16u) {  // server restarted / new stream epoch
                    flow_recv_bits_.fill(0);
                    flow_highest_ = seq;
                    flow_opus_seq_init_ = false;
                    for (auto& po : flow_opus_pending_) po.valid = false;
                    bit_set(seq);
                } else {
                    flow_dup_drops_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            } else if (bit_get(seq)) {
                flow_dup_drops_.fetch_add(1, std::memory_order_relaxed);
                return;
            } else {
                bit_set(seq);
            }
        }
    }

    // If this unique (first-arriving) frame is a +1.5 ms redundant copy, 1+1 redundancy just rescued a
    // dropped Wi-Fi primary packet with ZERO NACK RTT!
    const bool is_redundant = (fh.flags & kFlagRedundantCopy) != 0u;
    if (is_redundant) {
        flow_dups_recovered_.fetch_add(1, std::memory_order_relaxed);
    }

    flow_rx_pkts_.fetch_add(1, std::memory_order_relaxed);
    audio_rx_pkts_.fetch_add(1, std::memory_order_relaxed);
    last_rx_flow_mask_.store(fh.channel_mask, std::memory_order_relaxed);

    // Exclude occasional +1.5 ms staggered redundant copies from the min-one-way-delay clock filter
    // while primary packets are flowing; if primary link has failed over (> 50 ms without primary),
    // seamlessly promote the redundant/secondary stream to clock reference so clock tracking continues.
    if (!is_redundant) last_primary_flow_us_ = now;
    const bool primary_absent = (last_primary_flow_us_ == 0) ||
                                (now > last_primary_flow_us_ && (now - last_primary_flow_us_) >= 50'000ull);
    if (!flow_ow_have_ || !is_redundant || primary_absent) {
        const double media_end_us =
            double(fh.media_timestamp_samples + fh.num_samples) * 1e6 / double(fh.sample_rate);
        flow_one_way_observe(media_end_us, now);
    }

    if (fh.sample_format == AudioFormat2::kOpus) {
        if (!flow_opus_seq_init_) {
            flow_opus_seq_init_ = true;
            flow_opus_next_seq_ = seq;
        }
        const int32_t odiff = int32_t(seq - flow_opus_next_seq_);
        if (odiff == 0) {
            decode_flow_frame(fh, ch_ptrs, ch_lens);
            ++flow_opus_next_seq_;
            flush_flow_opus_inorder();
        } else if (odiff > 0 && odiff < int32_t(kFlowOpusReorderSlots)) {
            auto& po = flow_opus_pending_[seq & (kFlowOpusReorderSlots - 1u)];
            po.valid = true;
            po.seq = seq;
            po.arrived_us = now;
            po.hdr = fh;
            po.raw_payload.assign(payload, payload + len);
        } else if (odiff >= int32_t(kFlowOpusReorderSlots)) {
            for (auto& po : flow_opus_pending_) po.valid = false;
            flow_opus_next_seq_ = seq;
            decode_flow_frame(fh, ch_ptrs, ch_lens);
            ++flow_opus_next_seq_;
        }
        return;
    }

    // Stateless PCM formats decode directly into sample-addressed AudioSyncMatrix
    flow_opus_seq_init_ = false;
    decode_flow_frame(fh, ch_ptrs, ch_lens);
}

void PersonalMonitorClient::poll() {
    if (!connected_.load(std::memory_order_acquire)) return;
    uint8_t buf[4200];
    Endpoint src{};
    const uint64_t now = now_us();
    // Keep-awake + Receiver-Driven Subscription mask: 16-byte datagram ("CLKA" + client_id + active_channel_mask)
    // every 15 ms keeps the phone's Wi-Fi radio out of power-save AND tells the server which channels are active.
    if (now - last_tx_us_.load(std::memory_order_relaxed) >= kWakePingUs) {
        send_keepalive_ping(now);
    }
    auto drain_socket = [&](bool secondary) {
        for (int i = 0; i < 128; ++i) {
            int n = 0;
            {
                std::lock_guard<std::mutex> lk(sock_mu_);
                const UdpSocket& s = secondary ? sec_sock_ : sock_;
                if (!s.is_open()) break;
                n = s.recv_from(buf, sizeof(buf), src);
            }
            if (n <= 0) break;
            PacketHeader h{};
            const uint8_t* payload = nullptr;
            if (parse_packet(buf, size_t(n), h, &payload) != PacketParseStatus::kOk) continue;
            if (h.type == PacketType::kMonitorAudioFlow) {
                handle_flow_packet(h, payload, h.payload_size, now);
            } else if (h.type == PacketType::kAudioFrame) {
                audio_rx_pkts_.fetch_add(1, std::memory_order_relaxed);
                arx_.on_packet(h, payload, h.payload_size, now);
            } else if (h.type == PacketType::kMonitorControl) {
                uint32_t cid = 0;
                uint8_t nch = 0;
                MonitorMetaChannel tmp[kMonitorMaxInputChannels]{};
                MonitorBusTelemetry tel{};
                if (parse_monitor_meta(payload, h.payload_size, cid, nch, tmp, kMonitorMaxInputChannels, &tel)) {
                    std::lock_guard<std::mutex> lk(meta_mu_);
                    meta_.assign(tmp, tmp + nch);
                    bus_tel_ = tel;
                    meta_rx_pkts_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };
    drain_socket(false);
    if (sec_sock_active_.load(std::memory_order_acquire)) drain_socket(true);
    // Drive flow reorder timeout PLC and legacy AUDIO2 NACK/PLC!
    service_flow_reorder(now);
    arx_.service(now);
}

void PersonalMonitorClient::pop_stereo(float* out_interleaved, uint32_t frames) {
    if (!out_interleaved || frames == 0) return;
    matrix_.pop_interleaved(mclock_, now_us(), out_interleaved, 2, frames);
}

void PersonalMonitorClient::pop_multichannel(float* const* out_planes, uint32_t num_channels, uint32_t frames) {
    if (!out_planes || num_channels == 0 || frames == 0) return;
    const uint32_t nch = std::min<uint32_t>(num_channels, kMaxMonitorFlowChannels);
    matrix_.pop_planar(mclock_, now_us(), out_planes, nch, frames);
}

std::vector<MonitorMetaChannel> PersonalMonitorClient::last_meta() const {
    std::lock_guard<std::mutex> lk(meta_mu_);
    return meta_;
}

MonitorBusTelemetry PersonalMonitorClient::last_bus_telemetry() const {
    std::lock_guard<std::mutex> lk(meta_mu_);
    return bus_tel_;
}

}  // namespace cl
