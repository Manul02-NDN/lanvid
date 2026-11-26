// LanVid Video — Standalone Desktop Entrypoint (Multi-Camera, Tally, Director Intercom/IFB & OBS Output).

#include <cstdlib>
#include <cstring>

#include "gui/studio_common.hpp"
#include "gui/video_studio_ui.hpp"

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

    lv::gui::LanVidVideoStudioApp app;
    lv::gui::StudioWindowSpec spec{};
    spec.window_title = "LanVid Video v" LANVID_VERSION_STRING " — Cameras, Tally, Intercom & OBS";
    spec.window_class = "LanVidVideoStudioWindowClass";
    spec.width = 1280;
    spec.height = 780;
    spec.smoke_test_ms = smoke_ms;
    return lv::gui::run_studio_desktop_app(spec, app);
}

#if defined(_MSC_VER) && defined(LANVID_GUI_WINMAIN)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
