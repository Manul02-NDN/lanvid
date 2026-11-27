#pragma once
// LanVid Video SDK (v3.0.0) — Modern C++17/C++20 Header-Only RAII Wrapper
// Video + 2-channel stereo audio streaming over LAN (zero CrossLink AoIP/MIDI dependencies).
// Communicates strictly through the stable C ABI (lanvid/lanvid_sdk.h).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lanvid/lanvid_sdk.h"

namespace lanvid {

template <typename T>
class Result {
public:
    static Result ok(T val) { return Result(std::move(val), LV_OK); }
    static Result err(int code, std::string msg = {}) {
        return Result(std::nullopt, code, msg.empty() ? lv_error_string(code) : std::move(msg));
    }

    explicit operator bool() const noexcept { return code_ == LV_OK && value_.has_value(); }
    bool is_ok() const noexcept { return code_ == LV_OK && value_.has_value(); }
    int error_code() const noexcept { return code_; }
    const std::string& error_message() const noexcept { return message_; }
    const T& value() const { return *value_; }
    T& value() { return *value_; }

private:
    Result(std::optional<T> v, int c, std::string m = {})
        : value_(std::move(v)), code_(c), message_(std::move(m)) {}
    std::optional<T> value_;
    int code_ = LV_OK;
    std::string message_;
};

template <>
class Result<void> {
public:
    static Result ok() { return Result(LV_OK); }
    static Result err(int code, std::string msg = {}) {
        return Result(code, msg.empty() ? lv_error_string(code) : std::move(msg));
    }

    explicit operator bool() const noexcept { return code_ == LV_OK; }
    bool is_ok() const noexcept { return code_ == LV_OK; }
    int error_code() const noexcept { return code_; }
    const std::string& error_message() const noexcept { return message_; }

private:
    Result(int c, std::string m = {}) : code_(c), message_(std::move(m)) {}
    int code_ = LV_OK;
    std::string message_;
};

struct Peer {
    std::string ip;
    uint16_t port = 5000;
    uint32_t rtt_ms = 0;
    std::string name;
    std::string os;
    std::string simd;
};

class Runtime {
public:
    Runtime() { lv_init(); }
    ~Runtime() { lv_shutdown(); }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    static std::string version() { return lv_get_version_string(); }
    static bool check_abi() {
        return lv_check_abi_compatibility(LV_SDK_VERSION_MAJOR, LV_SDK_VERSION_MINOR) != 0;
    }
    static std::string simd_info() { return lv_get_cpu_simd_info(); }
    static void set_simd_enabled(bool enabled) { lv_set_simd_enabled(enabled ? 1 : 0); }
    static std::string enumerate_devices_json() {
        char* s = lv_enumerate_devices();
        std::string out(s ? s : "{}");
        lv_free_string(s);
        return out;
    }
    static std::string supported_codecs_json() {
        char* s = lv_get_supported_codecs();
        std::string out(s ? s : "[]");
        lv_free_string(s);
        return out;
    }
};

class Discovery {
public:
    static std::vector<Peer> scan(int timeout_ms = 400, uint16_t discovery_port = 0) {
        LV_DiscoveredPeer raw[64];
        const int n = lv_discovery_scan_peers(timeout_ms, discovery_port, raw, 64);
        std::vector<Peer> out;
        out.reserve(size_t(n > 0 ? n : 0));
        for (int i = 0; i < n; ++i) {
            out.push_back({raw[i].ip, raw[i].port, raw[i].rtt_ms, raw[i].name, raw[i].os, raw[i].simd});
        }
        return out;
    }
};

class Sender {
public:
    Sender() : handle_(lv_sender_create()) {}
    ~Sender() {
        if (handle_) lv_sender_destroy(handle_);
    }

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    Sender& operator=(Sender&& o) noexcept {
        if (this != &o) {
            if (handle_) lv_sender_destroy(handle_);
            handle_ = o.handle_;
            o.handle_ = nullptr;
        }
        return *this;
    }

    static LV_SenderConfig default_config(const std::string& dest_ip = "127.0.0.1",
                                          uint16_t dest_port = 5000) {
        LV_SenderConfig c{};
        lv_sdk_sender_config_default(&c);
        std::snprintf(c.dest_ip, sizeof(c.dest_ip), "%s", dest_ip.c_str());
        c.dest_port = dest_port;
        return c;
    }

    Result<void> start(const LV_SenderConfig& cfg) {
        const int rc = lv_sdk_sender_start(handle_, &cfg);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    void stop() { lv_sender_stop(handle_); }

    Result<void> push_frame(const uint8_t* data, int width, int height, int stride = 0,
                            LV_PixelFormat fmt = LV_PIXEL_FORMAT_BGRA8888, uint32_t ts_us = 0) {
        const int rc = lv_sender_push_packed_frame(handle_, data, width, height, stride, int(fmt), ts_us);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> push_yuv(const LV_YuvPlanes& planes, int width, int height, uint32_t ts_us = 0) {
        const int rc = lv_sender_push_yuv_planes(handle_, &planes, width, height, ts_us);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    // Pushes 1-2 channel stereo audio alongside video.
    Result<void> push_audio(const void* data, int num_frames, int sample_rate = 48000, int channels = 2,
                            LV_AudioFormat fmt = LV_AUDIO_FORMAT_PCM16, uint32_t ts_us = 0) {
        const int rc =
            lv_sender_push_audio_chunk(handle_, data, num_frames, sample_rate, channels, int(fmt), ts_us);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_quality(int quality) {
        const int rc = lv_sender_request_keyframe_quality(handle_, quality);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_bitrate_kbps(uint32_t bitrate_kbps) {
        const int rc = lv_sender_set_bitrate(handle_, bitrate_kbps);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> force_keyframe() {
        const int rc = lv_sender_force_keyframe(handle_);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_fec_group(int group_size) {
        const int rc = lv_sender_set_fec_group(handle_, group_size);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_adaptive_quality(bool enabled, int q_min = 35, int q_max = 85) {
        const int rc = lv_sender_set_adaptive_quality(handle_, enabled ? 1 : 0, q_min, q_max);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    LV_StreamStats stats() const {
        LV_StreamStats s{};
        lv_sender_get_stream_stats(handle_, &s);
        return s;
    }

private:
    LvSenderHandle* handle_ = nullptr;
};

class Receiver {
public:
    using VideoCallback = std::function<void(const LV_VideoFrameView&)>;
    using AudioCallback = std::function<void(const LV_AudioChunkView&)>;
    using StateCallback = std::function<void(LV_StreamState, LV_ErrorCode, const char*)>;

    Receiver() : handle_(lv_receiver_create()) {}
    ~Receiver() {
        if (handle_) {
            lv_receiver_set_video_callback(handle_, nullptr, nullptr);
            lv_receiver_set_audio_callback(handle_, nullptr, nullptr);
            lv_receiver_set_state_callback(handle_, nullptr, nullptr);
            lv_receiver_destroy(handle_);
        }
    }

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    static LV_ReceiverConfig default_config(uint16_t bind_port = 5000,
                                            LV_PixelFormat out_fmt = LV_PIXEL_FORMAT_BGRA8888) {
        LV_ReceiverConfig c{};
        lv_sdk_receiver_config_default(&c);
        c.bind_port = bind_port;
        c.output_format = int(out_fmt);
        return c;
    }

    void on_video(VideoCallback cb) {
        bool has_cb = false;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            video_cb_ = std::move(cb);
            has_cb = static_cast<bool>(video_cb_);
        }
        if (handle_) {
            lv_receiver_set_video_callback(handle_, has_cb ? &Receiver::video_trampoline : nullptr, this);
        }
    }

    void on_audio(AudioCallback cb) {
        bool has_cb = false;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            audio_cb_ = std::move(cb);
            has_cb = static_cast<bool>(audio_cb_);
        }
        if (handle_) {
            lv_receiver_set_audio_callback(handle_, has_cb ? &Receiver::audio_trampoline : nullptr, this);
        }
    }

    void on_state(StateCallback cb) {
        bool has_cb = false;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            state_cb_ = std::move(cb);
            has_cb = static_cast<bool>(state_cb_);
        }
        if (handle_) {
            lv_receiver_set_state_callback(handle_, has_cb ? &Receiver::state_trampoline : nullptr, this);
        }
    }

    Result<void> start(const LV_ReceiverConfig& cfg) {
        const int rc = lv_sdk_receiver_start(handle_, &cfg);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    void stop() { lv_receiver_stop(handle_); }
    uint16_t local_port() const { return lv_receiver_get_local_port(handle_); }
    void set_volume(float vol) { lv_receiver_set_volume(handle_, vol); }
    void set_concealment(LV_ConcealmentMode m) { lv_receiver_set_concealment(handle_, int(m)); }
    Result<void> set_audio_target_ms(double ms) {
        const int rc = lv_receiver_set_audio_target_ms(handle_, ms);
        return rc == LV_OK ? Result<void>::ok() : Result<void>::err(rc);
    }
    bool save_screenshot(const std::string& ppm_path) {
        return lv_receiver_save_screenshot(handle_, ppm_path.c_str()) != 0;
    }
    int pop_audio_pcm16(int16_t* stereo_out, int num_frames) {
        return lv_receiver_pop_audio_pcm16(handle_, stereo_out, num_frames);
    }

    LV_StreamStats stats() const {
        LV_StreamStats s{};
        lv_receiver_get_stream_stats(handle_, &s);
        return s;
    }

private:
    static void video_trampoline(const LV_VideoFrameView* f, void* ud) {
        if (!f || !ud) return;
        auto* self = static_cast<Receiver*>(ud);
        VideoCallback cb;
        {
            std::lock_guard<std::mutex> lk(self->cb_mu_);
            cb = self->video_cb_;
        }
        if (cb) cb(*f);
    }
    static void audio_trampoline(const LV_AudioChunkView* a, void* ud) {
        if (!a || !ud) return;
        auto* self = static_cast<Receiver*>(ud);
        AudioCallback cb;
        {
            std::lock_guard<std::mutex> lk(self->cb_mu_);
            cb = self->audio_cb_;
        }
        if (cb) cb(*a);
    }
    static void state_trampoline(int st, int err, const char* msg, void* ud) {
        if (!ud) return;
        auto* self = static_cast<Receiver*>(ud);
        StateCallback cb;
        {
            std::lock_guard<std::mutex> lk(self->cb_mu_);
            cb = self->state_cb_;
        }
        if (cb) cb(LV_StreamState(st), LV_ErrorCode(err), msg ? msg : "");
    }

    LvReceiverHandle* handle_ = nullptr;
    mutable std::mutex cb_mu_;
    VideoCallback video_cb_;
    AudioCallback audio_cb_;
    StateCallback state_cb_;
};

}  // namespace lanvid
