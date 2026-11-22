#pragma once
// Low-latency audio capture interface (Windows WASAPI Loopback / Microphone; stub on UNIX).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lv {

enum class AudioCaptureMode : uint8_t {
    kNone = 0,
    kLoopback = 1,  // system audio (WASAPI loopback on render endpoint)
    kMic = 2,       // microphone / line-in (WASAPI capture endpoint)
};

struct AudioDeviceInfo {
    int index = 0;
    std::string name;
    bool is_loopback = false;  // true = render endpoint (for loopback), false = capture endpoint (mic)
    bool is_default = false;
};

// Lists audio endpoints: loopback=true lists render endpoints, loopback=false lists microphones.
std::vector<AudioDeviceInfo> list_audio_devices(bool loopback);

class AudioCapture {
public:
    // Called from capture thread for each fixed-size PCM16 stereo chunk (default 240 frames = 5 ms at 48 kHz).
    using ChunkCallback =
        std::function<void(uint32_t seq_id, uint32_t timestamp_us, const int16_t* pcm16, uint16_t num_frames)>;

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // device_index < 0 picks system default endpoint. chunk_frames = 120 (2.5ms) or 240 (5ms).
    bool start(AudioCaptureMode mode, int device_index, uint16_t chunk_frames, ChunkCallback on_chunk);
    void stop();
    bool is_running() const;

    uint32_t sample_rate() const { return 48000; }
    uint8_t channels() const { return 2; }
    std::string device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lv
