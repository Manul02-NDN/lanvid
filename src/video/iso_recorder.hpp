#pragma once
// Per-camera ISO recorder: one file per camera (CAM1_Wide.mp4, CAM2_Close.mp4, ...).
//
//  * Zero-transcode remux: H.264 / H.265 access units are written exactly as they arrived from the phone
//    (Annex-B -> length-prefixed samples, SPS/PPS/VPS moved into avcC / hvcC). No CPU cost, original bitrate.
//  * Built-in muxers, no FFmpeg dependency:
//      MP4 (ISO BMFF, 'isom'/'avc1'), MOV (QuickTime 'qt  ')  - moov written on close, 64-bit offsets
//      MKV (Matroska)                                          - Cues + Duration written on close
//  * Optional PCM audio track (camera's embedded audio): MOV 'sowt', MP4 'ipcm', MKV A_PCM/INT/LIT.
//  * Timecode: every file of one session gets the same start timecode (studio wall clock at REC ALL) in a
//    QuickTime 'tmcd' track (MP4/MOV) / Matroska tag - DaVinci Resolve, Premiere Pro and Final Cut line the
//    cameras up automatically in a multicam clip.
//  * Thread-safe: video and audio may be written from different threads.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lv {

enum class IsoContainer : uint8_t { kMp4 = 0, kMkv = 1, kMov = 2 };
enum class IsoVideoCodec : uint8_t { kH264 = 0, kH265 = 1 };

const char* iso_container_extension(IsoContainer c);  // "mp4" / "mkv" / "mov"

struct IsoRecorderConfig {
    std::string path;
    IsoContainer container = IsoContainer::kMp4;
    IsoVideoCodec codec = IsoVideoCodec::kH264;
    int width = 1920;
    int height = 1080;
    int timecode_fps = 30;             // nominal rate of the timecode track (24/25/30/50/60)
    uint64_t timecode_start_frame = 0; // frames since midnight at the session start (same for all cameras)
    uint64_t creation_unix_s = 0;      // session start (UTC seconds); 0 = now
    // Embedded audio (0 channels = no audio track)
    uint32_t audio_sample_rate = 48000;
    uint16_t audio_channels = 0;       // PCM16 interleaved
    std::string camera_label;          // stored as title metadata
};

struct IsoRecorderStats {
    uint64_t video_frames = 0;
    uint64_t audio_frames = 0;         // PCM sample frames
    uint64_t bytes_written = 0;
    uint64_t duration_us = 0;
    bool waiting_for_keyframe = true;
};

class IsoRecorder {
public:
    IsoRecorder();
    ~IsoRecorder();
    IsoRecorder(const IsoRecorder&) = delete;
    IsoRecorder& operator=(const IsoRecorder&) = delete;

    bool open(const IsoRecorderConfig& cfg);
    // One complete Annex-B access unit. `pts_us` on the session clock (0 = session start). Frames before the
    // first keyframe are skipped (a file must start decodable).
    bool write_video(const uint8_t* annexb, size_t size, bool is_keyframe, uint64_t pts_us);
    // PCM16 interleaved, `frames` sample frames, `pts_us` of the first one.
    bool write_audio(const int16_t* pcm, uint32_t frames, uint64_t pts_us);
    // Finalizes the container (moov / Cues / sizes). Safe to call twice. Returns false on I/O errors.
    bool close();

    bool is_open() const;
    IsoRecorderStats stats() const;
    const std::string& path() const { return cfg_.path; }

    struct Impl;

private:
    IsoRecorderConfig cfg_;
    IsoRecorderStats final_stats_;  // kept after close()
    mutable std::mutex mu_;
    std::unique_ptr<Impl> impl_;
};

// ---- helpers (exposed for tests and the multicam manager) ----
// Splits an Annex-B stream into NAL units (without start codes).
std::vector<std::pair<const uint8_t*, size_t>> annexb_split(const uint8_t* data, size_t size);
// Picture size from an H.264 SPS NAL (without start code). Returns false if it cannot be parsed.
bool h264_sps_dimensions(const uint8_t* sps, size_t size, int& width, int& height);
// "HH:MM:SS:FF" for a frame count at `fps`.
std::string iso_timecode_string(uint64_t frames, int fps);
// Frames since local midnight for "now" at `fps` (session timecode start).
uint64_t iso_timecode_now(int fps);

}  // namespace lv
