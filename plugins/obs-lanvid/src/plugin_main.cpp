// plugin_main.cpp — OBS Studio Module Entry Point for obs-lanvid
// Registers LanVid Source, LanVid Program Output, LanVid Dedicated Send Filter,
// Auto-Tally OBS Frontend event callbacks, and Qt6 Control & Tally Dock.

#include "obs_api_compat.hpp"
#include "obs_lanvid_core.hpp"
#include "obs_lanvid_dock.hpp"

#if defined(LV_HAVE_REAL_LIBOBS)

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-lanvid", "en-US")

namespace lv::obs {
void register_lanvid_source();
void register_lanvid_output_and_filter();

namespace {

void on_obs_frontend_event(enum obs_frontend_event event, void*) {
    auto& mgr = LanVidObsManager::instance();
    const bool studio = obs_frontend_preview_program_mode_active();
    const bool streaming = obs_frontend_streaming_active();
    const bool recording = obs_frontend_recording_active();

    switch (event) {
        case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
        case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
        case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
        case OBS_FRONTEND_EVENT_STREAMING_STARTED:
        case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
        case OBS_FRONTEND_EVENT_RECORDING_STARTED:
        case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
            mgr.set_obs_global_state(studio, streaming, recording);
            break;
        default:
            break;
    }
}

}  // namespace
}  // namespace lv::obs

bool obs_module_load(void) {
    lv::obs::LanVidObsManager::instance().ensure_studio_started();
    lv::obs::register_lanvid_source();
    lv::obs::register_lanvid_output_and_filter();
    obs_frontend_add_event_callback(lv::obs::on_obs_frontend_event, nullptr);
    lv::obs::register_lanvid_control_dock();
    blog(LOG_INFO, "[obs-lanvid] Loaded LanVid Studio Plugin v2.6.0 (Tally UDP :%u)",
         unsigned(lv::obs::LanVidObsManager::instance().studio_port()));
    return true;
}

void obs_module_unload(void) {
    obs_frontend_remove_event_callback(lv::obs::on_obs_frontend_event, nullptr);
    lv::obs::unregister_lanvid_control_dock();
    lv::obs::LanVidObsManager::instance().shutdown();
    blog(LOG_INFO, "[obs-lanvid] Unloaded LanVid Studio Plugin");
}

const char* obs_module_name(void) {
    return "LanVid Studio Plugin (Camera, AoIP 64ch, Auto-Tally & Intercom)";
}

const char* obs_module_description(void) {
    return "Ultra-low latency 360p-4K60 video, 64-channel AoIP audio, Studio Auto-Tally, and Duplex Intercom for OBS Studio.";
}

#endif  // LV_HAVE_REAL_LIBOBS
