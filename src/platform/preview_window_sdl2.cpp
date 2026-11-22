// Optional SDL2 Preview Window backend for Linux / macOS (compiled when SDL2 is found by CMake).
// Implements the exact same IPreviewWindow interface, aspect-preserving letterbox, live HUD title,
// and hotkeys (Esc/Q, F/F11/dblclick, C, S) as Win32 D3D11 preview.

#include "platform/preview_window.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

namespace lv {

namespace {

class Sdl2PreviewWindow final : public IPreviewWindow {
public:
    ~Sdl2PreviewWindow() override { close(); }

    bool open(int init_width, int init_height, const std::string& title, PreviewCallbacks cbs) override {
        close();
        cbs_ = std::move(cbs);
        stop_.store(false, std::memory_order_relaxed);

        std::promise<bool> ready;
        auto fut = ready.get_future();

        ui_thread_ = std::thread([this, init_width, init_height, title, p = std::move(ready)]() mutable {
            if (SDL_Init(SDL_INIT_VIDEO) != 0) {
                p.set_value(false);
                return;
            }
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");  // bilinear GPU scaling

            SDL_Window* win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                               std::max(320, init_width), std::max(180, init_height),
                                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!win) {
                SDL_Quit();
                p.set_value(false);
                return;
            }

            // Accelerated renderer without VSync for lowest display latency.
            SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
            if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
            if (!ren) {
                SDL_DestroyWindow(win);
                SDL_Quit();
                p.set_value(false);
                return;
            }

            SDL_Texture* tex = nullptr;
            int tex_w = 0, tex_h = 0;
            bool fullscreen = false;

            open_flag_.store(true, std::memory_order_release);
            p.set_value(true);

            while (!stop_.load(std::memory_order_relaxed)) {
                SDL_Event ev;
                bool need_redraw = false;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT) {
                        stop_.store(true, std::memory_order_relaxed);
                        if (cbs_.on_close) cbs_.on_close();
                    } else if (ev.type == SDL_WINDOWEVENT &&
                               (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                                ev.window.event == SDL_WINDOWEVENT_EXPOSED)) {
                        need_redraw = true;
                    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.clicks == 2) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        need_redraw = true;
                    } else if (ev.type == SDL_KEYDOWN) {
                        const SDL_Keycode k = ev.key.keysym.sym;
                        if (k == SDLK_ESCAPE || k == SDLK_q) {
                            stop_.store(true, std::memory_order_relaxed);
                            if (cbs_.on_close) cbs_.on_close();
                        } else if (k == SDLK_f || k == SDLK_F11) {
                            fullscreen = !fullscreen;
                            SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            need_redraw = true;
                        } else if (k == SDLK_c) {
                            if (cbs_.on_toggle_conceal) cbs_.on_toggle_conceal();
                        } else if (k == SDLK_s) {
                            if (cbs_.on_snapshot && front_.width > 0) cbs_.on_snapshot(front_);
                        }
                    }
                }

                // Check pending title / frame updates
                bool got_frame = false;
                std::string new_title;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (has_new_title_) {
                        new_title = pending_title_;
                        has_new_title_ = false;
                    }
                    if (has_new_frame_) {
                        std::swap(front_, staging_);
                        has_new_frame_ = false;
                        got_frame = true;
                    }
                }
                if (!new_title.empty()) SDL_SetWindowTitle(win, new_title.c_str());

                if (got_frame && front_.width > 0 && front_.height > 0) {
                    if (!tex || tex_w != front_.width || tex_h != front_.height) {
                        if (tex) SDL_DestroyTexture(tex);
                        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                front_.width, front_.height);
                        tex_w = front_.width;
                        tex_h = front_.height;
                    }
                    if (tex) SDL_UpdateTexture(tex, nullptr, front_.data.data(), int(front_.stride));
                    need_redraw = true;
                }

                if (need_redraw) {
                    int out_w = 0, out_h = 0;
                    SDL_GetRendererOutputSize(ren, &out_w, &out_h);
                    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                    SDL_RenderClear(ren);
                    if (tex && tex_w > 0 && tex_h > 0 && out_w > 0 && out_h > 0) {
                        const double src_ar = double(tex_w) / double(tex_h);
                        const double dst_ar = double(out_w) / double(out_h);
                        SDL_Rect dst{0, 0, out_w, out_h};
                        if (dst_ar > src_ar) {
                            dst.w = std::max(1, int(double(out_h) * src_ar + 0.5));
                            dst.x = (out_w - dst.w) / 2;
                        } else {
                            dst.h = std::max(1, int(double(out_w) / src_ar + 0.5));
                            dst.y = (out_h - dst.h) / 2;
                        }
                        SDL_RenderCopy(ren, tex, nullptr, &dst);
                    }
                    SDL_RenderPresent(ren);
                } else {
                    SDL_Delay(1);
                }
            }

            if (tex) SDL_DestroyTexture(tex);
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
            open_flag_.store(false, std::memory_order_release);
        });

        const bool ok = fut.get();
        if (!ok && ui_thread_.joinable()) ui_thread_.join();
        return ok;
    }

    void close() override {
        stop_.store(true, std::memory_order_release);
        if (ui_thread_.joinable()) ui_thread_.join();
        open_flag_.store(false, std::memory_order_release);
    }

    bool is_open() const override { return open_flag_.load(std::memory_order_acquire); }
    const char* renderer_name() const override { return "sdl2"; }

    void present(const Image& img) override {
        if (!is_open() || img.width <= 0 || img.height <= 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (staging_.width != img.width || staging_.height != img.height) {
            staging_.allocate(img.width, img.height, PixelOrder::kBGRA);
        }
        staging_.order = PixelOrder::kBGRA;
        if (img.order == PixelOrder::kBGRA) {
            std::memcpy(staging_.data.data(), img.data.data(), img.data.size());
        } else {
            const size_t pixels = size_t(img.width) * size_t(img.height);
            const uint8_t* s = img.data.data();
            uint8_t* d = staging_.data.data();
            for (size_t i = 0; i < pixels; ++i) {
                d[i * 4 + 0] = s[i * 4 + 2];
                d[i * 4 + 1] = s[i * 4 + 1];
                d[i * 4 + 2] = s[i * 4 + 0];
                d[i * 4 + 3] = 255;
            }
        }
        has_new_frame_ = true;
    }

    void set_title(const std::string& title) override {
        if (!is_open()) return;
        std::lock_guard<std::mutex> lk(mu_);
        pending_title_ = title;
        has_new_title_ = true;
    }

private:
    std::thread ui_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> open_flag_{false};
    PreviewCallbacks cbs_;
    std::mutex mu_;
    Image staging_;
    Image front_;
    bool has_new_frame_ = false;
    std::string pending_title_;
    bool has_new_title_ = false;
};

}  // namespace

std::unique_ptr<IPreviewWindow> create_preview_window(bool) {
    return std::make_unique<Sdl2PreviewWindow>();
}

}  // namespace lv
