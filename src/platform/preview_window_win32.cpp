// Windows Preview Window:
//  - Primary: Direct3D 11 hardware flip-model SwapChain (DXGI_SWAP_EFFECT_FLIP_DISCARD,
//    DXGI_PRESENT_ALLOW_TEARING when supported), dynamic BGRA texture + fullscreen-triangle HLSL
//    shader with bilinear GPU scaling and aspect-preserving D3D11_VIEWPORT letterboxing.
//  - Automatic fallback: Win32 GDI StretchDIBits when D3D11 initialization fails or is disabled.

#include "platform/preview_window.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <windows.h>

namespace lv {

namespace {

constexpr UINT kWmAppFrame = WM_APP + 1;
constexpr UINT kWmAppTitle = WM_APP + 2;
constexpr UINT kWmAppClose = WM_APP + 3;
constexpr wchar_t kWndClassName[] = L"LvspPreviewWindowClass";

constexpr const char* kHlslSource = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main_vs(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

float4 main_ps(VSOut i) : SV_Target {
    return float4(tex.Sample(samp, i.uv).rgb, 1.0);
}
)";

template <class T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

void enable_dpi_awareness() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            using SetDpiFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            auto fn = reinterpret_cast<SetDpiFn>(GetProcAddress(u32, "SetProcessDpiAwarenessContext"));
            if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    });
}

class Win32PreviewWindow final : public IPreviewWindow {
public:
    explicit Win32PreviewWindow(bool force_gdi) : force_gdi_(force_gdi) {}
    ~Win32PreviewWindow() override { close(); }

    bool open(int init_width, int init_height, const std::string& title, PreviewCallbacks cbs) override {
        close();
        enable_dpi_awareness();
        cbs_ = std::move(cbs);

        std::promise<bool> ready;
        auto fut = ready.get_future();

        ui_thread_ = std::thread([this, init_width, init_height, title, p = std::move(ready)]() mutable {
            HINSTANCE hinst = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc = &Win32PreviewWindow::wnd_proc;
            wc.hInstance = hinst;
            wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
            wc.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = kWndClassName;
            RegisterClassExW(&wc);

            const int cw = std::max(320, init_width);
            const int ch = std::max(180, init_height);
            RECT wr{0, 0, cw, ch};
            AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
            const std::wstring wt = utf8_to_wide(title);

            HWND h = CreateWindowExW(0, kWndClassName, wt.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                     CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr,
                                     hinst, this);
            if (!h) {
                p.set_value(false);
                return;
            }

            hwnd_ = h;
            if (!force_gdi_) {
                using_d3d11_ = init_d3d11(h, cw, ch);
            } else {
                using_d3d11_ = false;
            }
            renderer_name_.store(using_d3d11_ ? "d3d11" : "gdi", std::memory_order_release);

            open_flag_.store(true, std::memory_order_release);
            ShowWindow(h, SW_SHOW);
            UpdateWindow(h);
            p.set_value(true);

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            release_d3d11();
            open_flag_.store(false, std::memory_order_release);
        });

        const bool ok = fut.get();
        if (!ok && ui_thread_.joinable()) ui_thread_.join();
        return ok;
    }

    void close() override {
        if (open_flag_.load(std::memory_order_acquire) && hwnd_) {
            PostMessageW(hwnd_, kWmAppClose, 0, 0);
        }
        if (ui_thread_.joinable()) ui_thread_.join();
        open_flag_.store(false, std::memory_order_release);
    }

    bool is_open() const override { return open_flag_.load(std::memory_order_acquire); }

    const char* renderer_name() const override { return renderer_name_.load(std::memory_order_acquire); }

    void present(const Image& img) override {
        if (!is_open() || img.width <= 0 || img.height <= 0) return;

        bool should_post = false;
        {
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
            if (!frame_msg_pending_) {
                frame_msg_pending_ = true;
                should_post = true;
            }
        }
        if (should_post && hwnd_) {
            PostMessageW(hwnd_, kWmAppFrame, 0, 0);
        }
    }

    void set_title(const std::string& title) override {
        if (!is_open()) return;
        bool should_post = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_title_ = title;
            if (!title_msg_pending_) {
                title_msg_pending_ = true;
                should_post = true;
            }
        }
        if (should_post && hwnd_) {
            PostMessageW(hwnd_, kWmAppTitle, 0, 0);
        }
    }

private:
    // --------------------------------------------------------- D3D11 backend
    bool init_d3d11(HWND h, int w, int hgt) {
        release_d3d11();
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL got_fl{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                                       &dev_, &got_fl, &ctx_);
        if (FAILED(hr) || !dev_ || !ctx_) {
            release_d3d11();
            return false;
        }

        IDXGIDevice* dxgi_dev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory2 = nullptr;
        hr = dev_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev));
        if (SUCCEEDED(hr)) hr = dxgi_dev->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2));
        safe_release(adapter);
        safe_release(dxgi_dev);
        if (FAILED(hr) || !factory2) {
            safe_release(factory2);
            release_d3d11();
            return false;
        }

        allow_tearing_ = false;
        IDXGIFactory5* factory5 = nullptr;
        if (SUCCEEDED(factory2->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5)))) {
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                        sizeof(tearing)))) {
                allow_tearing_ = (tearing == TRUE);
            }
            factory5->Release();
        }

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = UINT(std::max(1, w));
        scd.Height = UINT(std::max(1, hgt));
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd.Flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        hr = factory2->CreateSwapChainForHwnd(dev_, h, &scd, nullptr, nullptr, &swap_);
        if (FAILED(hr) && allow_tearing_) {
            // Retry flip-discard without tearing flag if driver/RDP rejects tearing.
            allow_tearing_ = false;
            scd.Flags = 0;
            hr = factory2->CreateSwapChainForHwnd(dev_, h, &scd, nullptr, nullptr, &swap_);
        }
        factory2->MakeWindowAssociation(h, DXGI_MWA_NO_ALT_ENTER);
        factory2->Release();
        if (FAILED(hr) || !swap_) {
            release_d3d11();
            return false;
        }

        bb_w_ = int(scd.Width);
        bb_h_ = int(scd.Height);
        if (!create_rtv()) {
            release_d3d11();
            return false;
        }

        // Compile fullscreen-triangle VS + bilinear PS.
        ID3DBlob* vs_blob = nullptr;
        ID3DBlob* ps_blob = nullptr;
        const size_t hlsl_len = std::strlen(kHlslSource);
        hr = D3DCompile(kHlslSource, hlsl_len, "lvsp_preview.hlsl", nullptr, nullptr, "main_vs", "vs_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, nullptr);
        if (SUCCEEDED(hr)) {
            hr = D3DCompile(kHlslSource, hlsl_len, "lvsp_preview.hlsl", nullptr, nullptr, "main_ps",
                            "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, nullptr);
        }
        if (SUCCEEDED(hr)) {
            hr = dev_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_);
        }
        if (SUCCEEDED(hr)) {
            hr = dev_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps_);
        }
        safe_release(vs_blob);
        safe_release(ps_blob);
        if (FAILED(hr) || !vs_ || !ps_) {
            release_d3d11();
            return false;
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = dev_->CreateSamplerState(&sd, &sampler_);
        if (FAILED(hr)) {
            release_d3d11();
            return false;
        }

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        hr = dev_->CreateRasterizerState(&rd, &rs_);
        if (FAILED(hr)) {
            release_d3d11();
            return false;
        }
        return true;
    }

    bool create_rtv() {
        safe_release(rtv_);
        ID3D11Texture2D* bb = nullptr;
        HRESULT hr = swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
        if (FAILED(hr) || !bb) return false;
        hr = dev_->CreateRenderTargetView(bb, nullptr, &rtv_);
        bb->Release();
        return SUCCEEDED(hr) && rtv_ != nullptr;
    }

    bool ensure_texture(int w, int h) {
        if (tex_ && srv_ && tex_w_ == w && tex_h_ == h) return true;
        safe_release(srv_);
        safe_release(tex_);
        tex_w_ = tex_h_ = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w);
        td.Height = UINT(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex_)) || !tex_) return false;
        if (FAILED(dev_->CreateShaderResourceView(tex_, nullptr, &srv_)) || !srv_) {
            safe_release(tex_);
            return false;
        }
        tex_w_ = w;
        tex_h_ = h;
        return true;
    }

    void resize_d3d11(int w, int h) {
        if (!using_d3d11_ || !swap_ || w <= 0 || h <= 0) return;
        if (w == bb_w_ && h == bb_h_) return;

        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        safe_release(rtv_);
        const UINT flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        const HRESULT hr = swap_->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, flags);
        if (FAILED(hr) || !create_rtv()) {
            // Fall back to GDI if swapchain resize fails (e.g. device lost).
            release_d3d11();
            using_d3d11_ = false;
            renderer_name_.store("gdi", std::memory_order_release);
            return;
        }
        bb_w_ = w;
        bb_h_ = h;
    }

    void render_d3d11(bool upload_frame) {
        if (!using_d3d11_ || !ctx_ || !rtv_) return;

        if (upload_frame && front_.width > 0 && front_.height > 0) {
            if (ensure_texture(front_.width, front_.height)) {
                D3D11_MAPPED_SUBRESOURCE map{};
                if (SUCCEEDED(ctx_->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    if (map.RowPitch == front_.stride) {
                        std::memcpy(map.pData, front_.data.data(), front_.data.size());
                    } else {
                        uint8_t* dst = static_cast<uint8_t*>(map.pData);
                        const size_t row_bytes = size_t(front_.width) * 4;
                        for (int y = 0; y < front_.height; ++y) {
                            std::memcpy(dst + size_t(y) * map.RowPitch, front_.row(y), row_bytes);
                        }
                    }
                    ctx_->Unmap(tex_, 0);
                }
            }
        }

        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        ctx_->ClearRenderTargetView(rtv_, black);

        if (srv_ && tex_w_ > 0 && tex_h_ > 0 && bb_w_ > 0 && bb_h_ > 0) {
            // Aspect-ratio preserving letterbox viewport (input texture is never recreated on resize).
            const double src_ar = double(tex_w_) / double(tex_h_);
            const double dst_ar = double(bb_w_) / double(bb_h_);
            float dw = float(bb_w_), dh = float(bb_h_), dx = 0.0f, dy = 0.0f;
            if (dst_ar > src_ar) {
                dh = float(bb_h_);
                dw = std::max(1.0f, float(double(bb_h_) * src_ar));
                dx = (float(bb_w_) - dw) * 0.5f;
            } else {
                dw = float(bb_w_);
                dh = std::max(1.0f, float(double(bb_w_) / src_ar));
                dy = (float(bb_h_) - dh) * 0.5f;
            }

            D3D11_VIEWPORT vp{};
            vp.TopLeftX = dx;
            vp.TopLeftY = dy;
            vp.Width = dw;
            vp.Height = dh;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;

            ctx_->RSSetViewports(1, &vp);
            ctx_->RSSetState(rs_);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->IASetInputLayout(nullptr);
            ctx_->VSSetShader(vs_, nullptr, 0);
            ctx_->PSSetShader(ps_, nullptr, 0);
            ctx_->PSSetShaderResources(0, 1, &srv_);
            ctx_->PSSetSamplers(0, 1, &sampler_);
            ctx_->Draw(3, 0);
        }

        const UINT pflags = allow_tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
        const HRESULT hr = swap_->Present(0, pflags);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            release_d3d11();
            using_d3d11_ = false;
            renderer_name_.store("gdi", std::memory_order_release);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void release_d3d11() {
        safe_release(srv_);
        safe_release(tex_);
        safe_release(rs_);
        safe_release(sampler_);
        safe_release(ps_);
        safe_release(vs_);
        safe_release(rtv_);
        safe_release(swap_);
        safe_release(ctx_);
        safe_release(dev_);
        tex_w_ = tex_h_ = 0;
        bb_w_ = bb_h_ = 0;
        allow_tearing_ = false;
    }

    // --------------------------------------------------------- GDI fallback
    void paint_gdi() {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int cw = rc.right - rc.left;
        const int ch = rc.bottom - rc.top;

        if (front_.width <= 0 || front_.height <= 0 || cw <= 0 || ch <= 0) {
            FillRect(hdc, &rc, HBRUSH(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd_, &ps);
            return;
        }

        const double src_ar = double(front_.width) / double(front_.height);
        const double dst_ar = double(cw) / double(ch);
        int dw = cw, dh = ch, dx = 0, dy = 0;
        if (dst_ar > src_ar) {
            dh = ch;
            dw = std::max(1, int(double(ch) * src_ar + 0.5));
            dx = (cw - dw) / 2;
        } else {
            dw = cw;
            dh = std::max(1, int(double(cw) / src_ar + 0.5));
            dy = (ch - dh) / 2;
        }

        HBRUSH black = HBRUSH(GetStockObject(BLACK_BRUSH));
        if (dx > 0) {
            RECT l{0, 0, dx, ch}, r{dx + dw, 0, cw, ch};
            FillRect(hdc, &l, black);
            FillRect(hdc, &r, black);
        }
        if (dy > 0) {
            RECT t{0, 0, cw, dy}, b{0, dy + dh, cw, ch};
            FillRect(hdc, &t, black);
            FillRect(hdc, &b, black);
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = front_.width;
        bmi.bmiHeader.biHeight = -front_.height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        const bool downscale = (dw < front_.width || dh < front_.height);
        SetStretchBltMode(hdc, downscale ? HALFTONE : COLORONCOLOR);
        if (downscale) SetBrushOrgEx(hdc, 0, 0, nullptr);

        StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, front_.width, front_.height, front_.data.data(), &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
        EndPaint(hwnd_, &ps);
    }

    void toggle_fullscreen() {
        if (!hwnd_) return;
        if (!fullscreen_) {
            saved_style_ = DWORD(GetWindowLongW(hwnd_, GWL_STYLE));
            GetWindowPlacement(hwnd_, &saved_placement_);
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
                SetWindowLongW(hwnd_, GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
                SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                fullscreen_ = true;
            }
        } else {
            SetWindowLongW(hwnd_, GWL_STYLE, saved_style_);
            SetWindowPlacement(hwnd_, &saved_placement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen_ = false;
        }
    }

    static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Win32PreviewWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<Win32PreviewWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self));
            return DefWindowProcW(h, msg, wp, lp);
        }
        if (!self) return DefWindowProcW(h, msg, wp, lp);

        switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE: {
                if (wp != SIZE_MINIMIZED && self->using_d3d11_) {
                    const int nw = LOWORD(lp);
                    const int nh = HIWORD(lp);
                    self->resize_d3d11(nw, nh);
                    self->render_d3d11(false);
                }
                return 0;
            }
            case kWmAppFrame: {
                bool got_frame = false;
                {
                    std::lock_guard<std::mutex> lk(self->mu_);
                    self->frame_msg_pending_ = false;
                    if (self->has_new_frame_) {
                        std::swap(self->front_, self->staging_);
                        self->has_new_frame_ = false;
                        got_frame = true;
                    }
                }
                if (self->using_d3d11_) {
                    self->render_d3d11(got_frame);
                } else {
                    InvalidateRect(h, nullptr, FALSE);
                    UpdateWindow(h);
                }
                return 0;
            }
            case kWmAppTitle: {
                std::string t;
                {
                    std::lock_guard<std::mutex> lk(self->mu_);
                    self->title_msg_pending_ = false;
                    t = self->pending_title_;
                }
                const std::wstring wt = utf8_to_wide(t);
                SetWindowTextW(h, wt.c_str());
                return 0;
            }
            case kWmAppClose:
                DestroyWindow(h);
                return 0;
            case WM_PAINT:
                if (self->using_d3d11_) {
                    ValidateRect(h, nullptr);
                    self->render_d3d11(false);
                } else {
                    self->paint_gdi();
                }
                return 0;
            case WM_LBUTTONDBLCLK:
                self->toggle_fullscreen();
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE || wp == 'Q') {
                    PostMessageW(h, WM_CLOSE, 0, 0);
                } else if (wp == 'F' || wp == VK_F11) {
                    self->toggle_fullscreen();
                } else if (wp == 'C') {
                    if (self->cbs_.on_toggle_conceal) self->cbs_.on_toggle_conceal();
                } else if (wp == 'S') {
                    if (self->cbs_.on_snapshot && self->front_.width > 0) self->cbs_.on_snapshot(self->front_);
                }
                return 0;
            case WM_CLOSE:
                self->open_flag_.store(false, std::memory_order_release);
                if (self->cbs_.on_close) self->cbs_.on_close();
                DestroyWindow(h);
                return 0;
            case WM_DESTROY:
                self->open_flag_.store(false, std::memory_order_release);
                self->hwnd_ = nullptr;
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    bool force_gdi_ = false;
    std::thread ui_thread_;
    std::atomic<bool> open_flag_{false};
    std::atomic<const char*> renderer_name_{"none"};
    HWND hwnd_ = nullptr;
    PreviewCallbacks cbs_;

    // D3D11 resources (UI thread only)
    bool using_d3d11_ = false;
    bool allow_tearing_ = false;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11RasterizerState* rs_ = nullptr;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    int bb_w_ = 0, bb_h_ = 0;

    // Frame handoff & HUD title
    std::mutex mu_;
    Image staging_;
    Image front_;
    bool has_new_frame_ = false;
    bool frame_msg_pending_ = false;
    std::string pending_title_;
    bool title_msg_pending_ = false;

    // Fullscreen state
    bool fullscreen_ = false;
    DWORD saved_style_ = 0;
    WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
};

}  // namespace

std::unique_ptr<IPreviewWindow> create_preview_window(bool force_software) {
    return std::make_unique<Win32PreviewWindow>(force_software);
}

}  // namespace lv
