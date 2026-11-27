#pragma once
// Low-latency audio playback interface (Windows WASAPI Shared Mode; stub on UNIX).
// Pulls stereo PCM16 frames directly from a lock-free AudioJitterBuffer.

#include <memory>
#include <string>

#include "audio/stereo_audio_buffer.hpp"

namespace lv {

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    // Starts playback thread pulling from `buffer` (buffer must outlive playback).
    bool start(AudioJitterBuffer* buffer);
    void stop();
    bool is_running() const;
    std::string device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lv
