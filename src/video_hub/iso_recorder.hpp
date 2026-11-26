#pragma once
// IsoMultiRecorder — Parallel Zero-Transcode multi-camera ISO recorder + PGM Master writer.
//
// Features:
//   * Simultaneous per-camera files ({SessionDir}/CAM1_{Label}.mp4, ...) + optional PGM_Master.mp4.
//   * Zero-Transcode remux for incoming H.264 / H.265 Annex-B access units.
//   * Background non-blocking worker queue for DCT/BGRA transcoding & PGM Master recording so disk
//     writes never stall UI preview or virtual cameras.
//   * Unified Master Clock start timecode shared across all camera files for NLE multicam sync.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codec/video_encoder.hpp"
#include "common/image.hpp"
#include "video/iso_recorder.hpp"
#include "video_hub/camera_slot.hpp"

namespace lv::hub {

struct IsoMultiSessionConfig {
    std::string output_dir;
    std::string session_name = "LanVid";
    lv::IsoContainer container = lv::IsoContainer::kMp4;
    bool record_program = false;
    uint32_t slot_mask = 0xFFFF;
    uint32_t slot_count = kMaxHubSlots;
};

struct IsoTrackStats {
    bool recording = false;
    bool waiting_for_keyframe = true;
    std::string path;
    uint64_t bytes = 0;
    uint64_t duration_us = 0;
    uint64_t frames = 0;
    double write_mbps = 0.0;
};

class IsoMultiRecorder {
public:
    IsoMultiRecorder();
    ~IsoMultiRecorder();

    IsoMultiRecorder(const IsoMultiRecorder&) = delete;
    IsoMultiRecorder& operator=(const IsoMultiRecorder&) = delete;

    bool start_session(const IsoMultiSessionConfig& cfg,
                       const std::array<std::string, kMaxHubSlots>& slot_labels);
    void stop_session();
    bool set_slot_recording(uint32_t slot, bool on, const std::string& label);

    bool is_recording_any() const;
    bool is_slot_recording(uint32_t slot) const;
    void set_program_slot(int slot);

    // Feeds an encoded Annex-B H.264/H.265 access unit (Zero-Transcode remux).
    void feed_encoded_access_unit(uint32_t slot, lv::IsoVideoCodec codec, const uint8_t* annexb, size_t size,
                                  bool is_keyframe, uint64_t arrival_us);
    // Feeds decoded/DCT BGRA frames into background encoder queue (ISO DCT slots & PGM Master).
    void feed_bgra_frame(uint32_t slot, const lv::Image& bgra, int codec_id, bool is_program, uint64_t arrival_us);
    // Feeds camera PCM audio track into matching open slot recorder.
    void feed_audio_pcm16(uint32_t slot, const int16_t* pcm16, uint32_t frames, uint64_t arrival_us);

    IsoTrackStats slot_stats(uint32_t slot) const;
    IsoTrackStats program_stats() const;
    double total_write_mbps() const;
    std::string session_dir() const;
    uint64_t session_elapsed_us() const;
    std::string session_timecode() const;

    static std::string default_output_dir();
    static uint64_t disk_free_bytes(const std::string& dir);

private:
    struct SlotWriter {
        std::mutex mu;
        std::unique_ptr<lv::IsoRecorder> rec;
        std::unique_ptr<lv::IVideoEncoder> enc;
        std::vector<uint8_t> scaled;
        std::string label;
        bool want_rec = false;
        lv::IsoRecorderStats last_stats{};
        std::string last_path;
    };

    struct BgraJob {
        uint32_t slot = 0;
        lv::Image image;
        uint64_t timestamp_us = 0;
        bool is_program = false;
        bool needs_encode = false;
    };

    void worker_loop();
    void process_job(const BgraJob& job);
    std::string build_slot_path(uint32_t slot, const std::string& label, lv::IsoContainer c) const;
    void close_writer(SlotWriter& w);

    std::array<std::unique_ptr<SlotWriter>, kMaxHubSlots> writers_;
    SlotWriter program_writer_;

    mutable std::mutex cfg_mu_;
    IsoMultiSessionConfig cfg_{};
    bool session_active_ = false;
    std::string session_dir_;
    uint64_t session_t0_us_ = 0;
    uint64_t session_unix_s_ = 0;
    uint64_t session_tc_start_frame_ = 0;
    std::atomic<int> program_slot_{0};

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<BgraJob> q_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace lv::hub
