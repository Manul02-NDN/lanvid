#include "crosslink/crosslink_sdk.h"

#include <algorithm>
#include <atomic>
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

#include "audio/channel_patchbay.hpp"
#include "audio/personal_monitor_bus.hpp"
#ifdef CL_HAVE_SHM_BUS
#include "audio/shm_audio_host.hpp"
#endif
#include "common/clock.hpp"
#include "common/cpu_features.hpp"
#include "common/network_interfaces.hpp"
#include "common/rt_thread.hpp"
#include "net/audio_discovery.hpp"
#include "net/audio_receiver.hpp"
#include "net/audio_sender.hpp"

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

bool cl_bus_cfg_valid(const CL_AudioBusConfig* c) {
    return c && (c->sample_rate == 48000 || c->sample_rate == 96000) && c->latency_ms >= 2 &&
           c->latency_ms <= 2000 && c->num_channels >= 1 && c->num_channels <= 64;
}

bool cl_map_format(CL_AudioFormat f, cl::AudioFormat2& out) {
    switch (f) {
        case CL_AUDIO_FORMAT_PCM16:        out = cl::AudioFormat2::kPcm16; return true;
        case CL_AUDIO_FORMAT_F32:          out = cl::AudioFormat2::kFloat32; return true;
        case CL_AUDIO_FORMAT_PCM24_PACKED: out = cl::AudioFormat2::kPcm24Packed; return true;
        default:                           return false;
    }
}

cl::ControlSyncPayload to_wire_cs(const CL_ControlSyncState& s) {
    cl::ControlSyncPayload cs{};
    cs.revision = s.revision;
    cs.origin_id = s.origin_id;
    cs.audio_channels = s.audio_channels;
    cs.audio_bitrate_kbps = s.audio_bitrate_kbps;
    cs.target_buffer_ms = s.target_buffer_ms;
    cs.latency_mode = s.latency_mode;
    cs.midi_only = s.midi_only;
    cs.active_preset_id = s.active_preset_id;
    cs.flags = s.flags;
    cs.master_gain_cb = s.master_gain_cb;
    cs.audio_codec = s.audio_codec;
    return cs;
}

std::atomic<int> g_init_refs{0};

}  // namespace

#define CL_LIVE_LOCK(h) std::shared_lock<std::shared_mutex> cl_live_lock_((h)->life_mu)

struct ClSenderHandle {
    std::mutex api_mu;
    mutable std::shared_mutex life_mu;
    std::unique_ptr<cl::AudioSender> sender;
    std::thread ctl_thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> state{CL_STATE_STOPPED};

    CL_AudioBusConfig bus_cfg{48000, 20, 64, 240};
    bool bus_configured = false;
    std::string primary_nic_ip;
    std::string secondary_nic_ip;

    std::mutex cs_cb_mu;
    CL_ControlSyncCallback cs_cb = nullptr;
    void* cs_ud = nullptr;

    std::mutex stats_mu;
    uint64_t win_start_us = 0;
    uint64_t win_prev_bytes = 0;
    double cached_audio_kbps = 0.0;

    void stop_internal() {
        stop.store(true, std::memory_order_release);
        if (ctl_thread.joinable()) ctl_thread.join();
        running.store(false, std::memory_order_release);
        std::unique_ptr<cl::AudioSender> dead;
        {
            std::unique_lock<std::shared_mutex> lk(life_mu);
            dead = std::move(sender);
        }
        dead.reset();
        state.store(CL_STATE_STOPPED, std::memory_order_release);
    }

    ~ClSenderHandle() { stop_internal(); }
};

struct ClReceiverHandle {
    std::mutex api_mu;
    mutable std::shared_mutex life_mu;
    std::unique_ptr<cl::AudioReceiver> receiver;
    std::unique_ptr<cl::AudioDiscoveryResponder> discovery;
    std::thread rx_thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> state{CL_STATE_STOPPED};
    std::atomic<int> adaptive_jitter{1};

    CL_AudioBusConfig bus_cfg{48000, 20, 64, 240};
    bool bus_configured = false;
    std::mutex cb_mu;
    CL_MultichannelAudioCallback mc_cb = nullptr;
    void* mc_ud = nullptr;
    CL_MidiEventCallback midi_cb = nullptr;
    void* midi_ud = nullptr;
    CL_ControlSyncCallback cs_cb = nullptr;
    void* cs_ud = nullptr;
    CL_StateChangedCallback state_cb = nullptr;
    void* state_ud = nullptr;

    std::thread mc_thread;
    std::atomic<bool> mc_stop{false};
    std::atomic<bool> mc_active{false};

    std::mutex stats_mu;
    uint64_t win_start_us = 0;
    uint64_t win_prev_bytes = 0;
    double cached_audio_kbps = 0.0;

    void notify_state(int new_state, int err_code, const char* msg) {
        state.store(new_state, std::memory_order_release);
        CL_StateChangedCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk(cb_mu);
            cb = state_cb;
            ud = state_ud;
        }
        if (cb) cb(new_state, err_code, msg ? msg : "", ud);
    }

    void stop_mc_pump() {
        mc_stop.store(true, std::memory_order_release);
        if (mc_thread.joinable()) mc_thread.join();
        mc_active.store(false, std::memory_order_release);
    }

    void start_mc_pump() {
        stop_mc_pump();
        if (!mc_cb || !receiver) return;
        const uint32_t frames = bus_cfg.block_frames ? uint32_t(bus_cfg.block_frames) : 240u;
        const int ch = std::clamp(bus_cfg.num_channels, 1, 64);
        const uint32_t rate = bus_cfg.sample_rate > 0 ? uint32_t(bus_cfg.sample_rate) : 48000u;
        mc_stop.store(false, std::memory_order_release);
        mc_active.store(true, std::memory_order_release);
        mc_thread = std::thread([this, frames, ch, rate]() {
            cl::RealtimeThreadScope rt(uint32_t(uint64_t(frames) * 1000000ull / uint64_t(rate)));
            std::vector<float> buf(size_t(frames) * size_t(ch));
            const uint64_t period_us = uint64_t(frames) * 1000000ull / rate;
            uint64_t next = cl::now_us() + period_us;
            uint64_t index = 0;
            while (!mc_stop.load(std::memory_order_acquire)) {
                cl::sleep_until_us(next, [] {});
                const uint64_t now = cl::now_us();
                next += period_us;
                if (next + period_us * 4 < now) next = now + period_us;
                if (!receiver) break;
                receiver->pop_audio_multichannel_interleaved(buf.data(), uint32_t(ch), frames);
                CL_MultichannelAudioCallback cb = nullptr;
                void* ud = nullptr;
                {
                    std::lock_guard<std::mutex> lk(cb_mu);
                    cb = mc_cb;
                    ud = mc_ud;
                }
                if (!cb) continue;
                CL_MultichannelAudioBlock blk{};
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

    void stop_internal() {
        const bool was_running = running.load(std::memory_order_acquire) || receiver != nullptr;
        stop.store(true, std::memory_order_release);
        stop_mc_pump();
        if (discovery) {
            discovery->stop();
            discovery.reset();
        }
        if (rx_thread.joinable()) rx_thread.join();
        running.store(false, std::memory_order_release);
        std::unique_ptr<cl::AudioReceiver> dead;
        {
            std::unique_lock<std::shared_mutex> lk(life_mu);
            dead = std::move(receiver);
        }
        dead.reset();
        if (was_running) notify_state(CL_STATE_STOPPED, CL_OK, "Stopped");
    }

    ~ClReceiverHandle() { stop_internal(); }
};

struct ClDuplexHandle {
    std::mutex api_mu;
    ClSenderHandle* tx = nullptr;
    ClReceiverHandle* rx = nullptr;
#ifdef CL_HAVE_SHM_BUS
    cl::ShmAudioHost tx_host;
    cl::ShmAudioHost rx_host;
#endif
    cl::ChannelPatchbay tx_patch;
    cl::ChannelPatchbay rx_patch;
    std::unique_ptr<cl::AudioDiscoveryResponder> disc;
    CL_DuplexConfig cfg{};
    CL_MidiEventCallback midi_cb = nullptr;
    void* midi_ud = nullptr;
    CL_ControlSyncCallback cs_cb = nullptr;
    void* cs_ud = nullptr;
    std::atomic<bool> running{false};

    void stop_internal() {
        running.store(false, std::memory_order_release);
        if (disc) {
            disc->stop();
            disc.reset();
        }
#ifdef CL_HAVE_SHM_BUS
        tx_host.stop();
        rx_host.stop();
#endif
        if (tx) { cl_sender_destroy(tx); tx = nullptr; }
        if (rx) { cl_receiver_destroy(rx); rx = nullptr; }
    }
    ~ClDuplexHandle() { stop_internal(); }
};

struct ClMonitorServerHandle {
    std::unique_ptr<cl::PersonalMonitorServer> srv;
};

struct ClMonitorClientHandle {
    std::unique_ptr<cl::PersonalMonitorClient> cli;
};

extern "C" {

/* ---- Version & Error Helpers ---- */
const char* cl_get_version_string(void) { return CL_SDK_VERSION_STRING; }

uint32_t cl_get_version_number(void) {
    return (uint32_t(CL_SDK_VERSION_MAJOR) << 16) | (uint32_t(CL_SDK_VERSION_MINOR) << 8) |
           uint32_t(CL_SDK_VERSION_PATCH);
}

int cl_check_abi_compatibility(int header_major, int header_minor) {
    if (header_major != CL_SDK_VERSION_MAJOR) return 0;
    if (header_minor > CL_SDK_VERSION_MINOR) return 0;
    return 1;
}

const char* cl_error_string(int err) {
    switch (err) {
        case CL_OK: return "OK";
        case CL_ERR_INVALID_ARG: return "Invalid argument";
        case CL_ERR_NOT_INITIALIZED: return "SDK not initialized";
        case CL_ERR_NOT_RUNNING: return "Stream not running";
        case CL_ERR_SOCKET: return "UDP socket bind/open error";
        case CL_ERR_CODEC: return "Audio codec error";
        case CL_ERR_BUFFER_TOO_SMALL: return "Output buffer too small";
        case CL_ERR_UNSUPPORTED: return "Unsupported operation";
        case CL_ERR_INTERNAL: return "Internal error";
        default: return "Unknown error";
    }
}

/* ---- Core Lifecycle ---- */
int cl_init(void) {
    g_init_refs.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

void cl_shutdown(void) {
    if (g_init_refs.load(std::memory_order_relaxed) > 0) {
        g_init_refs.fetch_sub(1, std::memory_order_relaxed);
    }
}

void cl_free_string(char* str) { std::free(str); }

const char* cl_get_cpu_simd_info(void) {
    if (!cl::simd_enabled()) return "scalar";
    if (cl::cpu_has_avx2()) return "AVX2";
    if (cl::cpu_has_neon()) return "NEON";
    if (cl::cpu_has_sse42()) return "SSE4.2";
    return "scalar";
}

void cl_set_simd_enabled(int enabled) { cl::set_simd_disabled(enabled == 0); }

/* ---- Default Configurations ---- */
void cl_audio_bus_config_default(CL_AudioBusConfig* cfg) {
    if (!cfg) return;
    cfg->sample_rate = 48000;
    cfg->latency_ms = 20;
    cfg->num_channels = 64;
    cfg->block_frames = 240;
}

void cl_sender_config_default(CL_SenderConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->dest_ip, sizeof(cfg->dest_ip), "127.0.0.1");
    cfg->dest_port = cl::kAudioStreamPort;
    cfg->mtu = int(cl::kDefaultMtu);
    cfg->sample_rate = 48000;
    cfg->latency_ms = 20;
    cfg->audio_codec = 0;
    cfg->audio_bitrate_kbps = 0;
}

void cl_receiver_config_default(CL_ReceiverConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->bind_ip, sizeof(cfg->bind_ip), "0.0.0.0");
    cfg->bind_port = cl::kAudioStreamPort;
    cfg->discovery_port = cl::kAudioDiscoveryPort;
    cfg->sample_rate = 48000;
    cfg->latency_ms = 20;
    cfg->num_channels = 64;
    cfg->block_frames = 240;
    cfg->adaptive_jitter = 1;
    cfg->enable_discovery = 1;
}

void cl_duplex_config_default(CL_DuplexConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->peer_host, sizeof(cfg->peer_host), "127.0.0.1");
    cfg->peer_port = cl::kAudioStreamPort;
    cfg->listen_port = cl::kAudioDuplexListenPort;
    std::snprintf(cfg->bind_ip, sizeof(cfg->bind_ip), "0.0.0.0");
    cfg->latency_ms = 20;
    cfg->sample_rate = 48000;
    cfg->num_channels = 64;
    cfg->discovery_port = cl::kAudioDiscoveryPort;
    cfg->enable_discovery = 1;
    cfg->adaptive_jitter = 1;
}

/* ---- 64-Channel AoIP Sender ---- */
ClSenderHandle* cl_sender_create(void) { return new (std::nothrow) ClSenderHandle(); }
void cl_sender_destroy(ClSenderHandle* handle) { delete handle; }

int cl_sender_configure_audio_bus(ClSenderHandle* handle, const CL_AudioBusConfig* cfg) {
    if (!handle || !cl_bus_cfg_valid(cfg)) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->bus_cfg = *cfg;
    handle->bus_configured = true;
    if (handle->sender) handle->sender->set_latency_ms(cfg->latency_ms);
    return CL_OK;
}

int cl_sender_start(ClSenderHandle* handle, const CL_SenderConfig* cfg) {
    if (!handle || !cfg) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
    handle->state.store(CL_STATE_STARTING, std::memory_order_release);

    cl::AudioSenderConfig scfg;
    scfg.dest_ip = cfg->dest_ip[0] ? cfg->dest_ip : "127.0.0.1";
    scfg.dest_port = cfg->dest_port ? cfg->dest_port : cl::kAudioStreamPort;
    scfg.bind_port = cfg->bind_port;
    scfg.bind_ip = cfg->bind_ip[0] ? cfg->bind_ip : handle->primary_nic_ip;
    scfg.secondary_bind_ip = cfg->secondary_bind_ip[0] ? cfg->secondary_bind_ip : handle->secondary_nic_ip;
    scfg.mtu = cfg->mtu >= 256 ? size_t(cfg->mtu) : cl::kDefaultMtu;
    const uint32_t sr = (cfg->sample_rate == 96000 || handle->bus_cfg.sample_rate == 96000) ? 96000u : 48000u;
    const uint32_t lat = (handle->bus_configured && cfg->latency_ms == 20)
                             ? handle->bus_cfg.latency_ms
                             : (cfg->latency_ms >= 2 ? cfg->latency_ms : handle->bus_cfg.latency_ms);
    scfg.sample_rate = sr;
    scfg.latency_ms = std::clamp<uint32_t>(lat, 2, 2000);

    auto s = std::make_unique<cl::AudioSender>(scfg);
    if (!s->init()) {
        handle->state.store(CL_STATE_ERROR, std::memory_order_release);
        return CL_ERR_SOCKET;
    }
    if (cfg->audio_bitrate_kbps > 0) {
        s->set_audio_bitrate_kbps(cfg->audio_bitrate_kbps);
    } else {
        s->set_audio_codec_profile(static_cast<cl::AudioCodecProfile>(std::clamp(cfg->audio_codec, 0, 8)));
    }
    s->set_control_sync_callback([handle](const cl::ControlSyncPayload& cs) {
        CL_ControlSyncCallback f = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk_cb(handle->cs_cb_mu);
            f = handle->cs_cb;
            ud = handle->cs_ud;
        }
        if (!f) return;
        CL_ControlSyncState st{cs.revision, cs.origin_id, cs.audio_channels, cs.audio_bitrate_kbps,
                               cs.target_buffer_ms, cs.latency_mode, cs.midi_only, cs.active_preset_id,
                               cs.flags, cs.master_gain_cb, cs.audio_codec};
        f(&st, ud);
    });

    {
        std::unique_lock<std::shared_mutex> lk_life(handle->life_mu);
        handle->sender = std::move(s);
    }
    {
        std::lock_guard<std::mutex> slk(handle->stats_mu);
        handle->win_start_us = cl::now_us();
        handle->win_prev_bytes = 0;
        handle->cached_audio_kbps = 0.0;
    }
    handle->stop.store(false, std::memory_order_release);
    handle->running.store(true, std::memory_order_release);
    handle->state.store(CL_STATE_RUNNING, std::memory_order_release);

    handle->ctl_thread = std::thread([handle]() {
        while (!handle->stop.load(std::memory_order_relaxed)) {
            {
                CL_LIVE_LOCK(handle);
                if (handle->sender) handle->sender->service_control_try();
            }
            cl::sleep_until_us(cl::now_us() + 2000, [] {});
        }
    });
    return CL_OK;
}

void cl_sender_stop(ClSenderHandle* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
}

uint16_t cl_sender_get_local_port(ClSenderHandle* handle) {
    if (!handle) return 0;
    CL_LIVE_LOCK(handle);
    return handle->sender ? handle->sender->local_port() : 0;
}

int cl_sender_push_audio_channel(ClSenderHandle* handle, uint16_t channel_index, const void* samples,
                                 uint32_t num_samples, CL_AudioFormat format,
                                 uint64_t media_timestamp_samples) {
    if (!handle || !samples || num_samples == 0 || channel_index >= CL_AUDIO_MAX_CHANNELS)
        return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    cl::AudioFormat2 f;
    if (!cl_map_format(format, f)) return CL_ERR_INVALID_ARG;
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return CL_ERR_NOT_RUNNING;
    return handle->sender->push_audio_channel(channel_index, samples, num_samples, f,
                                              media_timestamp_samples) == 0
               ? CL_OK
               : CL_ERR_INVALID_ARG;
}

int cl_sender_push_audio_multichannel(ClSenderHandle* handle, const void* data, uint16_t num_channels,
                                      uint32_t num_samples, CL_AudioFormat format, CL_AudioLayout layout,
                                      uint64_t media_timestamp_samples) {
    if (!handle || !data || num_samples == 0 || num_channels == 0 || num_channels > CL_AUDIO_MAX_CHANNELS)
        return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    cl::AudioFormat2 f;
    if (!cl_map_format(format, f)) return CL_ERR_INVALID_ARG;
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return CL_ERR_NOT_RUNNING;
    if (media_timestamp_samples == CL_MEDIA_TS_AUTO)
        media_timestamp_samples = handle->sender->next_auto_media_ts(num_samples);
    const size_t bps = cl::audio2_bytes_per_sample(f);
    if (layout == CL_AUDIO_LAYOUT_PLANAR) {
        const void* const* planes = static_cast<const void* const*>(data);
        for (uint16_t c = 0; c < num_channels; ++c) {
            if (!planes[c]) return CL_ERR_INVALID_ARG;
            if (handle->sender->push_audio_channel(c, planes[c], num_samples, f, media_timestamp_samples) != 0)
                return CL_ERR_INVALID_ARG;
        }
        return CL_OK;
    }
    std::vector<uint8_t> tmp(size_t(num_samples) * bps);
    const uint8_t* in = static_cast<const uint8_t*>(data);
    for (uint16_t c = 0; c < num_channels; ++c) {
        for (uint32_t i = 0; i < num_samples; ++i)
            std::memcpy(&tmp[size_t(i) * bps], in + (size_t(i) * num_channels + c) * bps, bps);
        if (handle->sender->push_audio_channel(c, tmp.data(), num_samples, f, media_timestamp_samples) != 0)
            return CL_ERR_INVALID_ARG;
    }
    return CL_OK;
}

int cl_sender_set_audio_latency_ms(ClSenderHandle* handle, uint32_t latency_ms) {
    if (!handle || latency_ms < 2 || latency_ms > 2000) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    handle->bus_cfg.latency_ms = latency_ms;
    if (handle->sender) handle->sender->set_latency_ms(latency_ms);
    return CL_OK;
}

int cl_sender_set_audio_codec(ClSenderHandle* handle, int codec) {
    if (!handle || codec < 0 || codec > 8) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (handle->sender) handle->sender->set_audio_codec_profile(static_cast<cl::AudioCodecProfile>(codec));
    return CL_OK;
}

int cl_sender_set_audio_bitrate(ClSenderHandle* handle, int kbps_per_ch) {
    if (!handle) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (handle->sender) handle->sender->set_audio_bitrate_kbps(kbps_per_ch);
    return CL_OK;
}

int cl_sender_set_wifi_redundancy(ClSenderHandle* handle, int enabled, uint32_t stagger_us) {
    if (!handle) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (handle->sender) handle->sender->set_wifi_redundancy(enabled != 0, stagger_us == 0 ? 1500u : stagger_us);
    return CL_OK;
}

int cl_sender_set_network_interfaces(ClSenderHandle* handle, const char* primary_ip,
                                     const char* secondary_ip) {
    if (!handle) return CL_ERR_INVALID_ARG;
    const std::string pri = primary_ip ? primary_ip : "";
    const std::string sec = secondary_ip ? secondary_ip : "";
    {
        std::lock_guard<std::mutex> lk(handle->api_mu);
        handle->primary_nic_ip = pri;
        handle->secondary_nic_ip = sec;
    }
    CL_LIVE_LOCK(handle);
    if (handle->sender) handle->sender->set_network_interfaces(pri, sec);
    return CL_OK;
}

int cl_sender_get_failover_stats(ClSenderHandle* handle, uint64_t* out_failover_packets,
                                 uint64_t* out_secondary_bytes) {
    if (!handle) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    uint64_t fp = 0, sb = 0;
    if (handle->sender) {
        fp = handle->sender->failover_packets();
        sb = handle->sender->secondary_bytes_sent();
    }
    if (out_failover_packets) *out_failover_packets = fp;
    if (out_secondary_bytes) *out_secondary_bytes = sb;
    return CL_OK;
}

uint64_t cl_sender_get_master_clock_samples(ClSenderHandle* handle) {
    if (!handle) return 0;
    CL_LIVE_LOCK(handle);
    return handle->sender ? handle->sender->master_samples_now() : 0;
}

int cl_sender_get_stream_stats(ClSenderHandle* handle, CL_StreamStats* out) {
    if (!handle || !out) return CL_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->state = handle->state.load(std::memory_order_acquire);
    CL_LIVE_LOCK(handle);
    if (!handle->sender) return CL_OK;
    const auto st = handle->sender->stats();
    const uint64_t now = cl::now_us();
    {
        std::lock_guard<std::mutex> slk(handle->stats_mu);
        const double dt_s = double(now - handle->win_start_us) / 1e6;
        if (dt_s >= 0.5) {
            const uint64_t db = st.audio_bytes_sent - handle->win_prev_bytes;
            handle->cached_audio_kbps = (double(db) * 8.0 / 1e3) / dt_s;
            handle->win_start_us = now;
            handle->win_prev_bytes = st.audio_bytes_sent;
        }
        out->audio_kbps = handle->cached_audio_kbps;
    }
    out->audio_packets = st.audio_packets_sent;
    out->audio_bytes = st.audio_bytes_sent;
    out->fec_packets = st.fec_packets_sent;
    out->retransmits = st.retransmits_sent;
    out->midi_events = st.midi_events_sent;
    return CL_OK;
}

/* ---- 64-Channel AoIP Receiver ---- */
ClReceiverHandle* cl_receiver_create(void) { return new (std::nothrow) ClReceiverHandle(); }
void cl_receiver_destroy(ClReceiverHandle* handle) { delete handle; }

int cl_receiver_configure_audio_bus(ClReceiverHandle* handle, const CL_AudioBusConfig* cfg) {
    if (!handle || !cl_bus_cfg_valid(cfg)) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->bus_cfg = *cfg;
    handle->bus_configured = true;
    if (handle->receiver) handle->receiver->set_latency_ms(cfg->latency_ms);
    return CL_OK;
}

void cl_receiver_set_state_callback(ClReceiverHandle* handle, CL_StateChangedCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->state_cb = cb;
    handle->state_ud = user_data;
}

int cl_receiver_start(ClReceiverHandle* handle, const CL_ReceiverConfig* cfg) {
    if (!handle || !cfg) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
    handle->notify_state(CL_STATE_STARTING, CL_OK, "Starting audio receiver");

    cl::AudioReceiverConfig rcfg;
    rcfg.bind_ip = cfg->bind_ip[0] ? cfg->bind_ip : "0.0.0.0";
    rcfg.bind_port = cfg->bind_port;
    rcfg.sample_rate = (cfg->sample_rate == 96000 || handle->bus_cfg.sample_rate == 96000) ? 96000u : 48000u;
    const uint32_t lat = (handle->bus_configured && cfg->latency_ms == 20)
                             ? handle->bus_cfg.latency_ms
                             : (cfg->latency_ms >= 2 ? cfg->latency_ms : handle->bus_cfg.latency_ms);
    rcfg.latency_ms = std::clamp<uint32_t>(lat, 2, 2000);
    handle->adaptive_jitter.store(cfg->adaptive_jitter != 0 ? 1 : 0, std::memory_order_release);
    rcfg.adaptive_jitter = cfg->adaptive_jitter != 0;
    if (!(handle->bus_configured && cfg->num_channels == 64) && cfg->num_channels >= 1 && cfg->num_channels <= 64)
        handle->bus_cfg.num_channels = cfg->num_channels;
    if (!(handle->bus_configured && cfg->block_frames == 240) && cfg->block_frames > 0)
        handle->bus_cfg.block_frames = cfg->block_frames;
    handle->bus_cfg.sample_rate = int32_t(rcfg.sample_rate);
    handle->bus_cfg.latency_ms = rcfg.latency_ms;

    auto rx = std::make_unique<cl::AudioReceiver>(rcfg);
    rx->set_midi_callback([handle](const cl::TimedMidiEvent& ev, int32_t off) {
        CL_MidiEventCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk_cb(handle->cb_mu);
            cb = handle->midi_cb;
            ud = handle->midi_ud;
        }
        if (!cb) return;
        CL_MidiEvent e{};
        e.media_ts_us = ev.media_ts_us;
        e.event_seq = ev.event_seq;
        e.block_offset_frames = off;
        e.status = ev.status;
        e.data1 = ev.data1;
        e.data2 = ev.data2;
        e.port = ev.port_index;
        cb(&e, ud);
    });
    rx->set_control_sync_callback([handle](const cl::ControlSyncPayload& cs) {
        CL_ControlSyncCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk_cb(handle->cb_mu);
            cb = handle->cs_cb;
            ud = handle->cs_ud;
        }
        if (!cb) return;
        CL_ControlSyncState st{cs.revision, cs.origin_id, cs.audio_channels, cs.audio_bitrate_kbps,
                               cs.target_buffer_ms, cs.latency_mode, cs.midi_only, cs.active_preset_id,
                               cs.flags, cs.master_gain_cb, cs.audio_codec};
        cb(&st, ud);
    });

    if (!rx->init()) {
        handle->notify_state(CL_STATE_ERROR, CL_ERR_SOCKET, "Failed to bind UDP audio receiver socket");
        return CL_ERR_SOCKET;
    }
    const uint16_t bound_port = rx->local_port();
    {
        std::unique_lock<std::shared_mutex> lk_life(handle->life_mu);
        handle->receiver = std::move(rx);
    }

    if (cfg->enable_discovery) {
        auto disc = std::make_unique<cl::AudioDiscoveryResponder>();
        const uint16_t dport = cfg->discovery_port ? cfg->discovery_port : cl::kAudioDiscoveryPort;
        const int ch_count = handle->bus_cfg.num_channels;
        disc->set_aoip_provider([bound_port, ch_count]() {
            cl::AoipAnnounce a;
            a.enabled = true;
            a.hostname = cl::local_host_name();
            a.channels = uint16_t(ch_count);
            a.rx_port = bound_port;
            a.duplex = false;
            return a;
        });
        disc->start(bound_port, cfg->device_name, dport);
        handle->discovery = std::move(disc);
    }

    {
        std::lock_guard<std::mutex> slk(handle->stats_mu);
        handle->win_start_us = cl::now_us();
        handle->win_prev_bytes = 0;
        handle->cached_audio_kbps = 0.0;
    }
    handle->stop.store(false, std::memory_order_release);
    handle->running.store(true, std::memory_order_release);
    handle->notify_state(CL_STATE_RUNNING, CL_OK, "Listening");
    if (handle->mc_cb) handle->start_mc_pump();

    handle->rx_thread = std::thread([handle]() {
        cl::RealtimeThreadScope rt(2000, 400, 1500);
        while (!handle->stop.load(std::memory_order_relaxed)) {
            handle->receiver->step_once();
        }
        handle->running.store(false, std::memory_order_release);
    });
    return CL_OK;
}

void cl_receiver_stop(ClReceiverHandle* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    handle->stop_internal();
}

uint16_t cl_receiver_get_local_port(ClReceiverHandle* handle) {
    if (!handle) return 0;
    CL_LIVE_LOCK(handle);
    return handle->receiver ? handle->receiver->local_port() : 0;
}

int cl_receiver_set_audio_latency_ms(ClReceiverHandle* handle, uint32_t latency_ms) {
    if (!handle || latency_ms < 2 || latency_ms > 2000) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    handle->bus_cfg.latency_ms = latency_ms;
    if (handle->receiver) handle->receiver->set_latency_ms(latency_ms);
    return CL_OK;
}

int cl_receiver_set_adaptive_jitter(ClReceiverHandle* handle, int enabled) {
    if (!handle) return CL_ERR_INVALID_ARG;
    handle->adaptive_jitter.store(enabled ? 1 : 0, std::memory_order_release);
    std::lock_guard<std::mutex> lk(handle->api_mu);
    if (handle->receiver) handle->receiver->set_adaptive_jitter(enabled != 0);
    return CL_OK;
}

double cl_receiver_get_effective_latency_ms(ClReceiverHandle* handle) {
    if (!handle) return 0.0;
    std::lock_guard<std::mutex> lk(handle->api_mu);
    return handle->receiver ? handle->receiver->effective_latency_ms() : 0.0;
}

int cl_receiver_set_multichannel_audio_callback(ClReceiverHandle* handle, CL_MultichannelAudioCallback cb,
                                                void* user_data) {
    if (!handle) return CL_ERR_INVALID_ARG;
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
    return CL_OK;
}

int cl_receiver_pop_audio_multichannel(ClReceiverHandle* handle, float* out, uint16_t num_channels,
                                       uint32_t num_frames, CL_AudioLayout layout) {
    if (!handle || !out || num_channels == 0 || num_channels > CL_AUDIO_MAX_CHANNELS || num_frames == 0)
        return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (!handle->receiver) {
        std::memset(out, 0, size_t(num_channels) * num_frames * sizeof(float));
        return CL_ERR_NOT_RUNNING;
    }
    if (handle->mc_active.load(std::memory_order_acquire)) return CL_ERR_UNSUPPORTED;
    if (layout == CL_AUDIO_LAYOUT_PLANAR) {
        float* planes[CL_AUDIO_MAX_CHANNELS];
        for (uint16_t c = 0; c < num_channels; ++c) planes[c] = out + size_t(c) * num_frames;
        handle->receiver->pop_audio_multichannel_planar(planes, num_channels, num_frames);
    } else {
        handle->receiver->pop_audio_multichannel_interleaved(out, num_channels, num_frames);
    }
    return int(num_frames);
}

int cl_receiver_get_channel_stats(ClReceiverHandle* handle, uint16_t channel_index,
                                  CL_AudioChannelStats* out) {
    if (!handle || !out || channel_index >= CL_AUDIO_MAX_CHANNELS) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    std::memset(out, 0, sizeof(*out));
    out->level_dbfs = -120.0f;
    if (!handle->receiver) return CL_ERR_NOT_RUNNING;
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
    return CL_OK;
}

int cl_receiver_get_stream_stats(ClReceiverHandle* handle, CL_StreamStats* out) {
    if (!handle || !out) return CL_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->state = handle->state.load(std::memory_order_acquire);
    out->adaptive_jitter = handle->adaptive_jitter.load(std::memory_order_acquire);
    CL_LIVE_LOCK(handle);
    if (!handle->receiver) return CL_OK;
    const uint64_t bytes = handle->receiver->audio_bytes_rx();
    const uint64_t now = cl::now_us();
    {
        std::lock_guard<std::mutex> slk(handle->stats_mu);
        const double dt_s = double(now - handle->win_start_us) / 1e6;
        if (dt_s >= 0.5) {
            const uint64_t db = bytes - handle->win_prev_bytes;
            handle->cached_audio_kbps = (double(db) * 8.0 / 1e3) / dt_s;
            handle->win_start_us = now;
            handle->win_prev_bytes = bytes;
        }
        out->audio_kbps = handle->cached_audio_kbps;
    }
    const auto& mc = handle->receiver->master_clock();
    out->clock_synced = mc.synced() ? 1 : 0;
    out->rtt_us = mc.min_rtt_us();
    out->clock_skew_ppm = mc.skew_ppm();
    out->effective_latency_ms = handle->receiver->effective_latency_ms();
    out->effective_target_ms = handle->receiver->effective_target_ms();
    out->audio_packets = handle->receiver->audio_packets_rx();
    out->audio_bytes = bytes;
    out->midi_events = handle->receiver->midi_stats().events_unique;
    out->stream_epoch_resets = handle->receiver->stream_epoch_resets();
    return CL_OK;
}

/* ---- Full-Duplex 64x64 AoIP Transceiver ---- */
ClDuplexHandle* cl_duplex_create(void) { return new (std::nothrow) ClDuplexHandle(); }
void cl_duplex_destroy(ClDuplexHandle* handle) { delete handle; }

int cl_duplex_start(ClDuplexHandle* h, const CL_DuplexConfig* cfg) {
    if (!h || !cfg) return CL_ERR_INVALID_ARG;
#ifndef CL_HAVE_SHM_BUS
    (void)cfg;
    return CL_ERR_UNSUPPORTED;
#else
    if (cfg->latency_ms < 2 || cfg->latency_ms > 2000 ||
        (cfg->sample_rate != 48000 && cfg->sample_rate != 96000) ||
        cfg->num_channels < 1 || cfg->num_channels > 64 || cfg->peer_port == 0 || !cfg->peer_host[0]) {
        return CL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->stop_internal();
    h->cfg = *cfg;
    h->tx_patch.set_sample_rate(uint32_t(cfg->sample_rate));
    h->rx_patch.set_sample_rate(uint32_t(cfg->sample_rate));

    CL_AudioBusConfig bus;
    cl_audio_bus_config_default(&bus);
    bus.sample_rate = cfg->sample_rate;
    bus.latency_ms = cfg->latency_ms;
    bus.num_channels = cfg->num_channels;

    h->rx = cl_receiver_create();
    cl_receiver_set_midi_callback(h->rx, h->midi_cb, h->midi_ud);
    cl_receiver_set_control_sync_callback(h->rx, h->cs_cb, h->cs_ud);
    cl_receiver_configure_audio_bus(h->rx, &bus);
    CL_ReceiverConfig rc;
    cl_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "%s", cfg->bind_ip[0] ? cfg->bind_ip : "0.0.0.0");
    rc.bind_port = cfg->listen_port;
    rc.sample_rate = cfg->sample_rate;
    rc.latency_ms = cfg->latency_ms;
    rc.num_channels = cfg->num_channels;
    rc.adaptive_jitter = cfg->adaptive_jitter;
    rc.enable_discovery = 0;
    int r = cl_receiver_start(h->rx, &rc);
    if (r != CL_OK) { h->stop_internal(); return r; }

    h->tx = cl_sender_create();
    cl_sender_configure_audio_bus(h->tx, &bus);
    CL_SenderConfig sc;
    cl_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "%s", cfg->peer_host);
    sc.dest_port = cfg->peer_port;
    sc.sample_rate = cfg->sample_rate;
    sc.latency_ms = cfg->latency_ms;
    sc.audio_codec = std::clamp<int>(cfg->audio_codec, 0, 8);
    r = cl_sender_start(h->tx, &sc);
    if (r != CL_OK) { h->stop_internal(); return r; }

    cl::ShmAudioHost::Config hc;
    hc.channels = uint32_t(cfg->num_channels);
    hc.bus_name = cfg->bus_name;
    hc.rx_target_frames = cfg->rx_headroom_frames;
    ClDuplexHandle* self = h;
    const bool ok_tx = h->tx_host.start(hc, [self](const float* const* p, uint32_t ch, uint32_t n, bool disc) {
        static thread_local std::vector<float> buf;
        static thread_local std::vector<float*> outp;
        buf.resize(size_t(ch) * n);
        outp.resize(ch);
        for (uint32_t c = 0; c < ch; ++c) outp[c] = buf.data() + size_t(c) * n;
        self->tx_patch.process(p, outp.data(), ch, n);
        const uint64_t mc = disc ? cl_sender_get_master_clock_samples(self->tx) : 0;
        const uint64_t ts = disc ? (mc >= n ? mc - n : 0) : CL_MEDIA_TS_AUTO;
        cl_sender_push_audio_multichannel(self->tx, outp.data(), uint16_t(ch), n, CL_AUDIO_FORMAT_F32,
                                          CL_AUDIO_LAYOUT_PLANAR, ts);
    }, nullptr);
    const bool ok_rx = h->rx_host.start(hc, nullptr, [self](float* const* p, uint32_t ch, uint32_t n) {
        static thread_local std::vector<float> buf;
        static thread_local std::vector<const float*> inp;
        buf.resize(size_t(ch) * n);
        inp.resize(ch);
        cl_receiver_pop_audio_multichannel(self->rx, buf.data(), uint16_t(ch), n, CL_AUDIO_LAYOUT_PLANAR);
        for (uint32_t c = 0; c < ch; ++c) inp[c] = buf.data() + size_t(c) * n;
        self->rx_patch.process(inp.data(), p, ch, n);
    });
    if (!ok_tx || !ok_rx) { h->stop_internal(); return CL_ERR_INTERNAL; }

    if (cfg->enable_discovery) {
        h->disc = std::make_unique<cl::AudioDiscoveryResponder>();
        h->disc->set_aoip_provider([self]() {
            cl::AoipAnnounce a;
            a.enabled = true;
            a.hostname = cl::local_host_name();
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
                       cfg->discovery_port ? cfg->discovery_port : cl::kAudioDiscoveryPort);
    }
    h->running.store(true, std::memory_order_release);
    return CL_OK;
#endif
}

void cl_duplex_stop(ClDuplexHandle* h) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->stop_internal();
}

int cl_duplex_set_latency_ms(ClDuplexHandle* h, uint32_t ms) {
    if (!h || ms < 2 || ms > 2000) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.latency_ms = ms;
    if (h->tx) cl_sender_set_audio_latency_ms(h->tx, ms);
    if (h->rx) cl_receiver_set_audio_latency_ms(h->rx, ms);
    return CL_OK;
}

int cl_duplex_set_adaptive_jitter(ClDuplexHandle* h, int enabled) {
    if (!h) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.adaptive_jitter = enabled ? 1 : 0;
    if (h->rx) cl_receiver_set_adaptive_jitter(h->rx, h->cfg.adaptive_jitter);
    return CL_OK;
}

int cl_duplex_set_audio_codec(ClDuplexHandle* h, int codec) {
    if (!h || codec < 0 || codec > 8) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.audio_codec = codec;
    if (h->tx) cl_sender_set_audio_codec(h->tx, codec);
    return CL_OK;
}

int cl_duplex_set_audio_bitrate(ClDuplexHandle* h, int kbps_per_ch) {
    if (!h) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cfg.audio_codec = int(cl::audio_codec_profile_from_bitrate(kbps_per_ch));
    if (h->tx) cl_sender_set_audio_bitrate(h->tx, kbps_per_ch);
    return CL_OK;
}

int cl_duplex_set_wifi_redundancy(ClDuplexHandle* h, int enabled, uint32_t stagger_us) {
    if (!h) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (h->tx) cl_sender_set_wifi_redundancy(h->tx, enabled, stagger_us);
    return CL_OK;
}

int cl_duplex_set_network_interfaces(ClDuplexHandle* h, const char* primary_ip, const char* secondary_ip) {
    if (!h) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (h->tx) cl_sender_set_network_interfaces(h->tx, primary_ip, secondary_ip);
    return CL_OK;
}

int cl_duplex_get_stats(ClDuplexHandle* h, CL_DuplexStats* out) {
    if (!h || !out) return CL_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    std::lock_guard<std::mutex> lk(h->api_mu);
    out->running = h->running.load() ? 1 : 0;
    out->adaptive_jitter = h->cfg.adaptive_jitter ? 1 : 0;
    out->effective_target_ms = h->cfg.latency_ms;
    if (!out->running) return CL_ERR_NOT_RUNNING;
#ifdef CL_HAVE_SHM_BUS
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
        out->effective_latency_ms = h->rx->receiver->effective_latency_ms();
        out->effective_target_ms = h->rx->receiver->effective_target_ms();
        for (uint16_t c = 0; c < uint16_t(h->cfg.num_channels); ++c) {
            const auto st = h->rx->receiver->audio_channel_rx_stats(c);
            out->rx_packets += st.packets_rx;
            out->rx_lost += st.packets_lost;
        }
    }
    if (h->tx && h->tx->sender) {
        const auto st = h->tx->sender->stats();
        out->tx_packets = st.audio_packets_sent;
        out->tx_pongs_sent = st.pongs_sent;
        out->midi_tx_events = st.midi_events_sent;
    }
    return CL_OK;
}

static cl::ChannelPatchbay* cl_duplex_patch(ClDuplexHandle* h, CL_AudioDirection dir) {
    return dir == CL_AUDIO_DIR_TX ? &h->tx_patch : &h->rx_patch;
}

int cl_audio_set_tx_routing(ClDuplexHandle* h, const int8_t dst_of_src[64]) {
    if (!h) return CL_ERR_INVALID_ARG;
    if (dst_of_src) h->tx_patch.set_routing(dst_of_src);
    else h->tx_patch.reset_identity();
    return CL_OK;
}

int cl_audio_set_rx_routing(ClDuplexHandle* h, const int8_t dst_of_src[64]) {
    if (!h) return CL_ERR_INVALID_ARG;
    if (dst_of_src) h->rx_patch.set_routing(dst_of_src);
    else h->rx_patch.reset_identity();
    return CL_OK;
}

int cl_audio_get_routing(ClDuplexHandle* h, CL_AudioDirection dir, int8_t dst_of_src[64]) {
    if (!h || !dst_of_src || (dir != CL_AUDIO_DIR_TX && dir != CL_AUDIO_DIR_RX)) return CL_ERR_INVALID_ARG;
    cl_duplex_patch(h, dir)->get_routing(dst_of_src);
    return CL_OK;
}

int cl_audio_set_channel_mute_solo(ClDuplexHandle* h, CL_AudioDirection dir, uint16_t ch, int mute, int solo) {
    if (!h || ch >= 64 || (dir != CL_AUDIO_DIR_TX && dir != CL_AUDIO_DIR_RX)) return CL_ERR_INVALID_ARG;
    cl::ChannelPatchbay* p = cl_duplex_patch(h, dir);
    if (mute >= 0) p->set_mute(ch, mute != 0);
    if (solo >= 0) p->set_solo(ch, solo != 0);
    return CL_OK;
}

/* ---- MIDI2 ---- */
int cl_sender_push_midi(ClSenderHandle* handle, uint8_t status, uint8_t data1, uint8_t data2, uint8_t port,
                        uint64_t media_ts_us) {
    if (!handle || status < 0x80 || port >= cl::kMaxMidiPorts) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (!handle->running.load(std::memory_order_acquire) || !handle->sender) return CL_ERR_NOT_RUNNING;
    return handle->sender->push_midi(status, data1 & 0x7F, data2 & 0x7F, port, media_ts_us) ? CL_OK
                                                                                           : CL_ERR_SOCKET;
}

void cl_receiver_set_midi_callback(ClReceiverHandle* handle, CL_MidiEventCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->midi_cb = cb;
    handle->midi_ud = user_data;
}

int cl_receiver_get_stream_epoch_info(ClReceiverHandle* handle, uint64_t* epoch_resets,
                                      uint32_t* active_midi_notes, uint64_t* midi_events_rx) {
    if (!handle) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (!handle->receiver) return CL_ERR_NOT_RUNNING;
    if (epoch_resets) *epoch_resets = handle->receiver->stream_epoch_resets();
    if (active_midi_notes) *active_midi_notes = handle->receiver->active_midi_notes();
    if (midi_events_rx) *midi_events_rx = handle->receiver->midi_stats().events_unique;
    return CL_OK;
}

int cl_duplex_send_midi(ClDuplexHandle* h, uint8_t status, uint8_t data1, uint8_t data2, uint8_t port,
                        uint64_t media_ts_us) {
    if (!h) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (!h->running.load(std::memory_order_acquire) || !h->tx) return CL_ERR_NOT_RUNNING;
    return cl_sender_push_midi(h->tx, status, data1, data2, port, media_ts_us);
}

void cl_duplex_set_midi_callback(ClDuplexHandle* h, CL_MidiEventCallback cb, void* user_data) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->midi_cb = cb;
    h->midi_ud = user_data;
    if (h->rx) cl_receiver_set_midi_callback(h->rx, cb, user_data);
}

int cl_receiver_get_midi_dejitter_stats(ClReceiverHandle* handle, CL_MidiDejitterStats* out) {
    if (!handle || !out) return CL_ERR_INVALID_ARG;
    *out = {};
    CL_LIVE_LOCK(handle);
    if (!handle->receiver) return CL_ERR_NOT_RUNNING;
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
    return CL_OK;
}

int cl_duplex_get_midi_dejitter_stats(ClDuplexHandle* h, CL_MidiDejitterStats* out) {
    if (!h || !out) return CL_ERR_INVALID_ARG;
    *out = {};
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (!h->running.load(std::memory_order_acquire) || !h->rx) return CL_ERR_NOT_RUNNING;
    return cl_receiver_get_midi_dejitter_stats(h->rx, out);
}

int cl_sender_set_midi_only(ClSenderHandle* handle, int enabled) {
    if (!handle) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (!handle->sender) return CL_ERR_NOT_RUNNING;
    handle->sender->set_midi_only(enabled != 0);
    return CL_OK;
}

int cl_receiver_set_midi_only(ClReceiverHandle* handle, int enabled) {
    if (!handle) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (!handle->receiver) return CL_ERR_NOT_RUNNING;
    handle->receiver->set_midi_only(enabled != 0);
    return CL_OK;
}

int cl_duplex_set_midi_only(ClDuplexHandle* h, int enabled) {
    if (!h) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (h->tx) cl_sender_set_midi_only(h->tx, enabled);
    if (h->rx) cl_receiver_set_midi_only(h->rx, enabled);
    return CL_OK;
}

/* ---- ControlSync ---- */
int cl_sender_send_control_sync(ClSenderHandle* handle, const CL_ControlSyncState* state) {
    if (!handle || !state) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (!handle->sender) return CL_ERR_NOT_RUNNING;
    return handle->sender->send_control_sync(to_wire_cs(*state)) ? CL_OK : CL_ERR_SOCKET;
}

void cl_sender_set_control_sync_callback(ClSenderHandle* handle, CL_ControlSyncCallback cb, void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cs_cb_mu);
    handle->cs_cb = cb;
    handle->cs_ud = user_data;
}

int cl_receiver_send_control_sync(ClReceiverHandle* handle, const CL_ControlSyncState* state) {
    if (!handle || !state) return CL_ERR_INVALID_ARG;
    CL_LIVE_LOCK(handle);
    if (!handle->receiver) return CL_ERR_NOT_RUNNING;
    return handle->receiver->send_control_sync(to_wire_cs(*state)) ? CL_OK : CL_ERR_SOCKET;
}

void cl_receiver_set_control_sync_callback(ClReceiverHandle* handle, CL_ControlSyncCallback cb,
                                           void* user_data) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(handle->cb_mu);
    handle->cs_cb = cb;
    handle->cs_ud = user_data;
}

int cl_duplex_send_control_sync(ClDuplexHandle* h, const CL_ControlSyncState* state) {
    if (!h || !state) return CL_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(h->api_mu);
    if (!h->tx) return CL_ERR_NOT_RUNNING;
    return cl_sender_send_control_sync(h->tx, state);
}

void cl_duplex_set_control_sync_callback(ClDuplexHandle* h, CL_ControlSyncCallback cb, void* user_data) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->api_mu);
    h->cs_cb = cb;
    h->cs_ud = user_data;
    if (h->rx) cl_receiver_set_control_sync_callback(h->rx, cb, user_data);
}

/* ---- Personal Monitor Bus Server & Client ---- */
ClMonitorServerHandle* cl_monitor_server_create(uint32_t sample_rate) {
    auto* h = new (std::nothrow) ClMonitorServerHandle();
    if (!h) return nullptr;
    h->srv = std::make_unique<cl::PersonalMonitorServer>(sample_rate ? sample_rate : 48000);
    return h;
}

void cl_monitor_server_destroy(ClMonitorServerHandle* srv) {
    if (!srv) return;
    if (srv->srv) srv->srv->stop();
    delete srv;
}

int cl_monitor_server_start(ClMonitorServerHandle* srv, uint16_t bind_port) {
    if (!srv || !srv->srv) return CL_ERR_INVALID_ARG;
    return srv->srv->start(bind_port) ? CL_OK : CL_ERR_SOCKET;
}

void cl_monitor_server_stop(ClMonitorServerHandle* srv) {
    if (srv && srv->srv) srv->srv->stop();
}

uint16_t cl_monitor_server_port(ClMonitorServerHandle* srv) {
    return (srv && srv->srv) ? srv->srv->bound_port() : 0;
}

void cl_monitor_server_set_channel_name(ClMonitorServerHandle* srv, int ch, const char* name) {
    if (srv && srv->srv && name) srv->srv->set_channel_name(ch, name);
}

void cl_monitor_server_set_client_mute(ClMonitorServerHandle* srv, int slot, int muted) {
    if (srv && srv->srv) srv->srv->set_client_mute(slot, muted != 0);
}

int cl_monitor_server_get_clients(ClMonitorServerHandle* srv, CL_MonitorClientInfo* out, int max_clients) {
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

void cl_monitor_server_push_audio_planar(ClMonitorServerHandle* srv, const float* const* planes,
                                         int num_channels, uint32_t frames) {
    if (srv && srv->srv) srv->srv->process_and_send(planes, num_channels, frames);
}

int cl_monitor_server_set_wifi_redundancy(ClMonitorServerHandle* srv, int enabled, uint32_t stagger_us) {
    if (!srv || !srv->srv) return CL_ERR_INVALID_ARG;
    srv->srv->set_wifi_redundancy(enabled != 0, stagger_us == 0 ? 1500u : stagger_us);
    return CL_OK;
}

int cl_monitor_server_set_multichannel_flow(ClMonitorServerHandle* srv, int enabled, uint8_t flow_channels) {
    if (!srv || !srv->srv) return CL_ERR_INVALID_ARG;
    srv->srv->set_multichannel_flow(enabled != 0, flow_channels < 2 ? uint8_t(2) : flow_channels);
    return CL_OK;
}

ClMonitorClientHandle* cl_monitor_client_create(uint32_t sample_rate) {
    auto* h = new (std::nothrow) ClMonitorClientHandle();
    if (!h) return nullptr;
    h->cli = std::make_unique<cl::PersonalMonitorClient>(sample_rate ? sample_rate : 48000);
    return h;
}

void cl_monitor_client_destroy(ClMonitorClientHandle* cli) {
    if (!cli) return;
    if (cli->cli) cli->cli->disconnect();
    delete cli;
}

int cl_monitor_client_connect(ClMonitorClientHandle* cli, const char* server_ip, uint16_t server_port,
                              const char* client_name, uint16_t bitrate_kbps, uint16_t latency_ms) {
    if (!cli || !cli->cli || !server_ip) return CL_ERR_INVALID_ARG;
    return cli->cli->connect(server_ip, server_port, client_name ? client_name : "Monitor",
                             bitrate_kbps ? bitrate_kbps : 256, latency_ms ? latency_ms : 20)
               ? CL_OK
               : CL_ERR_SOCKET;
}

void cl_monitor_client_disconnect(ClMonitorClientHandle* cli) {
    if (cli && cli->cli) cli->cli->disconnect();
}

int cl_monitor_client_request_meta(ClMonitorClientHandle* cli) {
    if (!cli || !cli->cli) return CL_ERR_INVALID_ARG;
    return cli->cli->request_meta() ? CL_OK : CL_ERR_SOCKET;
}

int cl_monitor_client_set_channel_mix(ClMonitorClientHandle* cli, int ch, float gain_db, float pan,
                                      int mute, int solo) {
    if (!cli || !cli->cli) return CL_ERR_INVALID_ARG;
    cl::PersonalMonitorChannelMix mix{};
    mix.gain_db = gain_db;
    mix.pan = pan;
    mix.mute = mute != 0;
    mix.solo = solo != 0;
    return cli->cli->set_channel_mix(ch, mix) ? CL_OK : CL_ERR_SOCKET;
}

int cl_monitor_client_set_dsp(ClMonitorClientHandle* cli, const CL_MonitorDspParams* dsp) {
    if (!cli || !cli->cli || !dsp) return CL_ERR_INVALID_ARG;
    cl::PersonalMonitorDspParams p{};
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
    return cli->cli->set_dsp(p) ? CL_OK : CL_ERR_SOCKET;
}

void cl_monitor_client_poll(ClMonitorClientHandle* cli) {
    if (cli && cli->cli) cli->cli->poll();
}

void cl_monitor_client_pop_stereo(ClMonitorClientHandle* cli, float* out_interleaved, uint32_t frames) {
    if (cli && cli->cli) cli->cli->pop_stereo(out_interleaved, frames);
}

int cl_monitor_client_get_meta(ClMonitorClientHandle* cli, CL_MonitorMetaChannel* out, int max_channels,
                               CL_MonitorTelemetry* tel) {
    if (!cli || !cli->cli) return CL_ERR_INVALID_ARG;
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

int cl_monitor_client_set_stream(ClMonitorClientHandle* cli, uint16_t bitrate_kbps, uint16_t latency_ms) {
    if (!cli || !cli->cli || latency_ms < 2 || latency_ms > 500) return CL_ERR_INVALID_ARG;
    if (bitrate_kbps != CL_MONITOR_BITRATE_LOSSLESS && (bitrate_kbps < 128 || bitrate_kbps > 512))
        return CL_ERR_INVALID_ARG;
    return cli->cli->update_stream(bitrate_kbps, latency_ms) ? CL_OK : CL_ERR_NOT_RUNNING;
}

int cl_monitor_client_keepalive(ClMonitorClientHandle* cli) {
    if (!cli || !cli->cli) return CL_ERR_INVALID_ARG;
    return cli->cli->keepalive() ? CL_OK : CL_ERR_NOT_RUNNING;
}

int cl_monitor_client_set_wifi_redundancy(ClMonitorClientHandle* cli, int enabled) {
    if (!cli || !cli->cli) return CL_ERR_INVALID_ARG;
    cli->cli->set_wifi_redundancy(enabled != 0);
    return CL_OK;
}

int cl_monitor_client_set_subscribed_mask(ClMonitorClientHandle* cli, uint64_t channel_mask) {
    if (!cli || !cli->cli) return CL_ERR_INVALID_ARG;
    cli->cli->set_active_channel_mask(channel_mask);
    return CL_OK;
}

int cl_monitor_client_get_flow_telemetry(ClMonitorClientHandle* cli, CL_MonitorFlowTelemetry* out) {
    if (!cli || !cli->cli || !out) return CL_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    const auto t = cli->cli->dante_telemetry();
    out->clock_locked = t.clock_locked ? 1 : 0;
    out->clock_offset_jitter_us = t.clock_offset_jitter_us;
    out->duplicates_recovered = t.duplicates_recovered;
    out->active_flow_channels = t.active_flow_channels;
    out->effective_playout_ms = t.effective_playout_ms;
    out->wifi_redundancy_enabled = t.wifi_redundancy_enabled ? 1 : 0;
    out->flow_packets_rx = t.flow_packets_rx;
    out->nacks_sent = t.nacks_sent;
    return CL_OK;
}

int cl_monitor_client_set_network_interfaces(ClMonitorClientHandle* cli, const char* primary_ip,
                                             const char* secondary_ip) {
    if (!cli || !cli->cli) return CL_ERR_INVALID_ARG;
    const std::string pri = primary_ip ? primary_ip : "";
    const std::string sec = secondary_ip ? secondary_ip : "";
    return cli->cli->set_network_interfaces(pri, sec) ? CL_OK : CL_ERR_SOCKET;
}

/* ---- Network Interfaces & Audio Discovery ---- */
int cl_net_list_interfaces_ex(ClNetInterfaceInfo* out_list, int max_count,
                              int include_disconnected, int show_all) {
    const auto list = cl::enumerate_network_interfaces(include_disconnected != 0, show_all != 0);
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

int cl_net_list_interfaces(ClNetInterfaceInfo* out_list, int max_count) {
    return cl_net_list_interfaces_ex(out_list, max_count, 0, 0);
}

int cl_discovery_scan_audio_nodes(int timeout_ms, uint16_t discovery_port, CL_AudioNode* out, int max_nodes) {
    if (!out || max_nodes <= 0) return 0;
    const auto found = cl::audio_discovery_scan(timeout_ms,
                                                discovery_port ? discovery_port : cl::kAudioDiscoveryPort);
    std::vector<const cl::DiscoveredAudioNode*> uniq;
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
        CL_AudioNode& o = out[n++];
        std::memset(&o, 0, sizeof(o));
        std::snprintf(o.ip, sizeof(o.ip), "%s", r.ip.c_str());
        std::snprintf(o.name, sizeof(o.name), "%s", r.device_name.c_str());
        std::snprintf(o.hostname, sizeof(o.hostname), "%s", r.hostname.c_str());
        std::snprintf(o.audio_driver, sizeof(o.audio_driver), "%s", r.audio_driver.c_str());
        std::snprintf(o.os, sizeof(o.os), "%s", r.os.c_str());
        o.rx_port = r.aoip_rx_port ? r.aoip_rx_port : r.stream_port;
        o.tx_port = r.aoip_tx_port;
        o.channels = r.audio_channels;
        o.duplex = r.duplex ? 1 : 0;
        o.driver_connected = r.driver_connected ? 1 : 0;
        o.rtt_ms = r.rtt_ms;
    }
    return n;
}

char* cl_discovery_scan_json(int timeout_ms, uint16_t discovery_port) {
    const auto found = cl::audio_discovery_scan(timeout_ms,
                                                discovery_port ? discovery_port : cl::kAudioDiscoveryPort);
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < found.size(); ++i) {
        if (i) os << ",";
        os << "{\"ip\":\"" << json_escape(found[i].ip) << "\",\"port\":" << found[i].stream_port
           << ",\"name\":\"" << json_escape(found[i].device_name) << "\",\"os\":\""
           << json_escape(found[i].os) << "\",\"aoip\":" << (found[i].aoip ? "true" : "false")
           << ",\"channels\":" << found[i].audio_channels
           << ",\"duplex\":" << (found[i].duplex ? "true" : "false")
           << ",\"driver\":\"" << json_escape(found[i].audio_driver)
           << "\",\"rtt_ms\":" << found[i].rtt_ms << "}";
    }
    os << "]";
    return dup_cstr(os.str());
}

}  // extern "C"
