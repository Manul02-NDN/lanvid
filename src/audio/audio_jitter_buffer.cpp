#include "audio/audio_jitter_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lv {

namespace {

inline int16_t f32_to_i16(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    const int x = int(std::lrintf(v * 32767.0f));
    return int16_t(std::clamp(x, -32768, 32767));
}

}  // namespace

AudioJitterBuffer::AudioJitterBuffer(uint32_t sample_rate, double target_ms)
    : sample_rate_(sample_rate ? sample_rate : kDefaultSampleRate) {
    target_ms = std::clamp(target_ms, 5.0, 40.0);
    target_frames_ = std::max<size_t>(120, size_t(double(sample_rate_) * target_ms / 1000.0));
    max_frames_ = std::min<size_t>(kCapacityFrames - 256, target_frames_ * 3);
    drift_band_ = std::max<size_t>(48, size_t(double(sample_rate_) * 0.003));  // +/- 3 ms
    avg_fill_ = double(target_frames_);
}

void AudioJitterBuffer::reset() {
    write_pos_.store(0, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
    has_seq_ = false;
    expected_seq_ = 0;
    prebuffering_ = true;
    avg_fill_ = double(target_frames_);
    last_l_ = last_r_ = 0;
    drift_cooldown_ = 0;
}

size_t AudioJitterBuffer::available_frames() const {
    const uint64_t wp = write_pos_.load(std::memory_order_acquire);
    const uint64_t rp = read_pos_.load(std::memory_order_acquire);
    return wp >= rp ? size_t(wp - rp) : 0;
}

double AudioJitterBuffer::buffered_ms() const {
    return 1000.0 * double(available_frames()) / double(sample_rate_);
}

void AudioJitterBuffer::push_pcm16(const int16_t* stereo_frames, size_t num_frames) {
    if (!stereo_frames || num_frames == 0) return;
    uint64_t wp = write_pos_.load(std::memory_order_relaxed);
    const uint64_t rp = read_pos_.load(std::memory_order_acquire);
    const size_t used = wp >= rp ? size_t(wp - rp) : 0;
    const size_t free_space = kCapacityFrames > used ? (kCapacityFrames - used - 1) : 0;

    if (num_frames > free_space) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
        num_frames = free_space;
        if (num_frames == 0) return;
    }

    for (size_t i = 0; i < num_frames; ++i) {
        const size_t idx = size_t(wp & kCapacityMask) * 2;
        ring_[idx + 0] = stereo_frames[i * 2 + 0];
        ring_[idx + 1] = stereo_frames[i * 2 + 1];
        ++wp;
    }
    write_pos_.store(wp, std::memory_order_release);
}

void AudioJitterBuffer::push_packet(uint32_t audio_seq_id, const AudioHeader& hdr,
                                    const uint8_t* pcm_data, size_t dgram_bytes) {
    packets_rx_.fetch_add(1, std::memory_order_relaxed);
    bytes_rx_.fetch_add(dgram_bytes, std::memory_order_relaxed);

    if (has_seq_) {
        const int32_t gap = frame_diff(audio_seq_id, expected_seq_);
        if (gap < 0) return;  // late / duplicate audio packet -> ignore
        if (gap > 0) {
            packets_lost_.fetch_add(uint64_t(gap), std::memory_order_relaxed);
        }
        expected_seq_ = audio_seq_id + 1;
    } else {
        has_seq_ = true;
        expected_seq_ = audio_seq_id + 1;
    }

    const size_t n = hdr.num_frames;
    if (n == 0 || !pcm_data) return;

    uint64_t wp = write_pos_.load(std::memory_order_relaxed);
    const uint64_t rp = read_pos_.load(std::memory_order_acquire);
    const size_t used = wp >= rp ? size_t(wp - rp) : 0;
    const size_t free_space = kCapacityFrames > used ? (kCapacityFrames - used - 1) : 0;
    size_t to_write = n;
    if (to_write > free_space) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
        to_write = free_space;
        if (to_write == 0) return;
    }

    if (hdr.sample_format == AudioSampleFormat::kPcm16) {
        const int16_t* s = reinterpret_cast<const int16_t*>(pcm_data);
        if (hdr.channels == 2) {
            for (size_t i = 0; i < to_write; ++i) {
                const size_t idx = size_t(wp & kCapacityMask) * 2;
                ring_[idx + 0] = s[i * 2 + 0];
                ring_[idx + 1] = s[i * 2 + 1];
                ++wp;
            }
        } else {
            for (size_t i = 0; i < to_write; ++i) {
                const size_t idx = size_t(wp & kCapacityMask) * 2;
                ring_[idx + 0] = s[i];
                ring_[idx + 1] = s[i];
                ++wp;
            }
        }
    } else {
        // Float32 -> PCM16
        const float* s = reinterpret_cast<const float*>(pcm_data);
        if (hdr.channels == 2) {
            for (size_t i = 0; i < to_write; ++i) {
                const size_t idx = size_t(wp & kCapacityMask) * 2;
                ring_[idx + 0] = f32_to_i16(s[i * 2 + 0]);
                ring_[idx + 1] = f32_to_i16(s[i * 2 + 1]);
                ++wp;
            }
        } else {
            for (size_t i = 0; i < to_write; ++i) {
                const int16_t v = f32_to_i16(s[i]);
                const size_t idx = size_t(wp & kCapacityMask) * 2;
                ring_[idx + 0] = v;
                ring_[idx + 1] = v;
                ++wp;
            }
        }
    }
    write_pos_.store(wp, std::memory_order_release);
}

void AudioJitterBuffer::set_volume(float vol) {
    volume_.store(std::clamp(vol, 0.0f, 2.0f), std::memory_order_relaxed);
}

void AudioJitterBuffer::pop_pcm16(int16_t* out_stereo, size_t num_frames) {
    if (!out_stereo || num_frames == 0) return;

    uint64_t rp = read_pos_.load(std::memory_order_relaxed);
    const uint64_t wp = write_pos_.load(std::memory_order_acquire);
    size_t avail = wp >= rp ? size_t(wp - rp) : 0;

    if (prebuffering_) {
        if (avail < target_frames_) {
            std::memset(out_stereo, 0, num_frames * 4);
            return;
        }
        prebuffering_ = false;
        avg_fill_ = double(avail);
    }

    // Hard trim if buffer grew beyond max_frames_ (bounds worst-case audio latency).
    if (avail > max_frames_) {
        const size_t skip = avail - target_frames_;
        rp += skip;
        avail = target_frames_;
        avg_fill_ = double(target_frames_);
        overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    if (avail == 0) {
        // Underrun: gentle fade-out of last sample to avoid audible click, then re-prebuffer.
        underruns_.fetch_add(1, std::memory_order_relaxed);
        const size_t fade = std::min<size_t>(num_frames, 48);
        for (size_t i = 0; i < fade; ++i) {
            const int scale = int(fade - i);
            out_stereo[i * 2 + 0] = int16_t((int(last_l_) * scale) / int(fade));
            out_stereo[i * 2 + 1] = int16_t((int(last_r_) * scale) / int(fade));
        }
        if (num_frames > fade) std::memset(out_stereo + fade * 2, 0, (num_frames - fade) * 4);
        last_l_ = last_r_ = 0;
        prebuffering_ = true;
        read_pos_.store(rp, std::memory_order_release);
        return;
    }

    // Update smoothed fill level (EWMA) for gentle clock-drift compensation.
    avg_fill_ += (double(avail) - avg_fill_) * 0.10;

    // Decide whether to drop 1 frame (buffer creeping up) or duplicate 1 frame (buffer creeping down).
    int drift_action = 0;  // +1 = drop 1 input frame, -1 = duplicate 1 output frame
    if (drift_cooldown_ > 0) {
        --drift_cooldown_;
    } else if (avg_fill_ > double(target_frames_ + drift_band_) && avail > num_frames + 1) {
        drift_action = +1;
        drift_cooldown_ = 2;
    } else if (avg_fill_ < double(target_frames_ > drift_band_ ? target_frames_ - drift_band_ : 0) &&
               avail >= 2) {
        drift_action = -1;
        drift_cooldown_ = 2;
    }

    size_t out_i = 0;
    const size_t dup_at = (drift_action == -1) ? (num_frames / 2) : size_t(-1);
    const size_t drop_at = (drift_action == +1) ? (num_frames / 2) : size_t(-1);

    while (out_i < num_frames && avail > 0) {
        if (out_i == drop_at && avail > 1) {
            // Skip 1 frame smoothly (average with next frame to minimize phase step).
            ++rp;
            --avail;
            drift_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        const size_t idx = size_t(rp & kCapacityMask) * 2;
        last_l_ = ring_[idx + 0];
        last_r_ = ring_[idx + 1];
        out_stereo[out_i * 2 + 0] = last_l_;
        out_stereo[out_i * 2 + 1] = last_r_;
        ++out_i;
        ++rp;
        --avail;

        if (out_i == dup_at + 1 && out_i < num_frames) {
            out_stereo[out_i * 2 + 0] = last_l_;
            out_stereo[out_i * 2 + 1] = last_r_;
            ++out_i;
            drift_dups_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Partial underrun tail -> fade smoothly to zero.
    if (out_i < num_frames) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
        const size_t rem = num_frames - out_i;
        const size_t fade = std::min<size_t>(rem, 32);
        for (size_t i = 0; i < fade; ++i) {
            const int scale = int(fade - i);
            out_stereo[(out_i + i) * 2 + 0] = int16_t((int(last_l_) * scale) / int(fade));
            out_stereo[(out_i + i) * 2 + 1] = int16_t((int(last_r_) * scale) / int(fade));
        }
        if (rem > fade) std::memset(out_stereo + (out_i + fade) * 2, 0, (rem - fade) * 4);
        last_l_ = last_r_ = 0;
        prebuffering_ = true;
    }

    read_pos_.store(rp, std::memory_order_release);

    const float vol = volume_.load(std::memory_order_relaxed);
    if (vol <= 0.0001f) {
        std::memset(out_stereo, 0, num_frames * 4);
    } else if (std::fabs(vol - 1.0f) > 0.001f) {
        const int32_t gain_q14 = int32_t(std::lrintf(vol * 16384.0f));
        const size_t total = num_frames * 2;
        for (size_t i = 0; i < total; ++i) {
            const int32_t scaled = (int32_t(out_stereo[i]) * gain_q14) >> 14;
            out_stereo[i] = int16_t(std::clamp(scaled, -32768, 32767));
        }
    }
}

AudioStats AudioJitterBuffer::stats() const {
    return {packets_rx_.load(std::memory_order_relaxed),
            packets_lost_.load(std::memory_order_relaxed),
            bytes_rx_.load(std::memory_order_relaxed),
            underruns_.load(std::memory_order_relaxed),
            overruns_.load(std::memory_order_relaxed),
            drift_drops_.load(std::memory_order_relaxed),
            drift_dups_.load(std::memory_order_relaxed),
            buffered_ms()};
}

}  // namespace lv
