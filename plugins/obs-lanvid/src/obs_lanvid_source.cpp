// obs_lanvid_source.cpp — OBS Studio Input Source ("LanVid Camera / Stream Source")
// Supports 360p..4K DCI 60fps + 9:16 vertical, dynamic resolution/FPS switching,
// zero-copy NV12/I420/BGRA delivery, 64ch AoIP audio routing, LAN discovery & Auto-Tally.

#include "obs_api_compat.hpp"
#include "obs_lanvid_core.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(LV_HAVE_REAL_LIBOBS)

namespace lv::obs {

namespace {

constexpr const char* kPropDevice = "lanvid_device";
constexpr const char* kPropRefresh = "lanvid_refresh";
constexpr const char* kPropBindIp = "bind_ip";
constexpr const char* kPropPort = "listen_port";
constexpr const char* kPropCamId = "camera_id";
constexpr const char* kPropJitter = "jitter_preset";
constexpr const char* kPropForceIdr = "force_idr";
constexpr const char* kPropAudioRoute = "audio_route";
constexpr const char* kPropAudioPair = "audio_pair";
constexpr const char* kPropAudioLat = "audio_latency_ms";
constexpr const char* kPropAudioCh = "audio_channels";
constexpr const char* kPropAutoTally = "auto_tally";
constexpr const char* kPropLiveOnly = "only_on_air_when_live";
constexpr const char* kPropSyncRec = "sync_remote_rec";

struct LanVidSourceContext {
    obs_source_t* obs_source = nullptr;
    LanVidObsSourceEngine engine;
};

SourceConfig config_from_settings(obs_data_t* settings, obs_source_t* src) {
    SourceConfig cfg{};
    const char* name = src ? obs_source_get_name(src) : "LanVid Camera";
    cfg.source_name = name ? name : "LanVid Camera";
    const char* bind = obs_data_get_string(settings, kPropBindIp);
    cfg.bind_ip = (bind && bind[0]) ? bind : "0.0.0.0";
    cfg.listen_port = static_cast<uint16_t>(obs_data_get_int(settings, kPropPort));
    if (cfg.listen_port == 0) cfg.listen_port = 5004;
    cfg.camera_id = static_cast<uint32_t>(obs_data_get_int(settings, kPropCamId));
    if (cfg.camera_id == 0) cfg.camera_id = 1;
    cfg.jitter_preset = static_cast<JitterPreset>(obs_data_get_int(settings, kPropJitter));
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

bool on_refresh_devices(obs_properties_t* props, obs_property_t*, void*) {
    obs_property_t* list = obs_properties_get(props, kPropDevice);
    if (!list) return false;
    obs_property_list_clear(list);
    obs_property_list_add_string(list, "Manual (Bind IP & UDP Port)", "");
    const auto peers = LanVidObsManager::instance().scan_lan_devices(350);
    for (const auto& p : peers) {
        char label[128];
        std::snprintf(label, sizeof(label), "%s (%s:%u)", p.name, p.ip, unsigned(p.port));
        obs_property_list_add_string(list, label, p.ip);
    }
    return true;
}

bool on_force_idr(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (ctx) ctx->engine.request_idr_keyframe();
    return false;
}

const char* source_get_name(void*) {
    return obs_module_text("LanVid.Source.Name");
}

void source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, kPropBindIp, "0.0.0.0");
    obs_data_set_default_int(settings, kPropPort, LanVidObsManager::instance().allocate_next_port());
    obs_data_set_default_int(settings, kPropCamId, 1);
    obs_data_set_default_int(settings, kPropJitter, static_cast<int>(JitterPreset::kBalanced));
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

    // 1. LAN / mDNS Auto-Discovery
    obs_property_t* dev_list = obs_properties_add_list(props, kPropDevice, obs_module_text("LanVid.Source.Device"),
                                                       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(dev_list, "Manual (Bind IP & UDP Port)", "");
    obs_properties_add_button(props, kPropRefresh, obs_module_text("LanVid.Source.RefreshDevices"), on_refresh_devices);

    obs_properties_add_text(props, kPropBindIp, obs_module_text("LanVid.Source.BindIp"), OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, kPropPort, obs_module_text("LanVid.Source.UdpPort"), 1024, 65535, 1);
    obs_properties_add_int(props, kPropCamId, obs_module_text("LanVid.Source.CameraId"), 1, 8, 1);

    // 2. Video Jitter & Sync Mode
    obs_property_t* jit = obs_properties_add_list(props, kPropJitter, obs_module_text("LanVid.Source.JitterMode"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(jit, "Ultra-Low Latency (5-10 ms)", static_cast<int>(JitterPreset::kUltraLow));
    obs_property_list_add_int(jit, "Balanced (15-30 ms)", static_cast<int>(JitterPreset::kBalanced));
    obs_property_list_add_int(jit, "Smooth Broadcast (50-100 ms)", static_cast<int>(JitterPreset::kSmooth));
    obs_properties_add_button(props, kPropForceIdr, obs_module_text("LanVid.Source.ForceIdr"), on_force_idr);

    // 3. Multi-Channel AoIP Audio Routing (up to 64ch)
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
    obs_property_list_add_int(lat, "5 ms", 5);
    obs_property_list_add_int(lat, "10 ms", 10);
    obs_property_list_add_int(lat, "20 ms", 20);
    obs_property_list_add_int(lat, "40 ms", 40);

    obs_property_t* chs = obs_properties_add_list(props, kPropAudioCh, obs_module_text("LanVid.Source.AudioChannels"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int c : {2, 8, 16, 32, 64}) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d ch", c);
        obs_property_list_add_int(chs, buf, c);
    }

    // 4. Studio Auto-Tally & Remote REC Sync
    obs_properties_add_bool(props, kPropAutoTally, obs_module_text("LanVid.Source.AutoTally"));
    obs_properties_add_bool(props, kPropLiveOnly, obs_module_text("LanVid.Source.OnlyOnAirWhenLive"));
    obs_properties_add_bool(props, kPropSyncRec, obs_module_text("LanVid.Source.SyncRemoteRec"));

    on_refresh_devices(props, nullptr, data);
    return props;
}

void* source_create(obs_data_t* settings, obs_source_t* source) {
    auto* ctx = new LanVidSourceContext();
    ctx->obs_source = source;
    const SourceConfig cfg = config_from_settings(settings, source);
    LanVidObsManager::instance().ensure_studio_started();
    ctx->engine.start(
        cfg,
        [source](const VideoFrameView& fv) { push_obs_video(source, fv); },
        [source](const AudioBlockView& av) { push_obs_audio(source, av); });
    return ctx;
}

void source_destroy(void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (!ctx) return;
    ctx->engine.stop();
    delete ctx;
}

void source_update(void* data, obs_data_t* settings) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (!ctx) return;
    ctx->engine.update_config(config_from_settings(settings, ctx->obs_source));
}

void source_activate(void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (ctx) ctx->engine.set_obs_visibility(/*in_program=*/true, /*in_preview=*/false);
}

void source_deactivate(void* data) {
    auto* ctx = static_cast<LanVidSourceContext*>(data);
    if (ctx) ctx->engine.set_obs_visibility(/*in_program=*/false, /*in_preview=*/false);
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
