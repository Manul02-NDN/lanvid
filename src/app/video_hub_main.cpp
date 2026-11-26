// LanVid Video Hub — Standalone Desktop Entrypoint (Multi-Camera Hub, Playout Buffer, ISO & OBS).

#include <cstdlib>
#include <cstring>

#include "gui/studio_common.hpp"
#include "gui/video_hub_ui.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#ifndef LANVID_VERSION_STRING
#define LANVID_VERSION_STRING "2.7.0"
#endif

int main(int argc, char** argv) {
    int smoke_ms = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test-ms") == 0 && i + 1 < argc) {
            smoke_ms = std::atoi(argv[++i]);
        }
    }

    lv::gui::LanVidVideoHubApp app;
    lv::gui::StudioWindowSpec spec{};
    spec.window_title = "LanVid Video Hub v" LANVID_VERSION_STRING " — Multi-Cam, ISO, Tally & Intercom";
    spec.window_class = "LanVidVideoHubWindowClass";
    spec.width = 1320;
    spec.height = 820;
    spec.smoke_test_ms = smoke_ms;
    return lv::gui::run_studio_desktop_app(spec, app);
}

#if defined(_MSC_VER) && defined(LANVID_GUI_WINMAIN)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
