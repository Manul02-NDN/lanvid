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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "codec/dct.hpp"
#include "common/clock.hpp"
#include "common/cpu_features.hpp"
#include "common/image.hpp"
#include "net/discovery.hpp"
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

struct LvSenderHandle {
    std::mutex api_mu;
    std::unique_ptr<lv::UdpSender> sender;
#ifndef LANVID_SDK_CORE_ONLY
    std::unique_ptr<lv::AudioCapture> audio_cap;
    std::thread cap_thread;
    LvSenderConfig legacy_cfg{};
#endif
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> state{LV_STATE_STOPPED};

    lv::CodecParams codec_params{};
    std::atomic<int> override_quality{0};  // 0 = use codec_params/AIMD, >0 = live override
    std::atomic<bool> adaptive_enabled{false};
    std::atomic<uint32_t> ext_frame_id{0};
    std::atomic<uint32_t> ext_audio_seq{0};

    std::mutex push_mu;
    lv::Image ext_img;

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
        sender.reset();
        running.store(false, std::memory_order_release);
        state.store(LV_STATE_STOPPED, std::memory_order_release);
    }

    ~LvSenderHandle() { stop_internal(); }
};

struct LvReceiverHandle {
    std::mutex api_mu;
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

    // Zero-copy SDK callbacks
    std::mutex cb_mu;
    LV_VideoFrameCallback video_cb = nullptr;
    void* video_ud = nullptr;
    LV_AudioSamplesCallback audio_cb = nullptr;
    void* audio_ud = nullptr;
    LV_StateChangedCallback state_cb = nullptr;
    void* state_ud = nullptr;

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
        receiver = std::move(rx);

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
        receiver.reset();
        running.store(false, std::memory_order_release);
        if (was_running) notify_state(LV_STATE_STOPPED, LV_OK, "Stopped");
    }

    ~LvReceiverHandle() { stop_internal(); }
};

extern "C" {

/* ---- Version & Error Helpers ---- */
const char* lv_get_version_string(void) { return "1.0.0"; }

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
    scfg.adaptive.enabled = cfg->adaptive_quality != 0;
    scfg.adaptive.q_min = std::clamp(cfg->q_min > 0 ? cfg->q_min : 35, 1, 100);
    scfg.adaptive.q_max = std::clamp(cfg->q_max > 0 ? cfg->q_max : 85, scfg.adaptive.q_min, 100);
    scfg.adaptive.initial_q = std::clamp(cfg->quality > 0 ? cfg->quality : 80, 1, 100);

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
    handle->sender = std::move(s);
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

int lv_sender_push_yuv_planes(LvSenderHandle* handle, const LV_YuvPlanes* planes, int width, int height,
                              uint32_t timestamp_us) {
    if (!handle || !planes || !planes->plane_y || !planes->plane_u || width <= 0 || height <= 0) {
        return LV_ERR_INVALID_ARG;
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

    lv::CodecParams cp = handle->codec_params;
    cp.width = width;
    cp.height = height;
    cp.chroma = lv::ChromaFormat::k420;
    cp.quality = handle->resolve_quality();

    const uint32_t fid = handle->ext_frame_id.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ts = timestamp_us ? timestamp_us : lv::now_us32();
    const uint64_t t0 = lv::now_us();
    const bool ok = handle->sender->send_frame_yuv420(yuv, cp, fid, ts);
    const uint64_t t1 = lv::now_us();
    if (!ok) return LV_ERR_CODEC;
    handle->state.store(LV_STATE_CONNECTED, std::memory_order_relaxed);
    handle->record_encode_sample(double(t1 - t0) / 1000.0, width, height);
    return LV_OK;
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

int lv_sender_request_keyframe_quality(LvSenderHandle* handle, int quality) {
    if (!handle || quality < 1 || quality > 100) return LV_ERR_INVALID_ARG;
    handle->override_quality.store(quality, std::memory_order_relaxed);
    handle->codec_params.quality = quality;
    if (handle->sender && handle->adaptive_enabled.load(std::memory_order_relaxed)) {
        handle->sender->set_adaptive_quality(true, 35, 95, quality);
    }
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
    const uint64_t now = lv::now_us();

    std::lock_guard<std::mutex> slk(handle->stats_mu);
    const double dt_s = double(now - handle->win_start_us) / 1e6;
    if (dt_s >= 0.5) {
        const uint64_t df = st.frames_sent - handle->win_prev_frames;
        const uint64_t db = st.bytes_sent - handle->win_prev_bytes;
        const uint64_t dab = st.audio_bytes_sent - handle->win_prev_ab;
        handle->cached_fps = double(df) / dt_s;
        handle->cached_mbps = (double(db) * 8.0 / 1e6) / dt_s;
        handle->cached_audio_kbps = (double(dab) * 8.0 / 1e3) / dt_s;
        handle->cached_enc_ms = handle->enc_ms_cnt ? (handle->enc_ms_sum / double(handle->enc_ms_cnt)) : 0.0;
        handle->win_start_us = now;
        handle->win_prev_frames = st.frames_sent;
        handle->win_prev_bytes = st.bytes_sent;
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
    const uint64_t rx_pkts = handle->receiver->seq_tracker().received();
    const uint64_t lost_pkts = handle->receiver->seq_tracker().lost(0);
    const uint64_t now = lv::now_us();

    std::lock_guard<std::mutex> slk(handle->stats_mu);
    const double dt_s = double(now - handle->win_start_us) / 1e6;
    if (dt_s >= 0.5) {
        const uint64_t db = ast.bytes_total - handle->win_prev_bytes;
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
        handle->win_prev_bytes = ast.bytes_total;
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

    if (lv_sdk_sender_start(handle, &sc) != LV_OK) return 0;

    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->legacy_cfg = *cfg;
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
                handle->running.store(false, std::memory_order_release);
                return;
            }
            cp.width = screen.native_width();
            cp.height = screen.native_height();
            frame.allocate(cp.width, cp.height, lv::PixelOrder::kBGRA);
        } else if (cap_cfg.source_type == LV_SOURCE_CAMERA) {
            if (!camera.open(cap_cfg.camera_index, cap_cfg.width, cap_cfg.height, int(fps))) {
                handle->running.store(false, std::memory_order_release);
                return;
            }
            cp.width = camera.native_width();
            cp.height = camera.native_height();
            frame.allocate(cp.width, cp.height, lv::PixelOrder::kBGRA);
        } else if (cap_cfg.source_type == LV_SOURCE_FILE) {
            if (!fsrc.open(cap_cfg.file_path, cap_cfg.width, cap_cfg.height, cap_cfg.file_is_rgba != 0)) {
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
    if (handle->sender) {
        const auto raw = handle->sender->stats();
        out->send_errors = raw.send_errors;
        out->rx_fec_pct = double(raw.last_rx_fec_permille) / 10.0;
    }
    return 1;
}

int lv_receiver_start(LvReceiverHandle* handle, const LvReceiverConfig* cfg) {
    if (!handle || !cfg) return 0;
    LV_ReceiverConfig rc{};
    lv_sdk_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "%s", cfg->bind_ip);
    rc.bind_port = cfg->bind_port;
    rc.discovery_port = cfg->discovery_port;
    rc.decode_threads = cfg->decode_threads;
    rc.conceal_mode = cfg->conceal_mode;
    rc.frame_timeout_ms = cfg->frame_timeout_ms;
    rc.audio_target_ms = cfg->audio_target_ms;
    rc.enable_os_audio = (cfg->enable_audio != 0 && cfg->external_audio_pull == 0) ? 1 : 0;
    rc.enable_discovery = cfg->enable_discovery;
    std::snprintf(rc.device_name, sizeof(rc.device_name), "%s", cfg->device_name);
    return lv_sdk_receiver_start(handle, &rc) == LV_OK ? 1 : 0;
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
    if (handle->receiver) out->audio_packets_lost = handle->receiver->audio_stats().packets_lost;
    return 1;
}

#endif  // !LANVID_SDK_CORE_ONLY

}  // extern "C"
