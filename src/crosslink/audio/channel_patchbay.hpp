#pragma once
// 64x64 channel patchbay with per-channel Mute / Solo for the AoIP bus.
//
//   out[dst_of_src[s]] += in[s] * gain[s]      (several sources may feed one destination = mix)
//
// * Routing (src -> dst, -1 = unrouted) can be changed from any thread at any time; the RT side
//   picks the new table up at the next block and crossfades old -> new routes over 5 ms.
// * Mute / Solo act on the SOURCE channel (pre-routing): target gain is 0 when muted, or when any
//   channel is soloed and this one is not. Gains ramp linearly over 5 ms (no clicks).
// * In steady state (identity, gain 1, no ramp) the output is a bit-exact copy of the input.
// RT-safe: process() uses only atomics and arithmetic (no locks / allocation).

#include <atomic>
#include <cstdint>

namespace cl {

class ChannelPatchbay {
public:
    static constexpr int kChannels = 64;
    static constexpr double kRampMs = 5.0;

    explicit ChannelPatchbay(uint32_t sample_rate = 48000);

    void set_sample_rate(uint32_t rate);

    // ---- control (any thread) ----
    void reset_identity();
    // dst_of_src[s] in [-1, 63]; values out of range are treated as -1 (unrouted).
    void set_routing(const int8_t* dst_of_src);
    void set_route(int src, int dst);
    void get_routing(int8_t* dst_of_src) const;
    void set_mute(int ch, bool on);
    void set_solo(int ch, bool on);
    bool muted(int ch) const;
    bool soloed(int ch) const;
    void clear_mute_solo();

    // ---- RT (single audio/pump thread) ----
    // in[c] may be null (silence) for c < channels; out[c] must hold `frames` floats for c < channels.
    void process(const float* const* in, float* const* out, uint32_t channels, uint32_t frames);

    // Current (ramped) gain of a source channel, for meters / tests.
    float current_gain(int ch) const;

private:
    float target_gain(int ch, bool any_solo) const;

    std::atomic<uint32_t> rate_{48000};
    std::atomic<int8_t> dst_[kChannels];
    std::atomic<uint32_t> version_{1};
    std::atomic<bool> mute_[kChannels];
    std::atomic<bool> solo_[kChannels];
    std::atomic<int> solo_count_{0};

    // RT state
    uint32_t seen_version_ = 0;
    int8_t cur_[kChannels];
    int8_t prev_[kChannels];
    uint32_t xfade_left_ = 0;
    uint32_t xfade_len_ = 0;
    float gain_[kChannels];
    std::atomic<float> gain_pub_[kChannels];
};

}  // namespace cl
