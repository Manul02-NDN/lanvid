#pragma once
// Streaming planar linear resampler with exact cross-block continuity (no frames dropped or
// duplicated between calls). Used by the CoreAudio matrix backend to convert any device rate
// (44.1 / 88.2 / 96 / ... kHz) to and from the 48 kHz matrix clock, with a small drift trim.
//
//   push()    appends source-rate frames
//   produce() emits destination-rate frames while enough source history is buffered
//
// ratio = src_rate / dst_rate (source frames consumed per output frame). All storage is reserved in
// configure(); push/produce never allocate (RT-safe).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lv {

class StreamResampler {
public:
    void configure(int channels, uint32_t max_frames) {
        ch_ = std::max(1, channels);
        cap_ = std::max<uint32_t>(max_frames, 64) + 4;
        buf_.assign(size_t(ch_) * cap_, 0.0f);
        len_ = 0;
        pos_ = 0.0;
    }
    void reset() {
        len_ = 0;
        pos_ = 0.0;
    }
    int channels() const { return ch_; }
    uint32_t buffered() const { return len_; }
    uint32_t space() const { return cap_ - len_; }

    // Frames that must be buffered so that produce(n, ratio) can emit n frames.
    uint32_t needed_for(uint32_t n, double ratio) const {
        if (n == 0) return 0;
        const double last = pos_ + double(n - 1) * ratio;
        return uint32_t(last) + 2;
    }

    // Appends planar frames (planes[c] may be null -> silence). Returns frames accepted.
    uint32_t push(const float* const* planes, int channels, uint32_t frames) {
        const uint32_t n = std::min(frames, space());
        for (int c = 0; c < ch_; ++c) {
            float* dst = buf_.data() + size_t(c) * cap_ + len_;
            const float* src = (c < channels && planes) ? planes[c] : nullptr;
            if (src) std::memcpy(dst, src, size_t(n) * sizeof(float));
            else std::memset(dst, 0, size_t(n) * sizeof(float));
        }
        len_ += n;
        return n;
    }

    // Appends interleaved frames with `stride` floats per frame, taking channels [0, channels).
    uint32_t push_interleaved(const float* data, uint32_t stride, int channels, uint32_t frames) {
        const uint32_t n = std::min(frames, space());
        for (int c = 0; c < ch_; ++c) {
            float* dst = buf_.data() + size_t(c) * cap_ + len_;
            if (data && c < channels && uint32_t(c) < stride) {
                for (uint32_t i = 0; i < n; ++i) dst[i] = data[size_t(i) * stride + c];
            } else {
                std::memset(dst, 0, size_t(n) * sizeof(float));
            }
        }
        len_ += n;
        return n;
    }

    // Emits up to `max_out` frames into out[c] (c < channels; null entries skipped). Returns count.
    uint32_t produce(float* const* out, int channels, uint32_t max_out, double ratio) {
        const double pos0 = pos_;
        uint32_t k = 0;
        while (k < max_out) {
            const double p = pos0 + double(k) * ratio;
            const uint32_t i0 = uint32_t(p);
            if (i0 + 1 >= len_) break;  // need one sample of look-ahead
            const float frac = float(p - double(i0));
            for (int c = 0; c < channels && c < ch_; ++c) {
                if (!out || !out[c]) continue;
                const float* s = buf_.data() + size_t(c) * cap_;
                out[c][k] = s[i0] + (s[i0 + 1] - s[i0]) * frac;
            }
            ++k;
        }
        pos_ = pos0 + double(k) * ratio;
        // Drop fully consumed source frames, keep the rest (exact continuity).
        const uint32_t drop = std::min<uint32_t>(uint32_t(pos_), len_);
        if (drop) {
            for (int c = 0; c < ch_; ++c) {
                float* s = buf_.data() + size_t(c) * cap_;
                std::memmove(s, s + drop, size_t(len_ - drop) * sizeof(float));
            }
            len_ -= drop;
            pos_ -= double(drop);
        }
        return k;
    }

private:
    int ch_ = 1;
    uint32_t cap_ = 0;
    std::vector<float> buf_;
    uint32_t len_ = 0;
    double pos_ = 0.0;
};

}  // namespace lv
