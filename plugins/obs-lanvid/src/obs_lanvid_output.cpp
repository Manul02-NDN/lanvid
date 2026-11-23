// obs_lanvid_output.cpp — OBS Studio Program Output ("LanVid Program Output")
// and Dedicated Send Filter ("LanVid Dedicated Send Filter") for 360p-4K60 streaming
// from OBS Studio back into the LanVid network (Director / Operator return feeds).

#include "obs_api_compat.hpp"
#include "obs_lanvid_core.hpp"

#include <algorithm>
#include <vector>

#if defined(LV_HAVE_REAL_LIBOBS)

namespace lv::obs {

namespace {

constexpr const char* kOutDestIp = "dest_ip";
constexpr const char* kOutDestPort = "dest_port";
constexpr const char* kOutCodec = "codec";
constexpr const char* kOutBitrate = "bitrate_kbps";
constexpr const char* kOutQuality = "quality";
constexpr const char* kOutFec = "fec_group";

struct LanVidOutputContext {
    obs_output_t* output = nullptr;
    LanVidObsOutputEngine engine;
    std::vector<int16_t> pcm16_tmp;
};

struct LanVidFilterContext {
    obs_source_t* filter = nullptr;
    LanVidObsOutputEngine engine;
    OutputConfig cfg;
};

OutputConfig output_config_from_settings(obs_data_t* settings, uint32_t w, uint32_t h, uint32_t fps) {
    OutputConfig cfg{};
    const char* ip = obs_data_get_string(settings, kOutDestIp);
    cfg.dest_ip = (ip && ip[0]) ? ip : "127.0.0.1";
    cfg.dest_port = static_cast<uint16_t>(obs_data_get_int(settings, kOutDestPort));
    if (cfg.dest_port == 0) cfg.dest_port = 5010;
    cfg.width = w > 0 ? w : 1920;
    cfg.height = h > 0 ? h : 1080;
    cfg.fps = fps > 0 ? fps : 60;
    cfg.codec = static_cast<int>(obs_data_get_int(settings, kOutCodec));
    cfg.bitrate_kbps = static_cast<int>(obs_data_get_int(settings, kOutBitrate));
    if (cfg.bitrate_kbps < 2000) cfg.bitrate_kbps = 25000;
    cfg.quality = static_cast<int>(obs_data_get_int(settings, kOutQuality));
    if (cfg.quality <= 0) cfg.quality = 82;
    cfg.fec_group = static_cast<int>(obs_data_get_int(settings, kOutFec));
    if (cfg.fec_group <= 0) cfg.fec_group = 6;
    return cfg;
}

obs_properties_t* build_output_properties() {
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, kOutDestIp, obs_module_text("LanVid.Output.DestIp"), OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, kOutDestPort, obs_module_text("LanVid.Output.DestPort"), 1024, 65535, 1);
    obs_property_t* c = obs_properties_add_list(props, kOutCodec, obs_module_text("LanVid.Output.Codec"),
                                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(c, "LanVid Low-Latency DCT", 0);
    obs_property_list_add_int(c, "H.264 (AVC)", 1);
    obs_property_list_add_int(c, "H.265 (HEVC)", 2);
    obs_properties_add_int_slider(props, kOutBitrate, obs_module_text("LanVid.Output.BitrateKbps"), 2000, 150000, 1000);
    obs_properties_add_int_slider(props, kOutQuality, obs_module_text("LanVid.Output.Quality"), 40, 98, 1);
    obs_properties_add_int(props, kOutFec, obs_module_text("LanVid.Output.FecGroup"), 0, 20, 1);
    return props;
}

void set_output_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, kOutDestIp, "127.0.0.1");
    obs_data_set_default_int(settings, kOutDestPort, 5010);
    obs_data_set_default_int(settings, kOutCodec, 0);
    obs_data_set_default_int(settings, kOutBitrate, 25000);
    obs_data_set_default_int(settings, kOutQuality, 82);
    obs_data_set_default_int(settings, kOutFec, 6);
}

// ---- Program Output (obs_output_info) ----
const char* out_get_name(void*) { return obs_module_text("LanVid.Output.Name"); }

void* out_create(obs_data_t* settings, obs_output_t* output) {
    (void)settings;
    auto* ctx = new LanVidOutputContext();
    ctx->output = output;
    return ctx;
}

void out_destroy(void* data) {
    auto* ctx = static_cast<LanVidOutputContext*>(data);
    if (!ctx) return;
    ctx->engine.stop();
    delete ctx;
}

bool out_start(void* data) {
    auto* ctx = static_cast<LanVidOutputContext*>(data);
    if (!ctx || !ctx->output) return false;
    obs_data_t* settings = obs_output_get_settings(ctx->output);
    const uint32_t w = obs_output_get_width(ctx->output);
    const uint32_t h = obs_output_get_height(ctx->output);
    const OutputConfig cfg = output_config_from_settings(settings, w, h, 60);
    obs_data_release(settings);

    if (!ctx->engine.start(cfg)) return false;
    if (!obs_output_begin_data_capture(ctx->output, 0)) {
        ctx->engine.stop();
        return false;
    }
    return true;
}

void out_stop(void* data, uint64_t) {
    auto* ctx = static_cast<LanVidOutputContext*>(data);
    if (!ctx) return;
    obs_output_end_data_capture(ctx->output);
    ctx->engine.stop();
}

void out_raw_video(void* data, struct video_data* frame) {
    auto* ctx = static_cast<LanVidOutputContext*>(data);
    if (!ctx || !frame) return;
    const auto cfg = ctx->engine.config();
    if (frame->data[0] && frame->data[1] && frame->data[2]) {
        ctx->engine.push_i420(frame->data[0], frame->data[1], frame->data[2],
                              static_cast<int>(frame->linesize[0]),
                              static_cast<int>(frame->linesize[1]),
                              static_cast<int>(frame->linesize[2]),
                              cfg.width, cfg.height);
    } else if (frame->data[0]) {
        ctx->engine.push_bgra(frame->data[0], cfg.width, cfg.height, frame->linesize[0]);
    }
}

void out_raw_audio(void* data, struct audio_data* frames) {
    auto* ctx = static_cast<LanVidOutputContext*>(data);
    if (!ctx || !frames || !frames->data[0] || frames->frames == 0) return;
    const float* l = reinterpret_cast<const float*>(frames->data[0]);
    const float* r = frames->data[1] ? reinterpret_cast<const float*>(frames->data[1]) : l;
    ctx->pcm16_tmp.resize(size_t(frames->frames) * 2);
    for (uint32_t i = 0; i < frames->frames; ++i) {
        ctx->pcm16_tmp[i * 2 + 0] = static_cast<int16_t>(std::clamp(l[i], -1.0f, 1.0f) * 32767.0f);
        ctx->pcm16_tmp[i * 2 + 1] = static_cast<int16_t>(std::clamp(r[i], -1.0f, 1.0f) * 32767.0f);
    }
    ctx->engine.push_audio_pcm16(ctx->pcm16_tmp.data(), frames->frames);
}

// ---- Dedicated Send Filter (OBS_SOURCE_TYPE_FILTER) ----
const char* filter_get_name(void*) { return obs_module_text("LanVid.Filter.Name"); }

void* filter_create(obs_data_t* settings, obs_source_t* filter) {
    auto* ctx = new LanVidFilterContext();
    ctx->filter = filter;
    ctx->cfg = output_config_from_settings(settings, 1920, 1080, 60);
    ctx->engine.start(ctx->cfg);
    return ctx;
}

void filter_destroy(void* data) {
    auto* ctx = static_cast<LanVidFilterContext*>(data);
    if (!ctx) return;
    ctx->engine.stop();
    delete ctx;
}

void filter_update(void* data, obs_data_t* settings) {
    auto* ctx = static_cast<LanVidFilterContext*>(data);
    if (!ctx) return;
    ctx->cfg = output_config_from_settings(settings, ctx->cfg.width, ctx->cfg.height, ctx->cfg.fps);
    ctx->engine.start(ctx->cfg);
}

struct obs_source_frame* filter_video(void* data, struct obs_source_frame* frame) {
    auto* ctx = static_cast<LanVidFilterContext*>(data);
    if (ctx && frame && frame->data[0]) {
        if (frame->format == VIDEO_FORMAT_I420 && frame->data[1] && frame->data[2]) {
            ctx->engine.push_i420(frame->data[0], frame->data[1], frame->data[2],
                                  static_cast<int>(frame->linesize[0]),
                                  static_cast<int>(frame->linesize[1]),
                                  static_cast<int>(frame->linesize[2]),
                                  frame->width, frame->height);
        } else if (frame->format == VIDEO_FORMAT_BGRA) {
            ctx->engine.push_bgra(frame->data[0], frame->width, frame->height, frame->linesize[0]);
        }
    }
    return frame;
}

}  // namespace

void register_lanvid_output_and_filter() {
    obs_output_info oinfo{};
    oinfo.id = "lanvid_program_output";
    oinfo.flags = OBS_OUTPUT_AV;
    oinfo.get_name = out_get_name;
    oinfo.create = out_create;
    oinfo.destroy = out_destroy;
    oinfo.start = out_start;
    oinfo.stop = out_stop;
    oinfo.raw_video = out_raw_video;
    oinfo.raw_audio = out_raw_audio;
    oinfo.get_defaults = set_output_defaults;
    oinfo.get_properties = [](void*) { return build_output_properties(); };
    obs_register_output(&oinfo);

    obs_source_info finfo{};
    finfo.id = "lanvid_dedicated_send_filter";
    finfo.type = OBS_SOURCE_TYPE_FILTER;
    finfo.output_flags = OBS_SOURCE_ASYNC_VIDEO;
    finfo.get_name = filter_get_name;
    finfo.create = filter_create;
    finfo.destroy = filter_destroy;
    finfo.get_defaults = set_output_defaults;
    finfo.get_properties = [](void*) { return build_output_properties(); };
    finfo.update = filter_update;
    finfo.filter_video = filter_video;
    obs_register_source(&finfo);
}

}  // namespace lv::obs

#endif  // LV_HAVE_REAL_LIBOBS
