#pragma once
// Backend-agnostic GPU dynamic texture interface for LanVid Studio (D3D11 on Windows, OpenGL3 on Linux).

#include <cstdint>

#include "imgui.h"

namespace lv::gui {

class IGpuTexture {
public:
    virtual ~IGpuTexture() = default;
    // Uploads packed 32-bpp frame (BGRA on D3D11, RGBA on OpenGL3).
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

}  // namespace lv::gui
