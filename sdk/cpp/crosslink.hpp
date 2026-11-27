#pragma once
// CrossLink Audio SDK (v1.0.0) — Modern C++17/C++20 Header-Only RAII Wrapper
// 64-channel AoIP, MasterClock, Full-Duplex DAW Bus, MIDI2, ControlSync, and Personal Monitor Bus.
// Communicates strictly through the stable C ABI (crosslink/crosslink_sdk.h).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crosslink/crosslink_sdk.h"

namespace crosslink {

template <typename T>
class Result {
public:
    static Result ok(T val) { return Result(std::move(val), CL_OK); }
    static Result err(int code, std::string msg = {}) {
        return Result(std::nullopt, code, msg.empty() ? cl_error_string(code) : std::move(msg));
    }

    explicit operator bool() const noexcept { return code_ == CL_OK && value_.has_value(); }
    bool is_ok() const noexcept { return code_ == CL_OK && value_.has_value(); }
    int error_code() const noexcept { return code_; }
    const std::string& error_message() const noexcept { return message_; }
    const T& value() const { return *value_; }
    T& value() { return *value_; }

private:
    Result(std::optional<T> v, int c, std::string m = {})
        : value_(std::move(v)), code_(c), message_(std::move(m)) {}
    std::optional<T> value_;
    int code_ = CL_OK;
    std::string message_;
};

template <>
class Result<void> {
public:
    static Result ok() { return Result(CL_OK); }
    static Result err(int code, std::string msg = {}) {
        return Result(code, msg.empty() ? cl_error_string(code) : std::move(msg));
    }

    explicit operator bool() const noexcept { return code_ == CL_OK; }
    bool is_ok() const noexcept { return code_ == CL_OK; }
    int error_code() const noexcept { return code_; }
    const std::string& error_message() const noexcept { return message_; }

private:
    Result(int c, std::string m = {}) : code_(c), message_(std::move(m)) {}
    int code_ = CL_OK;
    std::string message_;
};

class Runtime {
public:
    Runtime() { cl_init(); }
    ~Runtime() { cl_shutdown(); }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    static std::string version() { return cl_get_version_string(); }
    static bool check_abi() {
        return cl_check_abi_compatibility(CL_SDK_VERSION_MAJOR, CL_SDK_VERSION_MINOR) != 0;
    }
    static std::string simd_info() { return cl_get_cpu_simd_info(); }
    static void set_simd_enabled(bool enabled) { cl_set_simd_enabled(enabled ? 1 : 0); }
};

class Discovery {
public:
    static std::vector<CL_AudioNode> scan(int timeout_ms = 400, uint16_t discovery_port = 0) {
        CL_AudioNode raw[64];
        const int n = cl_discovery_scan_audio_nodes(timeout_ms, discovery_port, raw, 64);
        std::vector<CL_AudioNode> out;
        if (n > 0) out.assign(raw, raw + n);
        return out;
    }
};

class Sender {
public:
    Sender() : handle_(cl_sender_create()) {}
    ~Sender() {
        if (handle_) cl_sender_destroy(handle_);
    }

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    Sender& operator=(Sender&& o) noexcept {
        if (this != &o) {
            if (handle_) cl_sender_destroy(handle_);
            handle_ = o.handle_;
            o.handle_ = nullptr;
        }
        return *this;
    }

    static CL_SenderConfig default_config(const std::string& dest_ip = "127.0.0.1",
                                          uint16_t dest_port = 5004,
                                          uint32_t latency_ms = 20,
                                          int32_t sample_rate = 48000) {
        CL_SenderConfig c{};
        cl_sender_config_default(&c);
        std::snprintf(c.dest_ip, sizeof(c.dest_ip), "%s", dest_ip.c_str());
        c.dest_port = dest_port;
        c.latency_ms = latency_ms;
        c.sample_rate = sample_rate;
        return c;
    }

    Result<void> configure_audio_bus(const CL_AudioBusConfig& bus) {
        const int rc = cl_sender_configure_audio_bus(handle_, &bus);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> start(const CL_SenderConfig& cfg) {
        const int rc = cl_sender_start(handle_, &cfg);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    void stop() { cl_sender_stop(handle_); }
    uint16_t local_port() const { return cl_sender_get_local_port(handle_); }

    Result<void> push_audio_channel(uint16_t channel, const void* samples, uint32_t num_samples,
                                    CL_AudioFormat fmt = CL_AUDIO_FORMAT_F32,
                                    uint64_t media_ts = CL_MEDIA_TS_AUTO) {
        const int rc = cl_sender_push_audio_channel(handle_, channel, samples, num_samples, fmt, media_ts);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> push_audio_multichannel(const void* data, uint16_t num_channels, uint32_t num_samples,
                                         CL_AudioFormat fmt = CL_AUDIO_FORMAT_F32,
                                         CL_AudioLayout layout = CL_AUDIO_LAYOUT_INTERLEAVED,
                                         uint64_t media_ts = CL_MEDIA_TS_AUTO) {
        const int rc = cl_sender_push_audio_multichannel(handle_, data, num_channels, num_samples, fmt,
                                                         layout, media_ts);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> push_multichannel(const void* data, uint16_t num_channels, uint32_t num_samples,
                                   CL_AudioFormat fmt = CL_AUDIO_FORMAT_F32,
                                   CL_AudioLayout layout = CL_AUDIO_LAYOUT_INTERLEAVED,
                                   uint64_t media_ts = CL_MEDIA_TS_AUTO) {
        return push_audio_multichannel(data, num_channels, num_samples, fmt, layout, media_ts);
    }

    Result<void> send_control_sync(const CL_ControlSyncState& state) {
        const int rc = cl_sender_send_control_sync(handle_, &state);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> push_midi(uint8_t status, uint8_t d1, uint8_t d2, uint8_t port = 0,
                           uint64_t media_ts_us = 0) {
        const int rc = cl_sender_push_midi(handle_, status, d1, d2, port, media_ts_us);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_audio_latency_ms(uint32_t ms) {
        const int rc = cl_sender_set_audio_latency_ms(handle_, ms);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_audio_codec(int codec) {
        const int rc = cl_sender_set_audio_codec(handle_, codec);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_audio_bitrate(int kbps_per_ch) {
        const int rc = cl_sender_set_audio_bitrate(handle_, kbps_per_ch);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_wifi_redundancy(bool enabled, uint32_t stagger_us = 1500) {
        const int rc = cl_sender_set_wifi_redundancy(handle_, enabled ? 1 : 0, stagger_us);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    uint64_t master_clock_samples() const { return cl_sender_get_master_clock_samples(handle_); }

    CL_StreamStats stats() const {
        CL_StreamStats s{};
        cl_sender_get_stream_stats(handle_, &s);
        return s;
    }

private:
    ClSenderHandle* handle_ = nullptr;
};

class Receiver {
public:
    using MultichannelCallback = std::function<void(const CL_MultichannelAudioBlock&)>;
    using MidiCallback = std::function<void(const CL_MidiEvent&)>;
    using ControlSyncCallback = std::function<void(const CL_ControlSyncState&)>;
    using StateCallback = std::function<void(CL_StreamState, CL_ErrorCode, const char*)>;

    Receiver() : handle_(cl_receiver_create()) {}
    ~Receiver() {
        if (handle_) {
            cl_receiver_set_multichannel_audio_callback(handle_, nullptr, nullptr);
            cl_receiver_set_midi_callback(handle_, nullptr, nullptr);
            cl_receiver_set_control_sync_callback(handle_, nullptr, nullptr);
            cl_receiver_set_state_callback(handle_, nullptr, nullptr);
            cl_receiver_destroy(handle_);
        }
    }

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    static CL_ReceiverConfig default_config(uint16_t bind_port = 5004,
                                            uint32_t latency_ms = 20,
                                            int32_t num_channels = 64,
                                            int32_t sample_rate = 48000) {
        CL_ReceiverConfig c{};
        cl_receiver_config_default(&c);
        c.bind_port = bind_port;
        c.latency_ms = latency_ms;
        c.num_channels = num_channels;
        c.sample_rate = sample_rate;
        return c;
    }

    Result<void> configure_audio_bus(const CL_AudioBusConfig& bus) {
        const int rc = cl_receiver_configure_audio_bus(handle_, &bus);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> start(const CL_ReceiverConfig& cfg) {
        const int rc = cl_receiver_start(handle_, &cfg);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    void stop() { cl_receiver_stop(handle_); }
    uint16_t local_port() const { return cl_receiver_get_local_port(handle_); }

    Result<void> set_audio_latency_ms(uint32_t ms) {
        const int rc = cl_receiver_set_audio_latency_ms(handle_, ms);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    Result<void> set_adaptive_jitter(bool enabled) {
        const int rc = cl_receiver_set_adaptive_jitter(handle_, enabled ? 1 : 0);
        return rc == CL_OK ? Result<void>::ok() : Result<void>::err(rc);
    }

    double effective_latency_ms() const { return cl_receiver_get_effective_latency_ms(handle_); }

    void on_multichannel_audio(MultichannelCallback cb) {
        bool has_cb = false;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            mc_cb_ = std::move(cb);
            has_cb = static_cast<bool>(mc_cb_);
        }
        if (handle_) {
            cl_receiver_set_multichannel_audio_callback(handle_, has_cb ? &Receiver::mc_trampoline : nullptr,
                                                        this);
        }
    }

    void on_midi(MidiCallback cb) {
        bool has_cb = false;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            midi_cb_ = std::move(cb);
            has_cb = static_cast<bool>(midi_cb_);
        }
        if (handle_) {
            cl_receiver_set_midi_callback(handle_, has_cb ? &Receiver::midi_trampoline : nullptr, this);
        }
    }

    void on_control_sync(ControlSyncCallback cb) {
        bool has_cb = false;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            cs_cb_ = std::move(cb);
            has_cb = static_cast<bool>(cs_cb_);
        }
        if (handle_) {
            cl_receiver_set_control_sync_callback(handle_, has_cb ? &Receiver::cs_trampoline : nullptr, this);
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
            cl_receiver_set_state_callback(handle_, has_cb ? &Receiver::state_trampoline : nullptr, this);
        }
    }

    int pop_audio_multichannel(float* out, uint16_t num_channels, uint32_t num_frames,
                               CL_AudioLayout layout = CL_AUDIO_LAYOUT_INTERLEAVED) {
        return cl_receiver_pop_audio_multichannel(handle_, out, num_channels, num_frames, layout);
    }

    int pop_multichannel(float* out, uint16_t num_channels, uint32_t num_frames,
                         CL_AudioLayout layout = CL_AUDIO_LAYOUT_INTERLEAVED) {
        return pop_audio_multichannel(out, num_channels, num_frames, layout);
    }

    CL_AudioChannelStats channel_stats(uint16_t channel) const {
        CL_AudioChannelStats st{};
        cl_receiver_get_channel_stats(handle_, channel, &st);
        return st;
    }

    CL_StreamStats stats() const {
        CL_StreamStats s{};
        cl_receiver_get_stream_stats(handle_, &s);
        return s;
    }

private:
    static void mc_trampoline(const CL_MultichannelAudioBlock* b, void* ud) {
        if (!b || !ud) return;
        auto* self = static_cast<Receiver*>(ud);
        MultichannelCallback cb;
        {
            std::lock_guard<std::mutex> lk(self->cb_mu_);
            cb = self->mc_cb_;
        }
        if (cb) cb(*b);
    }
    static void midi_trampoline(const CL_MidiEvent* ev, void* ud) {
        if (!ev || !ud) return;
        auto* self = static_cast<Receiver*>(ud);
        MidiCallback cb;
        {
            std::lock_guard<std::mutex> lk(self->cb_mu_);
            cb = self->midi_cb_;
        }
        if (cb) cb(*ev);
    }
    static void cs_trampoline(const CL_ControlSyncState* cs, void* ud) {
        if (!cs || !ud) return;
        auto* self = static_cast<Receiver*>(ud);
        ControlSyncCallback cb;
        {
            std::lock_guard<std::mutex> lk(self->cb_mu_);
            cb = self->cs_cb_;
        }
        if (cb) cb(*cs);
    }
    static void state_trampoline(int st, int err, const char* msg, void* ud) {
        if (!ud) return;
        auto* self = static_cast<Receiver*>(ud);
        StateCallback cb;
        {
            std::lock_guard<std::mutex> lk(self->cb_mu_);
            cb = self->state_cb_;
        }
        if (cb) cb(CL_StreamState(st), CL_ErrorCode(err), msg ? msg : "");
    }

    ClReceiverHandle* handle_ = nullptr;
    mutable std::mutex cb_mu_;
    MultichannelCallback mc_cb_;
    MidiCallback midi_cb_;
    ControlSyncCallback cs_cb_;
    StateCallback state_cb_;
};

}  // namespace crosslink
