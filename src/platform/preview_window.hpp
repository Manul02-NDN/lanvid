#pragma once
// Cross-platform preview window interface:
//  - Windows: Direct3D 11 flip-model GPU renderer with automatic GDI StretchDIBits fallback
//  - Linux / macOS: optional SDL2 backend (if found by CMake) or clean headless stub
// Hotkeys: Esc/Q = close, F/F11/dblclick = fullscreen, C = toggle conceal, S = save PPM snapshot.

#include <functional>
#include <memory>
#include <string>

#include "common/image.hpp"

namespace lv {

struct PreviewCallbacks {
    std::function<void()> on_close;
    std::function<void()> on_toggle_conceal;
    std::function<void(const Image&)> on_snapshot;
};

class IPreviewWindow {
public:
    virtual ~IPreviewWindow() = default;

    // Opens the preview window; returns false if unavailable on this platform/session.
    virtual bool open(int init_width, int init_height, const std::string& title,
                      PreviewCallbacks cbs = {}) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Presents a frame (BGRA preferred for zero-swizzle upload; RGBA also supported).
    // Thread-safe, non-blocking, coalesces frames if display lags behind stream.
    virtual void present(const Image& img) = 0;

    // Updates window title bar text (e.g. live FPS / latency / Q HUD).
    virtual void set_title(const std::string& title) = 0;

    // Active renderer identifier: "d3d11", "gdi", "sdl2", or "none".
    virtual const char* renderer_name() const = 0;
};

// Factory creating the platform preview window (`force_software=true` selects GDI on Windows).
std::unique_ptr<IPreviewWindow> create_preview_window(bool force_software = false);

}  // namespace lv
