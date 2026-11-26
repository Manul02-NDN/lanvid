// obs_lanvid_source.cpp — Zero-Crash OBS Studio Input Source ("LanVid Camera / Stream Source")
// Supports Auto-Discovery (LAN phones, Studios, and local LanVidVideoBus_v1 SHM) + Dedicated UDP Port,
// VideoPlayoutBuffer (Fixed 5..250 ms / Adaptive Wi-Fi), Camera Number Badge Sync, Remote Camera Control,
// Lavender Standby Placeholder, and 64ch AoIP Audio Routing.

#include "obs_api_compat.hpp"
#include "obs_lanvid_core.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if defined(LV_HAVE_REAL_LIBOBS)

namespace lv::obs {

namespace {

constexpr const char* kPropConnMode = "conn_mode";
constexpr const char* kPropDevice = "lanvid_device";
constexpr const char* kPropRefresh = "lanvid_refresh";
constexpr const char* kPropBindIp = "bind_ip";
constexpr const char* kPropPort = "listen_port";
constexpr const char* kPropPeerIp = "peer_ip";
constexpr const char* kPropPortStatus = "port_status";
constexpr const char* kPropCamId = "camera_id";
constexpr const char* kPropBadgeNum = "badge_number";
constexpr const char* kPropBadgeTag = "badge_tag";
constexpr const char* kPropApplyBadge = "apply_badge";
constexpr const char* kPropJitter = "jitter_preset";
constexpr const char* kPropDelayMs = "video_delay_ms";
constexpr const char* kPropPlaceholder = "show_placeholder";
constexpr const char* kPropForceIdr = "force_idr";
constexpr const char* kPropRemoteQuality = "remote_quality";
constexpr const char* kPropRemoteFps = "remote_fps";
constexpr const char* kPropRemoteBitrate = "remote_bitrate";
constexpr const char* kPropRemoteLens = "remote_lens";
constexpr const char* kPropApplyRemote = "apply_remote";
constexpr const char* kPropAudioRoute = "audio_route";
constexpr const char* kPropAudioPair = "audio_pair";
constexpr const char* kPropAudioLat = "audio_latency_ms";
constexpr const char* kPropAudioCh = "audio_channels";
constexpr const char* kPropAutoTally = "auto_tally";
constexpr const char* kPropLiveOnly = "only_on_air_when_live";
constexpr const char* kPropSyncRec = "sync_remote_rec";

struct LanVidSourceContext {
    obs_source_t* obs_source = nullptr;
    std::shared_ptr<LanVidObsSourceEngine> engine;
    std::shared_ptr<std::atomic<bool>> alive;
};

void parse_device_id(const std::string& dev_id, SourceConfig& cfg) {
    cfg.shm_slot = -1;
    if (dev_id.rfind("shm:", 0) == 0) {
        cfg.shm_slot = std::clamp(std::atoi(dev_id.c_str() + 4), 0, int(lv::kVideoBusSlots) - 1);
        return;
    }
    if (dev_id.rfind("tally:", 0) == 0) {
        const std::string rest = dev_id.substr(6);
        const auto c = rest.rfind(':');
        if (c != std::string::npos) {
            cfg.peer_ip = rest.substr(0, c);
            const uint32_t cid = uint32_t(std::strtoul(rest.substr(c + 1).c_str(), nullptr, 10));
            if (cid > 0) cfg.camera_id = cid;
        }
        return;
    }
    if (dev_id.rfind("lan:", 0) == 0) {
        const std::string rest = dev_id.substr(4);
        const auto c = rest.rfind(':');
        if (c != std::string::npos) {
            cfg.peer_ip = rest.substr(0, c);
            const uint16_t p = uint16_t(std::atoi(rest.substr(c + 1).c_str()));
            if (p >= 1024) cfg.listen_port = p;
        }
    }
}

SourceConfig config_from_settings(obs_data_t* settings, obs_source_t* src) {
    SourceConfig cfg{};
    const char* name = src ? obs_source_get_name(src) : "LanVid Camera";
    cfg.source_name = name ? name : "LanVid Camera";
    cfg.connection_mode = static_cast<ConnectionMode>(obs_data_get_int(settings, kPropConnMode));

    const char* bind = obs_data_get_string(settings, kPropBindIp);
    cfg.bind_ip = (bind && bind[0]) ? bind : "0.0.0.0";
    cfg.listen_port = static_cast<uint16_t>(obs_data_get_int(settings, kPropPort));
    if (cfg.listen_port == 0) cfg.listen_port = 5006;

    const char* peer = obs_data_get_string(settings, kPropPeerIp);
    cfg.peer_ip = peer ? peer : "";

    cfg.camera_id = static_cast<uint32_t>(obs_data_get_int(settings, kPropCamId));
    if (cfg.camera_id == 0) cfg.camera_id = 1;

    const char* dev = obs_data_get_string(settings, kPropDevice);
    cfg.device_id = dev ? dev : "";
    if (cfg.connection_mode == ConnectionMode::kAutoDiscovery && !cfg.device_id.empty()) {
        parse_device_id(cfg.device_id, cfg);
    }

    cfg.assigned_cam_number = static_cast<uint8_t>(std::clamp<long long>(obs_data_get_int(settings, kPropBadgeNum), 1, 16));
    const char* btag = obs_data_get_string(settings, kPropBadgeTag);
    cfg.assigned_cam_tag = (btag && btag[0]) ? std::string(btag).substr(0, 15)
                                             : ("CAM " + std::to_string(cfg.assigned_cam_number));

    cfg.jitter_preset = static_cast<JitterPreset>(obs_data_get_int(settings, kPropJitter));
    cfg.fixed_delay_mode = (cfg.jitter_preset != JitterPreset::kAdaptiveWifi);
    const int custom_ms = static_cast<int>(obs_data_get_int(settings, kPropDelayMs));
    if (cfg.jitter_preset == JitterPreset::kCustomFixed || cfg.jitter_preset == JitterPreset::kAdaptiveWifi) {
        cfg.video_delay_ms = static_cast<uint32_t>(std::clamp(custom_ms > 0 ? custom_ms : 33, 5, 250));
    } else {
        cfg.video_delay_ms = jitter_preset_to_ms(cfg.jitter_preset);
    }
    cfg.show_placeholder = obs_data_get_bool(settings, kPropPlaceholder);

    cfg.audio_route = static_cast<AudioRouteMode>(obs_data_get_int(settings, kPropAudioRoute));
    cfg.audio_pair_index = static_cast<int>(obs_data_get_int(settings, kPropAudioPair));
    cfg.audio_latency_ms = static_cast<uint32_t>(obs_data_get_int(settings, kPropAudioLat));
    cfg.audio_channels = static_cast<uint32_t>(obs_data_get_int(settings, kPropAudioCh));
    if (cfg.audio_channels < 2) cfg.audio_channels = 2;

    cfg.auto_tally = obs_data_get_bool(settings, kPropAutoTally);
    cfg.only_on_air_when_live = obs_data_get_bool(settings, kPropLiveOnly);
    cfg.sync_remote_rec = obs_data_get_bool(settings, kPropSyncRec);
    return cfg;
}

void push_obs_video(obs_source_t* src, const VideoFrameView& fv) {
    if (!src || fv.width == 0 || fv.height == 0) return;
    obs_source_frame frame{};
    for (int i = 0; i < 4; ++i) {
        frame.data[i] = const_cast<uint8_t*>(fv.data[i]);
        frame.linesize[i] = fv.linesize[i];
    }
    frame.width = fv.width;
    frame.height = fv.height;
    frame.timestamp = fv.timestamp_ns;
    switch (fv.format) {
        case VideoPixelFormat::kNV12: frame.format = VIDEO_FORMAT_NV12; break;
        case VideoPixelFormat::kI420: frame.format = VIDEO_FORMAT_I420; break;
        case VideoPixelFormat::kBGRA:
        default: frame.format = VIDEO_FORMAT_BGRA; break;
    }
    video_colorspace cs = VIDEO_CS_709;
    if (fv.color_space == ColorSpace::kBT2100_PQ) cs = VIDEO_CS_2100_PQ;
    else if (fv.color_space == ColorSpace::kBT2100_HLG) cs = VIDEO_CS_2100_HLG;
    video_format_get_parameters_for_format(cs, fv.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
                                           frame.format, frame.color_matrix, frame.color_range_min,
                                           frame.color_range_max);
    frame.full_range = fv.full_range;
    obs_source_output_video(src, &frame);
}

void push_obs_audio(obs_source_t* src, const AudioBlockView& av) {
    if (!src || av.frames == 0 || !av.planes[0]) return;
    obs_source_audio audio{};
    audio.data[0] = reinterpret_cast<const uint8_t*>(av.planes[0]);
    audio.data[1] = reinterpret_cast<const uint8_t*>(av.planes[1] ? av.planes[1] : av.planes[0]);
    audio.frames = av.frames;
    audio.speakers = (av.channels == 1) ? SPEAKERS_MONO : SPEAKERS_STEREO;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = av.sample_rate;
    audio.timestamp = av.timestamp_ns;
    obs_source_output_audio(src, &audio);
}

void populate_device_list(obs_properties_t* props) {
    obs_property_t* list = obs_properties_get(props, kPropDevice);
    if (!list) return;
    obs_property_list_clear(list);
    obs_property_list_add_string(list, "Auto / Default Port (Listen for any sender)", "");
    for (const auto& item : LanVidObsRuntime::instance().discovered_cameras()) {
        obs_property_list_add_string(list, item.display_name.c_str(), item.id.c_str());
    }
}

void update_port_conflict_banner(obs_properties_t* props, void* data) {
    obs_property_t* p = obs_properties_get(props, kPropPortStatus);
    if (!p) return;
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (!ctx || !ctx->engine) {
        obs_property_set_description(p, "Port status: ready");
        obs_property_text_set_info_type(p, OBS_TEXT_INFO_NORMAL);
        return;
    }
    const auto cfg = ctx->engine->config();
    if (cfg.shm_slot >= 0) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Shared Memory Mode: reading LanVidVideoBus_v1 slot %d (0 ms LAN overhead)",
                      cfg.shm_slot);
        obs_property_set_description(p, msg);
        obs_property_text_set_info_type(p, OBS_TEXT_INFO_NORMAL);
        return;
    }
    const auto conflict = LanVidObsRuntime::instance().check_port_conflict(cfg.listen_port, ctx->engine->uid());
    if (conflict.has_conflict) {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "WARNING: UDP port %u is already used by '%s'! Pick another port (e.g. %u).",
                      unsigned(cfg.listen_port), conflict.conflicting_source.c_str(), unsigned(cfg.listen_port + 2));
        obs_property_set_description(p, msg);
        obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
    } else {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "UDP port %u active and conflict-free.", unsigned(ctx->engine->bound_port()));
        obs_property_set_description(p, msg);
        obs_property_text_set_info_type(p, OBS_TEXT_INFO_NORMAL);
    }
}

bool on_refresh_devices(obs_properties_t* props, obs_property_t*, void* data) {
    LanVidObsRuntime::instance().refresh_discovery_now();
    populate_device_list(props);
    update_port_conflict_banner(props, data);
    return true;
}

bool on_force_idr(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (ctx && ctx->engine) ctx->engine->request_idr_keyframe();
    return false;
}

bool on_apply_badge(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (!ctx || !ctx->engine || !ctx->obs_source) return false;
    obs_data_t* s = obs_source_get_settings(ctx->obs_source);
    const uint8_t num = static_cast<uint8_t>(std::clamp<long long>(obs_data_get_int(s, kPropBadgeNum), 1, 16));
    const char* tag = obs_data_get_string(s, kPropBadgeTag);
    ctx->engine->assign_camera_number(num, tag ? tag : "");
    obs_data_release(s);
    return false;
}

bool on_apply_remote(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (!ctx || !ctx->engine || !ctx->obs_source) return false;
    obs_data_t* s = obs_source_get_settings(ctx->obs_source);
    RemoteCameraControl ctl{};
    ctl.quality = static_cast<CameraQualityPreset>(obs_data_get_int(s, kPropRemoteQuality));
    ctl.fps = static_cast<uint8_t>(std::clamp<long long>(obs_data_get_int(s, kPropRemoteFps), 24, 60));
    ctl.bitrate_kbps = static_cast<uint32_t>(std::clamp<long long>(obs_data_get_int(s, kPropRemoteBitrate), 5000, 150000));
    ctl.lens = static_cast<uint8_t>(std::clamp<long long>(obs_data_get_int(s, kPropRemoteLens), 0, 3));
    const auto cfg = ctx->engine->config();
    ctl.video_delay_ms = static_cast<uint16_t>(std::clamp<uint32_t>(cfg.video_delay_ms, 5, 250));
    ctl.fixed_delay_mode = cfg.fixed_delay_mode;
    ctx->engine->send_remote_camera_control(ctl);
    obs_data_release(s);
    return false;
}

const char* source_get_name(void*) {
    return obs_module_text("LanVid.Source.Name");
}

void source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, kPropConnMode, static_cast<int>(ConnectionMode::kAutoDiscovery));
    obs_data_set_default_string(settings, kPropDevice, "");
    obs_data_set_default_string(settings, kPropBindIp, "0.0.0.0");
    obs_data_set_default_int(settings, kPropPort, LanVidObsRuntime::instance().allocate_next_port());
    obs_data_set_default_string(settings, kPropPeerIp, "");
    obs_data_set_default_int(settings, kPropCamId, 1);
    obs_data_set_default_int(settings, kPropBadgeNum, 1);
    obs_data_set_default_string(settings, kPropBadgeTag, "CAM 1");
    obs_data_set_default_int(settings, kPropJitter, static_cast<int>(JitterPreset::kFixed33ms));
    obs_data_set_default_int(settings, kPropDelayMs, 33);
    obs_data_set_default_bool(settings, kPropPlaceholder, true);
    obs_data_set_default_int(settings, kPropRemoteQuality, static_cast<int>(CameraQualityPreset::kStudio1080p));
    obs_data_set_default_int(settings, kPropRemoteFps, 60);
    obs_data_set_default_int(settings, kPropRemoteBitrate, 25000);
    obs_data_set_default_int(settings, kPropRemoteLens, static_cast<int>(CameraLens::kLensWide));
    obs_data_set_default_int(settings, kPropAudioRoute, static_cast<int>(AudioRouteMode::kStereoMix));
    obs_data_set_default_int(settings, kPropAudioPair, 0);
    obs_data_set_default_int(settings, kPropAudioLat, 20);
    obs_data_set_default_int(settings, kPropAudioCh, 2);
    obs_data_set_default_bool(settings, kPropAutoTally, true);
    obs_data_set_default_bool(settings, kPropLiveOnly, false);
    obs_data_set_default_bool(settings, kPropSyncRec, false);
}

obs_properties_t* source_get_properties(void* data) {
    obs_properties_t* props = obs_properties_create();

    // 1. Connection & Discovery (NON-BLOCKING cached list!)
    obs_property_t* mode = obs_properties_add_list(props, kPropConnMode, obs_module_text("LanVid.Source.ConnMode"),
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, "Auto-Discovery (LAN Camera / Studio SHM)", static_cast<int>(ConnectionMode::kAutoDiscovery));
    obs_property_list_add_int(mode, "Dedicated Port / IP", static_cast<int>(ConnectionMode::kDedicatedPort));

    obs_properties_add_list(props, kPropDevice, obs_module_text("LanVid.Source.Device"),
                            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    populate_device_list(props);
    obs_properties_add_button(props, kPropRefresh, obs_module_text("LanVid.Source.RefreshDevices"), on_refresh_devices);

    obs_properties_add_text(props, kPropBindIp, obs_module_text("LanVid.Source.BindIp"), OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, kPropPort, obs_module_text("LanVid.Source.UdpPort"), 1024, 65535, 1);
    obs_properties_add_text(props, kPropPeerIp, obs_module_text("LanVid.Source.PeerIp"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, kPropPortStatus, obs_module_text("LanVid.Source.PortStatus"), OBS_TEXT_INFO);
    update_port_conflict_banner(props, data);

    // 2. Camera Number Badge & Tally ID
    obs_properties_add_int(props, kPropCamId, obs_module_text("LanVid.Source.CameraId"), 1, 16, 1);
    obs_properties_add_int(props, kPropBadgeNum, obs_module_text("LanVid.Source.BadgeNumber"), 1, 16, 1);
    obs_properties_add_text(props, kPropBadgeTag, obs_module_text("LanVid.Source.BadgeTag"), OBS_TEXT_DEFAULT);
    obs_properties_add_button(props, kPropApplyBadge, obs_module_text("LanVid.Source.ApplyBadge"), on_apply_badge);

    // 3. VideoPlayoutBuffer
    obs_property_t* jit = obs_properties_add_list(props, kPropJitter, obs_module_text("LanVid.Source.JitterMode"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(jit, "Fixed 16 ms (Ultra-Low LAN)", static_cast<int>(JitterPreset::kFixed16ms));
    obs_property_list_add_int(jit, "Fixed 33 ms (Smooth Studio, Default)", static_cast<int>(JitterPreset::kFixed33ms));
    obs_property_list_add_int(jit, "Fixed 50 ms (Wi-Fi Safe)", static_cast<int>(JitterPreset::kFixed50ms));
    obs_property_list_add_int(jit, "Fixed 80 ms (Heavy Wi-Fi)", static_cast<int>(JitterPreset::kFixed80ms));
    obs_property_list_add_int(jit, "Custom Fixed Delay (Slider)", static_cast<int>(JitterPreset::kCustomFixed));
    obs_property_list_add_int(jit, "Adaptive Wi-Fi (Auto 33..200 ms)", static_cast<int>(JitterPreset::kAdaptiveWifi));
    obs_properties_add_int_slider(props, kPropDelayMs, obs_module_text("LanVid.Source.DelayMs"), 5, 250, 1);
    obs_properties_add_bool(props, kPropPlaceholder, obs_module_text("LanVid.Source.Placeholder"));
    obs_properties_add_button(props, kPropForceIdr, obs_module_text("LanVid.Source.ForceIdr"), on_force_idr);

    // 4. Remote Camera Control
    obs_property_t* rq = obs_properties_add_list(props, kPropRemoteQuality, obs_module_text("LanVid.Source.RemoteQuality"),
                                                 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(rq, "360p Proxy (4 Mbps)", static_cast<int>(CameraQualityPreset::kProxy360p));
    obs_property_list_add_int(rq, "720p HD (12 Mbps)", static_cast<int>(CameraQualityPreset::kHd720p));
    obs_property_list_add_int(rq, "1080p Studio (25 Mbps)", static_cast<int>(CameraQualityPreset::kStudio1080p));
    obs_property_list_add_int(rq, "4K60 Master (85 Mbps)", static_cast<int>(CameraQualityPreset::kMaster4K60));

    obs_property_t* rfps = obs_properties_add_list(props, kPropRemoteFps, obs_module_text("LanVid.Source.RemoteFps"),
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int f : {24, 25, 30, 50, 60}) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d fps", f);
        obs_property_list_add_int(rfps, buf, f);
    }
    obs_properties_add_int_slider(props, kPropRemoteBitrate, obs_module_text("LanVid.Source.RemoteBitrate"), 5000, 150000, 1000);
    obs_property_t* rlens = obs_properties_add_list(props, kPropRemoteLens, obs_module_text("LanVid.Source.RemoteLens"),
                                                    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(rlens, "Wide (1x)", static_cast<int>(CameraLens::kLensWide));
    obs_property_list_add_int(rlens, "Ultra-Wide (0.5x)", static_cast<int>(CameraLens::kLensUltraWide));
    obs_property_list_add_int(rlens, "Tele (2x+)", static_cast<int>(CameraLens::kLensTele));
    obs_property_list_add_int(rlens, "Front", static_cast<int>(CameraLens::kLensFront));
    obs_properties_add_button(props, kPropApplyRemote, obs_module_text("LanVid.Source.ApplyRemote"), on_apply_remote);

    // 5. Multi-Channel AoIP Audio Routing
    obs_property_t* route = obs_properties_add_list(props, kPropAudioRoute, obs_module_text("LanVid.Source.AudioRoute"),
                                                    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(route, "Stereo Mix (Ch 1-2)", static_cast<int>(AudioRouteMode::kStereoMix));
    obs_property_list_add_int(route, "Stereo Pair (Ch 1-64)", static_cast<int>(AudioRouteMode::kStereoPair));
    obs_property_list_add_int(route, "Mono Sum", static_cast<int>(AudioRouteMode::kMonoSum));
    obs_property_list_add_int(route, "Video Only (Disable Audio)", static_cast<int>(AudioRouteMode::kVideoOnly));

    obs_property_t* pair = obs_properties_add_list(props, kPropAudioPair, obs_module_text("LanVid.Source.AudioPair"),
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int i = 0; i < 32; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Ch %d-%d", i * 2 + 1, i * 2 + 2);
        obs_property_list_add_int(pair, buf, i);
    }

    obs_property_t* lat = obs_properties_add_list(props, kPropAudioLat, obs_module_text("LanVid.Source.AudioLatency"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int ms : {5, 10, 20, 40}) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d ms", ms);
        obs_property_list_add_int(lat, buf, ms);
    }

    obs_property_t* chs = obs_properties_add_list(props, kPropAudioCh, obs_module_text("LanVid.Source.AudioChannels"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int c : {2, 8, 16, 32, 64}) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d ch", c);
        obs_property_list_add_int(chs, buf, c);
    }

    // 6. Auto-Tally & Remote REC
    obs_properties_add_bool(props, kPropAutoTally, obs_module_text("LanVid.Source.AutoTally"));
    obs_properties_add_bool(props, kPropLiveOnly, obs_module_text("LanVid.Source.OnlyOnAirWhenLive"));
    obs_properties_add_bool(props, kPropSyncRec, obs_module_text("LanVid.Source.SyncRemoteRec"));

    return props;
}

void* source_create(obs_data_t* settings, obs_source_t* source) {
    auto* ctx = new LanVidSourceContext();
    ctx->obs_source = source;
    ctx->alive = std::make_shared<std::atomic<bool>>(true);
    ctx->engine = LanVidObsSourceEngine::create();

    const SourceConfig cfg = config_from_settings(settings, source);
    LanVidObsRuntime::instance().startup();

    auto alive_flag = ctx->alive;
    ctx->engine->start(
        cfg,
        [source, alive_flag](const VideoFrameView& fv) {
            if (alive_flag->load(std::memory_order_acquire)) push_obs_video(source, fv);
        },
        [source, alive_flag](const AudioBlockView& av) {
            if (alive_flag->load(std::memory_order_acquire)) push_obs_audio(source, av);
        });
    return ctx;
}

void source_destroy(void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (!ctx) return;
    if (ctx->alive) ctx->alive->store(false, std::memory_order_release);
    if (ctx->engine) ctx->engine->stop();
    delete ctx;
}

void source_update(void* data, obs_data_t* settings) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (!ctx || !ctx->engine) return;
    ctx->engine->update_config(config_from_settings(settings, ctx->obs_source));
}

void source_activate(void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (ctx && ctx->engine) ctx->engine->set_obs_visibility(/*in_program=*/true, /*in_preview=*/false);
}

void source_deactivate(void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (ctx && ctx->engine) ctx->engine->set_obs_visibility(/*in_program=*/false, /*in_preview=*/false);
}

}  // namespace

void register_lanvid_source() {
    obs_source_info info{};
    info.id = "lanvid_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = source_get_name;
    info.create = source_create;
    info.destroy = source_destroy;
    info.get_defaults = source_get_defaults;
    info.get_properties = source_get_properties;
    info.update = source_update;
    info.activate = source_activate;
    info.deactivate = source_deactivate;
    obs_register_source(&info);
}

}  // namespace lv::obs

#endif  // LV_HAVE_REAL_LIBOBS
