#include "app/stats.hpp"

#include <algorithm>
#include <cstdio>

namespace lv {

ReceiverStatsTracker::ReceiverStatsTracker(uint64_t start_us) : start_us_(start_us), last_us_(start_us) {}

void ReceiverStatsTracker::record_frame(const CompletedFrame& cf, double latency_ms) {
    last_frame_us_ = cf.complete_time_us;
    if (cf.quality > 0) last_quality_ = cf.quality;
    const bool complete = (cf.slices_corrupt == 0 && cf.slices_lost == 0);
    if (complete) {
        ++win_frames_ok_;
        ++tot_frames_ok_;
    } else {
        ++win_frames_conc_;
        ++tot_frames_conc_;
    }
    win_slices_ok_ += cf.slices_ok;
    win_slices_bad_ += uint64_t(cf.slices_corrupt) + cf.slices_lost;
    tot_slices_ok_ += cf.slices_ok;
    tot_slices_bad_ += uint64_t(cf.slices_corrupt) + cf.slices_lost;

    if (latency_ms >= -50.0 && latency_ms < 10000.0) {
        const double clamped = latency_ms < 0.0 ? 0.0 : latency_ms;
        win_lat_ms_.push_back(clamped);
        if (tot_lat_ms_.size() < 200000) tot_lat_ms_.push_back(clamped);
    }
}

void ReceiverStatsTracker::compute_percentiles(std::vector<double>& s, double& p50, double& p99, double& mx) {
    if (s.empty()) {
        p50 = p99 = mx = 0.0;
        return;
    }
    std::sort(s.begin(), s.end());
    p50 = s[s.size() / 2];
    p99 = s[std::min(s.size() - 1, (s.size() * 99) / 100)];
    mx = s.back();
}

IntervalReport ReceiverStatsTracker::snapshot_interval(uint64_t now_us, const AssemblerStats& as,
                                                       const SeqLossTracker& seq, bool clock_synced,
                                                       uint32_t rtt_us, const AudioStats* audio) {
    IntervalReport r;
    const double dt = std::max(1e-6, double(now_us - last_us_) / 1e6);
    r.elapsed_s = double(now_us - start_us_) / 1e6;
    r.window_s = dt;
    r.frames_ok = win_frames_ok_;
    r.frames_concealed = win_frames_conc_;
    const uint64_t tot_f = win_frames_ok_ + win_frames_conc_;
    r.fps = double(tot_f) / dt;

    const uint64_t d_bytes = as.bytes_total - prev_bytes_;
    r.bitrate_mbps = double(d_bytes) * 8.0 / (dt * 1e6);

    compute_percentiles(win_lat_ms_, r.lat_p50_ms, r.lat_p99_ms, r.lat_max_ms);

    const uint64_t cur_rx = seq.received();
    const uint64_t cur_lost = seq.lost(32);
    const uint64_t d_rx = cur_rx >= prev_seq_rx_ ? (cur_rx - prev_seq_rx_) : 0;
    const uint64_t d_lost = cur_lost >= prev_seq_lost_ ? (cur_lost - prev_seq_lost_) : 0;
    const uint64_t d_exp = d_rx + d_lost;
    r.pkt_loss_pct = d_exp ? (100.0 * double(d_lost) / double(d_exp)) : 0.0;

    r.frame_drop_pct = tot_f ? (100.0 * double(win_frames_conc_) / double(tot_f)) : 0.0;
    const uint64_t tot_s = win_slices_ok_ + win_slices_bad_;
    r.slice_loss_pct = tot_s ? (100.0 * double(win_slices_bad_) / double(tot_s)) : 0.0;

    r.fec_recovered = as.fec_recovered_packets - prev_fec_rec_;
    r.crc_errors = as.crc_errors - prev_crc_;
    r.late_packets = as.late_packets - prev_late_;
    r.active_quality = last_quality_;
    r.clock_synced = clock_synced;
    r.rtt_us = rtt_us;
    if (audio && audio->packets_rx > 0) {
        r.has_audio = true;
        const uint64_t d_ab = audio->bytes_rx >= prev_audio_bytes_ ? (audio->bytes_rx - prev_audio_bytes_) : 0;
        r.audio_kbps = double(d_ab) * 8.0 / (dt * 1000.0);
        r.audio_buf_ms = audio->buffered_ms;
        prev_audio_bytes_ = audio->bytes_rx;
    }

    last_us_ = now_us;
    win_frames_ok_ = 0;
    win_frames_conc_ = 0;
    win_slices_ok_ = 0;
    win_slices_bad_ = 0;
    win_lat_ms_.clear();
    prev_bytes_ = as.bytes_total;
    prev_seq_rx_ = cur_rx;
    prev_seq_lost_ = cur_lost;
    prev_fec_rec_ = as.fec_recovered_packets;
    prev_crc_ = as.crc_errors;
    prev_late_ = as.late_packets;
    return r;
}

IntervalReport ReceiverStatsTracker::snapshot_total(uint64_t now_us, const AssemblerStats& as,
                                                    const SeqLossTracker& seq, bool clock_synced,
                                                    uint32_t rtt_us, const AudioStats* audio) const {
    IntervalReport r;
    const uint64_t end_us = (last_frame_us_ > start_us_) ? last_frame_us_ : now_us;
    const double dt = std::max(1e-6, double(end_us - start_us_) / 1e6);
    r.elapsed_s = dt;
    r.window_s = dt;
    r.frames_ok = tot_frames_ok_;
    r.frames_concealed = tot_frames_conc_;
    const uint64_t tot_f = tot_frames_ok_ + tot_frames_conc_;
    r.fps = double(tot_f) / dt;
    r.bitrate_mbps = double(as.bytes_total) * 8.0 / (dt * 1e6);

    auto copy = tot_lat_ms_;
    compute_percentiles(copy, r.lat_p50_ms, r.lat_p99_ms, r.lat_max_ms);

    r.pkt_loss_pct = seq.loss_percent(0);
    r.frame_drop_pct = tot_f ? (100.0 * double(tot_frames_conc_) / double(tot_f)) : 0.0;
    const uint64_t tot_s = tot_slices_ok_ + tot_slices_bad_;
    r.slice_loss_pct = tot_s ? (100.0 * double(tot_slices_bad_) / double(tot_s)) : 0.0;
    r.fec_recovered = as.fec_recovered_packets;
    r.crc_errors = as.crc_errors;
    r.late_packets = as.late_packets;
    r.active_quality = last_quality_;
    r.clock_synced = clock_synced;
    r.rtt_us = rtt_us;
    if (audio && audio->packets_rx > 0) {
        r.has_audio = true;
        r.audio_kbps = double(audio->bytes_rx) * 8.0 / (dt * 1000.0);
        r.audio_buf_ms = audio->buffered_ms;
    }
    return r;
}

std::string ReceiverStatsTracker::format_line(const IntervalReport& r, const char* prefix) {
    char buf[448];
    char tag[32];
    if (prefix) std::snprintf(tag, sizeof(tag), "%s", prefix);
    else std::snprintf(tag, sizeof(tag), "%5.1fs", r.elapsed_s);

    int n = std::snprintf(buf, sizeof(buf),
                          "[%s] FPS:%5.1f (ok:%llu conc:%llu) | Q:%-3u | %6.1f Mbps | "
                          "Lat(ms) p50:%5.2f p99:%5.2f max:%5.2f | "
                          "Loss:%5.2f%% | FEC:%llu | Drop:%5.1f%% (sl:%4.2f%%) | CRC:%llu Late:%llu",
                          tag, r.fps, (unsigned long long)r.frames_ok, (unsigned long long)r.frames_concealed,
                          unsigned(r.active_quality), r.bitrate_mbps, r.lat_p50_ms, r.lat_p99_ms,
                          r.lat_max_ms, r.pkt_loss_pct, (unsigned long long)r.fec_recovered,
                          r.frame_drop_pct, r.slice_loss_pct, (unsigned long long)r.crc_errors,
                          (unsigned long long)r.late_packets);
    if (r.has_audio && n > 0 && size_t(n) < sizeof(buf)) {
        std::snprintf(buf + n, sizeof(buf) - size_t(n), " | Aud:%5.0f kbps buf:%4.1fms", r.audio_kbps,
                      r.audio_buf_ms);
    }
    return buf;
}

}  // namespace lv
