#include <cstdlib>
#include <cstring>

#include "common_ui/app_host.hpp"
#include "video_app.hpp"

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

    lv::apps::LanVidVideoApp app;
    lv::ui::WindowSpec spec{};
    spec.window_title = "LanVid Video v" LANVID_VERSION_STRING " — Multi-Cam Hub, ISO & Virtual Cams";
    spec.window_class = "LanVidVideoWindowClass";
    spec.width = 1320;
    spec.height = 820;
    spec.smoke_test_ms = smoke_ms;
    return lv::ui::run_desktop_app(spec, app);
}

#if defined(_MSC_VER) && defined(LANVID_GUI_WINMAIN)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
