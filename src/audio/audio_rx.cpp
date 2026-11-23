#include "audio/audio_rx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lv {

namespace {
constexpr uint32_t kWindow = 8192;  // sequence window per stream (power of two)
constexpr uint32_t kStoreSlots = 64;

constexpr int kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr int kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

constexpr int kImaIndexTable5[16] = {-1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 3, 4, 5, 6, 7, 8};

void decode_adpcm4_samples(const uint8_t* in, uint32_t m, float* out) {
    if (m == 0) return;
    int pred = int16_t(uint16_t(in[0]) | (uint16_t(in[1]) << 8));
    int idx = std::clamp<int>(in[2], 0, 88);
    out[0] = float(pred) * (1.0f / 32768.0f);
    for (uint32_t i = 1; i < m; ++i) {
        const uint8_t packed = in[4 + (i - 1) / 2];
        const uint8_t code = ((i - 1) & 1u) == 0u ? (packed & 0x0Fu) : (packed >> 4);
        int step = kImaStepTable[idx];
        int diff = step >> 3;
        if (code & 4) diff += step;
        if (code & 2) diff += step >> 1;
        if (code & 1) diff += step >> 2;
        pred = std::clamp(pred + ((code & 8) ? -diff : diff), -32768, 32767);
        idx = std::clamp(idx + kImaIndexTable[code], 0, 88);
        out[i] = float(pred) * (1.0f / 32768.0f);
    }
}

void decode_adpcm5_samples(const uint8_t* in, uint32_t m, float* out) {
    if (m == 0) return;
    int pred = int16_t(uint16_t(in[0]) | (uint16_t(in[1]) << 8));
    int idx = std::clamp<int>(in[2], 0, 88);
    out[0] = float(pred) * (1.0f / 32768.0f);
    size_t bit_pos = 0;
    for (uint32_t i = 1; i < m; ++i) {
        const size_t byte_i = 4 + (bit_pos >> 3);
        const unsigned shift = unsigned(bit_pos & 7u);
        uint16_t raw = uint16_t(in[byte_i]) >> shift;
        if (shift > 3u) raw |= uint16_t(in[byte_i + 1]) << (8u - shift);
        const uint8_t code = uint8_t(raw & 0x1Fu);
        int step = kImaStepTable[idx];
        int diff = step >> 4;
        if (code & 8) diff += step;
        if (code & 4) diff += step >> 1;
        if (code & 2) diff += step >> 2;
        if (code & 1) diff += step >> 3;
        pred = std::clamp(pred + ((code & 16) ? -diff : diff), -32768, 32767);
        idx = std::clamp(idx + kImaIndexTable5[code & 15], 0, 88);
        out[i] = float(pred) * (1.0f / 32768.0f);
        bit_pos += 5;
    }
}

void upsample_linear(const float* dec, uint32_t m, uint32_t n, uint32_t factor, float* out) {
    if (m == 0 || n == 0) return;
    if (m == 1) {
        std::fill(out, out + n, dec[0]);
        return;
    }
    const float inv_f = 1.0f / float(factor);
    for (uint32_t i = 0; i < n; ++i) {
        const float pos = (float(i) + 0.5f) * inv_f - 0.5f;
        const float clamped = std::clamp(pos, 0.0f, float(m - 1));
        const uint32_t k0 = uint32_t(clamped);
        const uint32_t k1 = std::min(m - 1, k0 + 1u);
        const float frac = clamped - float(k0);
        out[i] = dec[k0] + (dec[k1] - dec[k0]) * frac;
    }
}

void decode_to_float(AudioFormat2 fmt, const uint8_t* in, uint32_t n, float* out) {
    switch (fmt) {
        case AudioFormat2::kPcm16:
            for (uint32_t i = 0; i < n; ++i) {
                const int16_t v = int16_t(uint16_t(in[2 * i]) | (uint16_t(in[2 * i + 1]) << 8));
                out[i] = float(v) * (1.0f / 32768.0f);
            }
            break;
        case AudioFormat2::kPcm24Packed:
            for (uint32_t i = 0; i < n; ++i) {
                int32_t v = int32_t(uint32_t(in[3 * i]) | (uint32_t(in[3 * i + 1]) << 8) |
                                    (uint32_t(in[3 * i + 2]) << 16));
                if (v & 0x800000) v -= 0x1000000;
                out[i] = float(v) * (1.0f / 8388608.0f);
            }
            break;
        case AudioFormat2::kFloat32:
            std::memcpy(out, in, size_t(n) * 4);
            break;
        case AudioFormat2::kAdpcm4:
            decode_adpcm4_samples(in, n, out);
            break;
        case AudioFormat2::kAdpcm5:
            decode_adpcm5_samples(in, n, out);
            break;
        case AudioFormat2::kAdpcm128: {
            const uint32_t m = (n + 1u) / 2u;
            static thread_local std::vector<float> dec;
            dec.resize(m);
            decode_adpcm5_samples(in, m, dec.data());
            upsample_linear(dec.data(), m, n, 2u, out);
            break;
        }
        case AudioFormat2::kAdpcm96: {
            const uint32_t m = (n + 1u) / 2u;
            static thread_local std::vector<float> dec;
            dec.resize(m);
            decode_adpcm4_samples(in, m, dec.data());
            upsample_linear(dec.data(), m, n, 2u, out);
            break;
        }
        case AudioFormat2::kAdpcm64: {
            const uint32_t m = (n + 2u) / 3u;
            static thread_local std::vector<float> dec;
            dec.resize(m);
            decode_adpcm4_samples(in, m, dec.data());
            upsample_linear(dec.data(), m, n, 3u, out);
            break;
        }
    }
}
}  // namespace

struct AudioRxEngine::Parity {
    uint32_t first_seq = 0;
    uint8_t k = 1, d = 1;
    uint64_t ts_first = 0;
    AudioHeader2 hdr;
    std::vector<uint8_t> xor_bytes;
    bool done = false;
};

struct AudioRxEngine::Stream {
    bool seen = false;
    uint32_t highest = 0;
    uint32_t first_unresolved = 0;
    std::vector<uint64_t> recv_bits;  // kWindow bits
    struct Miss {
        uint64_t detected_us = 0;
        uint64_t next_try_us = 0;
        uint8_t attempts = 0;
        uint8_t state = 0;  // 0 none/received, 1 pending, 2 lost
    };
    std::vector<Miss> miss;

    struct Stored { uint32_t seq = 0; bool valid = false; std::vector<uint8_t> bytes; };
    Stored store[kStoreSlots];
    std::vector<Parity> parities;

    // time reference for hole -> media timestamp estimation
    uint32_t ref_seq = 0;
    uint64_t ref_ts = 0;
    uint32_t chunk = 0;
    uint16_t channel = 0;
    AudioFormat2 fmt = AudioFormat2::kPcm16;

    // jitter
    bool has_transit = false;
    int32_t prev_transit = 0;
    double jitter = 0.0;

    std::atomic<uint64_t> rx{0}, lost{0}, fec_rec{0}, nack_rec{0}, nacks_sent{0}, dups{0}, late{0};
    std::atomic<uint32_t> jitter_us{0};
    std::atomic<bool> seen_pub{false};

    void reset_transport() {
        seen = false;
        highest = first_unresolved = 0;
        std::fill(recv_bits.begin(), recv_bits.end(), 0ull);
        for (auto& m : miss) m = {};
        for (auto& st : store) st.valid = false;
        parities.clear();
        ref_seq = 0;
        ref_ts = 0;
        chunk = 0;
        has_transit = false;
        prev_transit = 0;
        jitter = 0.0;
        jitter_us.store(0, std::memory_order_relaxed);
    }
};

void AudioRxEngine::reset_transport() {
    for (auto& sp : streams_)
        if (sp) sp->reset_transport();
}

AudioRxEngine::AudioRxEngine(AudioSyncMatrix& matrix, MasterClock& clock, SendFn send)
    : matrix_(matrix), clock_(clock), send_(std::move(send)) {
    for (auto& c : channel_to_stream_) c.store(-1);
    set_latency_ms(20);
    refresh_profile();
}

AudioRxEngine::~AudioRxEngine() = default;

void AudioRxEngine::set_latency_ms(uint32_t ms) {
    // Any thread: only atomics are touched here; the NACK profile is rebuilt on the network thread.
    ms = std::clamp<uint32_t>(ms, 2, 2000);
    latency_ms_.store(ms, std::memory_order_release);
    matrix_.set_target_latency_ms(ms);
}

void AudioRxEngine::refresh_profile() {
    const uint32_t ms = latency_ms_.load(std::memory_order_acquire);
    if (ms == prof_latency_) return;
    prof_ = audio_profile_for_latency(ms, matrix_.sample_rate(), kDefaultMtu, AudioFormat2::kPcm16);
    prof_latency_ = ms;
}

uint32_t AudioRxEngine::active_channel_count() const {
    uint32_t n = 0;
    for (uint16_t c = 0; c < kMaxAudioChannels; ++c) if (matrix_.meter(c).active) ++n;
    return n;
}

void AudioRxEngine::write_to_matrix(const AudioHeader2& ah, const uint8_t* samples) {
    if (scratch_.size() < ah.num_samples) scratch_.resize(ah.num_samples);
    decode_to_float(ah.sample_format, samples, ah.num_samples, scratch_.data());
    matrix_.write(ah.channel_index, ah.media_timestamp_samples, scratch_.data(), ah.num_samples);
}

bool AudioRxEngine::is_expired(const Stream& s, uint32_t seq, uint64_t now) const {
    if (s.chunk == 0) return false;
    const int64_t ts = int64_t(s.ref_ts) + int64_t(int32_t(seq - s.ref_seq)) * int64_t(s.chunk);
    const uint32_t rate = matrix_.sample_rate();
    // Effective delay (== target in Fixed mode; grown target in Adaptive mode, so packets the expanded
    // buffer can still play are neither counted late nor given up on).
    const uint32_t lat_ms = std::max(latency_ms_.load(std::memory_order_relaxed), matrix_.effective_latency_ms());
    const int64_t lat = int64_t(lat_ms) * rate / 1000;
    if (clock_.synced()) {
        const int64_t target = clock_.master_samples(now, rate) - lat;
        return ts + int64_t(s.chunk) <= target;
    }
    const int64_t newest = int64_t(matrix_.newest_media_ts());
    return ts + int64_t(s.chunk) <= newest - lat;
}

bool AudioRxEngine::on_packet(const PacketHeader& h, const uint8_t* payload, size_t len,
                              uint64_t now) {
    refresh_profile();
    AudioHeader2 ah;
    if (!parse_audio_header2(payload, len, ah)) return false;
    if (ah.sample_rate != matrix_.sample_rate()) {
        rate_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint32_t seq = h.frame_id;
    auto& sp = streams_[ah.stream_id];
    if (!sp) {
        sp = std::make_unique<Stream>();
        sp->recv_bits.assign(kWindow / 64, 0);
        sp->miss.assign(kWindow, {});
    }
    Stream& s = *sp;
    s.channel = ah.channel_index;
    channel_to_stream_[ah.channel_index].store(int16_t(ah.stream_id), std::memory_order_relaxed);

    if (ah.flags & kAudioFlagFecParity) {
        AudioFecInfo fi;
        if (!parse_audio_fec_info(payload + kAudioHeader2Size, len - kAudioHeader2Size, fi)) return false;
        if (!s.seen) return true;  // nothing to repair yet
        Parity p;
        p.first_seq = seq;
        p.k = fi.group_size;
        p.d = fi.interleave;
        p.ts_first = ah.media_timestamp_samples;
        p.hdr = ah;
        const size_t body = audio2_payload_bytes(ah.sample_format, ah.num_samples);
        p.xor_bytes.assign(payload + kAudioHeader2Size + kAudioFecInfoSize,
                           payload + kAudioHeader2Size + kAudioFecInfoSize + body);
        s.parities.push_back(std::move(p));
        if (s.parities.size() > 48) s.parities.erase(s.parities.begin());
        try_fec(s, s.parities.back(), now);
        return true;
    }

    const uint8_t* samples = payload + kAudioHeader2Size;
    const bool retransmit = (ah.flags & kAudioFlagRetransmit) != 0;
    return accept_data(s, ah, samples, seq, now, false, retransmit);
}

bool AudioRxEngine::accept_data(Stream& s, const AudioHeader2& ah, const uint8_t* samples,
                                uint32_t seq, uint64_t now, bool via_fec, bool via_nack) {
    auto bit_get = [&](uint32_t q) { return (s.recv_bits[(q & (kWindow - 1)) >> 6] >> (q & 63)) & 1ull; };
    auto bit_set = [&](uint32_t q) { s.recv_bits[(q & (kWindow - 1)) >> 6] |= 1ull << (q & 63); };
    auto bit_clr = [&](uint32_t q) { s.recv_bits[(q & (kWindow - 1)) >> 6] &= ~(1ull << (q & 63)); };

    if (!s.seen) {
        s.seen = true;
        s.seen_pub.store(true, std::memory_order_relaxed);
        s.highest = seq;
        s.first_unresolved = seq;
    } else {
        const int32_t d = int32_t(seq - s.highest);
        if (d > int32_t(kWindow) - 64 || d < -int32_t(kWindow) + 64) {
            // Stream restarted or huge gap: resynchronise state.
            std::fill(s.recv_bits.begin(), s.recv_bits.end(), 0ull);
            for (auto& m : s.miss) m = {};
            s.parities.clear();
            s.highest = seq;
            s.first_unresolved = seq;
        } else if (d > 0) {
            for (uint32_t q = s.highest + 1; q != seq; ++q) {  // register holes
                bit_clr(q);
                auto& m = s.miss[q & (kWindow - 1)];
                m = {};
                m.state = 1;
                m.detected_us = now;
                m.next_try_us = now + uint64_t(prof_.nack_first_wait_ms) * 1000;
            }
            bit_clr(seq);
            s.highest = seq;
        } else if (int32_t(seq - s.first_unresolved) < 0) {
            s.dups.fetch_add(1, std::memory_order_relaxed);  // older than anything unresolved
            return true;
        }
    }

    auto& m = s.miss[seq & (kWindow - 1)];
    if (bit_get(seq)) {  // duplicate (retransmit race, proactive duplicate, or FEC + original)
        s.dups.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const bool was_hole = m.state != 0;
    bit_set(seq);
    m.state = 0;

    s.chunk = ah.num_samples;
    s.ref_seq = seq;
    s.ref_ts = ah.media_timestamp_samples;
    s.fmt = ah.sample_format;

    if (is_expired(s, seq, now)) s.late.fetch_add(1, std::memory_order_relaxed);

    write_to_matrix(ah, samples);
    s.rx.fetch_add(1, std::memory_order_relaxed);
    if (via_fec) s.fec_rec.fetch_add(1, std::memory_order_relaxed);
    if (via_nack && was_hole) s.nack_rec.fetch_add(1, std::memory_order_relaxed);

    // keep the payload for later XOR recovery of neighbours
    auto& st = s.store[seq % kStoreSlots];
    st.seq = seq;
    st.valid = true;
    st.bytes.assign(samples, samples + audio2_payload_bytes(ah.sample_format, ah.num_samples));

    if (!via_fec) {
        // RFC 3550 jitter on (arrival - sender wallclock), modular 32-bit arithmetic.
        const int32_t transit = int32_t(uint32_t(now) - ah.wallclock_us);
        if (s.has_transit) {
            const double dv = std::fabs(double(int32_t(transit - s.prev_transit)));
            s.jitter += (dv - s.jitter) / 16.0;
            const uint32_t j_us = uint32_t(s.jitter);
            s.jitter_us.store(j_us, std::memory_order_relaxed);
            matrix_.observe_network_jitter_us(j_us);
        }
        s.has_transit = true;
        if (!via_nack && clock_.synced()) {
            // Arrival lag vs the MasterClock (what the playout delay must cover for this block).
            matrix_.observe_arrival_lag_samples(clock_.master_samples(now, matrix_.sample_rate()) -
                                                int64_t(ah.media_timestamp_samples + ah.num_samples));
        }
        s.prev_transit = transit;
    }

    while (s.first_unresolved != s.highest + 1 &&
           (bit_get(s.first_unresolved) || s.miss[s.first_unresolved & (kWindow - 1)].state == 2)) {
        ++s.first_unresolved;
    }
    try_fec_for(s, seq, now);
    return true;
}

void AudioRxEngine::try_fec_for(Stream& s, uint32_t seq, uint64_t now) {
    for (size_t i = 0; i < s.parities.size(); ++i) {
        Parity& p = s.parities[i];
        if (p.done) continue;
        const int32_t off = int32_t(seq - p.first_seq);
        if (off < 0 || off > int32_t(p.k - 1) * p.d || (off % p.d) != 0) continue;
        try_fec(s, p, now);
    }
}

void AudioRxEngine::try_fec(Stream& s, Parity& p, uint64_t now) {
    if (p.done) return;
    uint32_t missing_seq = 0;
    uint32_t missing_j = 0;
    int missing = 0;
    for (uint32_t j = 0; j < p.k; ++j) {
        const uint32_t q = p.first_seq + j * p.d;
        const auto& st = s.store[q % kStoreSlots];
        const bool have = st.valid && st.seq == q;
        if (!have) {
            ++missing;
            missing_seq = q;
            missing_j = j;
            if (missing > 1) return;
        }
    }
    if (missing == 0) { p.done = true; return; }
    if (int32_t(missing_seq - s.highest) > 0) return;  // not sent yet from our point of view

    std::vector<uint8_t> rec = p.xor_bytes;
    for (uint32_t j = 0; j < p.k; ++j) {
        if (j == missing_j) continue;
        const auto& st = s.store[(p.first_seq + j * p.d) % kStoreSlots];
        if (st.bytes.size() != rec.size()) return;  // mismatched chunk sizes: group unusable
        for (size_t b = 0; b < rec.size(); ++b) rec[b] ^= st.bytes[b];
    }
    AudioHeader2 ah = p.hdr;
    ah.flags = 0;
    ah.media_timestamp_samples = p.ts_first + uint64_t(missing_j) * p.d * p.hdr.num_samples;
    p.done = true;
    accept_data(s, ah, rec.data(), missing_seq, now, true, false);
}

void AudioRxEngine::service(uint64_t now) {
    refresh_profile();
    const uint32_t attempts_max = prof_.nack_attempts;
    for (uint16_t sid = 0; sid < kMaxAudioChannels; ++sid) {
        Stream* sp = streams_[sid].get();
        if (!sp || !sp->seen) continue;
        Stream& s = *sp;
        auto bit_get = [&](uint32_t q) { return (s.recv_bits[(q & (kWindow - 1)) >> 6] >> (q & 63)) & 1ull; };

        bool have_base = false;
        uint32_t base = 0;
        uint64_t mask = 0;
        for (uint32_t q = s.first_unresolved; int32_t(q - s.highest) <= 0; ++q) {
            auto& m = s.miss[q & (kWindow - 1)];
            if (m.state != 1 || bit_get(q)) continue;
            if (is_expired(s, q, now)) {
                m.state = 2;
                s.lost.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (m.attempts >= attempts_max || now < m.next_try_us) continue;
            if (!have_base) { have_base = true; base = q; }
            const uint32_t bit = q - base;
            if (bit >= 64) break;
            mask |= 1ull << bit;
            ++m.attempts;
            m.next_try_us = now + uint64_t(prof_.nack_spacing_ms) * 1000;
        }
        // Holes that exhausted every attempt and aged out are accounted for via is_expired above.
        while (s.first_unresolved != s.highest + 1 &&
               (bit_get(s.first_unresolved) ||
                s.miss[s.first_unresolved & (kWindow - 1)].state == 2)) {
            ++s.first_unresolved;
        }
        if (mask && send_) {
            AudioNack nk;
            nk.stream_id = sid;
            nk.base_seq = base;
            nk.mask = mask;
            uint8_t pl[kAudioNackSize];
            write_audio_nack(pl, nk);
            uint8_t dg[kPacketHeaderSize + kAudioNackSize];
            PacketHeader h;
            h.type = PacketType::kAudioNack;
            h.flags = kFlagHasCrc;
            h.slice_id = sid;
            if (write_packet(dg, sizeof(dg), h, pl, sizeof(pl)) && send_(dg, sizeof(dg))) {
                s.nacks_sent.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

AudioChannelRxStats AudioRxEngine::channel_stats(uint16_t ch) const {
    AudioChannelRxStats o;
    if (ch >= kMaxAudioChannels) return o;
    const int16_t sid = channel_to_stream_[ch].load(std::memory_order_relaxed);
    if (sid < 0 || !streams_[sid]) return o;
    const Stream& s = *streams_[sid];
    o.seen = s.seen_pub.load(std::memory_order_relaxed);
    o.stream_id = uint16_t(sid);
    o.packets_rx = s.rx.load(std::memory_order_relaxed);
    o.packets_lost = s.lost.load(std::memory_order_relaxed);
    o.fec_recovered = s.fec_rec.load(std::memory_order_relaxed);
    o.nack_recovered = s.nack_rec.load(std::memory_order_relaxed);
    o.nacks_sent = s.nacks_sent.load(std::memory_order_relaxed);
    o.duplicates = s.dups.load(std::memory_order_relaxed);
    o.late_packets = s.late.load(std::memory_order_relaxed);
    o.jitter_us = s.jitter_us.load(std::memory_order_relaxed);
    return o;
}

}  // namespace lv
