// plugin_main.cpp — OBS Studio Module Entry Point for obs-lanvid
// Registers Zero-Crash LanVid Camera Source, Program Output, Dedicated Send Filter,
// Frontend Auto-Tally lifecycle hooks, and Qt6 LanVid Studio Control Dock.

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
    auto& rt = LanVidObsRuntime::instance();
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        rt.shutdown();
        return;
    }

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
            rt.set_obs_global_state(studio, streaming, recording);
            break;
        default:
            break;
    }
}

}  // namespace
}  // namespace lv::obs

bool obs_module_load(void) {
    lv::obs::LanVidObsRuntime::instance().startup();
    lv::obs::register_lanvid_source();
    lv::obs::register_lanvid_output_and_filter();
    obs_frontend_add_event_callback(lv::obs::on_obs_frontend_event, nullptr);
    lv::obs::register_lanvid_control_dock();
    blog(LOG_INFO, "[obs-lanvid] Loaded LanVid Studio Plugin v2.7.0 (Tally UDP :%u%s)",
         unsigned(lv::obs::LanVidObsRuntime::instance().studio_port()),
         lv::obs::LanVidObsRuntime::instance().tally_port_fallback_active() ? ", shared fallback" : "");
    return true;
}

void obs_module_unload(void) {
    obs_frontend_remove_event_callback(lv::obs::on_obs_frontend_event, nullptr);
    lv::obs::unregister_lanvid_control_dock();
    lv::obs::LanVidObsRuntime::instance().shutdown();
    blog(LOG_INFO, "[obs-lanvid] Unloaded LanVid Studio Plugin");
}

const char* obs_module_name(void) {
    return "LanVid Studio Plugin (Multi-Cam, Playout Buffer, Auto-Tally & Intercom)";
}

const char* obs_module_description(void) {
    return "Zero-crash 360p-4K60 LAN & SHM camera receiver with VideoPlayoutBuffer, 64ch AoIP, camera badge sync, Auto-Tally, and Duplex Intercom for OBS Studio.";
}

#endif  // LV_HAVE_REAL_LIBOBS
