#pragma once
// Bridges an audio device running at ANY rate (44.1 / 48 / 88.2 / 96 kHz ...) and its own crystal to the
// 48 kHz matrix ring. One implementation for CoreAudio, WASAPI and external ASIO, so the same tested code
// decides how many frames move per callback:
//
//   render():  48 kHz ring -> device frames      capture(): device frames -> 48 kHz ring
//
// * Rate conversion: StreamResampler (exact cross-block continuity: no frame is dropped or repeated
//   between callbacks, unlike per-block resampling that restarts its phase every buffer).
// * Never plays 48 kHz samples at the device rate: ratio = 48000 / device_rate (render) or
//   device_rate / 48000 (capture) - a 44.1 kHz device is resampled, not slowed down by 8.8 %.
// * Drift: a proportional, low-passed trim on the ring fill, hard-limited to +/-500 ppm
//   (ratio stays within 0.9995 .. 1.0005 of nominal), so the two clocks are tracked without ever
//   locking into a slow-down.
// * Starvation (render): the ring is primed to `target` frames before playback starts and after it runs
//   dry; missing audio becomes silence at the END of the block - zeros are never pushed into the
//   resampler as if they were audio (that inserted a gap AND delayed everything after it).
//
// Real-time safe: configure() allocates, render()/capture() do not.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio/planar_ring.hpp"
#include "audio/stream_resampler.hpp"

namespace lv {

class DeviceRateAdapter {
public:
    static constexpr double kMaxTrim = 500e-6;      // +/-500 ppm, never more
    static constexpr uint32_t kMatrixRate = 48000;

    // channels: slot channels; max_block: largest device callback (frames); mix_block: matrix block the
    // other side of the ring moves per cycle (target fill = mix_block + device block at 48 kHz + 32).
    void configure(int channels, uint32_t max_block, uint32_t mix_block) {
        ch_ = std::clamp(channels, 1, LockFreePlanarRing::kMaxChannels);
        max_block_ = std::max<uint32_t>(max_block, 64);
        const uint32_t cap = max_block_ * 6 + 64;  // handles low device rates (e.g. 8/16 kHz -> 48 kHz) + headroom
        rs_.configure(ch_, cap);
        tmp_.assign(size_t(ch_), std::vector<float>(cap, 0.0f));
        mix_block_.store(std::max<uint32_t>(mix_block, 32), std::memory_order_relaxed);
        seen_block_.store(0, std::memory_order_relaxed);
        trim_.store(0.0, std::memory_order_relaxed);
        primed_.store(false, std::memory_order_relaxed);
    }
    void set_mix_block(uint32_t frames) { mix_block_.store(std::max<uint32_t>(frames, 32), std::memory_order_relaxed); }

    // Fill (48 kHz frames) the drift loop steers the ring to.
    uint32_t target_fill(double dev_rate) const {
        const double dev48 = double(seen_block_.load(std::memory_order_relaxed)) * double(kMatrixRate) /
                             std::max(1000.0, dev_rate);
        const uint32_t want = mix_block_.load(std::memory_order_relaxed) + uint32_t(std::ceil(dev48)) + 32;
        return std::min<uint32_t>(want, kPlanarRingCap - 64);
    }

    // Device wants `n` frames at `dev_rate` -> out[c] (c < out_ch; null planes skipped). Always writes n
    // frames. Returns the frames that carried real audio (n unless the ring was starved).
    uint32_t render(LockFreePlanarRing& ring, float* const* out, int out_ch, uint32_t n, double dev_rate) {
        n = std::min(n, max_block_);
        if (n == 0) return 0;
        const uint32_t prev_seen = seen_block_.load(std::memory_order_relaxed);
        if (n > prev_seen) seen_block_.store(n, std::memory_order_relaxed);
        dev_rate = dev_rate > 1000.0 ? dev_rate : double(kMatrixRate);
        const uint32_t target = target_fill(dev_rate);
        const int oc = std::min(out_ch, ch_);
        if (!primed_.load(std::memory_order_relaxed)) {
            if (ring.available() < target) {
                silence(out, out_ch, 0, n);
                return 0;
            }
            primed_.store(true, std::memory_order_relaxed);
            rs_.reset();
        }
        // Fill above target -> the device clock is slower than the matrix: consume slightly faster.
        const double fill = double(ring.available()) + double(rs_.buffered());
        update_trim(fill, double(target), n, dev_rate);
        const double ratio = double(kMatrixRate) / dev_rate * (1.0 + trim_.load(std::memory_order_relaxed));
        const uint32_t need = rs_.needed_for(n, ratio);
        if (rs_.buffered() < need) {
            const uint32_t pull = std::min(need - rs_.buffered(), rs_.space());
            float* t[LockFreePlanarRing::kMaxChannels];
            for (int c = 0; c < ch_; ++c) t[c] = tmp_[size_t(c)].data();
            const uint32_t got = ring.read(t, uint32_t(ch_), pull, false);  // only real frames
            const float* ct[LockFreePlanarRing::kMaxChannels];
            for (int c = 0; c < ch_; ++c) ct[c] = t[c];
            rs_.push(ct, ch_, got);
        }
        const uint32_t made = rs_.produce(out, oc, n, ratio);
        for (int c = oc; c < out_ch; ++c)
            if (out && out[c]) std::memset(out[c], 0, size_t(n) * sizeof(float));
        if (made < n) {
            silence(out, oc, made, n);
            underruns_.fetch_add(1, std::memory_order_relaxed);
            primed_.store(false, std::memory_order_relaxed);  // refill before playing again (one clean gap, not a crackle per block)
        }
        return made;
    }

    // Device delivered `n` frames at `dev_rate` (in[c], c < in_ch; null = silence) -> 48 kHz ring.
    // Returns frames written to the ring.
    uint32_t capture(LockFreePlanarRing& ring, const float* const* in, int in_ch, uint32_t n, double dev_rate) {
        n = std::min(n, max_block_);
        if (n == 0) return 0;
        const uint32_t prev_seen = seen_block_.load(std::memory_order_relaxed);
        if (n > prev_seen) seen_block_.store(n, std::memory_order_relaxed);
        dev_rate = dev_rate > 1000.0 ? dev_rate : double(kMatrixRate);
        const float* src[LockFreePlanarRing::kMaxChannels];
        for (int c = 0; c < ch_; ++c) src[c] = (c < in_ch && in) ? in[c] : nullptr;
        rs_.push(src, ch_, n);
        // Fill above target -> the matrix reads slower than the device delivers: emit slightly less.
        update_trim(double(ring.available()), double(target_fill(dev_rate)), n, dev_rate);
        const double ratio = dev_rate / double(kMatrixRate) * (1.0 + trim_.load(std::memory_order_relaxed));
        float* t[LockFreePlanarRing::kMaxChannels];
        for (int c = 0; c < ch_; ++c) t[c] = tmp_[size_t(c)].data();
        const uint32_t cap = uint32_t(tmp_.empty() ? 0 : tmp_[0].size());
        const uint32_t made = rs_.produce(t, ch_, cap, ratio);
        const float* ct[LockFreePlanarRing::kMaxChannels];
        for (int c = 0; c < ch_; ++c) ct[c] = t[c];
        return made ? ring.write(ct, uint32_t(ch_), made) : 0;
    }

    double trim_ppm() const { return trim_.load(std::memory_order_relaxed) * 1e6; }
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    bool primed() const { return primed_.load(std::memory_order_relaxed); }

private:
    void update_trim(double fill, double target, uint32_t n, double dev_rate) {
        const double err = std::clamp((fill - target) / std::max(64.0, target), -1.0, 1.0);
        const double want = std::clamp(err * kMaxTrim, -kMaxTrim, kMaxTrim);
        const double alpha = std::min(1.0, double(n) / (dev_rate * 0.5));  // ~0.5 s low-pass
        const double cur = trim_.load(std::memory_order_relaxed);
        trim_.store(std::clamp(cur + (want - cur) * alpha, -kMaxTrim, kMaxTrim), std::memory_order_relaxed);
    }
    static void silence(float* const* out, int ch, uint32_t from, uint32_t to) {
        if (!out) return;
        for (int c = 0; c < ch; ++c)
            if (out[c] && to > from) std::memset(out[c] + from, 0, size_t(to - from) * sizeof(float));
    }

    int ch_ = 2;
    uint32_t max_block_ = 4096;
    std::atomic<uint32_t> seen_block_{0};
    std::atomic<uint32_t> mix_block_{256};
    StreamResampler rs_;
    std::vector<std::vector<float>> tmp_;
    std::atomic<double> trim_{0.0};
    std::atomic<bool> primed_{false};
    std::atomic<uint64_t> underruns_{0};
};

}  // namespace lv
