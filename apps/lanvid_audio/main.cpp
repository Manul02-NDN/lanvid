#include <cstdlib>
#include <cstring>

#include "audio_app.hpp"
#include "common_ui/app_host.hpp"

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

    lv::apps::LanVidAudioApp app;
    lv::ui::WindowSpec spec{};
    spec.window_title = "LanVid Audio v" LANVID_VERSION_STRING " — AoIP, Patchbay 64x64, Monitor & Grid L2";
    spec.window_class = "LanVidAudioWindowClass";
    spec.width = 1280;
    spec.height = 780;
    spec.smoke_test_ms = smoke_ms;
    return lv::ui::run_desktop_app(spec, app);
}

#if defined(_MSC_VER) && defined(LANVID_GUI_WINMAIN)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
