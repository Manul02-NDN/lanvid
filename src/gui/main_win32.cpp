// LanVid Studio — Windows Entrypoint (Dear ImGui + Win32 + Direct3D 11, statically linked, zero external DLLs).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <tchar.h>
#include <windows.h>

#include "common/clock.hpp"
#include "gui/gpu_texture.hpp"
#include "gui/lanvid_studio.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

struct D3D11Host {
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    bool allow_tearing = false;
    bool warp_fallback = false;
    bool fullscreen = false;
    WINDOWPLACEMENT saved_wp{sizeof(WINDOWPLACEMENT)};
    UINT resize_w = 0, resize_h = 0;

    bool create_rtv() {
        if (rtv) {
            rtv->Release();
            rtv = nullptr;
        }
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) {
            return false;
        }
        const HRESULT hr = dev->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();
        return SUCCEEDED(hr);
    }

    bool init(HWND h) {
        hwnd = h;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                                       &dev, &got, &ctx);
        if (FAILED(hr) || !dev) {
            // Automatic WARP software D3D11 fallback for headless/RDP/VM hosts without GPU!
            warp_fallback = true;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                                   &dev, &got, &ctx);
            if (FAILED(hr) || !dev) return false;
        }

        IDXGIDevice* ddev = nullptr;
        IDXGIAdapter* adap = nullptr;
        IDXGIFactory2* fac2 = nullptr;
        hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&ddev));
        if (SUCCEEDED(hr)) hr = ddev->GetAdapter(&adap);
        if (SUCCEEDED(hr)) hr = adap->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&fac2));
        if (adap) adap->Release();
        if (ddev) ddev->Release();
        if (FAILED(hr) || !fac2) return false;

        IDXGIFactory5* fac5 = nullptr;
        if (SUCCEEDED(fac2->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&fac5)))) {
            BOOL t = FALSE;
            if (SUCCEEDED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t)))) {
                allow_tearing = (t == TRUE);
            }
            fac5->Release();
        }

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd.Flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        hr = fac2->CreateSwapChainForHwnd(dev, h, &scd, nullptr, nullptr, &swap);
        if (FAILED(hr) && allow_tearing) {
            allow_tearing = false;
            scd.Flags = 0;
            hr = fac2->CreateSwapChainForHwnd(dev, h, &scd, nullptr, nullptr, &swap);
        }
        fac2->MakeWindowAssociation(h, DXGI_MWA_NO_ALT_ENTER);
        fac2->Release();
        if (FAILED(hr) || !swap) return false;
        return create_rtv();
    }

    void cleanup() {
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (swap) { swap->Release(); swap = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
    }
};

class D3D11DynamicTexture final : public lv::gui::IGpuTexture {
public:
    explicit D3D11DynamicTexture(D3D11Host* host) : host_(host) {}
    ~D3D11DynamicTexture() override { release(); }

    bool update(const uint8_t* pixels, int width, int height) override {
        if (!host_ || !host_->dev || !host_->ctx || !pixels || width <= 0 || height <= 0) return false;
        if (!tex_ || w_ != width || h_ != height) {
            release();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = UINT(width);
            td.Height = UINT(height);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(host_->dev->CreateTexture2D(&td, nullptr, &tex_)) || !tex_) return false;
            if (FAILED(host_->dev->CreateShaderResourceView(tex_, nullptr, &srv_)) || !srv_) {
                release();
                return false;
            }
            w_ = width;
            h_ = height;
        }
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(host_->ctx->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
        const size_t row_bytes = size_t(width) * 4;
        if (map.RowPitch == row_bytes) {
            std::memcpy(map.pData, pixels, row_bytes * size_t(height));
        } else {
            uint8_t* dst = static_cast<uint8_t*>(map.pData);
            for (int y = 0; y < height; ++y) {
                std::memcpy(dst + size_t(y) * map.RowPitch, pixels + size_t(y) * row_bytes, row_bytes);
            }
        }
        host_->ctx->Unmap(tex_, 0);
        return true;
    }

    ImTextureID imgui_id() const override { return reinterpret_cast<ImTextureID>(srv_); }
    int width() const override { return w_; }
    int height() const override { return h_; }
    bool wants_rgba() const override { return false; }  // zero-copy BGRA on Direct3D 11!

private:
    void release() {
        if (srv_) { srv_->Release(); srv_ = nullptr; }
        if (tex_) { tex_->Release(); tex_ = nullptr; }
        w_ = h_ = 0;
    }

    D3D11Host* host_ = nullptr;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    int w_ = 0, h_ = 0;
};

D3D11Host g_host;

bool host_is_fs(void* ctx) { return static_cast<D3D11Host*>(ctx)->fullscreen; }

void host_set_fs(void* ctx, bool fs) {
    auto* h = static_cast<D3D11Host*>(ctx);
    if (h->fullscreen == fs || !h->hwnd) return;
    DWORD style = DWORD(GetWindowLongW(h->hwnd, GWL_STYLE));
    if (fs) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(h->hwnd, &h->saved_wp) &&
            GetMonitorInfoW(MonitorFromWindow(h->hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(h->hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(h->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            h->fullscreen = true;
        }
    } else {
        SetWindowLongW(h->hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h->hwnd, &h->saved_wp);
        SetWindowPos(h->hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        h->fullscreen = false;
    }
}

lv::gui::IGpuTexture* host_create_tex(void* ctx) {
    return new D3D11DynamicTexture(static_cast<D3D11Host*>(ctx));
}
void host_destroy_tex(void*, lv::gui::IGpuTexture* tex) { delete tex; }

LRESULT WINAPI wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_host.resize_w = LOWORD(lParam);
                g_host.resize_h = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void load_cyrillic_font(ImGuiIO& io) {
    const char* candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) {
            io.Fonts->AddFontFromFileTTF(p, 17.5f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

}  // namespace

int main(int argc, char** argv) {
    int smoke_ms = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test-ms") == 0 && i + 1 < argc) {
            smoke_ms = std::atoi(argv[++i]);
        }
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"LanVidStudioWindowClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"LanVid Studio v2.4.0 — Ultra-Low Latency LAN Video & Audio",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 780, nullptr,
                                nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    if (!g_host.init(hwnd)) {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 2;
    }

    ShowWindow(hwnd, smoke_ms > 0 ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // settings persisted via %APPDATA%\LanVid\config.json
    load_cyrillic_font(io);
    lv::gui::LanVidStudioApp::apply_theme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_host.dev, g_host.ctx);

    lv::gui::PlatformHost ph{};
    ph.renderer_name = g_host.warp_fallback ? "D3D11 (WARP)" : "Direct3D 11";
    ph.is_fullscreen = host_is_fs;
    ph.set_fullscreen = host_set_fs;
    ph.create_texture = host_create_tex;
    ph.destroy_texture = host_destroy_tex;
    ph.ctx = &g_host;

    lv::gui::LanVidStudioApp app(ph);
    app.init();

    const uint64_t t_start = lv::now_us();
    uint64_t rendered_frames = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (smoke_ms > 0 && lv::now_us() - t_start >= uint64_t(smoke_ms) * 1000ull) break;

        if (g_host.resize_w != 0 && g_host.resize_h != 0) {
            if (g_host.rtv) { g_host.rtv->Release(); g_host.rtv = nullptr; }
            const UINT flags = g_host.allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            g_host.swap->ResizeBuffers(0, g_host.resize_w, g_host.resize_h, DXGI_FORMAT_UNKNOWN, flags);
            g_host.resize_w = g_host.resize_h = 0;
            g_host.create_rtv();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.render_frame();

        ImGui::Render();
        const float clear_col[4] = {0.07f, 0.08f, 0.11f, 1.0f};
        g_host.ctx->OMSetRenderTargets(1, &g_host.rtv, nullptr);
        g_host.ctx->ClearRenderTargetView(g_host.rtv, clear_col);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const UINT sync = g_host.fullscreen ? 0u : 1u;  // zero-VSync in fullscreen for lowest latency!
        const UINT pflags = (sync == 0u && g_host.allow_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        g_host.swap->Present(sync, pflags);
        ++rendered_frames;
    }

    app.shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_host.cleanup();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (smoke_ms > 0) {
        std::printf("LanVid Studio Win32/D3D11 smoke test OK: renderer=%s frames=%llu config=%s\n",
                    ph.renderer_name, static_cast<unsigned long long>(rendered_frames),
                    lv::gui::config_file_path().c_str());
    }
    return 0;
}

#if defined(_MSC_VER) && defined(LANVID_GUI_WINMAIN)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
