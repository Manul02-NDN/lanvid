#include "lanvid/lanvid_sdk.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio/intercom_engine.hpp"
#include "codec/dct.hpp"
#include "codec/video_codec_common.hpp"
#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "common/cpu_features.hpp"
#include "common/image.hpp"
#include "common/network_interfaces.hpp"
#include "common/rt_thread.hpp"
#include "net/video_discovery.hpp"
#include "net/video_receiver.hpp"
#include "net/video_sender.hpp"
#include "video/multicam_manager.hpp"

#ifndef LANVID_SDK_CORE_ONLY
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

template <size_t N>
void lv_copy_str(char (&dst)[N], const std::string& s) {
    std::snprintf(dst, N, "%s", s.c_str());
}

std::atomic<int> g_init_refs{0};

}  // namespace

#define LV_LIVE_LOCK(h) std::shared_lock<std::shared_mutex> lv_live_lock_((h)->life_mu)

struct LvSenderHandle {
    std::mutex api_mu;
    mutable std::shared_mutex life_mu;
    std::unique_ptr<lv::UdpSender> sender;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> state{LV_STATE_STOPPED};

    lv::CodecParams codec_params{};
    std::atomic<int> override_quality{0};
    std::atomic<bool> adaptive_enabled{false};
    std::atomic<uint32_t> ext_frame_id{0};
    std::atomic<uint32_t> ext_audio_seq{0};
    std::string primary_nic_ip;
    std::string secondary_nic_ip;

    std::mutex push_mu;
    lv::Image ext_img;
    std::vector<uint8_t> yuv_scratch;

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
    int cur_w = 0;
    int cur_h = 0;

    int resolve_quality() const {
        if (adaptive_enabled.load(std::memory_order_relaxed) && sender) {
            return sender->active_quality();
        }
        const int oq = override_quality.load(std::memory_order_relaxed);
        return oq > 0 ? oq : codec_params.quality;
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
        running.store(false, std::memory_order_release);
        std::unique_ptr<lv::UdpSender> dead;
        {
            std::unique_lock<std::shared_mutex> lk(life_mu);
            dead = std::move(sender);
        }
        dead.reset();
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
    bool connected_notified = false;

    std::mutex cb_mu;
    LV_VideoFrameCallback video_cb = nullptr;
    void* video_ud = nullptr;
    LV_AudioSamplesCallback audio_cb = nullptr;
    void* audio_ud = nullptr;
    LV_StateChangedCallback state_cb = nullptr;
    void* state_ud = nullptr;

    std::mutex frame_mu;
    lv::Image latest_img;
    uint32_t latest_fid = 0;
    bool has_frame = false;

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

        rx_thread = std::thread([this, auto_os_audio]() {
            lv::RealtimeThreadScope rt(2000, 400, 1500);
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
            std::unique_lock<std::shared_mutex> lk(life_mu);
            dead = std::move(receiver);
        }
        dead.reset();
        if (was_running) notify_state(LV_STATE_STOPPED, LV_OK, "Stopped");
    }

    ~LvReceiverHandle() { stop_internal(); }
};

struct LvTallyIntercomHandle {
    std::unique_ptr<lv::TallyIntercomEngine> eng;
};

struct LvMultiCamHandle {
    lv::MultiCamManager mgr;
};

namespace {
std::mutex g_studio_ti_mu;
lv::TallyIntercomEngine* g_active_studio_ti = nullptr;
}  // namespace

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
LvSenderHandle* lv_sender_create(void) { return new (std::nothrow) LvSenderHandle(); }
void lv_sender_destroy(LvSenderHandle* handle) { delete handle; }

int lv_sdk_sender_start(LvSenderHandle* handle, const LV_SenderConfig* cfg) {
    if (!handle || !cfg) return LV_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
    handle->state.store(LV_STATE_STARTING, std::memory_order_release);

    lv::SenderConfig scfg;
    scfg.dest_ip = cfg->dest_ip[0] ? cfg->dest_ip : "127.0.0.1";
    scfg.dest_port = cfg->dest_port ? cfg->dest_port : 5000;
    scfg.bind_ip = handle->primary_nic_ip;
    scfg.secondary_bind_ip = handle->secondary_nic_ip;
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
    {
        std::unique_lock<std::shared_mutex> lk_life(handle->life_mu);
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
    if (stride > 0 && size_t(stride) < row_bytes) return LV_ERR_INVALID_ARG;
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
    return LV_OK;
}

int lv_sender_push_yuv_planes_ex(LvSenderHandle* handle, const LV_YuvPlanes* planes, int width,
                                 int height, int rotation_deg, int mirror_h,
                                 uint32_t timestamp_us) {
    if (!handle || !planes || !planes->plane_y || !planes->plane_u || width <= 0 || height <= 0) {
        return LV_ERR_INVALID_ARG;
    }
    LV_LIVE_LOCK(handle);
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

int lv_sender_push_cvpixelbuffer(LvSenderHandle* handle, const void* cv_pixel_buffer_ref,
                                 int width, int height, uint32_t timestamp_us) {
    if (!handle || !cv_pixel_buffer_ref || width <= 0 || height <= 0) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;
    if (handle->sender->active_codec() == lv::VideoCodecId::kLanVidDct) return LV_ERR_UNSUPPORTED;

    std::lock_guard<std::mutex> plk(handle->push_mu);
    handle->sender->service_control();

    const int even_w = width & ~1;
    const int even_h = height & ~1;
    const uint32_t fid = handle->ext_frame_id.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ts = timestamp_us ? timestamp_us : lv::now_us32();
    const uint64_t t0 = lv::now_us();
    const bool ok = handle->sender->send_frame_native(cv_pixel_buffer_ref, even_w, even_h, fid, ts);
    const uint64_t t1 = lv::now_us();
    if (!ok) return LV_ERR_CODEC;
    handle->state.store(LV_STATE_CONNECTED, std::memory_order_relaxed);
    handle->record_encode_sample(double(t1 - t0) / 1000.0, even_w, even_h);
    return LV_OK;
}

int lv_sender_push_audio_chunk(LvSenderHandle* handle, const void* data, int num_frames,
                               int sample_rate, int channels, int audio_format,
                               uint32_t timestamp_us) {
    if (!handle || !data || num_frames <= 0 || num_frames > 65535 || channels < 1 ||
        channels > LV_AUDIO_MAX_CHANNELS) {
        return LV_ERR_INVALID_ARG;
    }
    if (audio_format != LV_AUDIO_FORMAT_PCM16 && audio_format != LV_AUDIO_FORMAT_F32) {
        return LV_ERR_UNSUPPORTED;
    }
    LV_LIVE_LOCK(handle);
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return LV_ERR_NOT_RUNNING;
    const auto fmt = (audio_format == LV_AUDIO_FORMAT_F32) ? lv::AudioSampleFormat::kFloat32
                                                           : lv::AudioSampleFormat::kPcm16;
    const uint32_t aseq = handle->ext_audio_seq.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ts = timestamp_us ? timestamp_us : lv::now_us32();
    return handle->sender->send_audio_raw(aseq, ts, data, uint16_t(num_frames),
                                          uint32_t(sample_rate > 0 ? sample_rate : 48000),
                                          uint8_t(channels), fmt)
               ? LV_OK
               : LV_ERR_SOCKET;
}

int lv_sender_set_network_interfaces(LvSenderHandle* handle, const char* primary_ip,
                                     const char* secondary_ip) {
    if (!handle) return LV_ERR_INVALID_ARG;
    const std::string pri = primary_ip ? primary_ip : "";
    const std::string sec = secondary_ip ? secondary_ip : "";
    {
        std::lock_guard<std::mutex> lk(handle->api_mu);
        handle->primary_nic_ip = pri;
        handle->secondary_nic_ip = sec;
    }
    LV_LIVE_LOCK(handle);
    if (handle->sender) {
        handle->sender->set_network_interfaces(pri, sec);
    }
    return LV_OK;
}

int lv_sender_get_failover_stats(LvSenderHandle* handle, uint64_t* out_failover_packets,
                                 uint64_t* out_secondary_bytes) {
    if (!handle) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    uint64_t fp = 0, sb = 0;
    if (handle->sender) {
        fp = handle->sender->failover_packets();
        sb = handle->sender->secondary_bytes_sent();
    }
    if (out_failover_packets) *out_failover_packets = fp;
    if (out_secondary_bytes) *out_secondary_bytes = sb;
    return LV_OK;
}

int lv_sender_request_keyframe_quality(LvSenderHandle* handle, int quality) {
    if (!handle || quality < 1 || quality > 100) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    handle->override_quality.store(quality, std::memory_order_relaxed);
    if (handle->sender) {
        handle->sender->force_keyframe();
        if (handle->adaptive_enabled.load(std::memory_order_relaxed)) {
            handle->sender->set_adaptive_quality(true, 35, 95, quality);
        }
    }
    return LV_OK;
}

int lv_sender_set_bitrate(LvSenderHandle* handle, uint32_t bitrate_kbps) {
    if (!handle || bitrate_kbps < 250u || bitrate_kbps > 150000u) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    handle->sender->set_bitrate_kbps(bitrate_kbps);
    return LV_OK;
}

int lv_sender_force_keyframe(LvSenderHandle* handle) {
    if (!handle) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    handle->sender->force_keyframe();
    return LV_OK;
}

int lv_sender_set_fec_group(LvSenderHandle* handle, int fec_group_size) {
    if (!handle || fec_group_size < 0 || fec_group_size > 64) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->sender) return LV_ERR_NOT_RUNNING;
    handle->sender->set_fec_group_size(uint16_t(fec_group_size));
    return LV_OK;
}

int lv_sender_set_adaptive_quality(LvSenderHandle* handle, int enabled, int q_min, int q_max) {
    if (!handle || q_min < 1 || q_max > 100 || q_min > q_max) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
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
    LV_LIVE_LOCK(handle);
    if (!handle->sender) return LV_OK;

    const lv::SenderStats st = handle->sender->stats();
    handle->sender->service_control_try();
    const uint64_t total_tx_bytes = st.bytes_sent + st.audio_bytes_sent;
    const uint64_t now = lv::now_us();

    std::lock_guard<std::mutex> slk(handle->stats_mu);
    const double dt_s = double(now - handle->win_start_us) / 1e6;
    if (dt_s >= 0.5) {
        const uint64_t df = st.frames_sent - handle->win_prev_frames;
        const uint64_t db = total_tx_bytes - handle->win_prev_bytes;
        const uint64_t dab = st.audio_bytes_sent - handle->win_prev_ab;
        handle->cached_fps = double(df) / dt_s;
        handle->cached_mbps = (double(db) * 8.0 / 1e6) / dt_s;
        handle->cached_audio_kbps = (double(dab) * 8.0 / 1e3) / dt_s;
        handle->cached_enc_ms = handle->enc_ms_cnt ? (handle->enc_ms_sum / double(handle->enc_ms_cnt)) : 0.0;
        handle->win_start_us = now;
        handle->win_prev_frames = st.frames_sent;
        handle->win_prev_bytes = total_tx_bytes;
        handle->win_prev_ab = st.audio_bytes_sent;
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
    out->packets_total = st.packets_sent;
    out->audio_packets = st.audio_packets_sent;
    out->audio_kbps = handle->cached_audio_kbps;
    out->active_codec = int32_t(st.active_codec);
    out->codec_is_hardware = handle->sender->encoder_is_hardware() ? 1 : 0;
    out->target_bitrate_kbps = st.active_bitrate_kbps;
    std::snprintf(out->codec_backend, sizeof(out->codec_backend), "%s",
                  handle->sender->encoder_backend_name().c_str());
    return LV_OK;
}

/* ---- Receiver SDK API ---- */
LvReceiverHandle* lv_receiver_create(void) { return new (std::nothrow) LvReceiverHandle(); }
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

    return handle->start_common(rcfg, cfg->enable_os_audio != 0, cfg->enable_discovery != 0,
                                cfg->discovery_port, cfg->device_name);
}

void lv_receiver_stop(LvReceiverHandle* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
}

uint16_t lv_receiver_get_local_port(LvReceiverHandle* handle) {
    if (!handle) return 0;
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) return 0;
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

int lv_receiver_set_audio_target_ms(LvReceiverHandle* handle, double target_ms) {
    if (!handle || target_ms < 5.0 || target_ms > 250.0) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (handle->receiver) handle->receiver->set_audio_target_ms(target_ms);
    return LV_OK;
}

void lv_receiver_set_volume(LvReceiverHandle* handle, float volume) {
    if (!handle) return;
    const float clamped = std::clamp(volume, 0.0f, 2.0f);
    handle->volume.store(clamped, std::memory_order_relaxed);
    LV_LIVE_LOCK(handle);
    if (handle->receiver) handle->receiver->audio_buffer().set_volume(clamped);
}

void lv_receiver_set_concealment(LvReceiverHandle* handle, int mode) {
    if (!handle) return;
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) return;
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
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) return LV_OK;

    const auto ast = handle->receiver->assembler_stats();
    const auto aud = handle->receiver->audio_stats();
    const uint64_t total_rx_bytes = ast.bytes_total + aud.bytes_rx;
    const uint64_t rx_pkts = handle->receiver->seq_tracker().received();
    const uint64_t lost_pkts = handle->receiver->seq_tracker().lost(0);
    const uint64_t now = lv::now_us();

    std::lock_guard<std::mutex> slk(handle->stats_mu);
    const double dt_s = double(now - handle->win_start_us) / 1e6;
    if (dt_s >= 0.5) {
        const uint64_t db = total_rx_bytes - handle->win_prev_bytes;
        const uint64_t dab = aud.bytes_rx - handle->win_prev_ab;
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
        handle->win_prev_ab = aud.bytes_rx;
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
    out->packets_total = ast.datagrams_total;
    out->fec_recovered = ast.fec_recovered_packets;
    out->crc_errors = ast.crc_errors;
    out->late_packets = ast.late_packets;
    out->audio_packets = aud.packets_rx;
    out->audio_kbps = handle->cached_akbps;
    out->audio_buffer_ms = aud.buffered_ms;
    out->active_codec = int32_t(handle->receiver->active_codec());
    out->codec_is_hardware = handle->receiver->decoder_is_hardware() ? 1 : 0;
    std::snprintf(out->codec_backend, sizeof(out->codec_backend), "%s",
                  handle->receiver->decoder_backend_name().c_str());
    return LV_OK;
}

/* ---- Network Interface Enumeration ---- */
int lv_net_list_interfaces_ex(LvNetInterfaceInfo* out_list, int max_count,
                              int include_disconnected, int show_all) {
    const auto list = lv::enumerate_network_interfaces(include_disconnected != 0, show_all != 0);
    if (out_list && max_count > 0) {
        const int n = std::min<int>(int(list.size()), max_count);
        for (int i = 0; i < n; ++i) {
            const auto& src = list[size_t(i)];
            auto& dst = out_list[i];
            std::memset(&dst, 0, sizeof(dst));
            std::snprintf(dst.id, sizeof(dst.id), "%s", src.id.c_str());
            std::snprintf(dst.system_name, sizeof(dst.system_name), "%s", src.system_name.c_str());
            std::snprintf(dst.friendly_name, sizeof(dst.friendly_name), "%s", src.friendly_name.c_str());
            dst.type = uint8_t(src.type);
            dst.is_up = src.is_up ? 1 : 0;
            std::snprintf(dst.ipv4_address, sizeof(dst.ipv4_address), "%s", src.ipv4_address.c_str());
            std::snprintf(dst.subnet_mask, sizeof(dst.subnet_mask), "%s", src.subnet_mask.c_str());
            std::snprintf(dst.mac_address, sizeof(dst.mac_address), "%s", src.mac_address.c_str());
            dst.link_speed_mbps = src.link_speed_mbps;
            dst.if_index = src.if_index;
        }
    }
    return int(list.size());
}

int lv_net_list_interfaces(LvNetInterfaceInfo* out_list, int max_count) {
    return lv_net_list_interfaces_ex(out_list, max_count, 0, 0);
}

/* ---- Tally & Duplex Intercom C ABI ---- */
LvTallyIntercomHandle* lv_tally_intercom_create_studio(uint16_t bind_port) {
    auto* h = new (std::nothrow) LvTallyIntercomHandle();
    if (!h) return nullptr;
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
    auto* h = new (std::nothrow) LvTallyIntercomHandle();
    if (!h) return nullptr;
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

int lv_tally_intercom_set_camera_control(LvTallyIntercomHandle* studio, const LV_CameraControl* control) {
    if (!studio || !studio->eng || !control) return 0;
    lv::CameraControlPayload c;
    c.lens = control->lens;
    c.focus_auto = control->focus_auto;
    c.focus_pos = control->focus_pos;
    c.quality = control->quality;
    c.flags = control->flags;
    c.zoom_x100 = control->zoom_x100;
    return studio->eng->studio_set_camera_control(control->camera_id, c) ? 1 : 0;
}

void lv_camera_set_control_callback(LvTallyIntercomHandle* handle, LV_CameraControlCallback cb, void* user_data) {
    if (!handle || !handle->eng) return;
    if (!cb) {
        handle->eng->camera_set_control_callback(nullptr);
        return;
    }
    handle->eng->camera_set_control_callback([cb, user_data](const lv::CameraControlPayload& c) {
        LV_CameraControl out{};
        out.camera_id = c.camera_id;
        out.revision = c.revision;
        out.lens = c.lens;
        out.focus_auto = c.focus_auto;
        out.focus_pos = c.focus_pos;
        out.quality = c.quality;
        out.flags = c.flags;
        out.zoom_x100 = c.zoom_x100;
        cb(&out, user_data);
    });
}

int lv_tally_intercom_send_video_config_sync(LvTallyIntercomHandle* handle, const LV_VideoConfigSync* cfg) {
    if (!handle || !handle->eng || !cfg) return LV_ERR_INVALID_ARG;
    lv::VideoConfigSyncPayload p{};
    p.config_revision = cfg->config_revision;
    p.origin_id = cfg->origin_id;
    p.camera_id = cfg->camera_id;
    p.assigned_cam_number = cfg->assigned_cam_number;
    p.resolution_preset = cfg->resolution_preset;
    p.target_fps = cfg->target_fps;
    p.codec = cfg->codec;
    p.bitrate_kbps = cfg->bitrate_kbps;
    p.video_delay_ms = cfg->video_delay_ms;
    p.fixed_delay_mode = cfg->fixed_delay_mode;
    p.camera_lens = cfg->camera_lens;
    p.flags = cfg->flags;
    std::memcpy(p.badge_accent_rgba, cfg->badge_accent_rgba, 4);
    std::memcpy(p.assigned_cam_tag, cfg->assigned_cam_tag, sizeof(p.assigned_cam_tag));
    p.assigned_cam_tag[15] = '\0';
    return handle->eng->send_video_config_sync(cfg->camera_id, p) ? LV_OK : LV_ERR_SOCKET;
}

void lv_tally_intercom_set_video_config_sync_callback(LvTallyIntercomHandle* handle,
                                                     LV_VideoConfigSyncCallback cb, void* user_data) {
    if (!handle || !handle->eng) return;
    if (!cb) {
        handle->eng->set_video_config_sync_callback(nullptr);
        return;
    }
    handle->eng->set_video_config_sync_callback([cb, user_data](const lv::VideoConfigSyncPayload& p) {
        LV_VideoConfigSync out{};
        out.config_revision = p.config_revision;
        out.origin_id = p.origin_id;
        out.camera_id = p.camera_id;
        out.assigned_cam_number = p.assigned_cam_number;
        out.resolution_preset = p.resolution_preset;
        out.target_fps = p.target_fps;
        out.codec = p.codec;
        out.bitrate_kbps = p.bitrate_kbps;
        out.video_delay_ms = p.video_delay_ms;
        out.fixed_delay_mode = p.fixed_delay_mode;
        out.camera_lens = p.camera_lens;
        out.flags = p.flags;
        std::memcpy(out.badge_accent_rgba, p.badge_accent_rgba, 4);
        std::memcpy(out.assigned_cam_tag, p.assigned_cam_tag, sizeof(out.assigned_cam_tag));
        out.assigned_cam_tag[15] = '\0';
        cb(&out, user_data);
    });
}

int lv_tally_intercom_request_keyframe(LvTallyIntercomHandle* handle, uint32_t camera_id) {
    if (!handle || !handle->eng) return LV_ERR_INVALID_ARG;
    return handle->eng->studio_request_keyframe(camera_id) ? LV_OK : LV_ERR_SOCKET;
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

int lv_tally_intercom_list_cameras(LvTallyIntercomHandle* studio, LV_StudioCameraInfo* out_cams, int max_count) {
    if (!studio || !studio->eng) return 0;
    const auto list = studio->eng->studio_cameras();
    if (out_cams && max_count > 0) {
        const int n = std::min<int>(int(list.size()), max_count);
        for (int i = 0; i < n; ++i) {
            const auto& c = list[size_t(i)];
            auto& d = out_cams[i];
            std::memset(&d, 0, sizeof(d));
            d.camera_id = c.camera_id;
            lv_copy_str(d.label, c.label);
            lv_copy_str(d.address, c.address);
            d.tally_state = c.tally_state;
            d.rec_trigger = c.rec_trigger ? 1 : 0;
            d.talk_to_cam = c.talk_to_cam ? 1 : 0;
            d.listen_from_cam = c.listen_from_cam ? 1 : 0;
            d.battery_pct = c.battery_pct;
            d.temp_c = c.temp_c;
            d.operator_speaking = c.operator_speaking ? 1 : 0;
            d.connected = c.connected ? 1 : 0;
            d.voice_peak_dbfs = c.voice_peak_dbfs;
            d.tally_tx_count = c.tally_tx_count;
            d.intercom_rx_pkts = c.intercom_rx_pkts;
            d.intercom_tx_pkts = c.intercom_tx_pkts;
            d.assigned_cam_number = c.assigned_cam_number;
            d.config_synced = c.config_synced ? 1 : 0;
            lv_copy_str(d.assigned_cam_tag, c.assigned_cam_tag);
        }
    }
    return int(list.size());
}

int lv_tally_intercom_get_stats(LvTallyIntercomHandle* handle, LV_IntercomStats* out) {
    if (!handle || !handle->eng || !out) return LV_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->running = handle->eng->is_running() ? 1 : 0;
    out->bound_port = handle->eng->bound_port();
    out->director_speaking = handle->eng->is_director_speaking() ? 1 : 0;
    out->wifi_redundancy = handle->eng->wifi_redundancy() ? 1 : 0;
    out->ducking_gain = handle->eng->ducking_gain();
    out->tx_peak_dbfs = handle->eng->tx_peak_dbfs();
    out->rx_peak_dbfs = handle->eng->rx_peak_dbfs();
    out->tx_packets = handle->eng->intercom_tx_packets();
    out->rx_packets = handle->eng->intercom_rx_packets();
    out->duplicates_dropped = handle->eng->intercom_duplicates_dropped();
    return LV_OK;
}

void lv_tally_intercom_set_wifi_redundancy(LvTallyIntercomHandle* handle, int enabled, uint32_t stagger_us) {
    if (handle && handle->eng) {
        handle->eng->set_wifi_redundancy(enabled != 0, stagger_us > 0 ? stagger_us : 1500u);
    }
}

/* ---- LAN Video Auto-Discovery ---- */
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

char* lv_discovery_scan_port(int timeout_ms, uint16_t discovery_port) {
    const auto found = lv::discovery_scan(timeout_ms, discovery_port ? discovery_port : lv::kDiscoveryPort);
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < found.size(); ++i) {
        if (i) os << ",";
        os << "{\"ip\":\"" << json_escape(found[i].ip) << "\",\"port\":" << found[i].stream_port
           << ",\"name\":\"" << json_escape(found[i].device_name) << "\",\"os\":\""
           << json_escape(found[i].os) << "\",\"simd\":\"" << json_escape(found[i].simd)
           << "\",\"role\":\"" << json_escape(found[i].role)
           << "\",\"rtt_ms\":" << found[i].rtt_ms << "}";
    }
    os << "]";
    return dup_cstr(os.str());
}

char* lv_discovery_scan(int timeout_ms) {
    const auto found = lv::discovery_scan_video(timeout_ms, true);
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < found.size(); ++i) {
        if (i) os << ",";
        os << "{\"ip\":\"" << json_escape(found[i].ip) << "\",\"port\":" << found[i].stream_port
           << ",\"name\":\"" << json_escape(found[i].device_name) << "\",\"os\":\""
           << json_escape(found[i].os) << "\",\"simd\":\"" << json_escape(found[i].simd)
           << "\",\"role\":\"" << json_escape(found[i].role)
           << "\",\"rtt_ms\":" << found[i].rtt_ms << "}";
    }
    os << "]";
    return dup_cstr(os.str());
}

/* ---- Multi-Cam (LanVid Video) ---- */
void lv_multicam_config_default(LV_MultiCamConfig* cfg) {
    if (!cfg) return;
    *cfg = LV_MultiCamConfig{};
    std::snprintf(cfg->bind_ip, sizeof(cfg->bind_ip), "0.0.0.0");
    cfg->base_port = 5006;
    cfg->port_mode = 0;
    cfg->max_slots = 8;
    cfg->auto_assign = 1;
    cfg->publish_video_bus = 1;
    cfg->vcam_count = 8;
    cfg->vcam_placeholder = 1;
}

LvMultiCamHandle* lv_multicam_create(void) { return new (std::nothrow) LvMultiCamHandle(); }
void lv_multicam_destroy(LvMultiCamHandle* mc) { delete mc; }

int lv_multicam_start(LvMultiCamHandle* mc, const LV_MultiCamConfig* c) {
    if (!mc) return LV_ERR_INVALID_ARG;
    LV_MultiCamConfig d;
    lv_multicam_config_default(&d);
    if (!c) c = &d;
    lv::MultiCamConfig cfg;
    cfg.bind_ip = c->bind_ip[0] ? c->bind_ip : "0.0.0.0";
    cfg.base_port = c->base_port ? c->base_port : 5006;
    cfg.port_mode = c->port_mode == 1 ? lv::MultiCamPortMode::kSequentialPorts : lv::MultiCamPortMode::kSingleDemux;
    cfg.max_slots = c->max_slots ? c->max_slots : 8;
    cfg.auto_assign = c->auto_assign != 0;
    cfg.publish_video_bus = c->publish_video_bus != 0;
    cfg.vcam_count = c->vcam_count;
    cfg.vcam_placeholder = c->vcam_placeholder != 0;
    cfg.video_bus_name = c->video_bus_name;
    return mc->mgr.start(cfg) ? LV_OK : LV_ERR_SOCKET;
}

void lv_multicam_stop(LvMultiCamHandle* mc) {
    if (mc) mc->mgr.stop();
}

uint32_t lv_multicam_slot_count(LvMultiCamHandle* mc) { return mc ? mc->mgr.slot_count() : 0; }

int lv_multicam_get_slot_info(LvMultiCamHandle* mc, uint32_t slot, LV_MultiCamSlotInfo* out) {
    if (!mc || !out || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    const auto i = mc->mgr.slot_info(slot);
    *out = LV_MultiCamSlotInfo{};
    out->index = i.index;
    out->camera_id = i.camera_id;
    lv_copy_str(out->label, i.label);
    lv_copy_str(out->source_name, i.source_name);
    lv_copy_str(out->sender_ip, i.sender_ip);
    out->listen_port = i.listen_port;
    out->bound = i.bound;
    out->streaming = i.streaming;
    out->width = i.width;
    out->height = i.height;
    out->fps = i.fps;
    lv_copy_str(out->codec, i.codec);
    out->bitrate_mbps = i.bitrate_mbps;
    out->latency_ms = i.latency_ms;
    out->battery_pct = i.battery_pct;
    out->temp_c = i.temp_c;
    out->audio_peak_dbfs = i.audio_peak_dbfs;
    out->tally = i.tally;
    out->recording = i.recording;
    out->vcam_active = i.vcam_active;
    out->frames = i.frames;
    return LV_OK;
}

int lv_multicam_set_slot_label(LvMultiCamHandle* mc, uint32_t slot, const char* label) {
    return (mc && mc->mgr.set_slot_label(slot, label ? label : "")) ? LV_OK : LV_ERR_INVALID_ARG;
}

int lv_multicam_pin_slot(LvMultiCamHandle* mc, uint32_t slot, const char* ip) {
    return (mc && mc->mgr.pin_slot(slot, ip ? ip : "")) ? LV_OK : LV_ERR_INVALID_ARG;
}

void lv_multicam_release_slot(LvMultiCamHandle* mc, uint32_t slot) {
    if (mc) mc->mgr.release_slot(slot);
}

int lv_multicam_get_latest_frame_bgra(LvMultiCamHandle* mc, uint32_t slot, uint8_t* buf, size_t cap, int* width,
                                      int* height, uint64_t* seq) {
    if (!mc || !seq) return LV_ERR_INVALID_ARG;
    lv::Image img;
    uint64_t s = *seq;
    if (!mc->mgr.latest_frame(slot, img, s)) return 0;
    if (width) *width = img.width;
    if (height) *height = img.height;
    const size_t need = size_t(img.width) * size_t(img.height) * 4;
    if (!buf || cap < need) return LV_ERR_BUFFER_TOO_SMALL;
    for (int y = 0; y < img.height; ++y)
        std::memcpy(buf + size_t(y) * size_t(img.width) * 4, img.data.data() + size_t(y) * img.stride,
                    size_t(img.width) * 4);
    *seq = s;
    return 1;
}

int lv_multicam_set_program(LvMultiCamHandle* mc, int slot) {
    if (!mc) return LV_ERR_INVALID_ARG;
    mc->mgr.set_program_slot(slot);
    return LV_OK;
}

int lv_multicam_rec_start_all(LvMultiCamHandle* mc, const LV_IsoSessionConfig* session) {
    if (!mc) return LV_ERR_INVALID_ARG;
    lv::IsoSessionConfig sc;
    if (session) {
        sc.output_dir = session->output_dir;
        if (session->session_name[0]) sc.session_name = session->session_name;
        sc.container = session->container == 1 ? lv::IsoContainer::kMkv
                       : session->container == 2 ? lv::IsoContainer::kMov
                                                 : lv::IsoContainer::kMp4;
        sc.record_program = session->record_program != 0;
        sc.slot_mask = session->slot_mask;
    }
    return mc->mgr.rec_start_all(sc) ? LV_OK : LV_ERR_INTERNAL;
}

void lv_multicam_rec_stop_all(LvMultiCamHandle* mc) {
    if (mc) mc->mgr.rec_stop_all();
}

int lv_multicam_rec_slot(LvMultiCamHandle* mc, uint32_t slot, int on) {
    return (mc && mc->mgr.rec_slot(slot, on != 0)) ? LV_OK : LV_ERR_INVALID_ARG;
}

int lv_multicam_get_rec_stats(LvMultiCamHandle* mc, uint32_t slot, LV_IsoSlotStats* out) {
    if (!mc || !out || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    const auto st = mc->mgr.rec_stats(slot);
    *out = LV_IsoSlotStats{};
    out->recording = st.recording;
    out->waiting_for_keyframe = st.waiting_for_keyframe;
    lv_copy_str(out->path, st.path);
    out->bytes = st.bytes;
    out->duration_us = st.duration_us;
    out->frames = st.frames;
    out->write_mbps = st.write_mbps;
    return LV_OK;
}

void lv_multicam_set_vcam(LvMultiCamHandle* mc, uint32_t count, int placeholder) {
    if (mc) mc->mgr.set_vcam(count, placeholder != 0);
}

int lv_multicam_set_slot_playout(LvMultiCamHandle* mc, uint32_t slot, uint32_t delay_ms, int fixed_mode) {
    if (!mc || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    mc->mgr.set_slot_playout_config(slot, delay_ms, fixed_mode != 0);
    return LV_OK;
}

int lv_multicam_force_idr(LvMultiCamHandle* mc, uint32_t slot) {
    if (!mc || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    mc->mgr.force_slot_idr(slot);
    return LV_OK;
}

int lv_multicam_set_preview(LvMultiCamHandle* mc, int slot) {
    if (!mc) return LV_ERR_INVALID_ARG;
    mc->mgr.set_preview_slot(slot);
    return LV_OK;
}

int lv_multicam_sync_tally_telemetry(LvMultiCamHandle* mc, LvTallyIntercomHandle* studio) {
    if (!mc || !studio || !studio->eng) return LV_ERR_INVALID_ARG;
    mc->mgr.update_camera_telemetry(studio->eng->studio_cameras());
    lv::TallyIntercomEngine* eng = studio->eng.get();
    mc->mgr.set_idr_callback([eng](uint32_t /*slot*/, uint32_t camera_id) {
        if (eng && camera_id != 0) eng->studio_request_keyframe(camera_id);
    });
    return LV_OK;
}

int lv_multicam_get_slot_ext(LvMultiCamHandle* mc, uint32_t slot, LV_MultiCamSlotExt* out) {
    if (!mc || !out || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    const auto info = mc->mgr.slot_info(slot);
    const auto rx = mc->mgr.slot_rx_telemetry(slot);
    lv_copy_str(out->pinned_ip, info.pinned_ip);
    out->playout_delay_ms = info.playout_delay_ms;
    out->playout_effective_ms = info.playout_effective_ms;
    out->playout_fixed_mode = info.playout_fixed_mode ? 1 : 0;
    out->playout_jitter_ms = info.playout_jitter_ms;
    out->playout_drops = info.playout_drops;
    out->playout_holds = info.playout_holds;
    out->assigned_cam_number = info.assigned_cam_number;
    out->config_synced = info.config_synced ? 1 : 0;
    lv_copy_str(out->assigned_cam_tag, info.assigned_cam_tag);
    out->packets_total = rx.packets_total;
    out->fec_recovered = rx.fec_recovered;
    out->crc_errors = rx.crc_errors;
    out->late_packets = rx.late_packets;
    out->frames_concealed = rx.frames_concealed;
    out->audio_packets = rx.audio_packets;
    out->audio_buffer_ms = rx.audio_buffer_ms;
    out->packet_loss_pct = rx.packet_loss_pct;
    out->decoder_is_hw = rx.decoder_is_hw ? 1 : 0;
    lv_copy_str(out->decoder_backend, rx.decoder_backend);
    return LV_OK;
}

int lv_multicam_get_session_info(LvMultiCamHandle* mc, const char* disk_path, LV_MultiCamSessionInfo* out) {
    if (!mc || !out) return LV_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->recording_any = mc->mgr.recording_any() ? 1 : 0;
    out->video_bus_open = mc->mgr.video_bus_open() ? 1 : 0;
    lv_copy_str(out->session_dir, mc->mgr.session_dir());
    lv_copy_str(out->session_timecode, mc->mgr.session_timecode());
    const std::string dp = (disk_path && *disk_path) ? disk_path : lv::MultiCamManager::default_output_dir();
    out->disk_free_bytes = lv::MultiCamManager::disk_free_bytes(dp);
    const auto ps = mc->mgr.rec_program_stats();
    out->program_stats.recording = ps.recording ? 1 : 0;
    out->program_stats.waiting_for_keyframe = ps.waiting_for_keyframe ? 1 : 0;
    lv_copy_str(out->program_stats.path, ps.path);
    out->program_stats.bytes = ps.bytes;
    out->program_stats.duration_us = ps.duration_us;
    out->program_stats.frames = ps.frames;
    out->program_stats.write_mbps = ps.write_mbps;
    return LV_OK;
}

int lv_multicam_pop_slot_audio_pcm16(LvMultiCamHandle* mc, uint32_t slot, int16_t* out_stereo, int num_frames) {
    if (!mc) return 0;
    return mc->mgr.pop_slot_audio_pcm16(slot, out_stereo, num_frames);
}

int lv_multicam_set_slot_audio_config(LvMultiCamHandle* mc, uint32_t slot, double target_ms, float volume) {
    if (!mc || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    mc->mgr.set_slot_audio_config(slot, target_ms, volume);
    return LV_OK;
}

int lv_multicam_set_slot_concealment(LvMultiCamHandle* mc, uint32_t slot, int conceal_mode) {
    if (!mc || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    mc->mgr.set_slot_concealment(slot, conceal_mode);
    return LV_OK;
}

int lv_multicam_set_slot_assigned_camera(LvMultiCamHandle* mc, uint32_t slot, uint8_t cam_number,
                                         const char* tag, const uint8_t rgba[4]) {
    if (!mc || slot >= mc->mgr.slot_count()) return LV_ERR_INVALID_ARG;
    return mc->mgr.set_slot_assigned_camera(slot, cam_number, tag ? tag : "", rgba) ? LV_OK : LV_ERR_INVALID_ARG;
}

int lv_receiver_request_keyframe(LvReceiverHandle* handle) {
    if (!handle) return LV_ERR_INVALID_ARG;
    LV_LIVE_LOCK(handle);
    if (!handle->receiver) return LV_ERR_NOT_RUNNING;
    handle->receiver->request_keyframe();
    return LV_OK;
}

}  // extern "C"
