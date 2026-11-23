#include "audio/audio_tx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/clock.hpp"

namespace lv {

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
};

AudioTxEngine::AudioTxEngine(uint32_t sample_rate, size_t mtu, SendFn send)
    : rate_(sample_rate ? sample_rate : 48000), mtu_(mtu), send_(std::move(send)) {}

AudioTxEngine::~AudioTxEngine() = default;

void AudioTxEngine::set_latency_ms(uint32_t ms) {
    latency_ms_.store(std::clamp<uint32_t>(ms, 2, 2000), std::memory_order_relaxed);
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

void AudioTxEngine::send_payload(uint32_t seq, uint16_t stream, uint32_t wallclock_us,
                                 const uint8_t* payload, size_t len) {
    uint8_t buf[kMaxDatagram > 4096 ? 4096 : kMaxDatagram];
    if (kPacketHeaderSize + len > sizeof(buf)) return;
    PacketHeader h;
    h.type = PacketType::kAudio2;
    h.flags = kFlagHasCrc;
    h.frame_id = seq;
    h.timestamp_us = wallclock_us;
    h.slice_id = stream;
    h.seq = uint16_t(seq);
    const size_t n = write_packet(buf, sizeof(buf), h, payload, len);
    if (n && send_ && send_(buf, n)) {
        packets_sent_.fetch_add(1, std::memory_order_relaxed);
        bytes_sent_.fetch_add(n, std::memory_order_relaxed);
        win_sent_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioTxEngine::emit_data(Stream& s, uint16_t ch, const uint8_t* samples, uint32_t n,
                              AudioFormat2 fmt, uint64_t ts, uint32_t wallclock_us,
                              const AudioReliabilityProfile& prof) {
    const size_t bps = audio2_bytes_per_sample(fmt);
    std::vector<uint8_t> payload(kAudioHeader2Size + size_t(n) * bps);
    AudioHeader2 ah;
    ah.stream_id = ch;
    ah.channel_index = ch;
    ah.sample_rate = rate_;
    ah.num_samples = uint16_t(n);
    ah.sample_format = fmt;
    ah.flags = 0;
    ah.media_timestamp_samples = ts;
    ah.wallclock_us = wallclock_us;
    write_audio_header2(payload.data(), ah);
    std::memcpy(payload.data() + kAudioHeader2Size, samples, size_t(n) * bps);

    const uint32_t seq = s.seq++;
    // history for NACK servicing
    const uint32_t slot = seq & s.hist_mask;
    s.hist[slot] = payload;
    s.hist_seq[slot] = seq;
    s.hist_valid[slot] = 1;

    send_payload(seq, ch, wallclock_us, payload.data(), payload.size());
    if (prof.allow_duplication && dup_active_.load(std::memory_order_relaxed)) {
        payload[11] |= kAudioFlagDuplicate;
        send_payload(seq, ch, wallclock_us, payload.data(), payload.size());
        duplicates_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- interleaved XOR FEC ----
    const uint32_t k = prof.fec_k, d = prof.fec_interleave;
    if (k == 0) return;
    // Contiguity: members must be equal-sized, back to back in media time.
    if (s.block_count > 0 && (n != s.last_n || ts != s.last_ts + s.last_n)) {
        s.block_base = seq;
        s.block_count = 0;
    }
    const uint32_t idx = s.block_count;  // == seq - block_base
    const uint32_t col = idx % d;
    auto& acc = s.parity[col];
    const size_t body = size_t(n) * bps;
    if (idx < d) {
        acc.assign(samples, samples + body);
        s.member_ts[col] = ts;
        s.member_seq[col] = seq;
    } else {
        for (size_t i = 0; i < body; ++i) acc[i] ^= samples[i];
    }
    s.last_ts = ts;
    s.last_n = n;
    ++s.block_count;
    if (idx >= (k - 1) * d) {  // last member of this column
        emit_parity(s, ch, col, fmt, n, wallclock_us);
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

    // Optional transparent Float32 -> 24-bit Packed PCM wire compression (-25% traffic, 144 dB range).
    static thread_local std::vector<uint8_t> pcm24_buf;
    const uint8_t* src = static_cast<const uint8_t*>(samples);
    if (fmt == AudioFormat2::kFloat32 && wire_pcm24_.load(std::memory_order_acquire)) {
        pcm24_buf.resize(size_t(num_samples) * 3);
        const float* fp = static_cast<const float*>(samples);
        for (uint32_t i = 0; i < num_samples; ++i) {
            const float clamped = std::clamp(fp[i], -1.0f, 1.0f);
            int32_t q = int32_t(std::lrint(double(clamped) * 8388607.0));
            q = std::clamp<int32_t>(q, -8388608, 8388607);
            const uint32_t u = uint32_t(q) & 0xFFFFFFu;
            pcm24_buf[3 * i + 0] = uint8_t(u & 0xFFu);
            pcm24_buf[3 * i + 1] = uint8_t((u >> 8) & 0xFFu);
            pcm24_buf[3 * i + 2] = uint8_t((u >> 16) & 0xFFu);
        }
        src = pcm24_buf.data();
        fmt = AudioFormat2::kPcm24Packed;
    }

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
    uint64_t ts = media_ts;
    uint32_t left = num_samples;
    while (left) {
        if (s.stage_samples == 0) s.stage_ts = ts;
        const uint32_t take = std::min(left, s.prof.chunk_samples - s.stage_samples);
        s.stage.insert(s.stage.end(), src, src + size_t(take) * bps);
        s.stage_samples += take;
        src += size_t(take) * bps;
        ts += take;
        left -= take;
        if (s.stage_samples >= s.prof.chunk_samples) flush_stream(s, channel, wallclock_us);
    }
    s.next_ts = ts;
    return 0;
}

void AudioTxEngine::flush(uint32_t wallclock_us) {
    for (uint16_t c = 0; c < kMaxAudioChannels; ++c) {
        Stream* s = streams_[c].get();
        if (!s) continue;
        std::lock_guard<std::mutex> lk(s->mu);
        flush_stream(*s, c, wallclock_us);
    }
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
    st.nacks_received = nacks_rx_.load(std::memory_order_relaxed);
    st.nack_misses = nack_miss_.load(std::memory_order_relaxed);
    for (const auto& s : streams_) if (s) ++st.active_streams;
    st.mode = profile(AudioFormat2::kPcm16).mode;
    return st;
}

}  // namespace lv
