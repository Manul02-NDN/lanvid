// Linux SDL2 + OpenGL 3 window host for LanVid apps.
#include "common_ui/app_host.hpp"
#include "common_ui/theme.hpp"

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <filesystem>

#include "common/clock.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

namespace lv::ui {

namespace {

struct SdlGlHost {
    SDL_Window* win = nullptr;
    SDL_GLContext gl_ctx = nullptr;
    bool fullscreen = false;
};

class GlDynamicTexture final : public IGpuTexture {
public:
    ~GlDynamicTexture() override { release(); }

    bool update(const uint8_t* pixels, int width, int height) override {
        if (!pixels || width <= 0 || height <= 0) return false;
        if (!tex_) glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (w_ != width || h_ != height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            w_ = width;
            h_ = height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        return true;
    }

    ImTextureID imgui_id() const override { return static_cast<ImTextureID>(static_cast<intptr_t>(tex_)); }
    int width() const override { return w_; }
    int height() const override { return h_; }
    bool wants_rgba() const override { return true; }

private:
    void release() {
        if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
        w_ = h_ = 0;
    }
    GLuint tex_ = 0;
    int w_ = 0, h_ = 0;
};

bool host_is_fs(void* ctx) { return static_cast<SdlGlHost*>(ctx)->fullscreen; }
void host_set_fs(void* ctx, bool fs) {
    auto* h = static_cast<SdlGlHost*>(ctx);
    if (!h->win || h->fullscreen == fs) return;
    SDL_SetWindowFullscreen(h->win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    h->fullscreen = fs;
    SDL_GL_SetSwapInterval(fs ? 0 : 1);
}
IGpuTexture* host_create_tex(void*) { return new GlDynamicTexture(); }
void host_destroy_tex(void*, IGpuTexture* tex) { delete tex; }

void load_fonts(ImGuiIO& io) {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        static const ImWchar extra[] = {0x2010, 0x2027, 0x2030, 0x203A, 0x2190, 0x21FF, 0x2212, 0x2212,
                                        0x2500, 0x25FF, 0x2600, 0x26FF, 0x2713, 0x2717, 0};
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        b.AddRanges(extra);
        b.BuildRanges(&ranges);
    }
    const char* candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) {
            io.Fonts->AddFontFromFileTTF(p, 16.5f, nullptr, ranges.Data);
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

}  // namespace

int run_desktop_app(const WindowSpec& spec, IDesktopApp& app) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SdlGlHost host;
    host.win = SDL_CreateWindow(spec.window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                spec.width, spec.height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!host.win) {
        SDL_Quit();
        return 2;
    }
    host.gl_ctx = SDL_GL_CreateContext(host.win);
    SDL_GL_MakeCurrent(host.win, host.gl_ctx);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    load_fonts(io);
    apply_theme(false);

    ImGui_ImplSDL2_InitForOpenGL(host.win, host.gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    PlatformHost ph{};
    ph.renderer_name = "SDL2 / OpenGL 3";
    ph.is_fullscreen = host_is_fs;
    ph.set_fullscreen = host_set_fs;
    ph.create_texture = host_create_tex;
    ph.destroy_texture = host_destroy_tex;
    ph.ctx = &host;

    app.set_platform_host(ph);
    app.init();

    const uint64_t t0 = lv::now_us();
    bool done = false;
    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) done = true;
        }
        if (spec.smoke_test_ms > 0 && lv::now_us() - t0 >= uint64_t(spec.smoke_test_ms) * 1000ull) break;
        if (SDL_GetWindowFlags(host.win) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        app.render_frame();
        ImGui::Render();
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(host.win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        const auto& pal = theme_palette();
        glClearColor(pal.bg_bot_right.x, pal.bg_bot_right.y, pal.bg_bot_right.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(host.win);
    }

    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(host.gl_ctx);
    SDL_DestroyWindow(host.win);
    SDL_Quit();
    return 0;
}

}  // namespace lv::ui
