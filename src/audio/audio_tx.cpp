#include "audio/audio_tx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef LV_HAVE_OPUS
#include <opus.h>
#endif

#include "common/clock.hpp"

namespace lv {

namespace {

constexpr int kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr int kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// Encodes `n` float32 samples into a self-contained IMA-ADPCM 4-bit block (4B header + ceil((n-1)/2) B).
void encode_float32_to_adpcm4(const float* in, uint32_t n, std::vector<uint8_t>& out) {
    out.assign(audio2_payload_bytes(AudioFormat2::kAdpcm4, n), 0);
    if (n == 0) return;
    auto to_i16 = [](float f) -> int {
        const float c = std::clamp(f, -1.0f, 1.0f);
        return std::clamp<int>(int(std::lrint(double(c) * 32767.0)), -32768, 32767);
    };
    int pred = to_i16(in[0]);
    int idx = 0;
    out[0] = uint8_t(pred & 0xFF);
    out[1] = uint8_t((pred >> 8) & 0xFF);
    out[2] = uint8_t(idx);
    out[3] = 0;
    for (uint32_t i = 1; i < n; ++i) {
        const int target = to_i16(in[i]);
        int diff = target - pred;
        uint8_t code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        int step = kImaStepTable[idx];
        int vpdiff = step >> 3;
        if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 1; vpdiff += step; }
        pred = std::clamp(pred + ((code & 8) ? -vpdiff : vpdiff), -32768, 32767);
        idx = std::clamp(idx + kImaIndexTable[code], 0, 88);
        const size_t byte_i = 4 + (i - 1) / 2;
        if (((i - 1) & 1u) == 0u) out[byte_i] = code & 0x0Fu;
        else out[byte_i] |= uint8_t((code & 0x0Fu) << 4);
    }
}

constexpr int kImaIndexTable5[16] = {-1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 3, 4, 5, 6, 7, 8};

// Encodes `n` float32 samples into a self-contained 5-bit IMA-ADPCM block (4B header + ceil((n-1)*5/8) B).
void encode_float32_to_adpcm5(const float* in, uint32_t n, std::vector<uint8_t>& out) {
    out.assign(4u + (n > 1 ? (size_t(n - 1) * 5u + 7u) / 8u : 0u), 0);
    if (n == 0) return;
    auto to_i16 = [](float f) -> int {
        const float c = std::clamp(f, -1.0f, 1.0f);
        return std::clamp<int>(int(std::lrint(double(c) * 32767.0)), -32768, 32767);
    };
    int pred = to_i16(in[0]);
    int idx = 0;
    out[0] = uint8_t(pred & 0xFF);
    out[1] = uint8_t((pred >> 8) & 0xFF);
    out[2] = uint8_t(idx);
    out[3] = 0;
    size_t bit_pos = 0;
    for (uint32_t i = 1; i < n; ++i) {
        const int target = to_i16(in[i]);
        int diff = target - pred;
        uint8_t code = 0;
        if (diff < 0) { code = 16; diff = -diff; }
        int step = kImaStepTable[idx];
        int vpdiff = step >> 4;
        if (diff >= step) { code |= 8; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 1; vpdiff += step; }
        pred = std::clamp(pred + ((code & 16) ? -vpdiff : vpdiff), -32768, 32767);
        idx = std::clamp(idx + kImaIndexTable5[code & 15], 0, 88);
        const size_t byte_i = 4 + (bit_pos >> 3);
        const unsigned shift = unsigned(bit_pos & 7u);
        out[byte_i] |= uint8_t((code << shift) & 0xFFu);
        if (shift > 3u && byte_i + 1 < out.size()) {
            out[byte_i + 1] |= uint8_t(code >> (8u - shift));
        }
        bit_pos += 5;
    }
}

void decimate_box(const float* in, uint32_t n, uint32_t factor, std::vector<float>& dec) {
    const uint32_t m = (n + factor - 1u) / factor;
    dec.resize(m);
    for (uint32_t k = 0; k < m; ++k) {
        const uint32_t s0 = k * factor;
        const uint32_t s1 = std::min(n, s0 + factor);
        float sum = 0.0f;
        for (uint32_t j = s0; j < s1; ++j) sum += in[j];
        dec[k] = sum / float(std::max<uint32_t>(1u, s1 - s0));
    }
}

constexpr uint32_t kOpusFrameSamples = 120;  // 2.5 ms @ 48 kHz (CELT Restricted Low-Delay)

int opus_bitrate_bps_for_profile(AudioCodecProfile prof) {
    switch (prof) {
        case AudioCodecProfile::kBitrate256: return 256000;
        case AudioCodecProfile::kOpusAdpcm:
        case AudioCodecProfile::kBitrate192: return 192000;
        case AudioCodecProfile::kBitrate128: return 128000;
        case AudioCodecProfile::kBitrate96:  return 96000;
        case AudioCodecProfile::kBitrate64:  return 64000;
        default: return 0;
    }
}

void encode_float32_wire(AudioCodecProfile prof, const float* in, uint32_t n,
                         AudioFormat2& out_fmt, std::vector<uint8_t>& out) {
    switch (prof) {
        case AudioCodecProfile::kPcm24: {
            out_fmt = AudioFormat2::kPcm24Packed;
            out.resize(size_t(n) * 3);
            for (uint32_t i = 0; i < n; ++i) {
                const float c = std::clamp(in[i], -1.0f, 1.0f);
                const int32_t q = std::clamp<int32_t>(int32_t(std::lrint(double(c) * 8388607.0)), -8388608, 8388607);
                const uint32_t u = uint32_t(q) & 0xFFFFFFu;
                out[3 * i + 0] = uint8_t(u & 0xFFu);
                out[3 * i + 1] = uint8_t((u >> 8) & 0xFFu);
                out[3 * i + 2] = uint8_t((u >> 16) & 0xFFu);
            }
            return;
        }
        case AudioCodecProfile::kPcm16: {
            out_fmt = AudioFormat2::kPcm16;
            out.resize(size_t(n) * 2);
            for (uint32_t i = 0; i < n; ++i) {
                const float c = std::clamp(in[i], -1.0f, 1.0f);
                const int16_t q = int16_t(std::clamp<int>(int(std::lrint(double(c) * 32767.0)), -32768, 32767));
                out[2 * i + 0] = uint8_t(q & 0xFF);
                out[2 * i + 1] = uint8_t((q >> 8) & 0xFF);
            }
            return;
        }
        case AudioCodecProfile::kOpusAdpcm:
        case AudioCodecProfile::kBitrate192: {
            out_fmt = AudioFormat2::kAdpcm4;
            encode_float32_to_adpcm4(in, n, out);
            return;
        }
        case AudioCodecProfile::kBitrate256: {
            out_fmt = AudioFormat2::kAdpcm5;
            encode_float32_to_adpcm5(in, n, out);
            return;
        }
        case AudioCodecProfile::kBitrate128: {
            out_fmt = AudioFormat2::kAdpcm128;
            static thread_local std::vector<float> dec;
            decimate_box(in, n, 2u, dec);
            encode_float32_to_adpcm5(dec.data(), uint32_t(dec.size()), out);
            return;
        }
        case AudioCodecProfile::kBitrate96: {
            out_fmt = AudioFormat2::kAdpcm96;
            static thread_local std::vector<float> dec;
            decimate_box(in, n, 2u, dec);
            encode_float32_to_adpcm4(dec.data(), uint32_t(dec.size()), out);
            return;
        }
        case AudioCodecProfile::kBitrate64: {
            out_fmt = AudioFormat2::kAdpcm64;
            static thread_local std::vector<float> dec;
            decimate_box(in, n, 3u, dec);
            encode_float32_to_adpcm4(dec.data(), uint32_t(dec.size()), out);
            return;
        }
        case AudioCodecProfile::kPcm32F:
        default: {
            out_fmt = AudioFormat2::kFloat32;
            out.resize(size_t(n) * 4);
            std::memcpy(out.data(), in, size_t(n) * 4);
            return;
        }
    }
}

}  // namespace

struct AudioTxEngine::Stream {
    std::mutex mu;
    uint32_t seq = 0;  // next sequence number

    // chunk staging
    std::vector<uint8_t> stage;
    uint32_t stage_samples = 0;
    uint64_t stage_ts = 0;
    uint64_t next_ts = 0;
    bool have_next = false;
    AudioFormat2 fmt = AudioFormat2::kPcm16;

    AudioReliabilityProfile prof;
    bool configured = false;
    uint32_t cfg_latency = 0;
    AudioFormat2 cfg_fmt = AudioFormat2::kPcm16;

    // retransmit history: payload (AudioHeader2 + samples) per slot
    std::vector<std::vector<uint8_t>> hist;
    std::vector<uint32_t> hist_seq;
    std::vector<uint8_t> hist_valid;
    uint32_t hist_mask = 0;

    // FEC block state
    uint32_t block_base = 0;
    uint32_t block_count = 0;  // packets of the current block already sent
    uint64_t last_ts = 0;
    uint32_t last_n = 0;
    std::vector<std::vector<uint8_t>> parity;  // one accumulator per column
    std::vector<uint64_t> member_ts;           // ts of the first member of each column
    std::vector<uint32_t> member_seq;
    uint16_t channel = 0;

#ifdef LV_HAVE_OPUS
    OpusEncoder* opus_enc = nullptr;
    int opus_bitrate_bps = 0;

    ~Stream() {
        if (opus_enc) {
            opus_encoder_destroy(opus_enc);
            opus_enc = nullptr;
        }
    }

    bool ensure_opus_encoder(uint32_t sample_rate, int target_bps) {
        if (sample_rate != 48000) return false;
        if (!opus_enc) {
            int err = OPUS_OK;
            opus_enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
            if (!opus_enc || err != OPUS_OK) {
                opus_enc = nullptr;
                return false;
            }
            opus_encoder_ctl(opus_enc, OPUS_SET_COMPLEXITY(5));
            opus_encoder_ctl(opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
            opus_encoder_ctl(opus_enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
            opus_encoder_ctl(opus_enc, OPUS_SET_LSB_DEPTH(24));
            opus_encoder_ctl(opus_enc, OPUS_SET_INBAND_FEC(1));         // honoured when the mode allows (LBRR)
            opus_encoder_ctl(opus_enc, OPUS_SET_PACKET_LOSS_PERC(5));  // expected Wi-Fi loss: tunes CELT stability
            opus_encoder_ctl(opus_enc, OPUS_SET_VBR(1));
            opus_encoder_ctl(opus_enc, OPUS_SET_VBR_CONSTRAINT(1));  // constrained VBR: tight Wi-Fi rate + optimal SNR
            opus_bitrate_bps = 0;
        }
        if (opus_bitrate_bps != target_bps) {
            opus_encoder_ctl(opus_enc, OPUS_SET_BITRATE(target_bps));
            opus_bitrate_bps = target_bps;
        }
        return true;
    }
#endif
};

AudioTxEngine::AudioTxEngine(uint32_t sample_rate, size_t mtu, SendFn send)
    : rate_(sample_rate ? sample_rate : 48000), mtu_(mtu), send_(std::move(send)) {}

AudioTxEngine::~AudioTxEngine() = default;

void AudioTxEngine::set_latency_ms(uint32_t ms) {
    latency_ms_.store(std::clamp<uint32_t>(ms, 2, 2000), std::memory_order_relaxed);
}

void AudioTxEngine::set_wifi_redundancy(bool enabled, uint32_t stagger_us) {
    dup_stagger_us_.store(std::clamp<uint32_t>(stagger_us, 0, 10000), std::memory_order_relaxed);
    wifi_redundancy_.store(enabled, std::memory_order_release);
    if (!enabled) {
        std::lock_guard<std::mutex> lk(dup_mu_);
        pending_dups_.clear();
    }
}

void AudioTxEngine::set_secondary_send(SendFn secondary_send) {
    std::lock_guard<std::mutex> lk(dup_mu_);
    secondary_send_ = std::move(secondary_send);
    has_secondary_send_.store(static_cast<bool>(secondary_send_), std::memory_order_release);
}

void AudioTxEngine::set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip) {
    std::lock_guard<std::mutex> lk(dup_mu_);
    primary_nic_ip_ = primary_ip;
    secondary_nic_ip_ = secondary_ip;
}

std::string AudioTxEngine::primary_nic_ip() const {
    std::lock_guard<std::mutex> lk(dup_mu_);
    return primary_nic_ip_;
}

std::string AudioTxEngine::secondary_nic_ip() const {
    std::lock_guard<std::mutex> lk(dup_mu_);
    return secondary_nic_ip_;
}

void AudioTxEngine::queue_staggered_duplicate(uint32_t seq, uint16_t stream, uint32_t wallclock_us,
                                              const uint8_t* payload, size_t len, bool force_immediate,
                                              bool is_failover) {
    uint8_t buf[kMaxDatagram > 4096 ? 4096 : kMaxDatagram];
    if (kPacketHeaderSize + len > sizeof(buf)) return;
    PacketHeader h;
    h.type = PacketType::kAudio2;
    h.flags = kFlagHasCrc;
    h.frame_id = seq;
    h.timestamp_us = wallclock_us;
    h.slice_id = stream;
    h.total_slices = epoch_.load(std::memory_order_relaxed);
    h.seq = uint16_t(seq);
    const size_t n = write_packet(buf, sizeof(buf), h, payload, len);
    if (!n) return;

    const bool has_sec = has_secondary_send_.load(std::memory_order_acquire);
    const uint32_t stagger = (force_immediate || has_sec) ? 0u : dup_stagger_us_.load(std::memory_order_relaxed);
    if (stagger == 0) {
        SendFn fn;
        bool used_sec = false;
        {
            std::lock_guard<std::mutex> lk(dup_mu_);
            if (secondary_send_) {
                fn = secondary_send_;
                used_sec = true;
            } else {
                fn = send_;
            }
        }
        if (fn && fn(buf, n)) {
            packets_sent_.fetch_add(1, std::memory_order_relaxed);
            bytes_sent_.fetch_add(n, std::memory_order_relaxed);
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            if (used_sec) secondary_pkts_.fetch_add(1, std::memory_order_relaxed);
            if (is_failover) failover_pkts_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    std::lock_guard<std::mutex> lk(dup_mu_);
    if (pending_dups_.size() >= 1024) pending_dups_.pop_front();
    PendingDup item;
    item.due_wc_us = uint32_t(wallclock_us + stagger);
    item.dgram.assign(buf, buf + n);
    pending_dups_.push_back(std::move(item));
}

void AudioTxEngine::service_redundancy(uint32_t now_wallclock_us) {
    if (!wifi_redundancy_.load(std::memory_order_acquire) && !has_secondary_send_.load(std::memory_order_acquire))
        return;
    std::vector<std::vector<uint8_t>> due;
    SendFn fn;
    bool used_sec = false;
    {
        std::lock_guard<std::mutex> lk(dup_mu_);
        if (pending_dups_.empty()) return;
        if (secondary_send_) {
            fn = secondary_send_;
            used_sec = true;
        } else {
            fn = send_;
        }
        while (!pending_dups_.empty() &&
               int32_t(now_wallclock_us - pending_dups_.front().due_wc_us) >= 0) {
            due.push_back(std::move(pending_dups_.front().dgram));
            pending_dups_.pop_front();
        }
    }
    if (!fn) return;
    for (const auto& dg : due) {
        if (fn(dg.data(), dg.size())) {
            packets_sent_.fetch_add(1, std::memory_order_relaxed);
            bytes_sent_.fetch_add(dg.size(), std::memory_order_relaxed);
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            if (used_sec) secondary_pkts_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

AudioReliabilityProfile AudioTxEngine::profile(AudioFormat2 fmt) const {
    return audio_profile_for_latency(latency_ms_.load(std::memory_order_relaxed), rate_, mtu_, fmt);
}

void AudioTxEngine::reconfigure(Stream& s, const AudioReliabilityProfile& prof) {
    // Partial chunks of the previous profile are flushed by the caller before this point.
    s.prof = prof;
    s.configured = true;
    s.cfg_latency = prof.latency_ms;
    s.cfg_fmt = s.fmt;
    const uint64_t slots_needed =
        uint64_t(prof.history_ms) * rate_ / 1000 / std::max<uint32_t>(prof.chunk_samples, 1) + 16;
    uint32_t slots = 16;
    while (slots < slots_needed) slots <<= 1;
    s.hist.assign(slots, {});
    s.hist_seq.assign(slots, 0);
    s.hist_valid.assign(slots, 0);
    s.hist_mask = slots - 1;
    const size_t cols = prof.fec_interleave;
    s.parity.assign(cols, {});
    s.member_ts.assign(cols, 0);
    s.member_seq.assign(cols, 0);
    s.block_base = s.seq;
    s.block_count = 0;
}

bool AudioTxEngine::send_payload(uint32_t seq, uint16_t stream, uint32_t wallclock_us,
                                 const uint8_t* payload, size_t len) {
    uint8_t buf[kMaxDatagram > 4096 ? 4096 : kMaxDatagram];
    if (kPacketHeaderSize + len > sizeof(buf)) return false;
    PacketHeader h;
    h.type = PacketType::kAudio2;
    h.flags = kFlagHasCrc;
    h.frame_id = seq;
    h.timestamp_us = wallclock_us;
    h.slice_id = stream;
    h.total_slices = epoch_.load(std::memory_order_relaxed);  // sender session epoch
    h.seq = uint16_t(seq);
    const size_t n = write_packet(buf, sizeof(buf), h, payload, len);
    if (n && send_ && send_(buf, n)) {
        packets_sent_.fetch_add(1, std::memory_order_relaxed);
        bytes_sent_.fetch_add(n, std::memory_order_relaxed);
        win_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void AudioTxEngine::emit_data(Stream& s, uint16_t ch, const uint8_t* samples, uint32_t n,
                              AudioFormat2 fmt, uint64_t ts, uint32_t wallclock_us,
                              const AudioReliabilityProfile& prof) {
    // Live wire compression profile (only applies when caller feeds Float32; preserves raw PCM16/24 callers).
    static thread_local std::vector<uint8_t> encoded;
    AudioFormat2 wire_fmt = fmt;
    const uint8_t* wire_bytes = samples;
    size_t body = audio2_payload_bytes(fmt, n);
    if (fmt == AudioFormat2::kFloat32) {
        const AudioCodecProfile cp = codec_profile();
        if (cp != AudioCodecProfile::kPcm32F) {
            bool opus_encoded = false;
#ifdef LV_HAVE_OPUS
            const int target_bps = opus_bitrate_bps_for_profile(cp);
            if (target_bps > 0 && rate_ == 48000 &&
                (n == 120 || n == 240 || n == 480 || n == 960) &&
                s.ensure_opus_encoder(rate_, target_bps)) {
                encoded.resize(512);
                const int nb = opus_encode_float(s.opus_enc, reinterpret_cast<const float*>(samples),
                                                 int(n), encoded.data(), int(encoded.size()));
                if (nb > 0) {
                    encoded.resize(size_t(nb));
                    wire_fmt = AudioFormat2::kOpus;
                    wire_bytes = encoded.data();
                    body = encoded.size();
                    opus_encoded = true;
                }
            }
#endif
            if (!opus_encoded) {
                encode_float32_wire(cp, reinterpret_cast<const float*>(samples), n, wire_fmt, encoded);
                wire_bytes = encoded.data();
                body = encoded.size();
            }
        }
    }

    std::vector<uint8_t> payload(kAudioHeader2Size + body);
    AudioHeader2 ah;
    ah.stream_id = ch;
    ah.channel_index = ch;
    ah.sample_rate = rate_;
    ah.num_samples = uint16_t(n);
    ah.sample_format = wire_fmt;
    ah.flags = 0;
    ah.media_timestamp_samples = ts;
    ah.wallclock_us = wallclock_us;
    write_audio_header2(payload.data(), ah);
    std::memcpy(payload.data() + kAudioHeader2Size, wire_bytes, body);

    const uint32_t seq = s.seq++;
    // history for NACK servicing
    const uint32_t slot = seq & s.hist_mask;
    s.hist[slot] = payload;
    s.hist_seq[slot] = seq;
    s.hist_valid[slot] = 1;

    const bool pri_ok = send_payload(seq, ch, wallclock_us, payload.data(), payload.size());
    const bool has_sec = has_secondary_send_.load(std::memory_order_acquire);
    if (wifi_redundancy_.load(std::memory_order_acquire) || has_sec) {
        payload[11] |= kAudioFlagDuplicate;
        queue_staggered_duplicate(seq, ch, wallclock_us, payload.data(), payload.size(),
                                  /*force_immediate=*/!pri_ok || has_sec,
                                  /*is_failover=*/!pri_ok && has_sec);
    } else if (prof.allow_duplication && dup_active_.load(std::memory_order_relaxed)) {
        payload[11] |= kAudioFlagDuplicate;
        send_payload(seq, ch, wallclock_us, payload.data(), payload.size());
        duplicates_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- interleaved XOR FEC (skipped when 1+1 Redundancy or Dual-NIC already duplicates every packet) ----
    const uint32_t k = prof.fec_k, d = prof.fec_interleave;
    if (k == 0 || wifi_redundancy_.load(std::memory_order_relaxed) || has_sec) return;
    // Contiguity: members must be equal-sized (and same wire format), back to back in media time.
    if (s.block_count > 0 && (n != s.last_n || ts != s.last_ts + s.last_n ||
                              s.parity[0].size() != body)) {
        s.block_base = seq;
        s.block_count = 0;
    }
    const uint32_t idx = s.block_count;  // == seq - block_base
    const uint32_t col = idx % d;
    auto& acc = s.parity[col];
    if (idx < d) {
        acc.assign(wire_bytes, wire_bytes + body);
        s.member_ts[col] = ts;
        s.member_seq[col] = seq;
    } else {
        for (size_t i = 0; i < body; ++i) acc[i] ^= wire_bytes[i];
    }
    s.last_ts = ts;
    s.last_n = n;
    ++s.block_count;
    if (idx >= (k - 1) * d) {  // last member of this column
        emit_parity(s, ch, col, wire_fmt, n, wallclock_us);
    }
    if (s.block_count >= k * d) {
        s.block_base = seq + 1;
        s.block_count = 0;
    }
}

void AudioTxEngine::emit_parity(Stream& s, uint16_t ch, uint32_t col, AudioFormat2 fmt, uint32_t n,
                                uint32_t wallclock_us) {
    const auto& acc = s.parity[col];
    std::vector<uint8_t> payload(kAudioHeader2Size + kAudioFecInfoSize + acc.size());
    AudioHeader2 ah;
    ah.stream_id = ch;
    ah.channel_index = ch;
    ah.sample_rate = rate_;
    ah.num_samples = uint16_t(n);
    ah.sample_format = fmt;
    ah.flags = kAudioFlagFecParity;
    ah.media_timestamp_samples = s.member_ts[col];
    ah.wallclock_us = wallclock_us;
    write_audio_header2(payload.data(), ah);
    AudioFecInfo fi;
    fi.group_size = s.prof.fec_k;
    fi.interleave = s.prof.fec_interleave;
    write_audio_fec_info(payload.data() + kAudioHeader2Size, fi);
    std::memcpy(payload.data() + kAudioHeader2Size + kAudioFecInfoSize, acc.data(), acc.size());
    send_payload(s.member_seq[col], ch, wallclock_us, payload.data(), payload.size());
    parity_sent_.fetch_add(1, std::memory_order_relaxed);
}

void AudioTxEngine::flush_stream(Stream& s, uint16_t ch, uint32_t wallclock_us) {
    if (s.stage_samples == 0) return;
#ifdef LV_HAVE_OPUS
    if (s.fmt == AudioFormat2::kFloat32 && rate_ == 48000 &&
        opus_bitrate_bps_for_profile(codec_profile()) > 0 &&
        s.stage_samples != 120 && s.stage_samples != 240 &&
        s.stage_samples != 480 && s.stage_samples != 960) {
        // Zero-pad tail to the nearest valid Opus CELT frame size (120 / 240 / 480 / 960)
        uint32_t pad_to = 120;
        if (s.stage_samples > 480) pad_to = 960;
        else if (s.stage_samples > 240) pad_to = 480;
        else if (s.stage_samples > 120) pad_to = 240;
        s.stage.resize(size_t(pad_to) * 4, 0);
        s.stage_samples = pad_to;
    }
#endif
    emit_data(s, ch, s.stage.data(), s.stage_samples, s.fmt, s.stage_ts, wallclock_us, s.prof);
    s.stage_samples = 0;
    s.stage.clear();
}

int AudioTxEngine::push_channel(uint16_t channel, const void* samples, uint32_t num_samples,
                                AudioFormat2 fmt, uint64_t media_ts, uint32_t wallclock_us) {
    if (channel >= kMaxAudioChannels || !samples || num_samples == 0) return -1;
    if (uint8_t(fmt) < 1 || uint8_t(fmt) > 3) return -1;
    Stream* sp = streams_[channel].get();
    if (!sp) {
        std::lock_guard<std::mutex> lk(create_mu_);
        if (!streams_[channel]) streams_[channel] = std::make_unique<Stream>();
        sp = streams_[channel].get();
    }
    Stream& s = *sp;
    std::lock_guard<std::mutex> lk(s.mu);

    // Format change or discontinuity closes the partial chunk first.
    if (s.stage_samples && (fmt != s.fmt || media_ts != s.next_ts)) flush_stream(s, channel, wallclock_us);
    const uint32_t lat = latency_ms_.load(std::memory_order_relaxed);
    if (!s.configured || lat != s.cfg_latency || fmt != s.cfg_fmt) {
        flush_stream(s, channel, wallclock_us);
        s.fmt = fmt;
        reconfigure(s, audio_profile_for_latency(lat, rate_, mtu_, fmt));
    }
    s.fmt = fmt;
    update_loss_estimate();

    const size_t bps = audio2_bytes_per_sample(fmt);
    uint32_t target_chunk = s.prof.chunk_samples;
#ifdef LV_HAVE_OPUS
    if (fmt == AudioFormat2::kFloat32 && rate_ == 48000) {
        const int target_bps = opus_bitrate_bps_for_profile(codec_profile());
        if (target_bps > 0) {
            // Opus CELT-only low-delay framing (<= 5 ms guarantees pure CELT Fullband 20 Hz-20 kHz, never SILK):
            //   - 2.5 ms (120 samples) when latency budget <= 10 ms (ultra-low deterministic 5..10 ms mode)
            //   - 5.0 ms (240 samples) at standard 11..2000 ms latency (fullband even at 64 kbps, half Wi-Fi pkt rate)
            target_chunk = (lat <= 10) ? 120u : 240u;
        }
    }
#endif
    if (s.stage_samples > target_chunk) flush_stream(s, channel, wallclock_us);
    const uint8_t* src = static_cast<const uint8_t*>(samples);
    uint64_t ts = media_ts;
    uint32_t left = num_samples;
    while (left) {
        if (s.stage_samples == 0) s.stage_ts = ts;
        const uint32_t take = std::min(left, target_chunk - s.stage_samples);
        s.stage.insert(s.stage.end(), src, src + size_t(take) * bps);
        s.stage_samples += take;
        src += size_t(take) * bps;
        ts += take;
        left -= take;
        if (s.stage_samples >= target_chunk) flush_stream(s, channel, wallclock_us);
    }
    s.next_ts = ts;
    service_redundancy(wallclock_us);
    return 0;
}

void AudioTxEngine::flush(uint32_t wallclock_us) {
    for (uint16_t c = 0; c < kMaxAudioChannels; ++c) {
        Stream* s = streams_[c].get();
        if (!s) continue;
        std::lock_guard<std::mutex> lk(s->mu);
        flush_stream(*s, c, wallclock_us);
    }
    service_redundancy(wallclock_us);
}

void AudioTxEngine::update_loss_estimate() {
    const uint64_t now = now_us();
    uint64_t start = win_start_us_.load(std::memory_order_relaxed);
    if (start == 0) {
        win_start_us_.store(now, std::memory_order_relaxed);
        return;
    }
    if (now - start < 1'000'000) return;
    if (!win_start_us_.compare_exchange_strong(start, now)) return;
    const uint64_t sent = win_sent_.exchange(0), nacked = win_nacked_.exchange(0);
    if (sent < 20) return;
    const double ratio = double(nacked) / double(sent);
    if (ratio > 0.05) dup_active_.store(true, std::memory_order_relaxed);
    else if (ratio < 0.02) dup_active_.store(false, std::memory_order_relaxed);
}

void AudioTxEngine::on_nack(const uint8_t* payload, size_t len) {
    AudioNack nk;
    if (!parse_audio_nack(payload, len, nk)) return;
    nacks_rx_.fetch_add(1, std::memory_order_relaxed);
    Stream* sp = streams_[nk.stream_id].get();
    if (!sp) return;
    Stream& s = *sp;
    std::lock_guard<std::mutex> lk(s.mu);
    for (uint32_t i = 0; i < 64; ++i) {
        if (!((nk.mask >> i) & 1u)) continue;
        win_nacked_.fetch_add(1, std::memory_order_relaxed);
        const uint32_t seq = nk.base_seq + i;
        if (s.hist.empty()) continue;
        const uint32_t slot = seq & s.hist_mask;
        if (!s.hist_valid[slot] || s.hist_seq[slot] != seq) {
            nack_miss_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::vector<uint8_t> pl = s.hist[slot];
        pl[11] |= kAudioFlagRetransmit;
        send_payload(seq, nk.stream_id, now_us32(), pl.data(), pl.size());
        retransmits_.fetch_add(1, std::memory_order_relaxed);
    }
}

AudioTxStats AudioTxEngine::stats() const {
    AudioTxStats st;
    st.packets_sent = packets_sent_.load(std::memory_order_relaxed);
    st.bytes_sent = bytes_sent_.load(std::memory_order_relaxed);
    st.parity_sent = parity_sent_.load(std::memory_order_relaxed);
    st.retransmits = retransmits_.load(std::memory_order_relaxed);
    st.duplicates = duplicates_.load(std::memory_order_relaxed);
    st.secondary_packets = secondary_pkts_.load(std::memory_order_relaxed);
    st.failover_packets = failover_pkts_.load(std::memory_order_relaxed);
    st.nacks_received = nacks_rx_.load(std::memory_order_relaxed);
    st.nack_misses = nack_miss_.load(std::memory_order_relaxed);
    for (const auto& s : streams_) if (s) ++st.active_streams;
    st.mode = profile(AudioFormat2::kPcm16).mode;
    return st;
}

}  // namespace lv
