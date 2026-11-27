#pragma once
// Reliability profile derived from the user's target latency (2..2000 ms).
//
//   2 .. 10 ms   Ultra-Low-Latency : 0.5 / 1 ms chunks, no ARQ, XOR FEC 1+1 or 2+1
//   10 .. 100 ms Balanced / Live   : 2.5 / 5 ms chunks, XOR FEC 4+1, one fast NACK
//   100 .. 2000  High-Reliability  : 10 / 20 ms chunks, interleaved XOR FEC, 5..12 NACK rounds,
//                                    proactive duplication when measured loss exceeds 5 %
//
// FEC groups never span more than latency/3 so that parity still arrives inside the budget.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "net/packet.hpp"

namespace cl {

enum class AudioReliabilityMode : uint8_t { kUltraLow = 0, kBalanced = 1, kHighReliability = 2 };

struct AudioReliabilityProfile {
    AudioReliabilityMode mode = AudioReliabilityMode::kBalanced;
    uint32_t latency_ms = 20;
    uint32_t chunk_samples = 240;
    uint8_t fec_k = 4;           // data packets per parity (>= 1; k = 1 is a plain duplicate)
    uint8_t fec_interleave = 1;  // stride between group members
    uint8_t nack_attempts = 1;
    bool allow_duplication = false;
    uint32_t history_ms = 300;   // sender retransmit history depth
    uint32_t nack_first_wait_ms = 2;
    uint32_t nack_spacing_ms = 10;
};

inline const char* audio_mode_name(AudioReliabilityMode m) {
    switch (m) {
        case AudioReliabilityMode::kUltraLow: return "ultra-low";
        case AudioReliabilityMode::kBalanced: return "balanced";
        default: return "high-reliability";
    }
}

inline AudioReliabilityProfile audio_profile_for_latency(uint32_t latency_ms, uint32_t sample_rate,
                                                         size_t mtu, AudioFormat2 fmt) {
    AudioReliabilityProfile p;
    latency_ms = std::clamp<uint32_t>(latency_ms, 2, 2000);
    p.latency_ms = latency_ms;

    double chunk_ms;
    if (latency_ms <= 4) chunk_ms = 0.5;
    else if (latency_ms <= 10) chunk_ms = 1.0;
    else if (latency_ms <= 40) chunk_ms = 2.5;
    else if (latency_ms <= 100) chunk_ms = 5.0;
    else if (latency_ms <= 500) chunk_ms = 10.0;
    else chunk_ms = 20.0;

    p.mode = latency_ms <= 10 ? AudioReliabilityMode::kUltraLow
             : latency_ms <= 100 ? AudioReliabilityMode::kBalanced
                                 : AudioReliabilityMode::kHighReliability;

    uint32_t chunk = uint32_t(chunk_ms * sample_rate / 1000.0 + 0.5);
    // Packet = 28 B CrossLink + 24 B AudioHeader2 (+4 B FEC info on parity) + samples; keep within MTU.
    const size_t overhead = kPacketHeaderSize + kAudioHeader2Size + kAudioFecInfoSize;
    const size_t bps = audio2_bytes_per_sample(fmt);
    if (mtu > overhead + bps) {
        const uint32_t max_samples = uint32_t((mtu - overhead) / bps);
        chunk = std::min(chunk, max_samples);
    }
    p.chunk_samples = std::max<uint32_t>(chunk, 8);
    const double real_chunk_ms = 1000.0 * p.chunk_samples / sample_rate;

    const double span_ms = latency_ms / 3.0;
    const uint32_t kmax = p.mode == AudioReliabilityMode::kUltraLow ? 2 : 4;
    const uint32_t dmax = p.mode == AudioReliabilityMode::kHighReliability ? 4 : 1;
    uint32_t k = uint32_t(span_ms / real_chunk_ms);
    k = std::clamp<uint32_t>(k, 1, kmax);
    uint32_t d = uint32_t(span_ms / (real_chunk_ms * k));
    d = std::clamp<uint32_t>(d, 1, dmax);
    p.fec_k = uint8_t(k);
    p.fec_interleave = uint8_t(d);

    switch (p.mode) {
        case AudioReliabilityMode::kUltraLow: p.nack_attempts = 0; break;
        case AudioReliabilityMode::kBalanced: p.nack_attempts = 1; break;
        default: p.nack_attempts = uint8_t(std::clamp<uint32_t>(latency_ms / 100, 5, 12)); break;
    }
    p.allow_duplication = p.mode == AudioReliabilityMode::kHighReliability;
    p.history_ms = std::min<uint32_t>(2500, latency_ms + 250);
    p.nack_first_wait_ms = std::clamp<uint32_t>(uint32_t(latency_ms * 0.1), 2, 100);
    p.nack_spacing_ms = p.nack_attempts > 1
                            ? std::max<uint32_t>(5, uint32_t(latency_ms * 0.6 / p.nack_attempts))
                            : 10;
    return p;
}

}  // namespace cl
