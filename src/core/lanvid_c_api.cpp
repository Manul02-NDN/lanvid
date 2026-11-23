#include "lanvid/lanvid_sdk.h"
#ifndef LANVID_SDK_CORE_ONLY
#include "lanvid_c_api.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "codec/dct.hpp"
#include "codec/video_codec_common.hpp"
#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "common/rt_thread.hpp"
#include "common/cpu_features.hpp"
#include "common/image.hpp"
#include "net/discovery.hpp"
#include "audio/channel_patchbay.hpp"
#include "audio/intercom_engine.hpp"
#include "audio/personal_monitor_bus.hpp"
#ifdef LV_HAVE_SHM_BUS
#include "audio/shm_audio_host.hpp"
#endif
#include "net/receiver.hpp"
#include "net/sender.hpp"

#ifndef LANVID_SDK_CORE_ONLY
#include "app/file_source.hpp"
#include "app/test_pattern.hpp"
#include "platform/audio_capture.hpp"
#include "platform/audio_playback.hpp"
#include "platform/camera_capture.hpp"
#include "platform/screen_capture.hpp"
#endif

namespace {

char* dup_cstr(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
                    out += buf;
                } else {
                    out.push_back(char(c));
                }
        }
    }
    return out;
}

void copy_image_order(const lv::Image& src, uint8_t* dst, bool want_rgba) {
    const lv::PixelOrder target = want_rgba ? lv::PixelOrder::kRGBA : lv::PixelOrder::kBGRA;
    if (src.order == target) {
        std::memcpy(dst, src.data.data(), src.data.size());
        return;
    }
    const size_t pixels = size_t(src.width) * size_t(src.height);
    const uint8_t* s = src.data.data();
    for (size_t i = 0; i < pixels; ++i) {
        dst[i * 4 + 0] = s[i * 4 + 2];
        dst[i * 4 + 1] = s[i * 4 + 1];
        dst[i * 4 + 2] = s[i * 4 + 0];
        dst[i * 4 + 3] = 255;
    }
}

double percentile_vec(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t idx = std::min(v.size() - 1, size_t(double(v.size() - 1) * p));
    return v[idx];
}

std::atomic<int> g_init_refs{0};

}  // namespace

// Lifetime lock (see LV_LIVE_LOCK): the engine object is swapped out under an exclusive lock and destroyed
// outside it; every entry point that other threads call (audio/MIDI pushes, pops, clock, live setters)
// holds a shared lock while it uses the object. Stopping or restarting a handle (e.g. toggling Audio Only in
// LanVid Studio) therefore waits for an in-flight push from the matrix mixer thread instead of freeing the
// object under it (EXC_BAD_ACCESS in UdpSender::push_audio_channel). Callbacks the engine invokes must not
// re-enter the C ABI on the same handle.
#define LV_LIVE_LOCK(h) std::shared_lock<std::shared_mutex> lv_live_lock_((h)->life_mu)

struct LvSenderHandle {
    std::mutex api_mu;
    mutable std::shared_mutex life_mu;
    std::unique_ptr<lv::UdpSender> sender;
#ifndef LANVID_SDK_CORE_ONLY
    std::unique_ptr<lv::AudioCapture> audio_cap;
    std::thread cap_thread;
    LvSenderConfig legacy_cfg{};
#endif
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> state{LV_STATE_STOPPED};
    std::atomic<int> video_source_error{0};  // LvVideoSourceError: why the video source could not start

    lv::CodecParams codec_params{};
    std::atomic<int> override_quality{0};  // 0 = use codec_params/AIMD, >0 = live override
    std::atomic<bool> adaptive_enabled{false};
    std::atomic<uint32_t> ext_frame_id{0};
    std::atomic<uint32_t> ext_audio_seq{0};
    LV_AudioBusConfig bus_cfg{48000, 20, 64, 240};

    std::mutex push_mu;
    lv::Image ext_img;
    std::vector<uint8_t> yuv_scratch;

    std::mutex preview_mu;
    lv::Image preview_img;
    uint64_t preview_frame_id = 0;

    std::mutex stats_mu;
    uint64_t win_start_us = 0;
    uint64_t win_prev_frames = 0;
    uint64_t win_prev_bytes = 0;
    uint64_t win_prev_ab = 0;
    double enc_ms_sum = 0.0;
    uint64_t enc_ms_cnt = 0;
    double cached_fps = 0.0;
    double cached_mbps = 0.0;
    double cached_enc_ms = 0.0;
    double cached_audio_kbps = 0.0;

    std::mutex cs_cb_mu;
    LV_ControlSyncCallback cs_cb = nullptr;
    void* cs_ud = nullptr;
    int cur_w = 0;
    int cur_h = 0;

    int resolve_quality() const {
        if (adaptive_enabled.load(std::memory_order_relaxed) && sender) {
            return sender->active_quality();
        }
        const int oq = override_quality.load(std::memory_order_relaxed);
        return oq > 0 ? oq : codec_params.quality;
    }

    void update_preview(const lv::Image& img, uint64_t fid) {
        std::lock_guard<std::mutex> lk(preview_mu);
        if (preview_img.width != img.width || preview_img.height != img.height ||
            preview_img.order != img.order) {
            preview_img.allocate(img.width, img.height, img.order);
        }
        std::memcpy(preview_img.data.data(), img.data.data(), img.data.size());
        preview_frame_id = fid;
    }

    void record_encode_sample(double ms, int w, int h) {
        std::lock_guard<std::mutex> lk(stats_mu);
        enc_ms_sum += ms;
        ++enc_ms_cnt;
        cur_w = w;
        cur_h = h;
    }

    void reset_stats(int w, int h) {
        std::lock_guard<std::mutex> slk(stats_mu);
        win_start_us = lv::now_us();
        win_prev_frames = 0;
        win_prev_bytes = 0;
        win_prev_ab = 0;
        enc_ms_sum = 0.0;
        enc_ms_cnt = 0;
        cached_fps = 0.0;
        cached_mbps = 0.0;
        cached_enc_ms = 0.0;
        cached_audio_kbps = 0.0;
        cur_w = w;
        cur_h = h;
    }

    void stop_internal() {
        stop.store(true, std::memory_order_release);
#ifndef LANVID_SDK_CORE_ONLY
        if (audio_cap) {
            audio_cap->stop();
            audio_cap.reset();
        }
        if (cap_thread.joinable()) cap_thread.join();
#endif
        running.store(false, std::memory_order_release);
        std::unique_ptr<lv::UdpSender> dead;
        {
            std::unique_lock<std::shared_mutex> lk(life_mu);  // waits for in-flight pushes
            dead = std::move(sender);
        }
        dead.reset();  // joins engine threads without holding the lock
        state.store(LV_STATE_STOPPED, std::memory_order_release);
    }

    ~LvSenderHandle() { stop_internal(); }
};

struct LvReceiverHandle {
    std::mutex api_mu;
    mutable std::shared_mutex life_mu;
    std::unique_ptr<lv::UdpReceiver> receiver;
#ifndef LANVID_SDK_CORE_ONLY
    std::unique_ptr<lv::AudioPlayback> audio_play;
#endif
    std::unique_ptr<lv::DiscoveryResponder> discovery;
    std::thread rx_thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> state{LV_STATE_STOPPED};
    std::atomic<float> volume{1.0f};
    std::atomic<bool> audio_started{false};
    std::atomic<int> adaptive_jitter{1};  // 1=Adaptive Wi-Fi (default), 0=Fixed; survives restarts
    bool connected_notified = false;

    // Zero-copy SDK callbacks
    std::mutex cb_mu;
    LV_VideoFrameCallback video_cb = nullptr;
    void* video_ud = nullptr;
    LV_AudioSamplesCallback audio_cb = nullptr;
    void* audio_ud = nullptr;
    LV_StateChangedCallback state_cb = nullptr;
    LV_MidiEventCallback midi_cb = nullptr;
    void* midi_ud = nullptr;
    LV_ControlSyncCallback cs_cb = nullptr;
    void* cs_ud = nullptr;
    void* state_ud = nullptr;

    // 64-channel bus
    LV_AudioBusConfig bus_cfg{48000, 20, 64, 240};
    LV_MultichannelAudioCallback mc_cb = nullptr;
    void* mc_ud = nullptr;
    std::thread mc_thread;
    std::atomic<bool> mc_stop{false};
    std::atomic<bool> mc_active{false};

    void stop_mc_pump() {
        mc_stop.store(true, std::memory_order_release);
        if (mc_thread.joinable()) mc_thread.join();
        mc_active.store(false, std::memory_order_release);
    }
    void start_mc_pump() {  // caller holds api_mu; receiver must be running
        stop_mc_pump();
        if (!mc_cb || !receiver) return;
        mc_stop.store(false, std::memory_order_release);
        mc_active.store(true, std::memory_order_release);
        mc_thread = std::thread([this]() {
            lv::RealtimeThreadScope rt(uint32_t(uint64_t(bus_cfg.block_frames ? bus_cfg.block_frames : 240) * 1000000ull /
                                                uint64_t(bus_cfg.sample_rate > 0 ? bus_cfg.sample_rate : 48000)));
            const uint32_t frames = bus_cfg.block_frames ? bus_cfg.block_frames : 240;
            const int ch = std::clamp(bus_cfg.num_channels, 1, 64);
            const uint32_t rate = uint32_t(bus_cfg.sample_rate);
            std::vector<float> buf(size_t(frames) * size_t(ch));
            const uint64_t period_us = uint64_t(frames) * 1000000ull / rate;
            uint64_t next = lv::now_us() + period_us;
            uint64_t index = 0;
            while (!mc_stop.load(std::memory_order_acquire)) {
                lv::sleep_until_us(next, [] {});
                next += period_us;
                if (!receiver) break;
                receiver->pop_audio_multichannel_interleaved(buf.data(), uint32_t(ch), frames);
                LV_MultichannelAudioCallback cb;
                void* ud;
                {
                    std::lock_guard<std::mutex> lk(cb_mu);
                    cb = mc_cb;
                    ud = mc_ud;
                }
                if (!cb) continue;
                LV_MultichannelAudioBlock blk{};
                blk.data = buf.data();
                blk.num_channels = ch;
                blk.num_frames = int32_t(frames);
                blk.sample_rate = int32_t(rate);
                const double cur = receiver->audio_matrix().playout_cursor();
                blk.playout_position_samples = cur > double(frames) ? uint64_t(cur) - frames : 0;
                blk.block_index = index++;
                cb(&blk, ud);
            }
        });
    }

    // Latest decoded frame buffer (for optional polling & screenshot)
    std::mutex frame_mu;
    lv::Image latest_img;
    uint32_t latest_fid = 0;
    bool has_frame = false;

    // Rolling 1-second stats
    std::mutex stats_mu;
    uint64_t win_start_us = 0;
    uint64_t win_frames = 0;
    uint64_t win_prev_bytes = 0;
    uint64_t win_prev_ab = 0;
    uint64_t win_prev_rx = 0;
    uint64_t win_prev_lost = 0;
    std::vector<double> win_lats;
    double cached_fps = 0.0;
    double cached_mbps = 0.0;
    double cached_p50 = 0.0;
    double cached_p99 = 0.0;
    double cached_loss = 0.0;
    double cached_akbps = 0.0;
    int cur_w = 0;
    int cur_h = 0;
    int cur_slices = 0;
    int cur_quality = 0;

    void notify_state(int new_state, int err_code, const char* msg) {
        state.store(new_state, std::memory_order_release);
        LV_StateChangedCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk(cb_mu);
            cb = state_cb;
            ud = state_ud;
        }
        if (cb) cb(new_state, err_code, msg ? msg : "", ud);
    }

    int start_common(const lv::ReceiverConfig& rcfg, bool auto_os_audio, bool enable_discovery,
                     uint16_t discovery_port, const std::string& device_name) {
        stop_internal();
        notify_state(LV_STATE_STARTING, LV_OK, "Starting receiver");
        connected_notified = false;

        {
            std::lock_guard<std::mutex> flk(frame_mu);
            has_frame = false;
            latest_fid = 0;
        }
        {
            std::lock_guard<std::mutex> slk(stats_mu);
            win_start_us = lv::now_us();
            win_frames = 0;
            win_prev_bytes = 0;
            win_prev_ab = 0;
            win_prev_rx = 0;
            win_prev_lost = 0;
            win_lats.clear();
            cached_fps = cached_mbps = cached_p50 = cached_p99 = cached_loss = cached_akbps = 0.0;
            cur_w = cur_h = cur_slices = cur_quality = 0;
        }

        auto rx = std::make_unique<lv::UdpReceiver>(rcfg, [this](const lv::CompletedFrame& cf) {
            if (!cf.image) return;
            const lv::Image& img = *cf.image;

            if (!connected_notified) {
                connected_notified = true;
                notify_state(LV_STATE_CONNECTED, LV_OK, "Stream active");
            }

            // 1. Zero-copy push delivery directly to registered SDK video callback!
            LV_VideoFrameCallback vcb = nullptr;
            void* vud = nullptr;
            {
                std::lock_guard<std::mutex> clk(cb_mu);
                vcb = video_cb;
                vud = video_ud;
            }
            if (vcb) {
                LV_VideoFrameView view{};
                view.data = img.data.data();
                view.size = img.data.size();
                view.width = img.width;
                view.height = img.height;
                view.stride = int32_t(img.stride);
                view.pixel_format = (img.order == lv::PixelOrder::kRGBA) ? LV_PIXEL_FORMAT_RGBA8888
                                                                         : LV_PIXEL_FORMAT_BGRA8888;
                view.frame_id = cf.frame_id;
                view.timestamp_us = cf.sender_timestamp_us;
                view.slices_ok = cf.slices_ok;
                view.slices_total = cf.total_slices;
                view.quality = cf.quality;
                view.codec = uint8_t(cf.codec);
                view.is_keyframe = cf.is_keyframe ? 1 : 0;
                view.reserved = 0;
                vcb(&view, vud);
            }

            // 2. Snapshot latest frame for optional polling / screenshot API
            {
                std::lock_guard<std::mutex> flk(frame_mu);
                if (latest_img.width != img.width || latest_img.height != img.height ||
                    latest_img.order != img.order) {
                    latest_img.allocate(img.width, img.height, img.order);
                }
                std::memcpy(latest_img.data.data(), img.data.data(), img.data.size());
                latest_fid = latest_fid + 1;
                if (latest_fid == 0) latest_fid = 1;
                has_frame = true;
            }
            {
                const double lat_ms = receiver
                                          ? receiver->estimate_latency_ms(cf.complete_time_us,
                                                                          cf.sender_timestamp_us)
                                          : 0.0;
                std::lock_guard<std::mutex> slk(stats_mu);
                ++win_frames;
                win_lats.push_back(lat_ms);
                cur_w = img.width;
                cur_h = img.height;
                cur_slices = cf.total_slices;
                cur_quality = cf.quality;
            }
        });

        rx->set_midi_callback([this](const lv::TimedMidiEvent& ev, int32_t off) {
            LV_MidiEventCallback cb = nullptr;
            void* ud = nullptr;
            {
                std::lock_guard<std::mutex> lk(cb_mu);
                cb = midi_cb;
                ud = midi_ud;
            }
            if (!cb) return;
            LV_MidiEvent e{};
            e.media_ts_us = ev.media_ts_us;
            e.event_seq = ev.event_seq;
            e.block_offset_frames = off;
            e.status = ev.status;
            e.data1 = ev.data1;
            e.data2 = ev.data2;
            e.port = ev.port_index;
            cb(&e, ud);
        });
        rx->set_control_sync_callback([this](const lv::ControlSyncPayload& cs) {
            LV_ControlSyncCallback cb = nullptr;
            void* ud = nullptr;
            {
                std::lock_guard<std::mutex> lk(cb_mu);
                cb = cs_cb;
                ud = cs_ud;
            }
            if (!cb) return;
            LV_ControlSyncState st{cs.revision, cs.origin_id, cs.audio_channels, cs.audio_bitrate_kbps,
                                   cs.target_buffer_ms, cs.latency_mode, cs.audio_only, cs.active_preset_id,
                                   cs.flags, cs.master_gain_cb, cs.audio_codec};
            cb(&st, ud);
        });
        rx->set_audio_callback([this](uint32_t seq_id, uint32_t ts_us, const lv::AudioHeader& ah,
                                      const uint8_t* pcm_data) {
            LV_AudioSamplesCallback acb = nullptr;
            void* aud = nullptr;
            {
                std::lock_guard<std::mutex> clk(cb_mu);
                acb = audio_cb;
                aud = audio_ud;
            }
            if (acb) {
                LV_AudioChunkView view{};
                view.data = pcm_data;
                view.num_frames = ah.num_frames;
                view.sample_rate = int32_t(ah.sample_rate);
                view.channels = ah.channels;
                view.audio_format = (ah.sample_format == lv::AudioSampleFormat::kFloat32)
                                        ? LV_AUDIO_FORMAT_F32
                                        : LV_AUDIO_FORMAT_PCM16;
                view.seq_id = seq_id;
                view.timestamp_us = ts_us;
                acb(&view, aud);
            }
        });

        if (!rx->init()) {
            notify_state(LV_STATE_ERROR, LV_ERR_SOCKET, "Failed to bind UDP receiver socket");
            return LV_ERR_SOCKET;
        }
        rx->audio_buffer().set_volume(volume.load(std::memory_order_relaxed));
        const uint16_t bound_port = rx->local_port();
        {
            std::unique_lock<std::shared_mutex> lk(life_mu);
            receiver = std::move(rx);
        }

        if (enable_discovery) {
            auto disc = std::make_unique<lv::DiscoveryResponder>();
            const uint16_t dport = discovery_port ? discovery_port : lv::kDiscoveryPort;
            disc->start(bound_port, device_name, dport);
            discovery = std::move(disc);
        }

        audio_started.store(false, std::memory_order_release);
        stop.store(false, std::memory_order_release);
        running.store(true, std::memory_order_release);
        notify_state(LV_STATE_RUNNING, LV_OK, "Listening");
        if (mc_cb) start_mc_pump();

        rx_thread = std::thread([this, auto_os_audio]() {
            lv::RealtimeThreadScope rt(2000, 400, 1500);  // network RX (audio / MIDI2 / clock sync)
            while (!stop.load(std::memory_order_relaxed)) {
                receiver->step_once();
#ifndef LANVID_SDK_CORE_ONLY
                if (auto_os_audio && !audio_started.load(std::memory_order_relaxed) &&
                    receiver->audio_stats().packets_rx > 0) {
                    auto ap = std::make_unique<lv::AudioPlayback>();
                    if (ap->start(&receiver->audio_buffer())) {
                        audio_play = std::move(ap);
                    }
                    audio_started.store(true, std::memory_order_release);
                }
#else
                (void)auto_os_audio;
#endif
            }
            receiver->flush();
            running.store(false, std::memory_order_release);
        });

        return LV_OK;
    }

    void stop_internal() {
        const bool was_running = running.load(std::memory_order_acquire) || receiver != nullptr;
        stop.store(true, std::memory_order_release);
        stop_mc_pump();
        if (discovery) {
            discovery->stop();
            discovery.reset();
        }
        if (rx_thread.joinable()) rx_thread.join();
#ifndef LANVID_SDK_CORE_ONLY
        if (audio_play) {
            audio_play->stop();
            audio_play.reset();
        }
#endif
        audio_started.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        std::unique_ptr<lv::UdpReceiver> dead;
        {
            std::unique_lock<std::shared_mutex> lk(life_mu);  // waits for in-flight pops
            dead = std::move(receiver);
        }
        dead.reset();
        if (was_running) notify_state(LV_STATE_STOPPED, LV_OK, "Stopped");
    }

    ~LvReceiverHandle() { stop_internal(); }
};

extern "C" {

/* ---- Version & Error Helpers ---- */
const char* lv_get_version_string(void) { return LV_SDK_VERSION_STRING; }

uint32_t lv_get_version_number(void) {
    return (uint32_t(LV_SDK_VERSION_MAJOR) << 16) | (uint32_t(LV_SDK_VERSION_MINOR) << 8) |
           uint32_t(LV_SDK_VERSION_PATCH);
}

int lv_check_abi_compatibility(int header_major, int header_minor) {
    if (header_major != LV_SDK_VERSION_MAJOR) return 0;
    if (header_minor > LV_SDK_VERSION_MINOR) return 0;
    return 1;
}

const char* lv_error_string(int err) {
    switch (err) {
        case LV_OK: return "OK";
        case LV_ERR_INVALID_ARG: return "Invalid argument";
        case LV_ERR_NOT_INITIALIZED: return "SDK not initialized";
        case LV_ERR_NOT_RUNNING: return "Stream not running";
        case LV_ERR_SOCKET: return "UDP socket bind/open error";
        case LV_ERR_CODEC: return "Codec parameter or encoding error";
        case LV_ERR_BUFFER_TOO_SMALL: return "Output buffer too small";
        case LV_ERR_UNSUPPORTED: return "Unsupported operation or pixel format";
        case LV_ERR_INTERNAL: return "Internal error";
        default: return "Unknown error";
    }
}

/* ---- Core Lifecycle ---- */
int lv_init(void) {
    g_init_refs.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

void lv_shutdown(void) {
    if (g_init_refs.load(std::memory_order_relaxed) > 0) {
        g_init_refs.fetch_sub(1, std::memory_order_relaxed);
    }
}

void lv_free_string(char* str) { std::free(str); }
const char* lv_get_cpu_simd_info(void) { return lv::dct_kernels_best().name; }
void lv_set_simd_enabled(int enabled) { lv::set_simd_disabled(enabled == 0); }

char* lv_enumerate_devices(void) {
    const auto ifaces = lv::list_network_interfaces();
    const unsigned hw_threads = std::max(1u, std::thread::hardware_concurrency());

    std::ostringstream os;
    os << "{";
    os << "\"os\":\"" << lv::local_os_name() << "\",";
    os << "\"hostname\":\"" << json_escape(lv::local_device_name()) << "\",";
    os << "\"simd\":\"" << lv::dct_kernels_best().name << "\",";
    os << "\"hw_threads\":" << hw_threads << ",";

#ifndef LANVID_SDK_CORE_ONLY
    const auto mons = lv::list_monitors();
    const auto cams = lv::list_cameras();
    const auto loop_devs = lv::list_audio_devices(true);
    const auto mic_devs = lv::list_audio_devices(false);

    os << "\"monitors\":[";
    for (size_t i = 0; i < mons.size(); ++i) {
        if (i) os << ",";
        os << "{\"index\":" << mons[i].index << ",\"name\":\"" << json_escape(mons[i].name)
           << "\",\"width\":" << mons[i].width << ",\"height\":" << mons[i].height
           << ",\"left\":" << mons[i].left << ",\"top\":" << mons[i].top
           << ",\"primary\":" << (mons[i].primary ? "true" : "false") << "}";
    }
    os << "],\"cameras\":[";
    for (size_t i = 0; i < cams.size(); ++i) {
        if (i) os << ",";
        os << "{\"index\":" << cams[i].index << ",\"name\":\"" << json_escape(cams[i].name) << "\"}";
    }
    os << "],\"audio_loopback\":[";
    for (size_t i = 0; i < loop_devs.size(); ++i) {
        if (i) os << ",";
        os << "{\"index\":" << loop_devs[i].index << ",\"name\":\"" << json_escape(loop_devs[i].name)
           << "\",\"is_default\":" << (loop_devs[i].is_default ? "true" : "false") << "}";
    }
    os << "],\"audio_mic\":[";
    for (size_t i = 0; i < mic_devs.size(); ++i) {
        if (i) os << ",";
        os << "{\"index\":" << mic_devs[i].index << ",\"name\":\"" << json_escape(mic_devs[i].name)
           << "\",\"is_default\":" << (mic_devs[i].is_default ? "true" : "false") << "}";
    }
    os << "],";
#else
    os << "\"monitors\":[],\"cameras\":[],\"audio_loopback\":[],\"audio_mic\":[],";
#endif

    os << "\"network_interfaces\":[";
    for (size_t i = 0; i < ifaces.size(); ++i) {
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(ifaces[i].name) << "\",\"ip\":\""
           << json_escape(ifaces[i].ipv4) << "\",\"broadcast\":\""
           << json_escape(ifaces[i].broadcast_ipv4)
           << "\",\"loopback\":" << (ifaces[i].is_loopback ? "true" : "false") << "}";
    }
    os << "]}";
    return dup_cstr(os.str());
}

char* lv_get_supported_codecs(void) {
    return dup_cstr(lv::supported_codecs_json());
}

/* ---- SDK Default Configs ---- */
void lv_sdk_sender_config_default(LV_SenderConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->dest_ip, sizeof(cfg->dest_ip), "127.0.0.1");
    cfg->dest_port = 5000;
    cfg->width = 1920;
    cfg->height = 1080;
    cfg->fps = 60.0;
    cfg->quality = 80;
    cfg->chroma = LV_CHROMA_420;
    cfg->slice_height = 16;
    cfg->mtu = int(lv::kDefaultMtu);
    cfg->enable_crc = 1;
    cfg->fec_group_size = 6;
    cfg->adaptive_quality = 0;
    cfg->q_min = 35;
    cfg->q_max = 85;
    cfg->threads = 0;
    cfg->pace_mbps = 0.0;
    cfg->codec = LV_CODEC_LANVID_DCT;
    cfg->bitrate_kbps = 8000;
    cfg->keyframe_interval = 120;
}

void lv_sdk_receiver_config_default(LV_ReceiverConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->bind_ip, sizeof(cfg->bind_ip), "0.0.0.0");
    cfg->bind_port = 5000;
    cfg->discovery_port = lv::kDiscoveryPort;
    cfg->output_format = LV_PIXEL_FORMAT_BGRA8888;
    cfg->decode_threads = 0;
    cfg->conceal_mode = LV_CONCEALMENT_PREV_FRAME;
    cfg->frame_timeout_ms = 25.0;
    cfg->audio_target_ms = 12.0;
    cfg->enable_os_audio = 0;
    cfg->enable_discovery = 1;
}

/* ---- Sender API ---- */
LvSenderHandle* lv_sender_create(void) { return new LvSenderHandle(); }
void lv_sender_destroy(LvSenderHandle* handle) { delete handle; }

int lv_sdk_sender_start(LvSenderHandle* handle, const LV_SenderConfig* cfg) {
    if (!handle || !cfg) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
    handle->state.store(LV_STATE_STARTING, std::memory_order_release);

    lv::SenderConfig scfg;
    scfg.dest_ip = cfg->dest_ip[0] ? cfg->dest_ip : "127.0.0.1";
    scfg.dest_port = cfg->dest_port ? cfg->dest_port : 5000;
    scfg.mtu = cfg->mtu >= 256 ? size_t(cfg->mtu) : lv::kDefaultMtu;
    scfg.enable_crc = cfg->enable_crc != 0;
    scfg.fec_group_size = uint16_t(std::max(0, cfg->fec_group_size));
    scfg.pace_mbps = cfg->pace_mbps;
    scfg.threads = uint32_t(std::max(0, cfg->threads));
    scfg.codec = lv::VideoCodecId(std::clamp(cfg->codec, 0, 2));
    scfg.bitrate_kbps = cfg->bitrate_kbps > 0 ? cfg->bitrate_kbps : 8000u;
    scfg.keyframe_interval_frames = cfg->keyframe_interval > 0 ? cfg->keyframe_interval : 120u;
    scfg.fps = cfg->fps > 1.0 ? cfg->fps : 60.0;
    scfg.adaptive.enabled = cfg->adaptive_quality != 0;
    scfg.adaptive.q_min = std::clamp(cfg->q_min > 0 ? cfg->q_min : 35, 1, 100);
    scfg.adaptive.q_max = std::clamp(cfg->q_max > 0 ? cfg->q_max : 85, scfg.adaptive.q_min, 100);
    scfg.adaptive.initial_q = std::clamp(cfg->quality > 0 ? cfg->quality : 80, 1, 100);
    scfg.adaptive.initial_bitrate_kbps = scfg.bitrate_kbps;

    scfg.audio_sample_rate = handle->bus_cfg.sample_rate == 96000 ? 96000u : 48000u;
    scfg.audio_latency_ms = std::clamp<uint32_t>(handle->bus_cfg.latency_ms, 2, 2000);

    handle->codec_params = {};
    handle->codec_params.width = cfg->width;
    handle->codec_params.height = cfg->height;
    handle->codec_params.quality = std::clamp(cfg->quality > 0 ? cfg->quality : 80, 1, 100);
    handle->codec_params.chroma = lv::ChromaFormat(std::clamp(cfg->chroma, 0, 2));
    handle->codec_params.slice_height = cfg->slice_height >= 8 ? cfg->slice_height : 16;
    handle->override_quality.store(0, std::memory_order_relaxed);
    handle->adaptive_enabled.store(cfg->adaptive_quality != 0, std::memory_order_relaxed);

    auto s = std::make_unique<lv::UdpSender>(scfg);
    if (!s->init()) {
        handle->state.store(LV_STATE_ERROR, std::memory_order_release);
        return LV_ERR_SOCKET;
    }
    s->set_control_sync_callback([handle](const lv::ControlSyncPayload& cs) {
        LV_ControlSyncCallback f = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk(handle->cs_cb_mu);
            f = handle->cs_cb;
            ud = handle->cs_ud;
        }
        if (!f) return;
        LV_ControlSyncState st{cs.revision, cs.origin_id, cs.audio_channels, cs.audio_bitrate_kbps,
                               cs.target_buffer_ms, cs.latency_mode, cs.audio_only, cs.active_preset_id,
                               cs.flags, cs.master_gain_cb, cs.audio_codec};
        f(&st, ud);
    });
    {
        std::unique_lock<std::shared_mutex> lk(handle->life_mu);
        handle->sender = std::move(s);
    }
    handle->reset_stats(cfg->width, cfg->height);
    handle->ext_frame_id.store(0, std::memory_order_relaxed);
    handle->ext_audio_seq.store(0, std::memory_order_relaxed);
    handle->stop.store(false, std::memory_order_release);
    handle->running.store(true, std::memory_order_release);
    handle->state.store(LV_STATE_RUNNING, std::memory_order_release);
    return LV_OK;
}

void lv_sender_stop(LvSenderHandle* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
}

int lv_sender_push_packed_frame(LvSenderHandle* handle, const uint8_t* data, int width, int height,
                                int stride, int pixel_format, uint32_t timestamp_us) {
    if (!handle || !data || width <= 0 || height <= 0) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;
    if (pixel_format != LV_PIXEL_FORMAT_BGRA8888 && pixel_format != LV_PIXEL_FORMAT_RGBA8888) {
        return LV_ERR_UNSUPPORTED;
    }

    std::lock_guard<std::mutex> plk(handle->push_mu);
    handle->sender->service_control();

    const lv::PixelOrder order = (pixel_format == LV_PIXEL_FORMAT_BGRA8888) ? lv::PixelOrder::kBGRA
                                                                            : lv::PixelOrder::kRGBA;
    if (handle->ext_img.width != width || handle->ext_img.height != height ||
        handle->ext_img.order != order) {
        handle->ext_img.allocate(width, height, order);
    }
    const size_t row_bytes = size_t(width) * 4;
    const size_t src_stride = stride > 0 ? size_t(stride) : row_bytes;
    if (src_stride == handle->ext_img.stride) {
        std::memcpy(handle->ext_img.data.data(), data, handle->ext_img.data.size());
    } else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(handle->ext_img.row(y), data + size_t(y) * src_stride, row_bytes);
        }
    }

    lv::CodecParams cp = handle->codec_params;
    cp.width = width;
    cp.height = height;
    cp.quality = handle->resolve_quality();

    const uint32_t fid = handle->ext_frame_id.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ts = timestamp_us ? timestamp_us : lv::now_us32();
    const uint64_t t0 = lv::now_us();
    const bool ok = handle->sender->send_frame(handle->ext_img, cp, fid, ts);
    const uint64_t t1 = lv::now_us();
    if (!ok) return LV_ERR_CODEC;
    handle->state.store(LV_STATE_CONNECTED, std::memory_order_relaxed);
    handle->record_encode_sample(double(t1 - t0) / 1000.0, width, height);
    if ((fid & 1u) == 0u) handle->update_preview(handle->ext_img, fid + 1);
    return LV_OK;
}

int lv_sender_push_yuv_planes_ex(LvSenderHandle* handle, const LV_YuvPlanes* planes, int width,
                                 int height, int rotation_deg, int mirror_h,
                                 uint32_t timestamp_us) {
    if (!handle || !planes || !planes->plane_y || !planes->plane_u || width <= 0 || height <= 0) {
        return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    }
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;

    lv::Yuv420View yuv{};
    yuv.y = planes->plane_y;
    yuv.y_stride = planes->y_stride > 0 ? planes->y_stride : width;

    if (planes->pixel_format == LV_PIXEL_FORMAT_NV12) {
        yuv.u = planes->plane_u;
        yuv.v = planes->plane_v ? planes->plane_v : (planes->plane_u + 1);
        yuv.u_stride = planes->u_stride > 0 ? planes->u_stride : width;
        yuv.v_stride = planes->v_stride > 0 ? planes->v_stride : yuv.u_stride;
        yuv.uv_pixel_stride = planes->uv_pixel_stride > 0 ? planes->uv_pixel_stride : 2;
    } else if (planes->pixel_format == LV_PIXEL_FORMAT_NV21) {
        // NV21 stores interleaved V, U bytes: swap pointers so Cb=u, Cr=v!
        yuv.v = planes->plane_u;
        yuv.u = planes->plane_v ? planes->plane_v : (planes->plane_u + 1);
        yuv.v_stride = planes->u_stride > 0 ? planes->u_stride : width;
        yuv.u_stride = planes->v_stride > 0 ? planes->v_stride : yuv.v_stride;
        yuv.uv_pixel_stride = planes->uv_pixel_stride > 0 ? planes->uv_pixel_stride : 2;
    } else if (planes->pixel_format == LV_PIXEL_FORMAT_I420) {
        if (!planes->plane_v) return LV_ERR_INVALID_ARG;
        yuv.u = planes->plane_u;
        yuv.v = planes->plane_v;
        yuv.u_stride = planes->u_stride > 0 ? planes->u_stride : ((width + 1) >> 1);
        yuv.v_stride = planes->v_stride > 0 ? planes->v_stride : ((width + 1) >> 1);
        yuv.uv_pixel_stride = planes->uv_pixel_stride > 0 ? planes->uv_pixel_stride : 1;
    } else {
        return LV_ERR_UNSUPPORTED;
    }

    std::lock_guard<std::mutex> plk(handle->push_mu);
    handle->sender->service_control();

    int out_w = width;
    int out_h = height;
    lv::Yuv420View send_yuv = yuv;
    const int norm_rot = ((rotation_deg % 360) + 360) % 360;
    if (norm_rot != 0 || mirror_h != 0) {
        if (!lv::yuv420_transform_to_i420(yuv, width, height, norm_rot, mirror_h != 0,
                                          handle->yuv_scratch, out_w, out_h, send_yuv)) {
            return LV_ERR_INVALID_ARG;
        }
    }

    lv::CodecParams cp = handle->codec_params;
    cp.width = out_w;
    cp.height = out_h;
    cp.chroma = lv::ChromaFormat::k420;
    cp.quality = handle->resolve_quality();

    const uint32_t fid = handle->ext_frame_id.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ts = timestamp_us ? timestamp_us : lv::now_us32();
    const uint64_t t0 = lv::now_us();
    const bool ok = handle->sender->send_frame_yuv420(send_yuv, cp, fid, ts);
    const uint64_t t1 = lv::now_us();
    if (!ok) return LV_ERR_CODEC;
    handle->state.store(LV_STATE_CONNECTED, std::memory_order_relaxed);
    handle->record_encode_sample(double(t1 - t0) / 1000.0, out_w, out_h);
    return LV_OK;
}

int lv_sender_push_yuv_planes(LvSenderHandle* handle, const LV_YuvPlanes* planes, int width,
                              int height, uint32_t timestamp_us) {
    return lv_sender_push_yuv_planes_ex(handle, planes, width, height, 0, 0, timestamp_us);
}

int lv_sender_push_cvpixelbuffer_planes(LvSenderHandle* handle, const uint8_t* plane0, int stride0,
                                        const uint8_t* plane1, int stride1, int width, int height,
                                        int pixel_format, uint32_t timestamp_us) {
    if (pixel_format == LV_PIXEL_FORMAT_BGRA8888 || pixel_format == LV_PIXEL_FORMAT_RGBA8888) {
        return lv_sender_push_packed_frame(handle, plane0, width, height, stride0, pixel_format,
                                           timestamp_us);
    }
    if (pixel_format == LV_PIXEL_FORMAT_NV12 || pixel_format == LV_PIXEL_FORMAT_NV21) {
        LV_YuvPlanes yp{};
        yp.plane_y = plane0;
        yp.plane_u = plane1;
        yp.plane_v = nullptr;
        yp.y_stride = stride0;
        yp.u_stride = stride1;
        yp.v_stride = stride1;
        yp.uv_pixel_stride = 2;
        yp.pixel_format = pixel_format;
        return lv_sender_push_yuv_planes(handle, &yp, width, height, timestamp_us);
    }
    return LV_ERR_UNSUPPORTED;
}

int lv_sender_push_audio_chunk(LvSenderHandle* handle, const void* data, int num_frames,
                               int sample_rate, int channels, int audio_format,
                               uint32_t timestamp_us) {
    if (!handle || !data || num_frames <= 0) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;
    const auto fmt = (audio_format == LV_AUDIO_FORMAT_F32) ? lv::AudioSampleFormat::kFloat32
                                                           : lv::AudioSampleFormat::kPcm16;
    const uint32_t aseq = handle->ext_audio_seq.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ts = timestamp_us ? timestamp_us : lv::now_us32();
    return handle->sender->send_audio_raw(aseq, ts, data, uint16_t(num_frames),
                                          uint32_t(sample_rate > 0 ? sample_rate : 48000),
                                          uint8_t(channels == 1 ? 1 : 2), fmt)
               ? LV_OK
               : LV_ERR_SOCKET;
}


/* ---- 64-channel AoIP bus ---- */
static bool lv_bus_cfg_valid(const LV_AudioBusConfig* c) {
    return c && (c->sample_rate == 48000 || c->sample_rate == 96000) && c->latency_ms >= 2 &&
           c->latency_ms <= 2000 && c->num_channels >= 1 && c->num_channels <= 64;
}

static bool lv_map_format(LV_AudioFormat f, lv::AudioFormat2& out) {
    switch (f) {
        case LV_AUDIO_FORMAT_PCM16: out = lv::AudioFormat2::kPcm16; return true;
        case LV_AUDIO_FORMAT_F32: out = lv::AudioFormat2::kFloat32; return true;
        case LV_AUDIO_FORMAT_PCM24_PACKED: out = lv::AudioFormat2::kPcm24Packed; return true;
        default: return false;
    }
}

void lv_audio_bus_config_default(LV_AudioBusConfig* cfg) {
    if (!cfg) return;
    cfg->sample_rate = 48000;
    cfg->latency_ms = 20;
    cfg->num_channels = 64;
    cfg->block_frames = 240;
}

int lv_sender_configure_audio_bus(LvSenderHandle* handle, const LV_AudioBusConfig* cfg) {
    if (!handle || !lv_bus_cfg_valid(cfg)) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->bus_cfg = *cfg;
    if (handle->sender) handle->sender->set_audio_latency_ms(cfg->latency_ms);
    return LV_OK;
}

int lv_sender_push_audio_channel(LvSenderHandle* handle, uint16_t channel_index, const void* samples,
                                 uint32_t num_samples, LV_AudioFormat format,
                                 uint64_t media_timestamp_samples) {
    if (!handle || !samples || num_samples == 0 || channel_index >= LV_AUDIO_MAX_CHANNELS)
        return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    lv::AudioFormat2 f;
    if (!lv_map_format(format, f)) return LV_ERR_INVALID_ARG;
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;
    return handle->sender->push_audio_channel(channel_index, samples, num_samples, f,
                                              media_timestamp_samples) == 0
               ? LV_OK
               : LV_ERR_INVALID_ARG;
}

int lv_sender_push_audio_multichannel(LvSenderHandle* handle, const void* data, uint16_t num_channels,
                                      uint32_t num_samples, LV_AudioFormat format, LV_AudioLayout layout,
                                      uint64_t media_timestamp_samples) {
    if (!handle || !data || num_samples == 0 || num_channels == 0 || num_channels > LV_AUDIO_MAX_CHANNELS)
        return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    lv::AudioFormat2 f;
    if (!lv_map_format(format, f)) return LV_ERR_INVALID_ARG;
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;
    // One shared timestamp for all channels keeps them phase-locked at the receiver.
    if (media_timestamp_samples == LV_MEDIA_TS_AUTO)
        media_timestamp_samples = handle->sender->next_auto_media_ts(num_samples);
    const size_t bps = lv::audio2_bytes_per_sample(f);
    if (layout == LV_AUDIO_LAYOUT_PLANAR) {
        const void* const* planes = static_cast<const void* const*>(data);
        for (uint16_t c = 0; c < num_channels; ++c) {
            if (!planes[c]) return LV_ERR_INVALID_ARG;
            const int r = handle->sender->push_audio_channel(c, planes[c], num_samples, f, media_timestamp_samples);
            if (r != 0) return LV_ERR_INVALID_ARG;
        }
        return LV_OK;
    }
    std::vector<uint8_t> tmp(size_t(num_samples) * bps);
    const uint8_t* in = static_cast<const uint8_t*>(data);
    for (uint16_t c = 0; c < num_channels; ++c) {
        for (uint32_t i = 0; i < num_samples; ++i)
            std::memcpy(&tmp[size_t(i) * bps], in + (size_t(i) * num_channels + c) * bps, bps);
        if (handle->sender->push_audio_channel(c, tmp.data(), num_samples, f, media_timestamp_samples) != 0)
            return LV_ERR_INVALID_ARG;
    }
    return LV_OK;
}

int lv_sender_set_audio_latency_ms(LvSenderHandle* handle, uint32_t latency_ms) {
    if (!handle || latency_ms < 2 || latency_ms > 2000) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    handle->bus_cfg.latency_ms = latency_ms;
    if (handle->sender) handle->sender->set_audio_latency_ms(latency_ms);
    return LV_OK;
}

int lv_sender_set_audio_codec(LvSenderHandle* handle, int codec) {
    if (!handle || codec < 0 || codec > 8) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (handle->sender) {
        handle->sender->set_audio_codec_profile(static_cast<lv::AudioCodecProfile>(codec));
    }
    return LV_OK;
}

int lv_sender_set_audio_bitrate(LvSenderHandle* handle, int kbps_per_ch) {
    if (!handle) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (handle->sender) {
        handle->sender->set_audio_bitrate_kbps(kbps_per_ch);
    }
    return LV_OK;
}

uint64_t lv_sender_get_master_clock_samples(LvSenderHandle* handle) {
    if (!handle) return 0;
    LV_LIVE_LOCK(handle);
    if (!handle->sender) return 0;
    return handle->sender->master_samples_now();
}

int lv_receiver_configure_audio_bus(LvReceiverHandle* handle, const LV_AudioBusConfig* cfg) {
    if (!handle || !lv_bus_cfg_valid(cfg)) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->bus_cfg = *cfg;
    if (handle->receiver) handle->receiver->set_audio_latency_ms(cfg->latency_ms);
    return LV_OK;
}

int lv_receiver_set_audio_latency_ms(LvReceiverHandle* handle, uint32_t latency_ms) {
    if (!handle || latency_ms < 2 || latency_ms > 2000) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    handle->bus_cfg.latency_ms = latency_ms;
    if (handle->receiver) handle->receiver->set_audio_latency_ms(latency_ms);
    return LV_OK;
}

int lv_receiver_set_adaptive_jitter(LvReceiverHandle* handle, int enabled) {
    if (!handle) return LV_ERR_INVALID_ARG;
    handle->adaptive_jitter.store(enabled ? 1 : 0, std::memory_order_release);
    std::lock_guard<std::mutex> lk(handle->api_mu);
    if (handle->receiver) handle->receiver->set_audio_adaptive_jitter(enabled != 0);
    return LV_OK;
}

double lv_receiver_get_effective_latency_ms(LvReceiverHandle* handle) {
    if (!handle) return 0.0;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    return handle->receiver ? handle->receiver->audio_effective_latency_ms() : 0.0;
}

int lv_receiver_set_multichannel_audio_callback(LvReceiverHandle* handle, LV_MultichannelAudioCallback cb,
                                                void* user_data) {
    if (!handle) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    {
        std::lock_guard<std::mutex> clk(handle->cb_mu);
        handle->mc_cb = cb;
        handle->mc_ud = user_data;
    }
    if (!cb) {
        handle->stop_mc_pump();
    } else if (handle->receiver) {
        handle->start_mc_pump();
    }
    return LV_OK;
}

int lv_receiver_pop_audio_multichannel(LvReceiverHandle* handle, float* out, uint16_t num_channels,
                                       uint32_t num_frames, LV_AudioLayout layout) {
    if (!handle || !out || num_channels == 0 || num_channels > LV_AUDIO_MAX_CHANNELS || num_frames == 0)
        return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) {
        std::memset(out, 0, size_t(num_channels) * num_frames * sizeof(float));
        return LV_ERR_NOT_RUNNING;
    }
    if (handle->mc_active.load(std::memory_order_acquire)) return LV_ERR_UNSUPPORTED;
    if (layout == LV_AUDIO_LAYOUT_PLANAR) {
        float* planes[LV_AUDIO_MAX_CHANNELS];
        for (uint16_t c = 0; c < num_channels; ++c) planes[c] = out + size_t(c) * num_frames;
        handle->receiver->pop_audio_multichannel_planar(planes, num_channels, num_frames);
    } else {
        handle->receiver->pop_audio_multichannel_interleaved(out, num_channels, num_frames);
    }
    return int(num_frames);
}

int lv_receiver_get_channel_stats(LvReceiverHandle* handle, uint16_t channel_index, LV_AudioChannelStats* out) {
    if (!handle || !out || channel_index >= LV_AUDIO_MAX_CHANNELS) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    std::memset(out, 0, sizeof(*out));
    out->level_dbfs = -120.0f;
    if (!handle->receiver) return LV_ERR_NOT_RUNNING;
    const auto rx = handle->receiver->audio_channel_rx_stats(channel_index);
    const auto m = handle->receiver->audio_matrix().meter(channel_index);
    out->packets_rx = rx.packets_rx;
    out->packets_lost = rx.packets_lost;
    out->fec_recovered = rx.fec_recovered;
    out->nack_recovered = rx.nack_recovered;
    out->nacks_sent = rx.nacks_sent;
    out->late_packets = rx.late_packets;
    out->jitter_us = rx.jitter_us;
    out->underruns = m.underruns;
    out->level_dbfs = m.level_dbfs;
    out->active = m.active ? 1 : 0;
    return LV_OK;
}

int lv_sender_request_keyframe_quality(LvSenderHandle* handle, int quality) {
    if (!handle || quality < 1 || quality > 100) return LV_ERR_INVALID_ARG;
    handle->override_quality.store(quality, std::memory_order_relaxed);
    handle->codec_params.quality = quality;
    if (handle->sender) {
        handle->sender->force_keyframe();
        if (handle->adaptive_enabled.load(std::memory_order_relaxed)) {
            handle->sender->set_adaptive_quality(true, 35, 95, quality);
        }
    }
    return LV_OK;
}

int lv_sender_set_bitrate(LvSenderHandle* handle, uint32_t bitrate_kbps) {
    if (!handle || bitrate_kbps < 250u || bitrate_kbps > 100000u) return LV_ERR_INVALID_ARG;
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    handle->sender->set_bitrate_kbps(bitrate_kbps);
    return LV_OK;
}

int lv_sender_force_keyframe(LvSenderHandle* handle) {
    if (!handle) return LV_ERR_INVALID_ARG;
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    handle->sender->force_keyframe();
    return LV_OK;
}

int lv_sender_set_fec_group(LvSenderHandle* handle, int fec_group_size) {
    if (!handle || fec_group_size < 0 || fec_group_size > 64) return LV_ERR_INVALID_ARG;
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    handle->sender->set_fec_group_size(uint16_t(fec_group_size));
    return LV_OK;
}

int lv_sender_set_adaptive_quality(LvSenderHandle* handle, int enabled, int q_min, int q_max) {
    if (!handle || q_min < 1 || q_max > 100 || q_min > q_max) return LV_ERR_INVALID_ARG;
    handle->adaptive_enabled.store(enabled != 0, std::memory_order_relaxed);
    if (handle->sender) {
        handle->sender->set_adaptive_quality(enabled != 0, q_min, q_max, handle->resolve_quality());
    }
    return LV_OK;
}

int lv_sender_get_stream_stats(LvSenderHandle* handle, LV_StreamStats* out) {
    if (!handle || !out) return LV_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->state = handle->state.load(std::memory_order_acquire);
    if (!handle->sender) return LV_OK;

    const lv::SenderStats st = handle->sender->stats();
    handle->sender->service_control_try();  // deliver incoming ControlSync/PING even in Audio-Only idle
    const lv::AudioTxStats a2 = handle->sender->audio2_stats();
    const uint64_t total_audio_bytes = st.audio_bytes_sent + a2.bytes_sent;
    const uint64_t total_audio_pkts = st.audio_packets_sent + a2.packets_sent;
    const uint64_t total_tx_bytes = st.bytes_sent + total_audio_bytes;
    const uint64_t now = lv::now_us();

    std::lock_guard<std::mutex> slk(handle->stats_mu);
    const double dt_s = double(now - handle->win_start_us) / 1e6;
    if (dt_s >= 0.5) {
        const uint64_t df = st.frames_sent - handle->win_prev_frames;
        const uint64_t db = total_tx_bytes - handle->win_prev_bytes;
        const uint64_t dab = total_audio_bytes - handle->win_prev_ab;
        handle->cached_fps = double(df) / dt_s;
        handle->cached_mbps = (double(db) * 8.0 / 1e6) / dt_s;
        handle->cached_audio_kbps = (double(dab) * 8.0 / 1e3) / dt_s;
        handle->cached_enc_ms = handle->enc_ms_cnt ? (handle->enc_ms_sum / double(handle->enc_ms_cnt)) : 0.0;
        handle->win_start_us = now;
        handle->win_prev_frames = st.frames_sent;
        handle->win_prev_bytes = total_tx_bytes;
        handle->win_prev_ab = total_audio_bytes;
        handle->enc_ms_sum = 0.0;
        handle->enc_ms_cnt = 0;
    }

    out->width = handle->cur_w;
    out->height = handle->cur_h;
    out->active_quality = handle->resolve_quality();
    out->fps = handle->cached_fps;
    out->bitrate_mbps = handle->cached_mbps;
    out->encode_ms = handle->cached_enc_ms;
    out->packet_loss_pct = double(st.last_rx_loss_permille) / 10.0;
    out->frames_total = st.frames_sent;
    out->packets_total = st.packets_sent + a2.packets_sent;
    out->audio_packets = total_audio_pkts;
    out->audio_kbps = handle->cached_audio_kbps;
    out->active_codec = int32_t(st.active_codec);
    out->codec_is_hardware = handle->sender->encoder_is_hardware() ? 1 : 0;
    out->target_bitrate_kbps = st.active_bitrate_kbps;
    std::snprintf(out->codec_backend, sizeof(out->codec_backend), "%s",
                  handle->sender->encoder_backend_name().c_str());
    return LV_OK;
}

/* ---- Receiver SDK API ---- */
LvReceiverHandle* lv_receiver_create(void) { return new LvReceiverHandle(); }
void lv_receiver_destroy(LvReceiverHandle* handle) { delete handle; }

void lv_receiver_set_video_callback(LvReceiverHandle* handle, LV_VideoFrameCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->video_cb = cb;
    handle->video_ud = user_data;
}

void lv_receiver_set_audio_callback(LvReceiverHandle* handle, LV_AudioSamplesCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->audio_cb = cb;
    handle->audio_ud = user_data;
}

void lv_receiver_set_state_callback(LvReceiverHandle* handle, LV_StateChangedCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->state_cb = cb;
    handle->state_ud = user_data;
}

int lv_sdk_receiver_start(LvReceiverHandle* handle, const LV_ReceiverConfig* cfg) {
    if (!handle || !cfg) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);

    lv::ReceiverConfig rcfg;
    rcfg.bind_ip = cfg->bind_ip[0] ? cfg->bind_ip : "0.0.0.0";
    rcfg.bind_port = cfg->bind_port;
    rcfg.decode_threads = uint32_t(std::max(0, cfg->decode_threads));
    rcfg.assembler.conceal =
        (cfg->conceal_mode == LV_CONCEALMENT_GRAY) ? lv::ConcealMode::kGray : lv::ConcealMode::kPrev;
    rcfg.assembler.frame_timeout_us =
        uint64_t((cfg->frame_timeout_ms > 1.0 ? cfg->frame_timeout_ms : 25.0) * 1000.0);
    rcfg.assembler.output_order = (cfg->output_format == LV_PIXEL_FORMAT_RGBA8888)
                                      ? lv::PixelOrder::kRGBA
                                      : lv::PixelOrder::kBGRA;
    rcfg.audio_target_ms = cfg->audio_target_ms > 0.0 ? cfg->audio_target_ms : 12.0;
    rcfg.audio_sample_rate = handle->bus_cfg.sample_rate == 96000 ? 96000u : 48000u;
    rcfg.audio_latency_ms = std::clamp<uint32_t>(handle->bus_cfg.latency_ms, 2, 2000);
    rcfg.audio_adaptive_jitter = handle->adaptive_jitter.load(std::memory_order_acquire) != 0;

    return handle->start_common(rcfg, cfg->enable_os_audio != 0, cfg->enable_discovery != 0,
                                cfg->discovery_port, cfg->device_name);
}

void lv_receiver_stop(LvReceiverHandle* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
}

uint16_t lv_receiver_get_local_port(LvReceiverHandle* handle) {
    if (!handle || !handle->receiver) return 0;
    return handle->receiver->local_port();
}

int lv_receiver_get_latest_frame(LvReceiverHandle* handle, uint8_t* out_buf, size_t cap, int* out_w,
                                 int* out_h, uint32_t* inout_frame_id, int want_rgba) {
    if (!handle) return 0;
    std::lock_guard<std::mutex> lk(handle->frame_mu);
    if (!handle->has_frame || handle->latest_img.width <= 0) return 0;
    if (out_w) *out_w = handle->latest_img.width;
    if (out_h) *out_h = handle->latest_img.height;
    if (inout_frame_id && *inout_frame_id == handle->latest_fid) return 0;
    if (!out_buf || cap < handle->latest_img.data.size()) return -1;
    copy_image_order(handle->latest_img, out_buf, want_rgba != 0);
    if (inout_frame_id) *inout_frame_id = handle->latest_fid;
    return 1;
}

int lv_receiver_pop_audio_pcm16(LvReceiverHandle* handle, int16_t* out_stereo, int num_frames) {
    if (!handle || !out_stereo || num_frames <= 0) return 0;
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) {
        std::memset(out_stereo, 0, size_t(num_frames) * 4);
        return 0;
    }
    handle->receiver->audio_buffer().pop_pcm16(out_stereo, size_t(num_frames));
    return num_frames;
}

void lv_receiver_set_volume(LvReceiverHandle* handle, float volume) {
    if (!handle) return;
    handle->volume.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
    if (handle->receiver) handle->receiver->audio_buffer().set_volume(volume);
}

void lv_receiver_set_concealment(LvReceiverHandle* handle, int mode) {
    if (!handle || !handle->receiver) return;
    handle->receiver->set_conceal_mode(mode == 0 ? lv::ConcealMode::kGray : lv::ConcealMode::kPrev);
}

int lv_receiver_save_screenshot(LvReceiverHandle* handle, const char* ppm_path) {
    if (!handle || !ppm_path || !*ppm_path) return 0;
    lv::Image copy;
    {
        std::lock_guard<std::mutex> lk(handle->frame_mu);
        if (!handle->has_frame || handle->latest_img.width <= 0) return 0;
        copy = handle->latest_img;
    }
    return lv::write_ppm(ppm_path, copy) ? 1 : 0;
}

int lv_receiver_get_stream_stats(LvReceiverHandle* handle, LV_StreamStats* out) {
    if (!handle || !out) return LV_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->state = handle->state.load(std::memory_order_acquire);
    if (!handle->receiver) return LV_OK;

    const auto ast = handle->receiver->assembler_stats();
    const auto aud = handle->receiver->audio_stats();
    const uint64_t a2_bytes = handle->receiver->audio2_bytes_rx();
    const uint64_t a2_pkts = handle->receiver->audio2_packets_rx();
    const uint64_t total_audio_bytes = aud.bytes_rx + a2_bytes;
    const uint64_t total_audio_pkts = aud.packets_rx + a2_pkts;
    const uint64_t total_rx_bytes = ast.bytes_total + total_audio_bytes;
    const uint64_t rx_pkts = handle->receiver->seq_tracker().received();
    const uint64_t lost_pkts = handle->receiver->seq_tracker().lost(0);
    const uint64_t now = lv::now_us();

    std::lock_guard<std::mutex> slk(handle->stats_mu);
    const double dt_s = double(now - handle->win_start_us) / 1e6;
    if (dt_s >= 0.5) {
        const uint64_t db = total_rx_bytes - handle->win_prev_bytes;
        const uint64_t dab = total_audio_bytes - handle->win_prev_ab;
        const uint64_t drx = rx_pkts - handle->win_prev_rx;
        const uint64_t dlost = lost_pkts - handle->win_prev_lost;
        handle->cached_fps = double(handle->win_frames) / dt_s;
        handle->cached_mbps = (double(db) * 8.0 / 1e6) / dt_s;
        handle->cached_akbps = (double(dab) * 8.0 / 1e3) / dt_s;
        handle->cached_loss = (drx + dlost) ? (100.0 * double(dlost) / double(drx + dlost)) : 0.0;
        if (!handle->win_lats.empty()) {
            handle->cached_p50 = percentile_vec(handle->win_lats, 0.50);
            handle->cached_p99 = percentile_vec(handle->win_lats, 0.99);
            handle->win_lats.clear();
        }
        handle->win_start_us = now;
        handle->win_frames = 0;
        handle->win_prev_bytes = total_rx_bytes;
        handle->win_prev_ab = total_audio_bytes;
        handle->win_prev_rx = rx_pkts;
        handle->win_prev_lost = lost_pkts;
    }

    out->width = handle->cur_w;
    out->height = handle->cur_h;
    out->active_quality = handle->cur_quality;
    out->fps = handle->cached_fps;
    out->bitrate_mbps = handle->cached_mbps;
    out->latency_p50_ms = handle->cached_p50;
    out->latency_p99_ms = handle->cached_p99;
    out->packet_loss_pct = handle->cached_loss;
    out->frames_total = ast.frames_complete;
    out->frames_concealed = ast.frames_concealed;
    out->packets_total = ast.datagrams_total + a2_pkts;
    out->fec_recovered = ast.fec_recovered_packets;
    out->crc_errors = ast.crc_errors;
    out->late_packets = ast.late_packets;
    out->audio_packets = total_audio_pkts;
    out->audio_kbps = handle->cached_akbps;
    out->audio_buffer_ms = aud.buffered_ms;
    out->active_codec = int32_t(handle->receiver->active_codec());
    out->codec_is_hardware = handle->receiver->decoder_is_hardware() ? 1 : 0;
    std::snprintf(out->codec_backend, sizeof(out->codec_backend), "%s",
                  handle->receiver->decoder_backend_name().c_str());
    return LV_OK;
}

/* ---- LAN Discovery ---- */
int lv_discovery_scan_peers(int timeout_ms, uint16_t discovery_port, LV_DiscoveredPeer* out_peers,
                            int max_peers) {
    if (!out_peers || max_peers <= 0) return 0;
    const auto found = lv::discovery_scan(timeout_ms, discovery_port ? discovery_port : lv::kDiscoveryPort);
    const int n = std::min<int>(int(found.size()), max_peers);
    for (int i = 0; i < n; ++i) {
        std::memset(&out_peers[i], 0, sizeof(LV_DiscoveredPeer));
        std::snprintf(out_peers[i].ip, sizeof(out_peers[i].ip), "%s", found[size_t(i)].ip.c_str());
        out_peers[i].port = found[size_t(i)].stream_port;
        out_peers[i].rtt_ms = found[size_t(i)].rtt_ms;
        std::snprintf(out_peers[i].name, sizeof(out_peers[i].name), "%s",
                      found[size_t(i)].device_name.c_str());
        std::snprintf(out_peers[i].os, sizeof(out_peers[i].os), "%s", found[size_t(i)].os.c_str());
        std::snprintf(out_peers[i].simd, sizeof(out_peers[i].simd), "%s", found[size_t(i)].simd.c_str());
    }
    return n;
}

/* ---- Full-duplex 64x64 AoIP transceiver ---- */
}  // extern "C"

struct LvDuplexHandle {
    std::mutex api_mu;
    LvSenderHandle* tx = nullptr;
    LvReceiverHandle* rx = nullptr;
#ifdef LV_HAVE_SHM_BUS
    lv::ShmAudioHost tx_host;  // DAW TX ring -> patchbay -> network   (own thread)
    lv::ShmAudioHost rx_host;  // network -> patchbay -> DAW RX ring   (own thread)
#endif
    lv::ChannelPatchbay tx_patch;
    lv::ChannelPatchbay rx_patch;
    std::unique_ptr<lv::DiscoveryResponder> disc;
    LV_DuplexConfig cfg{};
    LV_MidiEventCallback midi_cb = nullptr;
    void* midi_ud = nullptr;
    LV_ControlSyncCallback cs_cb = nullptr;
    void* cs_ud = nullptr;
    std::atomic<bool> running{false};

    void stop_internal() {
        running.store(false, std::memory_order_release);
        if (disc) {
            disc->stop();
            disc.reset();
        }
#ifdef LV_HAVE_SHM_BUS
        tx_host.stop();
        rx_host.stop();
#endif
        if (tx) { lv_sender_destroy(tx); tx = nullptr; }
        if (rx) { lv_receiver_destroy(rx); rx = nullptr; }
    }
    ~LvDuplexHandle() { stop_internal(); }
};

extern "C" {

void lv_duplex_config_default(LV_DuplexConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->peer_host, sizeof(cfg->peer_host), "127.0.0.1");
    cfg->peer_port = 5004;
    cfg->listen_port = 5005;
    std::snprintf(cfg->bind_ip, sizeof(cfg->bind_ip), "0.0.0.0");
    cfg->latency_ms = 20;
    cfg->sample_rate = 48000;
    cfg->num_channels = 64;
    cfg->discovery_port = lv::kDiscoveryPort;
    cfg->enable_discovery = 1;
    cfg->adaptive_jitter = 1;
}

LvDuplexHandle* lv_duplex_create(void) { return new (std::nothrow) LvDuplexHandle(); }
void lv_duplex_destroy(LvDuplexHandle* handle) { delete handle; }

int lv_duplex_start(LvDuplexHandle* h, const LV_DuplexConfig* cfg) {
    if (!h || !cfg) return LV_ERR_INVALID_ARG;
#ifndef LV_HAVE_SHM_BUS
    (void)cfg;
    return LV_ERR_UNSUPPORTED;
#else
    if (cfg->latency_ms < 2 || cfg->latency_ms > 2000 || (cfg->sample_rate != 48000 && cfg->sample_rate != 96000) ||
        cfg->num_channels < 1 || cfg->num_channels > 64 || cfg->peer_port == 0 || !cfg->peer_host[0])
        return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->stop_internal();
    h->cfg = *cfg;
    h->tx_patch.set_sample_rate(uint32_t(cfg->sample_rate));
    h->rx_patch.set_sample_rate(uint32_t(cfg->sample_rate));

    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.sample_rate = cfg->sample_rate;
    bus.latency_ms = cfg->latency_ms;
    bus.num_channels = cfg->num_channels;

    // RX direction: our receiver (own socket, own MasterClock/RTT towards the peer's sender).
    h->rx = lv_receiver_create();
    lv_receiver_set_midi_callback(h->rx, h->midi_cb, h->midi_ud);
    lv_receiver_set_control_sync_callback(h->rx, h->cs_cb, h->cs_ud);
    lv_receiver_configure_audio_bus(h->rx, &bus);
    lv_receiver_set_adaptive_jitter(h->rx, cfg->adaptive_jitter);
    LV_ReceiverConfig rc;
    lv_sdk_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "%s", cfg->bind_ip[0] ? cfg->bind_ip : "0.0.0.0");
    rc.bind_port = cfg->listen_port;
    rc.enable_discovery = 0;  // the duplex node announces itself below
    int r = lv_sdk_receiver_start(h->rx, &rc);
    if (r != LV_OK) { h->stop_internal(); return r; }

    // TX direction: our sender (own socket; answers the peer receiver's clock-sync pings).
    h->tx = lv_sender_create();
    lv_sender_configure_audio_bus(h->tx, &bus);
    LV_SenderConfig sc;
    lv_sdk_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "%s", cfg->peer_host);
    sc.dest_port = cfg->peer_port;
    r = lv_sdk_sender_start(h->tx, &sc);
    if (r != LV_OK) { h->stop_internal(); return r; }
    lv_sender_set_audio_codec(h->tx, std::clamp<int>(cfg->audio_codec, 0, 8));

    lv::ShmAudioHost::Config hc;
    hc.channels = uint32_t(cfg->num_channels);
    hc.bus_name = cfg->bus_name;
    hc.rx_target_frames = cfg->rx_headroom_frames;
    LvDuplexHandle* self = h;
    const bool ok_tx = h->tx_host.start(hc, [self](const float* const* p, uint32_t ch, uint32_t n, bool disc) {
        static thread_local std::vector<float> buf;
        static thread_local std::vector<float*> outp;
        buf.resize(size_t(ch) * n);
        outp.resize(ch);
        for (uint32_t c = 0; c < ch; ++c) outp[c] = buf.data() + size_t(c) * n;
        self->tx_patch.process(p, outp.data(), ch, n);
        const uint64_t ts = disc ? lv_sender_get_master_clock_samples(self->tx) - n : LV_MEDIA_TS_AUTO;
        lv_sender_push_audio_multichannel(self->tx, outp.data(), uint16_t(ch), n, LV_AUDIO_FORMAT_F32,
                                          LV_AUDIO_LAYOUT_PLANAR, ts);
    }, nullptr);
    const bool ok_rx = h->rx_host.start(hc, nullptr, [self](float* const* p, uint32_t ch, uint32_t n) {
        static thread_local std::vector<float> buf;
        static thread_local std::vector<const float*> inp;
        buf.resize(size_t(ch) * n);
        inp.resize(ch);
        lv_receiver_pop_audio_multichannel(self->rx, buf.data(), uint16_t(ch), n, LV_AUDIO_LAYOUT_PLANAR);
        for (uint32_t c = 0; c < ch; ++c) inp[c] = buf.data() + size_t(c) * n;
        self->rx_patch.process(inp.data(), p, ch, n);
    });
    if (!ok_tx || !ok_rx) { h->stop_internal(); return LV_ERR_INTERNAL; }

    if (cfg->enable_discovery) {
        h->disc = std::make_unique<lv::DiscoveryResponder>();
        h->disc->set_aoip_provider([self]() {
            lv::AoipAnnounce a;
            a.enabled = true;
            a.hostname = lv::local_host_name();
            const auto st = self->rx_host.status();
            a.audio_driver = st.driver_name;
            a.driver_connected = st.driver_connected;
            a.channels = uint16_t(self->cfg.num_channels);
            a.rx_port = self->cfg.listen_port;
            a.tx_port = self->cfg.peer_port;
            a.duplex = true;
            return a;
        });
        h->disc->start(cfg->listen_port, cfg->device_name,
                       cfg->discovery_port ? cfg->discovery_port : lv::kDiscoveryPort);
    }
    h->running.store(true, std::memory_order_release);
    return LV_OK;
#endif
}

void lv_duplex_stop(LvDuplexHandle* h) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->stop_internal();
}

int lv_sender_push_midi(LvSenderHandle* handle, uint8_t status, uint8_t data1, uint8_t data2, uint8_t port,
                        uint64_t media_ts_us) {
    if (!handle || status < 0x80 || port >= lv::kMaxMidiPorts) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;
    return handle->sender->push_midi(status, data1 & 0x7F, data2 & 0x7F, port, media_ts_us) ? LV_OK : LV_ERR_SOCKET;
}

void lv_receiver_set_midi_callback(LvReceiverHandle* handle, LV_MidiEventCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->midi_cb = cb;
    handle->midi_ud = user_data;
}

int lv_receiver_get_stream_epoch_info(LvReceiverHandle* handle, uint64_t* epoch_resets, uint32_t* active_midi_notes,
                                      uint64_t* midi_events_rx) {
    if (!handle) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) return LV_ERR_NOT_RUNNING;
    if (epoch_resets) *epoch_resets = handle->receiver->stream_epoch_resets();
    if (active_midi_notes) *active_midi_notes = handle->receiver->active_midi_notes();
    if (midi_events_rx) *midi_events_rx = handle->receiver->midi_stats().events_unique;
    return LV_OK;
}

int lv_duplex_send_midi(LvDuplexHandle* h, uint8_t status, uint8_t data1, uint8_t data2, uint8_t port,
                        uint64_t media_ts_us) {
    if (!h) return LV_ERR_INVALID_ARG;
    if (!h->running.load(std::memory_order_acquire) || !h->tx) return LV_ERR_NOT_RUNNING;
    return lv_sender_push_midi(h->tx, status, data1, data2, port, media_ts_us);
}

void lv_duplex_set_midi_callback(LvDuplexHandle* h, LV_MidiEventCallback cb, void* user_data) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->midi_cb = cb;
    h->midi_ud = user_data;
    if (h->rx) lv_receiver_set_midi_callback(h->rx, cb, user_data);
}

int lv_receiver_get_midi_dejitter_stats(LvReceiverHandle* handle, LV_MidiDejitterStats* out) {
    if (!handle || !out) return LV_ERR_INVALID_ARG;
    *out = {};
    if (!handle->receiver) return LV_ERR_NOT_RUNNING;
    const auto st = handle->receiver->midi_dejitter_stats();
    out->target_buffer_ms = st.target_buffer_ms;
    out->playout_delay_ms = st.playout_delay_ms;
    out->absorbed_jitter_ms = st.absorbed_jitter_ms;
    out->fixed_mode = st.fixed_mode ? 1 : 0;
    out->synced_with_audio = st.synced_with_audio ? 1 : 0;
    out->midi_only_mode = st.midi_only_mode ? 1 : 0;
    out->queued_events = st.queued_events;
    out->total_enqueued = st.total_enqueued;
    out->total_dispatched = st.total_dispatched;
    out->late_events = st.late_events;
    return LV_OK;
}

int lv_duplex_get_midi_dejitter_stats(LvDuplexHandle* h, LV_MidiDejitterStats* out) {
    if (!h || !out) return LV_ERR_INVALID_ARG;
    *out = {};
    if (!h->running.load(std::memory_order_acquire) || !h->rx) return LV_ERR_NOT_RUNNING;
    return lv_receiver_get_midi_dejitter_stats(h->rx, out);
}

int lv_sender_set_midi_only(LvSenderHandle* handle, int enabled) {
    if (!handle) return LV_ERR_INVALID_ARG;
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    handle->sender->set_midi_only(enabled != 0);
    return LV_OK;
}

int lv_receiver_set_midi_only(LvReceiverHandle* handle, int enabled) {
    if (!handle) return LV_ERR_INVALID_ARG;
    if (!handle->receiver) return LV_ERR_NOT_RUNNING;
    handle->receiver->set_midi_only(enabled != 0);
    return LV_OK;
}

int lv_duplex_set_midi_only(LvDuplexHandle* h, int enabled) {
    if (!h) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (h->tx) lv_sender_set_midi_only(h->tx, enabled);
    if (h->rx) lv_receiver_set_midi_only(h->rx, enabled);
    return LV_OK;
}

// ---- Personal Monitor Bus Server / Client C ABI ----

struct LvMonitorServerHandle {
    std::unique_ptr<lv::PersonalMonitorServer> srv;
};

struct LvMonitorClientHandle {
    std::unique_ptr<lv::PersonalMonitorClient> cli;
};

LvMonitorServerHandle* lv_monitor_server_create(uint32_t sample_rate) {
    auto* h = new LvMonitorServerHandle();
    h->srv = std::make_unique<lv::PersonalMonitorServer>(sample_rate ? sample_rate : 48000);
    return h;
}

void lv_monitor_server_destroy(LvMonitorServerHandle* srv) {
    if (!srv) return;
    if (srv->srv) srv->srv->stop();
    delete srv;
}

int lv_monitor_server_start(LvMonitorServerHandle* srv, uint16_t bind_port) {
    if (!srv || !srv->srv) return LV_ERR_INVALID_ARG;
    return srv->srv->start(bind_port) ? LV_OK : LV_ERR_SOCKET;
}

void lv_monitor_server_stop(LvMonitorServerHandle* srv) {
    if (srv && srv->srv) srv->srv->stop();
}

uint16_t lv_monitor_server_port(LvMonitorServerHandle* srv) {
    return (srv && srv->srv) ? srv->srv->bound_port() : 0;
}

void lv_monitor_server_set_channel_name(LvMonitorServerHandle* srv, int ch, const char* name) {
    if (srv && srv->srv && name) srv->srv->set_channel_name(ch, name);
}

void lv_monitor_server_set_client_mute(LvMonitorServerHandle* srv, int slot, int muted) {
    if (srv && srv->srv) srv->srv->set_client_mute(slot, muted != 0);
}

int lv_monitor_server_get_clients(LvMonitorServerHandle* srv, LV_MonitorClientInfo* out, int max_clients) {
    if (!srv || !srv->srv || !out || max_clients <= 0) return 0;
    const auto list = srv->srv->clients();
    const int n = std::min<int>(int(list.size()), max_clients);
    for (int i = 0; i < n; ++i) {
        out[i] = {};
        out[i].slot = list[i].slot;
        out[i].client_id = list[i].client_id;
        std::snprintf(out[i].name, sizeof(out[i].name), "%s", list[i].name.c_str());
        std::snprintf(out[i].address, sizeof(out[i].address), "%s", list[i].address.c_str());
        out[i].bitrate_kbps = list[i].bitrate_kbps;
        out[i].latency_ms = list[i].latency_ms;
        out[i].muted = list[i].muted ? 1 : 0;
        out[i].connected = list[i].connected ? 1 : 0;
        out[i].packets_tx = list[i].packets_tx;
    }
    return n;
}

void lv_monitor_server_push_audio_planar(LvMonitorServerHandle* srv, const float* const* planes,
                                         int num_channels, uint32_t frames) {
    if (srv && srv->srv) srv->srv->process_and_send(planes, num_channels, frames);
}

LvMonitorClientHandle* lv_monitor_client_create(uint32_t sample_rate) {
    auto* h = new LvMonitorClientHandle();
    h->cli = std::make_unique<lv::PersonalMonitorClient>(sample_rate ? sample_rate : 48000);
    return h;
}

void lv_monitor_client_destroy(LvMonitorClientHandle* cli) {
    if (!cli) return;
    if (cli->cli) cli->cli->disconnect();
    delete cli;
}

int lv_monitor_client_connect(LvMonitorClientHandle* cli, const char* server_ip, uint16_t server_port,
                              const char* client_name, uint16_t bitrate_kbps, uint16_t latency_ms) {
    if (!cli || !cli->cli || !server_ip) return LV_ERR_INVALID_ARG;
    return cli->cli->connect(server_ip, server_port, client_name ? client_name : "Smartphone",
                             bitrate_kbps ? bitrate_kbps : 256, latency_ms ? latency_ms : 20)
               ? LV_OK
               : LV_ERR_SOCKET;
}

void lv_monitor_client_disconnect(LvMonitorClientHandle* cli) {
    if (cli && cli->cli) cli->cli->disconnect();
}

int lv_monitor_client_request_meta(LvMonitorClientHandle* cli) {
    if (!cli || !cli->cli) return LV_ERR_INVALID_ARG;
    return cli->cli->request_meta() ? LV_OK : LV_ERR_SOCKET;
}

int lv_monitor_client_set_channel_mix(LvMonitorClientHandle* cli, int ch, float gain_db, float pan,
                                      int mute, int solo) {
    if (!cli || !cli->cli) return LV_ERR_INVALID_ARG;
    lv::PersonalMonitorChannelMix mix{};
    mix.gain_db = gain_db;
    mix.pan = pan;
    mix.mute = mute != 0;
    mix.solo = solo != 0;
    return cli->cli->set_channel_mix(ch, mix) ? LV_OK : LV_ERR_SOCKET;
}

int lv_monitor_client_set_dsp(LvMonitorClientHandle* cli, const LV_MonitorDspParams* dsp) {
    if (!cli || !cli->cli || !dsp) return LV_ERR_INVALID_ARG;
    lv::PersonalMonitorDspParams p{};
    p.hpf_enabled = dsp->hpf_enabled != 0;
    p.hpf_hz = dsp->hpf_hz;
    p.eq_enabled = dsp->eq_enabled != 0;
    p.eq_low_db = dsp->eq_low_db;
    p.eq_mid_hz = dsp->eq_mid_hz;
    p.eq_mid_db = dsp->eq_mid_db;
    p.eq_high_db = dsp->eq_high_db;
    p.comp_enabled = dsp->comp_enabled != 0;
    p.comp_thresh_db = dsp->comp_thresh_db;
    p.comp_ratio = dsp->comp_ratio;
    p.comp_attack_ms = dsp->comp_attack_ms;
    p.comp_release_ms = dsp->comp_release_ms;
    p.comp_makeup_db = dsp->comp_makeup_db;
    p.master_volume_db = dsp->master_volume_db;
    p.master_mute = dsp->master_mute != 0;
    return cli->cli->set_dsp(p) ? LV_OK : LV_ERR_SOCKET;
}

void lv_monitor_client_poll(LvMonitorClientHandle* cli) {
    if (cli && cli->cli) cli->cli->poll();
}

int lv_monitor_client_get_meta(LvMonitorClientHandle* cli, LV_MonitorMetaChannel* out, int max_channels,
                               LV_MonitorTelemetry* tel) {
    if (!cli || !cli->cli) return LV_ERR_INVALID_ARG;
    const auto meta = cli->cli->last_meta();
    const int n = int(meta.size());
    if (out && max_channels > 0) {
        for (int i = 0; i < std::min(n, max_channels); ++i) {
            std::memset(&out[i], 0, sizeof(out[i]));
            std::memcpy(out[i].name, meta[size_t(i)].name, sizeof(out[i].name));
            out[i].name[sizeof(out[i].name) - 1] = '\0';
            out[i].peak_dbfs = meta[size_t(i)].peak_dbfs;
        }
    }
    if (tel) {
        std::memset(tel, 0, sizeof(*tel));
        const auto bt = cli->cli->last_bus_telemetry();
        tel->connected = cli->cli->is_connected() ? 1 : 0;
        tel->num_channels = n;
        tel->bus_gain_reduction_db = bt.gain_reduction_db;
        tel->bus_peak_dbfs = float(bt.mix_peak_dbfs);
        tel->buffer_ms = cli->cli->buffer_ms();
        tel->audio_packets_rx = cli->cli->audio_packets_rx();
        tel->meta_packets_rx = cli->cli->meta_packets_rx();
    }
    return n;
}

int lv_monitor_client_set_stream(LvMonitorClientHandle* cli, uint16_t bitrate_kbps, uint16_t latency_ms) {
    if (!cli || !cli->cli || latency_ms < 2 || latency_ms > 500) return LV_ERR_INVALID_ARG;
    if (bitrate_kbps != LV_MONITOR_BITRATE_LOSSLESS && (bitrate_kbps < 128 || bitrate_kbps > 512))
        return LV_ERR_INVALID_ARG;
    return cli->cli->update_stream(bitrate_kbps, latency_ms) ? LV_OK : LV_ERR_NOT_RUNNING;
}

int lv_monitor_client_keepalive(LvMonitorClientHandle* cli) {
    if (!cli || !cli->cli) return LV_ERR_INVALID_ARG;
    return cli->cli->keepalive() ? LV_OK : LV_ERR_NOT_RUNNING;
}

void lv_monitor_client_pop_stereo(LvMonitorClientHandle* cli, float* out_interleaved, uint32_t frames) {
    if (cli && cli->cli) cli->cli->pop_stereo(out_interleaved, frames);
}

// ---- Tally & Duplex Intercom C ABI ----

struct LvTallyIntercomHandle {
    std::unique_ptr<lv::TallyIntercomEngine> eng;
};

namespace {
std::mutex g_studio_ti_mu;
lv::TallyIntercomEngine* g_active_studio_ti = nullptr;
}  // namespace

LvTallyIntercomHandle* lv_tally_intercom_create_studio(uint16_t bind_port) {
    auto* h = new LvTallyIntercomHandle();
    h->eng = std::make_unique<lv::TallyIntercomEngine>();
    if (!h->eng->start_studio(bind_port)) {
        delete h;
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_studio_ti_mu);
        g_active_studio_ti = h->eng.get();
    }
    return h;
}

LvTallyIntercomHandle* lv_tally_intercom_create_camera(const char* studio_ip, uint16_t studio_port,
                                                       uint32_t camera_id, const char* camera_label) {
    if (!studio_ip) return nullptr;
    auto* h = new LvTallyIntercomHandle();
    h->eng = std::make_unique<lv::TallyIntercomEngine>();
    if (!h->eng->start_camera(studio_ip, studio_port, camera_id, camera_label ? camera_label : "CAM 1")) {
        delete h;
        return nullptr;
    }
    return h;
}

void lv_tally_intercom_destroy(LvTallyIntercomHandle* handle) {
    if (!handle) return;
    {
        std::lock_guard<std::mutex> lk(g_studio_ti_mu);
        if (g_active_studio_ti == handle->eng.get()) g_active_studio_ti = nullptr;
    }
    if (handle->eng) handle->eng->stop();
    delete handle;
}

uint16_t lv_tally_intercom_port(LvTallyIntercomHandle* handle) {
    return (handle && handle->eng) ? handle->eng->bound_port() : 0;
}

void lv_tally_intercom_poll(LvTallyIntercomHandle* handle) {
    if (handle && handle->eng) handle->eng->poll();
}

int lv_tally_intercom_set_camera_tally(LvTallyIntercomHandle* studio, uint32_t stream_id,
                                       uint8_t tally_state, const char* camera_label, int rec_trigger) {
    if (!studio || !studio->eng) return LV_ERR_INVALID_ARG;
    return studio->eng->studio_set_camera_tally(stream_id, tally_state, camera_label ? camera_label : "",
                                                rec_trigger != 0)
               ? LV_OK
               : LV_ERR_INVALID_ARG;
}

int lv_studio_set_camera_tally(uint32_t stream_id, uint8_t tally_state, const char* camera_label,
                               int rec_trigger) {
    std::lock_guard<std::mutex> lk(g_studio_ti_mu);
    if (!g_active_studio_ti) return LV_ERR_NOT_RUNNING;
    return g_active_studio_ti->studio_set_camera_tally(stream_id, tally_state, camera_label ? camera_label : "",
                                                       rec_trigger != 0)
               ? LV_OK
               : LV_ERR_INVALID_ARG;
}

void lv_camera_set_tally_callback(LvTallyIntercomHandle* handle, LV_TallyCallback cb, void* user_data) {
    if (!handle || !handle->eng) return;
    if (!cb) {
        handle->eng->camera_set_tally_callback(nullptr);
        return;
    }
    handle->eng->camera_set_tally_callback([cb, user_data](const lv::TallyStatePayload& tp) {
        LV_TallyState out{};
        out.camera_id = tp.camera_id;
        out.tally_state = tp.tally_state;
        out.flags = tp.flags;
        out.battery_pct = tp.battery_pct;
        out.temp_c = tp.temp_c;
        out.revision = tp.revision;
        std::memcpy(out.camera_label, tp.camera_label, sizeof(out.camera_label));
        out.camera_label[sizeof(out.camera_label) - 1] = '\0';
        cb(&out, user_data);
    });
}

void lv_tally_intercom_update_camera_telemetry(LvTallyIntercomHandle* handle, uint8_t battery_pct,
                                               int8_t temp_c) {
    if (handle && handle->eng) handle->eng->camera_update_telemetry(battery_pct, temp_c);
}

int lv_intercom_push_mic_pcm(LvTallyIntercomHandle* handle, const float* samples, int frames) {
    if (!handle || !handle->eng || !samples || frames <= 0) return LV_ERR_INVALID_ARG;
    handle->eng->push_mic_pcm(samples, frames);
    return LV_OK;
}

int lv_intercom_pull_rx_pcm(LvTallyIntercomHandle* handle, float* out_samples, int frames) {
    if (!handle || !handle->eng || !out_samples || frames <= 0) return 0;
    return handle->eng->pull_rx_pcm(out_samples, frames);
}

void lv_intercom_set_state(LvTallyIntercomHandle* handle, int talk_active, int listen_muted,
                           float rx_gain, float sidetone_gain) {
    if (handle && handle->eng) {
        handle->eng->set_intercom_state(talk_active != 0, listen_muted != 0, rx_gain, sidetone_gain);
    }
}

int lv_tally_intercom_set_camera_intercom(LvTallyIntercomHandle* studio, uint32_t camera_id,
                                          int talk_to_cam, int listen_from_cam, int all_call) {
    if (!studio || !studio->eng) return LV_ERR_INVALID_ARG;
    studio->eng->studio_set_camera_intercom(camera_id, talk_to_cam != 0, listen_from_cam != 0, all_call != 0);
    return LV_OK;
}

int lv_studio_set_camera_intercom(uint32_t camera_id, int talk_to_cam, int listen_from_cam, int all_call) {
    std::lock_guard<std::mutex> lk(g_studio_ti_mu);
    if (!g_active_studio_ti) return LV_ERR_NOT_RUNNING;
    g_active_studio_ti->studio_set_camera_intercom(camera_id, talk_to_cam != 0, listen_from_cam != 0,
                                                   all_call != 0);
    return LV_OK;
}

int lv_tally_intercom_is_director_speaking(LvTallyIntercomHandle* handle) {
    return (handle && handle->eng && handle->eng->is_director_speaking()) ? 1 : 0;
}

float lv_tally_intercom_ducking_gain(LvTallyIntercomHandle* handle) {
    return (handle && handle->eng) ? handle->eng->ducking_gain() : 1.0f;
}

static lv::ControlSyncPayload to_wire_cs(const LV_ControlSyncState& s) {
    lv::ControlSyncPayload cs{};
    cs.revision = s.revision;
    cs.origin_id = s.origin_id;
    cs.audio_channels = s.audio_channels;
    cs.audio_bitrate_kbps = s.audio_bitrate_kbps;
    cs.target_buffer_ms = s.target_buffer_ms;
    cs.latency_mode = s.latency_mode;
    cs.audio_only = s.audio_only;
    cs.active_preset_id = s.active_preset_id;
    cs.flags = s.flags;
    cs.master_gain_cb = s.master_gain_cb;
    cs.audio_codec = s.audio_codec;
    return cs;
}

int lv_sender_send_control_sync(LvSenderHandle* handle, const LV_ControlSyncState* state) {
    if (!handle || !state) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    return handle->sender->send_control_sync(to_wire_cs(*state)) ? LV_OK : LV_ERR_SOCKET;
}

void lv_sender_set_control_sync_callback(LvSenderHandle* handle, LV_ControlSyncCallback cb, void* user_data) {
    if (!handle) return;
    {
        std::lock_guard<std::mutex> lk(handle->cs_cb_mu);
        handle->cs_cb = cb;
        handle->cs_ud = user_data;
    }
    if (handle->sender) {
        handle->sender->set_control_sync_callback([handle](const lv::ControlSyncPayload& cs) {
            LV_ControlSyncCallback f = nullptr;
            void* ud = nullptr;
            {
                std::lock_guard<std::mutex> lk(handle->cs_cb_mu);
                f = handle->cs_cb;
                ud = handle->cs_ud;
            }
            if (!f) return;
            LV_ControlSyncState st{cs.revision, cs.origin_id, cs.audio_channels, cs.audio_bitrate_kbps,
                                   cs.target_buffer_ms, cs.latency_mode, cs.audio_only, cs.active_preset_id,
                                   cs.flags, cs.master_gain_cb, cs.audio_codec};
            f(&st, ud);
        });
    }
}

int lv_receiver_send_control_sync(LvReceiverHandle* handle, const LV_ControlSyncState* state) {
    if (!handle || !state) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) return LV_ERR_NOT_RUNNING;
    return handle->receiver->send_control_sync(to_wire_cs(*state)) ? LV_OK : LV_ERR_SOCKET;
}

void lv_receiver_set_control_sync_callback(LvReceiverHandle* handle, LV_ControlSyncCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->cs_cb = cb;
    handle->cs_ud = user_data;
}

int lv_duplex_send_control_sync(LvDuplexHandle* h, const LV_ControlSyncState* state) {
    if (!h || !state) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (!h->tx) return LV_ERR_NOT_RUNNING;
    return lv_sender_send_control_sync(h->tx, state);
}

void lv_duplex_set_control_sync_callback(LvDuplexHandle* h, LV_ControlSyncCallback cb, void* user_data) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cs_cb = cb;
    h->cs_ud = user_data;
    if (h->rx) lv_receiver_set_control_sync_callback(h->rx, cb, user_data);
}

int lv_duplex_set_latency_ms(LvDuplexHandle* h, uint32_t ms) {
    if (!h || ms < 2 || ms > 2000) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.latency_ms = ms;
    if (h->tx) lv_sender_set_audio_latency_ms(h->tx, ms);
    if (h->rx) lv_receiver_set_audio_latency_ms(h->rx, ms);
    return LV_OK;
}

int lv_duplex_set_adaptive_jitter(LvDuplexHandle* h, int enabled) {
    if (!h) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.adaptive_jitter = enabled ? 1 : 0;
    if (h->rx) lv_receiver_set_adaptive_jitter(h->rx, h->cfg.adaptive_jitter);
    return LV_OK;
}

int lv_duplex_set_audio_codec(LvDuplexHandle* h, int codec) {
    if (!h || codec < 0 || codec > 8) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.audio_codec = codec;
    if (h->tx) lv_sender_set_audio_codec(h->tx, codec);
    return LV_OK;
}

int lv_duplex_set_audio_bitrate(LvDuplexHandle* h, int kbps_per_ch) {
    if (!h) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.audio_codec = int(lv::audio_codec_profile_from_bitrate(kbps_per_ch));
    if (h->tx) lv_sender_set_audio_bitrate(h->tx, kbps_per_ch);
    return LV_OK;
}

int lv_duplex_get_stats(LvDuplexHandle* h, LV_DuplexStats* out) {
    if (!h || !out) return LV_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    std::lock_guard<std::mutex> lk(h->api_mu);
    out->running = h->running.load() ? 1 : 0;
    out->adaptive_jitter = h->cfg.adaptive_jitter ? 1 : 0;
    out->effective_target_ms = h->cfg.latency_ms;
    if (!out->running) return LV_ERR_NOT_RUNNING;
#ifdef LV_HAVE_SHM_BUS
    const auto rs = h->rx_host.status();
    const auto ts = h->tx_host.status();
    out->driver_connected = rs.driver_connected ? 1 : 0;
    std::snprintf(out->driver_name, sizeof(out->driver_name), "%s", rs.driver_name.c_str());
    out->sample_rate = rs.sample_rate;
    out->buffer_size = rs.buffer_size;
    out->rx_frames_to_daw = rs.rx_frames;
    out->rx_underruns = rs.rx_underruns;
    out->tx_frames_from_daw = ts.tx_frames;
    out->tx_overruns = ts.tx_overruns;
#endif
    if (h->rx && h->rx->receiver) {
        out->midi_rx_events = h->rx->receiver->midi_stats().events_unique;
        out->stream_epoch_resets = h->rx->receiver->stream_epoch_resets();
        out->active_midi_notes = h->rx->receiver->active_midi_notes();
        const auto& mc = h->rx->receiver->master_clock();
        out->rx_clock_synced = mc.synced() ? 1 : 0;
        out->rx_rtt_us = mc.min_rtt_us();
        out->rx_clock_skew_ppm = mc.skew_ppm();
        out->effective_latency_ms = h->rx->receiver->audio_effective_latency_ms();
        out->effective_target_ms = h->rx->receiver->audio_effective_target_ms();
        for (uint16_t c = 0; c < uint16_t(h->cfg.num_channels); ++c) {
            const auto st = h->rx->receiver->audio_channel_rx_stats(c);
            out->rx_packets += st.packets_rx;
            out->rx_lost += st.packets_lost;
        }
    }
    if (h->tx && h->tx->sender) {
        out->tx_packets = h->tx->sender->audio2_stats().packets_sent;
        out->tx_pongs_sent = h->tx->sender->stats().pongs_sent;
        out->midi_tx_events = h->tx->sender->midi_stats().events_pushed;
    }
    return LV_OK;
}

static lv::ChannelPatchbay* lv_duplex_patch(LvDuplexHandle* h, LV_AudioDirection dir) {
    return dir == LV_AUDIO_DIR_TX ? &h->tx_patch : &h->rx_patch;
}

int lv_audio_set_tx_routing(LvDuplexHandle* h, const int8_t dst_of_src[64]) {
    if (!h) return LV_ERR_INVALID_ARG;
    if (dst_of_src) h->tx_patch.set_routing(dst_of_src);
    else h->tx_patch.reset_identity();
    return LV_OK;
}

int lv_audio_set_rx_routing(LvDuplexHandle* h, const int8_t dst_of_src[64]) {
    if (!h) return LV_ERR_INVALID_ARG;
    if (dst_of_src) h->rx_patch.set_routing(dst_of_src);
    else h->rx_patch.reset_identity();
    return LV_OK;
}

int lv_audio_get_routing(LvDuplexHandle* h, LV_AudioDirection dir, int8_t dst_of_src[64]) {
    if (!h || !dst_of_src || (dir != LV_AUDIO_DIR_TX && dir != LV_AUDIO_DIR_RX)) return LV_ERR_INVALID_ARG;
    lv_duplex_patch(h, dir)->get_routing(dst_of_src);
    return LV_OK;
}

int lv_audio_set_channel_mute_solo(LvDuplexHandle* h, LV_AudioDirection dir, uint16_t ch, int mute, int solo) {
    if (!h || ch >= 64 || (dir != LV_AUDIO_DIR_TX && dir != LV_AUDIO_DIR_RX)) return LV_ERR_INVALID_ARG;
    lv::ChannelPatchbay* p = lv_duplex_patch(h, dir);
    if (mute >= 0) p->set_mute(ch, mute != 0);
    if (solo >= 0) p->set_solo(ch, solo != 0);
    return LV_OK;
}

int lv_discovery_scan_audio_nodes(int timeout_ms, uint16_t discovery_port, LV_AudioNode* out, int max_nodes) {
    if (!out || max_nodes <= 0) return 0;
    const auto found = lv::discovery_scan(timeout_ms, discovery_port ? discovery_port : lv::kDiscoveryPort);
    // One node may answer on several interfaces (LAN IP + 127.0.0.1): keep one entry per
    // (hostname, name, rx_port), preferring a non-loopback address.
    std::vector<const lv::DiscoveredReceiver*> uniq;
    for (const auto& r : found) {
        if (!r.aoip) continue;
        bool merged = false;
        for (auto& u : uniq) {
            if (u->hostname == r.hostname && u->device_name == r.device_name && u->aoip_rx_port == r.aoip_rx_port) {
                if (u->ip.rfind("127.", 0) == 0 && r.ip.rfind("127.", 0) != 0) u = &r;
                merged = true;
                break;
            }
        }
        if (!merged) uniq.push_back(&r);
    }
    int n = 0;
    for (const auto* rp : uniq) {
        if (n >= max_nodes) break;
        const auto& r = *rp;
        LV_AudioNode& o = out[n++];
        std::memset(&o, 0, sizeof(o));
        std::snprintf(o.ip, sizeof(o.ip), "%s", r.ip.c_str());
        std::snprintf(o.name, sizeof(o.name), "%s", r.device_name.c_str());
        std::snprintf(o.hostname, sizeof(o.hostname), "%s", r.hostname.c_str());
        std::snprintf(o.audio_driver, sizeof(o.audio_driver), "%s", r.audio_driver.c_str());
        std::snprintf(o.os, sizeof(o.os), "%s", r.os.c_str());
        o.rx_port = r.aoip_rx_port;
        o.tx_port = r.aoip_tx_port;
        o.channels = r.audio_channels;
        o.duplex = r.duplex ? 1 : 0;
        o.driver_connected = r.driver_connected ? 1 : 0;
        o.rtt_ms = r.rtt_ms;
    }
    return n;
}

char* lv_discovery_scan_port(int timeout_ms, uint16_t discovery_port) {
    const auto found = lv::discovery_scan(timeout_ms, discovery_port ? discovery_port : lv::kDiscoveryPort);
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < found.size(); ++i) {
        if (i) os << ",";
        os << "{\"ip\":\"" << json_escape(found[i].ip) << "\",\"port\":" << found[i].stream_port
           << ",\"name\":\"" << json_escape(found[i].device_name) << "\",\"os\":\""
           << json_escape(found[i].os) << "\",\"simd\":\"" << json_escape(found[i].simd)
           << "\",\"rtt_ms\":" << found[i].rtt_ms << "}";
    }
    os << "]";
    return dup_cstr(os.str());
}

char* lv_discovery_scan(int timeout_ms) { return lv_discovery_scan_port(timeout_ms, lv::kDiscoveryPort); }

/* ---- Legacy / Desktop Studio Compatibility Wrappers (excluded in LANVID_SDK_CORE_ONLY) ---- */
#ifndef LANVID_SDK_CORE_ONLY

void lv_sender_config_default(LvSenderConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->dest_ip, sizeof(cfg->dest_ip), "127.0.0.1");
    cfg->dest_port = 5000;
    cfg->source_type = LV_SOURCE_PATTERN;
    cfg->width = 1920;
    cfg->height = 1080;
    cfg->fps = 60.0;
    cfg->quality = 80;
    cfg->slice_height = 16;
    cfg->mtu = int(lv::kDefaultMtu);
    cfg->enable_crc = 1;
    cfg->q_min = 35;
    cfg->q_max = 85;
    cfg->audio_device_index = -1;
    cfg->audio_chunk_ms = 5.0;
    cfg->codec = LV_CODEC_LANVID_DCT;
    cfg->bitrate_kbps = 8000;
    cfg->keyframe_interval = 120;
}

void lv_receiver_config_default(LvReceiverConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->bind_ip, sizeof(cfg->bind_ip), "0.0.0.0");
    cfg->bind_port = 5000;
    cfg->conceal_mode = LV_CONCEAL_PREV_FRAME;
    cfg->frame_timeout_ms = 25.0;
    cfg->enable_audio = 1;
    cfg->audio_target_ms = 12.0;
    cfg->enable_discovery = 1;
    cfg->discovery_port = lv::kDiscoveryPort;
    cfg->adaptive_jitter = 1;
}

int lv_sender_start(LvSenderHandle* handle, const LvSenderConfig* cfg) {
    if (!handle || !cfg) return 0;
    LV_SenderConfig sc{};
    lv_sdk_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "%s", cfg->dest_ip);
    sc.dest_port = cfg->dest_port;
    sc.width = cfg->width;
    sc.height = cfg->height;
    sc.fps = cfg->fps;
    sc.quality = cfg->quality;
    sc.chroma = cfg->chroma;
    sc.slice_height = cfg->slice_height;
    sc.mtu = cfg->mtu;
    sc.enable_crc = cfg->enable_crc;
    sc.fec_group_size = cfg->fec_group_size;
    sc.adaptive_quality = cfg->adaptive_quality;
    sc.q_min = cfg->q_min;
    sc.q_max = cfg->q_max;
    sc.threads = cfg->threads;
    sc.pace_mbps = cfg->pace_mbps;
    sc.codec = cfg->codec;
    sc.bitrate_kbps = cfg->bitrate_kbps > 0 ? cfg->bitrate_kbps : 8000u;
    sc.keyframe_interval = cfg->keyframe_interval > 0 ? cfg->keyframe_interval : 120u;

    if (lv_sdk_sender_start(handle, &sc) != LV_OK) return 0;

    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->legacy_cfg = *cfg;
    if (handle->sender) {
        if (cfg->audio_bitrate_kbps > 0) {
            handle->sender->set_audio_bitrate_kbps(cfg->audio_bitrate_kbps);
        } else {
            handle->sender->set_audio_codec_profile(static_cast<lv::AudioCodecProfile>(std::clamp(cfg->audio_codec, 0, 8)));
        }
    }
    if (cfg->sim_loss_pct > 0.0 && handle->sender) handle->sender->set_sim_loss_pct(cfg->sim_loss_pct);

    if (cfg->audio_mode == LV_AUDIO_LOOPBACK || cfg->audio_mode == LV_AUDIO_MIC) {
        auto ac = std::make_unique<lv::AudioCapture>();
        const auto mode = (cfg->audio_mode == LV_AUDIO_LOOPBACK) ? lv::AudioCaptureMode::kLoopback
                                                                 : lv::AudioCaptureMode::kMic;
        const uint16_t chunk_frames = (cfg->audio_chunk_ms > 0.0 && cfg->audio_chunk_ms <= 3.0) ? 120 : 240;
        lv::UdpSender* raw_sender = handle->sender.get();
        if (ac->start(mode, cfg->audio_device_index, chunk_frames,
                      [raw_sender](uint32_t seq_id, uint32_t ts_us, const int16_t* pcm, uint16_t n) {
                          raw_sender->send_audio(seq_id, ts_us, pcm, n, 48000, 2);
                      })) {
            handle->audio_cap = std::move(ac);
        }
    }

    handle->video_source_error.store(LV_VIDEO_SOURCE_OK, std::memory_order_release);
    // Control loop without video: answers MasterClock pings / NACKs / control sync and keeps AoIP alive.
    // Used for audio-only mode AND as the fallback when a video source cannot be opened, so turning video
    // on never takes the audio stream down with it.
    static const auto audio_only_loop = [](LvSenderHandle* h) {
        while (!h->stop.load(std::memory_order_relaxed)) {
            h->sender->service_control_try();
            lv::sleep_until_us(lv::now_us() + 2000, [] {});
        }
    };
    if (cfg->audio_only) {
        // Audio-only mode (identical to CLI --audio-only): no video capture/encoder/packets.
        handle->cap_thread = std::thread([handle]() {
            audio_only_loop(handle);
            handle->running.store(false, std::memory_order_release);
        });
        return 1;
    }

    if (cfg->source_type == LV_SOURCE_EXTERNAL) return 1;

    const LvSenderConfig cap_cfg = *cfg;
    handle->cap_thread = std::thread([handle, cap_cfg]() {
        lv::ScreenCapture screen;
        lv::CameraCapture camera;
        lv::FileVideoSource fsrc;

        lv::CodecParams cp = handle->codec_params;
        double fps = cap_cfg.fps > 1.0 ? cap_cfg.fps : 60.0;
        lv::Image frame;

        if (cap_cfg.source_type == LV_SOURCE_SCREEN) {
            const auto be = (cap_cfg.capture_backend == LV_CAP_DXGI)
                                ? lv::CaptureBackend::kDxgi
                                : (cap_cfg.capture_backend == LV_CAP_GDI ? lv::CaptureBackend::kGdi
                                                                         : lv::CaptureBackend::kAuto);
            if (!screen.open(cap_cfg.monitor_index, be)) {
                // Screen Recording not granted (macOS TCC), no capture backend on this platform, device
                // busy / missing: report it and keep the audio stream running without video.
                handle->video_source_error.store(LV_VIDEO_SOURCE_SCREEN_UNAVAILABLE, std::memory_order_release);
                audio_only_loop(handle);
                handle->running.store(false, std::memory_order_release);
                return;
            }
            cp.width = screen.native_width();
            cp.height = screen.native_height();
            frame.allocate(cp.width, cp.height, lv::PixelOrder::kBGRA);
        } else if (cap_cfg.source_type == LV_SOURCE_CAMERA) {
            if (!camera.open(cap_cfg.camera_index, cap_cfg.width, cap_cfg.height, int(fps))) {
                // Screen Recording not granted (macOS TCC), no capture backend on this platform, device
                // busy / missing: report it and keep the audio stream running without video.
                handle->video_source_error.store(LV_VIDEO_SOURCE_CAMERA_UNAVAILABLE, std::memory_order_release);
                audio_only_loop(handle);
                handle->running.store(false, std::memory_order_release);
                return;
            }
            cp.width = camera.native_width();
            cp.height = camera.native_height();
            frame.allocate(cp.width, cp.height, lv::PixelOrder::kBGRA);
        } else if (cap_cfg.source_type == LV_SOURCE_FILE) {
            if (!fsrc.open(cap_cfg.file_path, cap_cfg.width, cap_cfg.height, cap_cfg.file_is_rgba != 0)) {
                // Screen Recording not granted (macOS TCC), no capture backend on this platform, device
                // busy / missing: report it and keep the audio stream running without video.
                handle->video_source_error.store(LV_VIDEO_SOURCE_FILE_UNAVAILABLE, std::memory_order_release);
                audio_only_loop(handle);
                handle->running.store(false, std::memory_order_release);
                return;
            }
            cp.width = fsrc.width();
            cp.height = fsrc.height();
            if (fsrc.fps() > 1.0 && cap_cfg.fps <= 0.0) fps = fsrc.fps();
            frame.allocate(cp.width, cp.height, lv::PixelOrder::kBGRA);
        } else {
            if (cp.width <= 0) cp.width = 1920;
            if (cp.height <= 0) cp.height = 1080;
            frame.allocate(cp.width, cp.height, lv::PixelOrder::kBGRA);
        }

        const uint64_t period_us = uint64_t(1e6 / fps);
        uint64_t next_tick = lv::now_us();
        uint32_t fid = 0;

        while (!handle->stop.load(std::memory_order_relaxed)) {
            handle->sender->service_control();
            bool ok_frame = false;
            if (cap_cfg.source_type == LV_SOURCE_SCREEN) {
                ok_frame = screen.capture(frame, 16);
                if (!ok_frame && fid > 0) ok_frame = true;
            } else if (cap_cfg.source_type == LV_SOURCE_CAMERA) {
                ok_frame = camera.grab(frame, 100);
            } else if (cap_cfg.source_type == LV_SOURCE_FILE) {
                ok_frame = fsrc.read_frame(frame);
            } else {
                lv::generate_test_frame(frame, fid);
                ok_frame = true;
            }

            if (ok_frame && frame.width > 0 && frame.height > 0) {
                cp.width = frame.width;
                cp.height = frame.height;
                cp.quality = handle->resolve_quality();
                const uint64_t t0 = lv::now_us();
                handle->sender->send_frame(frame, cp, fid, uint32_t(t0));
                const uint64_t t1 = lv::now_us();
                handle->record_encode_sample(double(t1 - t0) / 1000.0, cp.width, cp.height);
                if ((fid & 1u) == 0u || fps <= 30.0) handle->update_preview(frame, fid + 1);
                ++fid;
            }

            next_tick += period_us;
            const uint64_t now = lv::now_us();
            if (next_tick > now) {
                lv::sleep_until_us(next_tick, [&]() { handle->sender->service_control(); });
            } else if (now - next_tick > period_us * 2) {
                next_tick = now;
            }
        }
        screen.close();
        camera.close();
        fsrc.close();
        handle->running.store(false, std::memory_order_release);
    });
    return 1;
}

int lv_sender_push_raw_frame(LvSenderHandle* handle, const uint8_t* data, int width, int height,
                             int stride, int pixel_format, uint32_t timestamp_us) {
    const int fmt = (pixel_format == LV_PIXFMT_BGRA) ? LV_PIXEL_FORMAT_BGRA8888
                                                     : LV_PIXEL_FORMAT_RGBA8888;
    return lv_sender_push_packed_frame(handle, data, width, height, stride, fmt, timestamp_us) == LV_OK ? 1 : 0;
}

int lv_sender_push_yuv420_frame(LvSenderHandle* handle, const uint8_t* y, const uint8_t* u,
                                const uint8_t* v, int width, int height, int y_stride, int u_stride,
                                int v_stride, int uv_pixel_stride, uint32_t timestamp_us) {
    LV_YuvPlanes yp{};
    yp.plane_y = y;
    yp.plane_u = u;
    yp.plane_v = v;
    yp.y_stride = y_stride;
    yp.u_stride = u_stride;
    yp.v_stride = v_stride;
    yp.uv_pixel_stride = uv_pixel_stride > 0 ? uv_pixel_stride : 1;
    yp.pixel_format = (yp.uv_pixel_stride == 2) ? LV_PIXEL_FORMAT_NV12 : LV_PIXEL_FORMAT_I420;
    return lv_sender_push_yuv_planes(handle, &yp, width, height, timestamp_us) == LV_OK ? 1 : 0;
}

int lv_sender_push_audio_pcm16(LvSenderHandle* handle, const int16_t* pcm, int num_frames,
                               int sample_rate, int channels, uint32_t timestamp_us) {
    return lv_sender_push_audio_chunk(handle, pcm, num_frames, sample_rate, channels,
                                      LV_AUDIO_FORMAT_PCM16, timestamp_us) == LV_OK
               ? 1
               : 0;
}

int lv_sender_get_preview_frame(LvSenderHandle* handle, uint8_t* out_buf, size_t cap, int* out_w,
                                int* out_h, uint64_t* inout_frame_id, int want_rgba) {
    if (!handle) return 0;
    std::lock_guard<std::mutex> lk(handle->preview_mu);
    if (handle->preview_frame_id == 0 || handle->preview_img.width <= 0) return 0;
    if (out_w) *out_w = handle->preview_img.width;
    if (out_h) *out_h = handle->preview_img.height;
    if (inout_frame_id && *inout_frame_id == handle->preview_frame_id) return 0;
    if (!out_buf || cap < handle->preview_img.data.size()) return -1;
    copy_image_order(handle->preview_img, out_buf, want_rgba != 0);
    if (inout_frame_id) *inout_frame_id = handle->preview_frame_id;
    return 1;
}

int lv_sender_get_stats(LvSenderHandle* handle, LvSenderStats* out) {
    if (!handle || !out) return 0;
    LV_StreamStats st{};
    lv_sender_get_stream_stats(handle, &st);
    std::memset(out, 0, sizeof(*out));
    out->running = handle->running.load(std::memory_order_acquire) ? 1 : 0;
    out->video_source_error = handle->video_source_error.load(std::memory_order_acquire);
    out->width = st.width;
    out->height = st.height;
    out->tx_fps = st.fps;
    out->tx_mbps = st.bitrate_mbps;
    out->encode_tx_ms = st.encode_ms;
    out->active_quality = st.active_quality;
    out->rx_loss_pct = st.packet_loss_pct;
    out->frames_sent = st.frames_total;
    out->packets_sent = st.packets_total;
    out->audio_packets_sent = st.audio_packets;
    out->audio_kbps = st.audio_kbps;
    out->active_codec = st.active_codec;
    out->codec_is_hardware = st.codec_is_hardware;
    out->target_bitrate_kbps = st.target_bitrate_kbps;
    std::snprintf(out->codec_backend, sizeof(out->codec_backend), "%s", st.codec_backend);
    if (handle->sender) {
        const auto raw = handle->sender->stats();
        out->send_errors = raw.send_errors;
        out->rx_fec_pct = double(raw.last_rx_fec_permille) / 10.0;
    }
    return 1;
}

int lv_receiver_start(LvReceiverHandle* handle, const LvReceiverConfig* cfg) {
    if (!handle || !cfg) return 0;
    std::lock_guard<std::mutex> lk(handle->api_mu);

    lv::ReceiverConfig rcfg;
    rcfg.bind_ip = cfg->bind_ip[0] ? cfg->bind_ip : "0.0.0.0";
    rcfg.bind_port = cfg->bind_port;
    rcfg.decode_threads = cfg->audio_only ? 1u : uint32_t(std::max(0, cfg->decode_threads));
    rcfg.assembler.conceal =
        (cfg->conceal_mode == LV_CONCEAL_BLACK) ? lv::ConcealMode::kGray : lv::ConcealMode::kPrev;
    rcfg.assembler.frame_timeout_us =
        uint64_t((cfg->frame_timeout_ms > 1.0 ? cfg->frame_timeout_ms : 25.0) * 1000.0);
    rcfg.assembler.output_order = lv::PixelOrder::kBGRA;
    rcfg.audio_target_ms = cfg->audio_target_ms > 0.0 ? cfg->audio_target_ms : 12.0;
    rcfg.audio_sample_rate = handle->bus_cfg.sample_rate == 96000 ? 96000u : 48000u;
    rcfg.audio_latency_ms = std::clamp<uint32_t>(handle->bus_cfg.latency_ms, 2, 2000);
    rcfg.audio_only = (cfg->audio_only != 0);
    handle->adaptive_jitter.store(cfg->adaptive_jitter != 0 ? 1 : 0, std::memory_order_release);
    rcfg.audio_adaptive_jitter = cfg->adaptive_jitter != 0;

    const bool auto_os_audio = (cfg->enable_audio != 0 && cfg->external_audio_pull == 0);
    return handle->start_common(rcfg, auto_os_audio, cfg->enable_discovery != 0,
                                cfg->discovery_port, cfg->device_name) == LV_OK
               ? 1
               : 0;
}

int lv_receiver_get_stats(LvReceiverHandle* handle, LvReceiverStats* out) {
    if (!handle || !out) return 0;
    LV_StreamStats st{};
    lv_receiver_get_stream_stats(handle, &st);
    std::memset(out, 0, sizeof(*out));
    out->running = handle->running.load(std::memory_order_acquire) ? 1 : 0;
    out->width = st.width;
    out->height = st.height;
    out->total_slices = handle->cur_slices;
    out->fps = st.fps;
    out->rx_mbps = st.bitrate_mbps;
    out->latency_p50_ms = st.latency_p50_ms;
    out->latency_p99_ms = st.latency_p99_ms;
    out->loss_pct = st.packet_loss_pct;
    out->frames_complete = st.frames_total;
    out->frames_concealed = st.frames_concealed;
    out->fec_recovered = st.fec_recovered;
    out->bad_crc = st.crc_errors;
    out->late_packets = st.late_packets;
    out->active_quality = st.active_quality;
    out->audio_packets_rx = st.audio_packets;
    out->audio_kbps = st.audio_kbps;
    out->audio_buffer_ms = st.audio_buffer_ms;
    out->active_codec = st.active_codec;
    out->codec_is_hardware = st.codec_is_hardware;
    std::snprintf(out->codec_backend, sizeof(out->codec_backend), "%s", st.codec_backend);
    if (handle->receiver) {
        out->audio_packets_lost = handle->receiver->audio_stats().packets_lost;
        out->pli_requests = handle->receiver->assembler_stats().pli_requests;
        out->effective_latency_ms = handle->receiver->audio_effective_latency_ms();
        out->effective_target_ms = handle->receiver->audio_effective_target_ms();
    }
    out->adaptive_jitter = handle->adaptive_jitter.load(std::memory_order_acquire);
    return 1;
}

#endif  // !LANVID_SDK_CORE_ONLY

}  // extern "C"
