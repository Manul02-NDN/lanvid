#pragma once
// Periodic and cumulative receiver telemetry: FPS, Bitrate (Mbps), E2E Latency (p50/p99/max),
// Packet Loss %, Frame Drop/Conceal rate, Slice Loss %, FEC recovered packets, and active Q.

#include <cstdint>
#include <string>
#include <vector>

#include "audio/audio_jitter_buffer.hpp"
#include "net/assembler.hpp"

namespace lv {

struct IntervalReport {
    double elapsed_s = 0.0;
    double window_s = 0.0;
    double fps = 0.0;
    uint64_t frames_ok = 0;
    uint64_t frames_concealed = 0;
    double bitrate_mbps = 0.0;
    double lat_p50_ms = 0.0;
    double lat_p99_ms = 0.0;
    double lat_max_ms = 0.0;
    double pkt_loss_pct = 0.0;
    double frame_drop_pct = 0.0;
    double slice_loss_pct = 0.0;
    uint64_t fec_recovered = 0;
    uint64_t crc_errors = 0;
    uint64_t late_packets = 0;
    uint8_t active_quality = 0;
    VideoCodecId active_codec = VideoCodecId::kLanVidDct;
    uint64_t pli_requests = 0;
    bool clock_synced = false;
    uint32_t rtt_us = 0;
    bool has_audio = false;
    double audio_kbps = 0.0;
    double audio_buf_ms = 0.0;
};

class ReceiverStatsTracker {
public:
    explicit ReceiverStatsTracker(uint64_t start_us);

    void record_frame(const CompletedFrame& cf, double latency_ms);

    IntervalReport snapshot_interval(uint64_t now_us, const AssemblerStats& as,
                                     const SeqLossTracker& seq, bool clock_synced, uint32_t rtt_us,
                                     const AudioStats* audio = nullptr);

    IntervalReport snapshot_total(uint64_t now_us, const AssemblerStats& as,
                                  const SeqLossTracker& seq, bool clock_synced, uint32_t rtt_us,
                                  const AudioStats* audio = nullptr) const;

    static std::string format_line(const IntervalReport& r, const char* prefix = nullptr);

private:
    static void compute_percentiles(std::vector<double>& samples, double& p50, double& p99, double& mx);

    uint64_t start_us_ = 0;
    uint64_t last_us_ = 0;
    uint64_t last_frame_us_ = 0;
    uint8_t last_quality_ = 0;
    VideoCodecId last_codec_ = VideoCodecId::kLanVidDct;

    uint64_t win_frames_ok_ = 0;
    uint64_t win_frames_conc_ = 0;
    uint64_t win_slices_ok_ = 0;
    uint64_t win_slices_bad_ = 0;
    std::vector<double> win_lat_ms_;

    uint64_t tot_frames_ok_ = 0;
    uint64_t tot_frames_conc_ = 0;
    uint64_t tot_slices_ok_ = 0;
    uint64_t tot_slices_bad_ = 0;
    std::vector<double> tot_lat_ms_;

    uint64_t prev_bytes_ = 0;
    uint64_t prev_seq_rx_ = 0;
    uint64_t prev_seq_lost_ = 0;
    uint64_t prev_fec_rec_ = 0;
    uint64_t prev_crc_ = 0;
    uint64_t prev_late_ = 0;
    uint64_t prev_audio_bytes_ = 0;
};

}  // namespace lv
