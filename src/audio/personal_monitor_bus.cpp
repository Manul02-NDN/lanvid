#include "audio/personal_monitor_bus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/clock.hpp"

namespace lv {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// -0.5 dBFS protective brickwall ceiling = 10^(-0.5/20)
constexpr float kBrickwallCeiling = 0.94406088f;

inline float db_to_lin(float db) {
    if (db <= -60.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
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
    // 5 ms smooth time constant -> ~99% converged within 5 ms (click-free parameter changes).
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
    ch_mix_[ch].gain_db = std::clamp(mix.gain_db, -60.0f, 12.0f);
    ch_mix_[ch].pan = std::clamp(mix.pan, -1.0f, 1.0f);
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
    dsp_.hpf_hz = std::clamp(dsp.hpf_hz, 20.0f, 250.0f);
    dsp_.eq_low_db = std::clamp(dsp.eq_low_db, -12.0f, 12.0f);
    dsp_.eq_mid_hz = std::clamp(dsp.eq_mid_hz, 300.0f, 5000.0f);
    dsp_.eq_mid_db = std::clamp(dsp.eq_mid_db, -12.0f, 12.0f);
    dsp_.eq_high_db = std::clamp(dsp.eq_high_db, -12.0f, 12.0f);
    dsp_.comp_thresh_db = std::clamp(dsp.comp_thresh_db, -40.0f, 0.0f);
    dsp_.comp_ratio = std::clamp(dsp.comp_ratio, 1.0f, 10.0f);
    dsp_.comp_attack_ms = std::clamp(dsp.comp_attack_ms, 0.5f, 50.0f);
    dsp_.comp_release_ms = std::clamp(dsp.comp_release_ms, 10.0f, 500.0f);
    dsp_.comp_makeup_db = std::clamp(dsp.comp_makeup_db, 0.0f, 18.0f);
    dsp_.master_volume_db = std::clamp(dsp.master_volume_db, -60.0f, 12.0f);
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
    const float lim_rel = std::exp(-1.0f / std::max(1.0f, 0.030f * fs));  // 30 ms limiter release

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

        // 5. Protective Brickwall Limiter (-0.5 dBFS ceiling for ear safety)
        const float peak = std::max(std::fabs(l), std::fabs(r));
        const float req_lim = (peak > kBrickwallCeiling) ? (kBrickwallCeiling / peak) : 1.0f;
        if (req_lim < lim_gain_) {
            lim_gain_ = req_lim;  // instant attack (brickwall)
        } else {
            lim_gain_ = lim_rel * lim_gain_ + (1.0f - lim_rel) * 1.0f;
        }
        l = std::clamp(l * lim_gain_, -kBrickwallCeiling, kBrickwallCeiling);
        r = std::clamp(r * lim_gain_, -kBrickwallCeiling, kBrickwallCeiling);

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

bool PersonalMonitorServer::start(uint16_t bind_port, const std::string& bind_ip) {
    stop();
    if (!sock_.open()) return false;
    if (!sock_.bind(bind_ip, bind_port)) {
        sock_.close();
        return false;
    }
    sock_.set_low_latency_qos();
    sock_.set_nonblocking(true);  // non-blocking poll (SO_RCVTIMEO=0 blocks forever on Winsock!)
    bound_port_ = sock_.local_port();
    running_.store(true, std::memory_order_release);
    return true;
}

void PersonalMonitorServer::stop() {
    running_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : slots_) {
        s.active = false;
        s.tx.reset();
        s.bus.reset();
    }
    sock_.close();
    bound_port_ = 0;
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

void PersonalMonitorServer::set_client_bitrate(int slot, uint16_t bitrate_kbps) {
    if (slot < 0 || slot >= kMaxMonitorClients) return;
    std::lock_guard<std::mutex> lk(mu_);
    slots_[slot].bitrate_kbps = std::clamp<uint16_t>(bitrate_kbps, 128, 512);
    if (slots_[slot].tx) {
        const int per_ch = int(slots_[slot].bitrate_kbps) / 2;
        slots_[slot].tx->set_codec_profile(audio_codec_profile_from_bitrate(per_ch));
    }
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
        out.push_back(std::move(info));
    }
    return out;
}

PersonalMonitorBus* PersonalMonitorServer::bus(int slot) {
    if (slot < 0 || slot >= kMaxMonitorClients) return nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    return slots_[slot].bus.get();
}

int PersonalMonitorServer::find_or_alloc_slot_locked(uint32_t client_id, const Endpoint& ep) {
    for (int i = 0; i < kMaxMonitorClients; ++i) {
        if (slots_[i].active &&
            ((client_id != 0 && slots_[i].client_id == client_id) || (slots_[i].ep == ep))) {
            return i;
        }
    }
    for (int i = 0; i < kMaxMonitorClients; ++i) {
        if (!slots_[i].active) {
            auto& s = slots_[i];
            s.active = true;
            s.client_id = client_id ? client_id : uint32_t(i + 1);
            s.name = "Phone " + std::to_string(i + 1);
            s.ep = ep;
            s.bitrate_kbps = 256;
            s.latency_ms = 20;
            s.server_muted = false;
            s.packets_tx = 0;
            s.control_rx = 0;
            s.media_sample = 0;
            s.bus = std::make_unique<PersonalMonitorBus>(sample_rate_);
            const Endpoint target_ep = ep;
            s.tx = std::make_unique<AudioTxEngine>(
                sample_rate_, kDefaultMtu,
                [this, i, target_ep](const uint8_t* d, size_t n) {
                    const bool ok = sock_.send_to(d, n, target_ep) == int(n);
                    if (ok) ++slots_[i].packets_tx;
                    return ok;
                });
            s.tx->set_latency_ms(20);
            s.tx->set_codec_profile(audio_codec_profile_from_bitrate(128));  // 128 kbps/ch = 256 kbps stereo
            return i;
        }
    }
    return -1;
}

void PersonalMonitorServer::send_meta_response(const Endpoint& dst, uint32_t client_id) {
    MonitorMetaChannel meta[kMonitorMaxInputChannels]{};
    uint8_t n = active_in_channels_ ? active_in_channels_ : 8;
    for (uint8_t i = 0; i < n; ++i) {
        std::snprintf(meta[i].name, sizeof(meta[i].name), "%s", ch_names_[i].c_str());
        meta[i].peak_dbfs = ch_peaks_dbfs_[i];
    }
    uint8_t body[8 + kMonitorMaxInputChannels * 17];
    const size_t blen = write_monitor_meta(body, sizeof(body), client_id, n, meta);
    if (blen == 0) return;
    PacketHeader ph{};
    ph.type = PacketType::kMonitorControl;
    ph.flags = kFlagHasCrc;
    ph.timestamp_us = now_us32();
    uint8_t dgram[kPacketHeaderSize + sizeof(body)];
    const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, blen);
    if (dlen > 0) sock_.send_to(dgram, dlen, dst);
}

void PersonalMonitorServer::handle_control_packet(const PacketHeader&, const uint8_t* payload, size_t len,
                                                  const Endpoint& src, uint64_t now) {
    MonitorControlPayload mc{};
    if (!parse_monitor_control(payload, len, mc)) return;

    std::lock_guard<std::mutex> lk(mu_);
    const int idx = find_or_alloc_slot_locked(mc.client_id, src);
    if (idx < 0) return;
    auto& s = slots_[idx];
    s.ep = src;
    s.last_seen_us = now;
    ++s.control_rx;

    switch (mc.op) {
        case MonitorControlOp::kHello:
            if (mc.client_name[0] != '\0') s.name = mc.client_name;
            if (mc.bitrate_kbps >= 128 && mc.bitrate_kbps <= 512) {
                s.bitrate_kbps = mc.bitrate_kbps;
                if (s.tx) s.tx->set_codec_profile(audio_codec_profile_from_bitrate(int(mc.bitrate_kbps) / 2));
            }
            if (mc.target_latency_ms >= 2 && mc.target_latency_ms <= 500) {
                s.latency_ms = mc.target_latency_ms;
                if (s.tx) s.tx->set_latency_ms(mc.target_latency_ms);
            }
            send_meta_response(src, s.client_id);
            break;
        case MonitorControlOp::kSyncMetaReq:
            send_meta_response(src, s.client_id);
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
            s.active = false;
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
    for (int i = 0; i < 32; ++i) {
        const int n = sock_.recv_from(buf, sizeof(buf), src);
        if (n <= 0) break;
        PacketHeader h{};
        const uint8_t* payload = nullptr;
        if (parse_packet(buf, size_t(n), h, &payload) != PacketParseStatus::kOk) continue;
        if (h.type == PacketType::kMonitorControl) {
            handle_control_packet(h, payload, h.payload_size, src, now);
        }
    }
}

void PersonalMonitorServer::process_and_send(const float* const* in_planes, int num_in_ch, uint32_t frames) {
    if (!running_.load(std::memory_order_acquire) || frames == 0) return;
    poll_control();

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

    const uint32_t wc_us = now_us32();
    for (int i = 0; i < kMaxMonitorClients; ++i) {
        auto& s = slots_[i];
        if (!s.active || !s.bus || !s.tx) continue;
        if (s.server_muted) {
            std::memset(tmp_l_.data(), 0, frames * sizeof(float));
            std::memset(tmp_r_.data(), 0, frames * sizeof(float));
        } else {
            s.bus->process_block(in_planes, ch_n, tmp_l_.data(), tmp_r_.data(), frames);
        }
        s.tx->push_channel(0, tmp_l_.data(), frames, AudioFormat2::kFloat32, s.media_sample, wc_us);
        s.tx->push_channel(1, tmp_r_.data(), frames, AudioFormat2::kFloat32, s.media_sample, wc_us);
        s.media_sample += frames;
    }
}

// =========================================================================
// PersonalMonitorClient
// =========================================================================

PersonalMonitorClient::PersonalMonitorClient(uint32_t sample_rate)
    : sample_rate_(sample_rate ? sample_rate : 48000),
      matrix_(sample_rate_),
      arx_(matrix_, mclock_, [](const uint8_t*, size_t) { return true; }) {
    client_id_ = uint32_t((now_us() ^ 0xA5A55A5Aull) & 0xFFFFFFFFu);
    if (client_id_ == 0) client_id_ = 1;
}

PersonalMonitorClient::~PersonalMonitorClient() { disconnect(); }

bool PersonalMonitorClient::connect(const std::string& server_ip, uint16_t server_port,
                                    const std::string& client_name, uint16_t bitrate_kbps,
                                    uint16_t latency_ms) {
    disconnect();
    if (!sock_.open()) return false;
    if (!sock_.bind("0.0.0.0", 0)) {
        sock_.close();
        return false;
    }
    sock_.set_low_latency_qos();
    sock_.set_nonblocking(true);
    if (!Endpoint::parse(server_ip, server_port, server_ep_)) {
        sock_.close();
        return false;
    }
    arx_.set_latency_ms(latency_ms);
    arx_.set_adaptive_jitter_enabled(true);
    connected_.store(true, std::memory_order_release);

    MonitorControlPayload hello{};
    hello.op = MonitorControlOp::kHello;
    hello.client_id = client_id_;
    hello.bitrate_kbps = bitrate_kbps;
    hello.target_latency_ms = latency_ms;
    std::snprintf(hello.client_name, sizeof(hello.client_name), "%s",
                  client_name.empty() ? "Smartphone" : client_name.c_str());
    return send_control(hello);
}

void PersonalMonitorClient::disconnect() {
    if (connected_.exchange(false, std::memory_order_acq_rel)) {
        MonitorControlPayload bye{};
        bye.op = MonitorControlOp::kDisconnect;
        bye.client_id = client_id_;
        send_control(bye);
    }
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
    return dlen > 0 && sock_.send_to(dgram, dlen, server_ep_) == int(dlen);
}

bool PersonalMonitorClient::request_meta() {
    MonitorControlPayload req{};
    req.op = MonitorControlOp::kSyncMetaReq;
    req.client_id = client_id_;
    return send_control(req);
}

bool PersonalMonitorClient::set_channel_mix(int ch, const PersonalMonitorChannelMix& mix) {
    if (ch < 0 || ch >= kMonitorMaxInputChannels) return false;
    MonitorControlPayload mc{};
    mc.op = MonitorControlOp::kSetChannelMix;
    mc.client_id = client_id_;
    mc.channel_index = uint8_t(ch);
    mc.flags = uint8_t((mix.mute ? 0x01u : 0u) | (mix.solo ? 0x02u : 0u));
    mc.gain_db = mix.gain_db;
    mc.pan = mix.pan;
    return send_control(mc);
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

void PersonalMonitorClient::poll() {
    if (!connected_.load(std::memory_order_acquire)) return;
    uint8_t buf[2048];
    Endpoint src{};
    const uint64_t now = now_us();
    for (int i = 0; i < 64; ++i) {
        const int n = sock_.recv_from(buf, sizeof(buf), src);
        if (n <= 0) break;
        PacketHeader h{};
        const uint8_t* payload = nullptr;
        if (parse_packet(buf, size_t(n), h, &payload) != PacketParseStatus::kOk) continue;
        if (h.type == PacketType::kAudio2) {
            audio_rx_pkts_.fetch_add(1, std::memory_order_relaxed);
            arx_.on_packet(h, payload, h.payload_size, now);
        } else if (h.type == PacketType::kMonitorControl) {
            uint32_t cid = 0;
            uint8_t nch = 0;
            MonitorMetaChannel tmp[kMonitorMaxInputChannels]{};
            if (parse_monitor_meta(payload, h.payload_size, cid, nch, tmp, kMonitorMaxInputChannels)) {
                std::lock_guard<std::mutex> lk(meta_mu_);
                meta_.assign(tmp, tmp + nch);
            }
        }
    }
}

void PersonalMonitorClient::pop_stereo(float* out_interleaved, uint32_t frames) {
    if (!out_interleaved || frames == 0) return;
    matrix_.pop_interleaved(mclock_, now_us(), out_interleaved, 2, frames);
}

std::vector<MonitorMetaChannel> PersonalMonitorClient::last_meta() const {
    std::lock_guard<std::mutex> lk(meta_mu_);
    return meta_;
}

}  // namespace lv
