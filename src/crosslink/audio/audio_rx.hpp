#pragma once
// Receiver-side 64-stream audio engine: parsing, duplicate suppression, XOR-FEC recovery,
// NACK scheduling and delivery into the AudioSyncMatrix. Transport-agnostic and driven with an
// explicit local clock so it can run against sockets or a virtual-time simulator.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "audio/audio_reliability.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/master_clock.hpp"
#include "net/packet.hpp"

namespace cl {

struct AudioChannelRxStats {
    uint64_t packets_rx = 0;
    uint64_t packets_lost = 0;       // given up (never recovered)
    uint64_t fec_recovered = 0;
    uint64_t nack_recovered = 0;
    uint64_t nacks_sent = 0;
    uint64_t duplicates = 0;
    uint64_t late_packets = 0;       // arrived after their playout time
    uint32_t jitter_us = 0;          // RFC 3550 style inter-arrival jitter
    uint16_t stream_id = 0;
    bool seen = false;
};

class AudioRxEngine {
public:
    using SendFn = std::function<bool(const uint8_t* dgram, size_t len)>;

    AudioRxEngine(AudioSyncMatrix& matrix, MasterClock& clock, SendFn send_to_sender);
    ~AudioRxEngine();

    // Updates the playout latency AND the matching NACK policy (live, thread-safe).
    void set_latency_ms(uint32_t ms);
    uint32_t latency_ms() const { return latency_ms_.load(std::memory_order_relaxed); }

    // Fixed (false: delay held at latency_ms, for wired Ethernet / live monitoring) vs Adaptive Wi-Fi
    // (true: buffer grows within [latency_ms .. 200 ms] on jitter bursts and glides back). Live, any thread.
    void set_adaptive_jitter_enabled(bool enabled) { matrix_.set_adaptive_jitter_enabled(enabled); }
    bool adaptive_jitter_enabled() const { return matrix_.adaptive_jitter_enabled(); }
    // Measured playout delay of the jitter buffer right now (ms), see AudioSyncMatrix.
    double current_effective_latency_ms() const { return matrix_.current_effective_latency_ms(); }

    // kAudioFrame datagram body (after the 24-byte CrossLink header). Returns false if rejected.
    bool on_packet(const PacketHeader& h, const uint8_t* payload, size_t len, uint64_t local_now_us);
    // Periodic (<= every few ms): NACK scheduling and loss bookkeeping.
    void service(uint64_t local_now_us);

    // Forgets all transport state (sequence windows, FEC groups, NACK schedule, jitter estimate) for a
    // new sender epoch. Network thread only. Statistics counters are kept (cumulative).
    void reset_transport();

    // One-way clock (receivers without a ping exchange, e.g. CrossLink Monitor phones): the MasterClock is
    // derived from the media timestamps themselves - offset = max(media_end - arrival) over the last
    // ~2 s, i.e. the minimum-delay envelope. Bursts only ever add delay, so they never drag the playout
    // position; sender/phone crystal drift is followed as the window slides. Network thread only.
    void set_one_way_clock_sync(bool enabled) { one_way_ = enabled; }
    bool one_way_clock_sync() const { return one_way_; }

    AudioChannelRxStats channel_stats(uint16_t channel_index) const;
    uint64_t total_nacks_sent() const;
    uint64_t total_duplicates() const;
    uint64_t total_duplicates_recovered() const;
    uint64_t rate_mismatch_drops() const { return rate_drops_.load(std::memory_order_relaxed); }
    uint32_t active_channel_count() const;

private:
    struct Stream;
    struct Parity;
    bool accept_data(Stream& s, const AudioHeader2& ah, const uint8_t* samples, size_t payload_bytes,
                     uint32_t seq, uint64_t now, bool via_fec, bool via_nack);
    void try_fec(Stream& s, Parity& p, uint64_t now);
    void try_fec_for(Stream& s, uint32_t seq, uint64_t now);
    bool is_expired(const Stream& s, uint32_t seq, uint64_t now) const;
    void write_to_matrix(Stream& s, const AudioHeader2& ah, const uint8_t* samples, size_t payload_bytes);

    AudioSyncMatrix& matrix_;
    MasterClock& clock_;
    SendFn send_;
    std::atomic<uint32_t> latency_ms_{20};
    AudioReliabilityProfile prof_;      // network-thread copy, refreshed from latency_ms_
    uint32_t prof_latency_ = 0;
    void refresh_profile();
    std::atomic<uint64_t> rate_drops_{0};
    std::unique_ptr<Stream> streams_[kMaxAudioChannels];
    std::atomic<Stream*> stream_ptrs_[kMaxAudioChannels]{};
    std::atomic<int16_t> channel_to_stream_[kMaxAudioChannels];
    std::vector<float> scratch_;

    void one_way_observe(double media_end_us, uint64_t now);
    bool one_way_ = false;
    bool ow_have_ = false;
    double ow_applied_ = 0.0;
    double ow_win_[4] = {0, 0, 0, 0};
    double ow_cur_ = -1e300;
    uint64_t ow_win_start_ = 0;
    int ow_win_n_ = 0;
    double ow_slewed_ = 0.0;
    uint64_t ow_last_us_ = 0;
};

}  // namespace cl
