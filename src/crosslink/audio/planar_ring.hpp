#pragma once
// Lock-free SPSC multi-channel frame ring (interleaved [frame][ch], up to 64 ch x kPlanarRingCap frames).
// Whole multi-channel frames are published / consumed with one atomic store, so channels never skew.
//
// Priming (consumer side): read_primed() outputs silence until `prime` frames are queued, then plays;
// if the ring runs dry it outputs the frames it has, pads the rest with silence and re-primes. This turns
// a producer/consumer phase mismatch into one clean refill instead of a click on every block, and it
// never invents frames: silence is only ever emitted in place of audio that did not exist yet.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cl {

constexpr uint32_t kPlanarRingCap = 16384;  // power of two (~341 ms @ 48 kHz)

class LockFreePlanarRing {
public:
    static constexpr int kMaxChannels = 64;

    void configure(int channels) {
        channels_ = std::clamp(channels, 1, kMaxChannels);
        data_.assign(size_t(kPlanarRingCap) * size_t(channels_), 0.0f);
        wr_.store(0, std::memory_order_relaxed);
        rd_.store(0, std::memory_order_relaxed);
        xruns_.store(0, std::memory_order_relaxed);
        primed_ = false;
    }

    int channels() const { return channels_; }

    uint32_t available() const {
        const uint64_t w = wr_.load(std::memory_order_acquire);
        const uint64_t r = rd_.load(std::memory_order_acquire);
        return w > r ? uint32_t(std::min<uint64_t>(w - r, kPlanarRingCap)) : 0u;
    }

    uint32_t write(const float* const* planes, uint32_t ch, uint32_t frames) {
        if (data_.empty() || frames == 0) return 0;
        const uint64_t w = wr_.load(std::memory_order_relaxed);
        const uint64_t r = rd_.load(std::memory_order_acquire);
        const uint32_t free_n = uint32_t(kPlanarRingCap - std::min<uint64_t>(w - r, kPlanarRingCap));
        uint32_t n = frames;
        if (n > free_n) {
            n = free_n;
            xruns_.fetch_add(1, std::memory_order_relaxed);
        }
        const int copy_ch = std::min(int(ch), channels_);
        for (uint32_t i = 0; i < n; ++i) {
            float* frame_dst = data_.data() + size_t((w + i) & (kPlanarRingCap - 1)) * size_t(channels_);
            for (int c = 0; c < channels_; ++c) {
                frame_dst[c] = (c < copy_ch && planes && planes[c]) ? planes[c][i] : 0.0f;
            }
        }
        wr_.store(w + n, std::memory_order_release);  // publishes all channels of all n frames
        return n;
    }

    // Reads up to `frames` whole frames. With zero_pad the missing tail is silenced (and counted as an
    // underrun); without it the caller gets exactly what was available.
    uint32_t read(float* const* planes, uint32_t ch, uint32_t frames, bool zero_pad = true) {
        if (data_.empty() || frames == 0) return 0;
        const uint64_t w = wr_.load(std::memory_order_acquire);
        const uint64_t r = rd_.load(std::memory_order_relaxed);
        const uint32_t avail = w > r ? uint32_t(std::min<uint64_t>(w - r, kPlanarRingCap)) : 0u;
        const uint32_t n = std::min(frames, avail);
        if (zero_pad && n < frames) xruns_.fetch_add(1, std::memory_order_relaxed);
        const int out_ch = int(ch);
        for (uint32_t i = 0; i < n; ++i) {
            const float* frame_src = data_.data() + size_t((r + i) & (kPlanarRingCap - 1)) * size_t(channels_);
            for (int c = 0; c < out_ch; ++c) {
                if (planes && planes[c]) planes[c][i] = (c < channels_) ? frame_src[c] : 0.0f;
            }
        }
        if (zero_pad && n < frames) {
            for (int c = 0; c < out_ch; ++c) {
                if (planes && planes[c]) std::memset(planes[c] + n, 0, size_t(frames - n) * sizeof(float));
            }
        }
        rd_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer-side priming (see header). Always fills `frames` frames; returns the real frames played.
    uint32_t read_primed(float* const* planes, uint32_t ch, uint32_t frames, uint32_t prime) {
        if (!primed_) {
            if (available() < std::max(prime, frames)) {
                for (uint32_t c = 0; c < ch; ++c)
                    if (planes && planes[c]) std::memset(planes[c], 0, size_t(frames) * sizeof(float));
                return 0;
            }
            primed_ = true;
        }
        const uint32_t n = read(planes, ch, frames, true);
        if (n < frames) {
            primed_ = false;  // ran dry: refill to `prime` before playing again
            reprimes_.fetch_add(1, std::memory_order_relaxed);
        }
        return n;
    }
    bool primed() const { return primed_; }

    uint64_t xruns() const { return xruns_.load(std::memory_order_relaxed); }
    uint64_t reprimes() const { return reprimes_.load(std::memory_order_relaxed); }

private:
    int channels_ = 2;
    std::vector<float> data_;
    alignas(64) std::atomic<uint64_t> wr_{0};
    alignas(64) std::atomic<uint64_t> rd_{0};
    std::atomic<uint64_t> xruns_{0};
    std::atomic<uint64_t> reprimes_{0};
    bool primed_ = false;  // consumer-owned
};

}  // namespace cl
