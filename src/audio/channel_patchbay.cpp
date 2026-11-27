#include "audio/channel_patchbay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lv {

ChannelPatchbay::ChannelPatchbay(uint32_t sample_rate) {
    rate_.store(sample_rate ? sample_rate : 48000);
    for (int c = 0; c < kChannels; ++c) {
        dst_[c].store(int8_t(c));
        mute_[c].store(false);
        solo_[c].store(false);
        cur_[c] = prev_[c] = int8_t(c);
        gain_[c] = 1.0f;
        gain_pub_[c].store(1.0f);
    }
    seen_version_ = version_.load();
}

void ChannelPatchbay::set_sample_rate(uint32_t rate) { rate_.store(rate ? rate : 48000); }

void ChannelPatchbay::reset_identity() {
    for (int c = 0; c < kChannels; ++c) dst_[c].store(int8_t(c), std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_release);
}

void ChannelPatchbay::set_routing(const int8_t* dst_of_src) {
    if (!dst_of_src) return;
    for (int c = 0; c < kChannels; ++c) {
        const int8_t d = dst_of_src[c];
        dst_[c].store((d >= 0 && d < kChannels) ? d : int8_t(-1), std::memory_order_relaxed);
    }
    version_.fetch_add(1, std::memory_order_release);
}

void ChannelPatchbay::set_route(int src, int dst) {
    if (src < 0 || src >= kChannels) return;
    dst_[src].store((dst >= 0 && dst < kChannels) ? int8_t(dst) : int8_t(-1), std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_release);
}

void ChannelPatchbay::get_routing(int8_t* out) const {
    if (!out) return;
    for (int c = 0; c < kChannels; ++c) out[c] = dst_[c].load(std::memory_order_relaxed);
}

void ChannelPatchbay::set_mute(int ch, bool on) {
    if (ch >= 0 && ch < kChannels) mute_[ch].store(on, std::memory_order_relaxed);
}

void ChannelPatchbay::set_solo(int ch, bool on) {
    if (ch < 0 || ch >= kChannels) return;
    if (solo_[ch].exchange(on, std::memory_order_acq_rel) != on) {
        solo_count_.fetch_add(on ? 1 : -1, std::memory_order_acq_rel);
    }
}

bool ChannelPatchbay::muted(int ch) const { return ch >= 0 && ch < kChannels && mute_[ch].load(std::memory_order_relaxed); }
bool ChannelPatchbay::soloed(int ch) const { return ch >= 0 && ch < kChannels && solo_[ch].load(std::memory_order_acquire); }

void ChannelPatchbay::clear_mute_solo() {
    for (int c = 0; c < kChannels; ++c) {
        mute_[c].store(false, std::memory_order_relaxed);
        set_solo(c, false);
    }
}

float ChannelPatchbay::current_gain(int ch) const {
    return (ch >= 0 && ch < kChannels) ? gain_pub_[ch].load(std::memory_order_relaxed) : 0.0f;
}

float ChannelPatchbay::target_gain(int ch, bool any_solo) const {
    if (mute_[ch].load(std::memory_order_relaxed)) return 0.0f;
    if (any_solo && !solo_[ch].load(std::memory_order_acquire)) return 0.0f;
    return 1.0f;
}

void ChannelPatchbay::process(const float* const* in, float* const* out, uint32_t channels, uint32_t frames) {
    if (!out || frames == 0) return;
    channels = std::min<uint32_t>(channels, kChannels);
    const uint32_t rate = rate_.load(std::memory_order_relaxed);
    const uint32_t ramp = std::max<uint32_t>(1, uint32_t(std::lround(kRampMs * rate / 1000.0)));
    const float step = 1.0f / float(ramp);

    // Pick up a new routing table -> crossfade from the table in use.
    const uint32_t v = version_.load(std::memory_order_acquire);
    if (v != seen_version_) {
        seen_version_ = v;
        std::memcpy(prev_, cur_, sizeof(cur_));
        for (int c = 0; c < kChannels; ++c) cur_[c] = dst_[c].load(std::memory_order_relaxed);
        xfade_len_ = ramp;
        xfade_left_ = std::memcmp(prev_, cur_, sizeof(cur_)) ? ramp : 0;
    }

    for (uint32_t c = 0; c < channels; ++c) {
        if (out[c]) std::memset(out[c], 0, size_t(frames) * sizeof(float));
    }
    const bool any_solo = solo_count_.load(std::memory_order_acquire) > 0;

    for (uint32_t s = 0; s < channels; ++s) {
        const float tgt = target_gain(int(s), any_solo);
        const float* src = in ? in[s] : nullptr;
        const int dn = cur_[s];
        const int dp = prev_[s];
        float g = gain_[s];
        uint32_t xl = xfade_left_;
        const bool steady = (g == tgt) && xl == 0;
        if (steady) {
            // Fast path: unity / silence, no ramps (bit-exact copy for g == 1).
            if (g != 0.0f && src && dn >= 0 && uint32_t(dn) < channels && out[dn]) {
                float* d = out[dn];
                if (g == 1.0f) for (uint32_t i = 0; i < frames; ++i) d[i] += src[i];
                else for (uint32_t i = 0; i < frames; ++i) d[i] += src[i] * g;
            }
            continue;
        }
        float* d_new = (dn >= 0 && uint32_t(dn) < channels) ? out[dn] : nullptr;
        float* d_prev = (dp >= 0 && uint32_t(dp) < channels) ? out[dp] : nullptr;
        for (uint32_t i = 0; i < frames; ++i) {
            if (g < tgt) g = std::min(tgt, g + step);
            else if (g > tgt) g = std::max(tgt, g - step);
            const float x = src ? src[i] * g : 0.0f;
            float w_new = 1.0f;
            if (xl > 0) {
                w_new = 1.0f - float(xl) / float(xfade_len_);
                --xl;
            }
            if (d_new) d_new[i] += x * w_new;
            if (w_new < 1.0f && d_prev) d_prev[i] += x * (1.0f - w_new);
        }
        gain_[s] = g;
        gain_pub_[s].store(g, std::memory_order_relaxed);
    }
    // Keep ramps of channels beyond `channels` consistent.
    xfade_left_ = xfade_left_ > frames ? xfade_left_ - frames : 0;
}

}  // namespace lv
