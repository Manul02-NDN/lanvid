#pragma once
// Thin OS window & GPU texture host contract for LanVid desktop apps (Zero business logic).
// Backends:
//   * macOS   : Cocoa + Metal (host_macos_metal.mm)
//   * Windows : Win32 + Direct3D 11 (host_win32_d3d11.cpp)
//   * Linux   : SDL2 + OpenGL 3 (host_linux_sdl2.cpp)

#include <cstdint>
#include <string>

#include "imgui.h"

namespace lv::ui {

class IGpuTexture {
public:
    virtual ~IGpuTexture() = default;
    virtual bool update(const uint8_t* pixels, int width, int height) = 0;
    virtual ImTextureID imgui_id() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool wants_rgba() const = 0;
};

struct PlatformHost {
    const char* renderer_name = "unknown";
    bool (*is_fullscreen)(void* ctx) = nullptr;
    void (*set_fullscreen)(void* ctx, bool fs) = nullptr;
    IGpuTexture* (*create_texture)(void* ctx) = nullptr;
    void (*destroy_texture)(void* ctx, IGpuTexture* tex) = nullptr;
    void* ctx = nullptr;
};

class IDesktopApp {
public:
    virtual ~IDesktopApp() = default;
    virtual void set_platform_host(const PlatformHost& host) = 0;
    virtual void init() = 0;
    virtual void shutdown() = 0;
    virtual void render_frame() = 0;
    virtual const char* app_display_name() const = 0;
    virtual std::string active_config_path() const = 0;
};

struct WindowSpec {
    const char* window_title = "LanVid";
    const char* window_class = "LanVidWindowClass";
    int width = 1280;
    int height = 800;
    int smoke_test_ms = 0;
};

int run_desktop_app(const WindowSpec& spec, IDesktopApp& app);

}  // namespace lv::ui
