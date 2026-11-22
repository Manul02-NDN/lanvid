#include "platform/screen_capture.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mutex>
#include <windows.h>

namespace lv {

namespace {

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

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

template <class T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

BOOL CALLBACK enum_mon_proc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, &mi)) {
        MonitorInfo info;
        info.index = int(list->size());
        info.name = wide_to_utf8(mi.szDevice);
        info.left = mi.rcMonitor.left;
        info.top = mi.rcMonitor.top;
        info.width = mi.rcMonitor.right - mi.rcMonitor.left;
        info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        list->push_back(std::move(info));
    }
    return TRUE;
}

}  // namespace

std::vector<MonitorInfo> list_monitors() {
    enable_dpi_awareness();
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, enum_mon_proc, LPARAM(&out));
    // Place the primary monitor first so --monitor 0 defaults to the primary display.
    std::stable_sort(out.begin(), out.end(),
                     [](const MonitorInfo& a, const MonitorInfo& b) { return a.primary && !b.primary; });
    for (size_t i = 0; i < out.size(); ++i) out[i].index = int(i);
    return out;
}

struct ScreenCapture::Impl {
    MonitorInfo mon{};
    CaptureBackend requested = CaptureBackend::kAuto;
    bool using_dxgi = false;

    // DXGI Desktop Duplication state
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dupl = nullptr;
    ID3D11Texture2D* staging = nullptr;
    bool has_cached_frame = false;
    Image cached_native;  // holds last DXGI frame for DXGI_ERROR_WAIT_TIMEOUT (static screen)

    // GDI BitBlt fallback state
    HDC screen_dc = nullptr;
    bool screen_dc_created = false;
    bool gdi_use_zero_origin = false;
    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old_bmp = nullptr;
    uint8_t* dib_bits = nullptr;

    void release_dxgi() {
        safe_release(staging);
        safe_release(dupl);
        safe_release(ctx);
        safe_release(device);
        using_dxgi = false;
    }

    void release_gdi() {
        if (mem_dc && old_bmp) SelectObject(mem_dc, old_bmp);
        old_bmp = nullptr;
        if (dib) DeleteObject(dib);
        dib = nullptr;
        dib_bits = nullptr;
        if (mem_dc) DeleteDC(mem_dc);
        mem_dc = nullptr;
        if (screen_dc) {
            if (screen_dc_created) DeleteDC(screen_dc);
            else ReleaseDC(nullptr, screen_dc);
        }
        screen_dc = nullptr;
        screen_dc_created = false;
    }

    bool init_gdi(bool use_monitor_dc = false) {
        release_gdi();
        if (use_monitor_dc && !mon.name.empty()) {
            std::wstring wname(mon.name.begin(), mon.name.end());
            screen_dc = CreateDCW(nullptr, wname.c_str(), nullptr, nullptr);
            screen_dc_created = (screen_dc != nullptr);
            gdi_use_zero_origin = screen_dc_created;
        }
        if (!screen_dc) {
            screen_dc = GetDC(nullptr);
            screen_dc_created = false;
            gdi_use_zero_origin = false;
        }
        if (!screen_dc) return false;
        mem_dc = CreateCompatibleDC(screen_dc);
        if (!mem_dc) {
            release_gdi();
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = mon.width;
        bmi.bmiHeader.biHeight = -mon.height;  // top-down BGRA
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            release_gdi();
            return false;
        }
        dib_bits = static_cast<uint8_t*>(bits);
        old_bmp = SelectObject(mem_dc, dib);
        return true;
    }

    bool capture_gdi(Image& out) {
        if (!mem_dc && !init_gdi(false)) return false;
        int sx = gdi_use_zero_origin ? 0 : mon.left;
        int sy = gdi_use_zero_origin ? 0 : mon.top;
        if (!BitBlt(mem_dc, 0, 0, mon.width, mon.height, screen_dc, sx, sy, SRCCOPY)) {
            // Try dedicated per-monitor DC fallback (and recover from mode/desktop switches).
            if (!init_gdi(!gdi_use_zero_origin)) return false;
            sx = gdi_use_zero_origin ? 0 : mon.left;
            sy = gdi_use_zero_origin ? 0 : mon.top;
            if (!BitBlt(mem_dc, 0, 0, mon.width, mon.height, screen_dc, sx, sy, SRCCOPY)) {
                const DWORD err = GetLastError();
                std::fprintf(stderr, "GDI BitBlt failed (err=%lu) on %dx%d at (%d,%d)\n",
                             (unsigned long)err, mon.width, mon.height, sx, sy);
                return false;
            }
        }
        // Composite mouse cursor when over this monitor.
        CURSORINFO ci{sizeof(ci)};
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
            ICONINFO ii{};
            if (GetIconInfo(ci.hCursor, &ii)) {
                const int cx = ci.ptScreenPos.x - mon.left - int(ii.xHotspot);
                const int cy = ci.ptScreenPos.y - mon.top - int(ii.yHotspot);
                DrawIconEx(mem_dc, cx, cy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
        }
        out.order = PixelOrder::kBGRA;
        resize_bilinear_32bpp(dib_bits, mon.width, mon.height, ptrdiff_t(mon.width) * 4, out);
        return true;
    }

    bool init_dxgi() {
        release_dxgi();
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            return false;
        }

        IDXGIAdapter1* target_adapter = nullptr;
        IDXGIOutput* target_output = nullptr;

        // Match output by monitor coordinates (robust across multi-GPU & multi-monitor setups).
        for (UINT ai = 0; !target_output; ++ai) {
            IDXGIAdapter1* ad = nullptr;
            if (factory->EnumAdapters1(ai, &ad) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT oi = 0;; ++oi) {
                IDXGIOutput* out = nullptr;
                if (ad->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(out->GetDesc(&desc))) {
                    const RECT& r = desc.DesktopCoordinates;
                    if (r.left == mon.left && r.top == mon.top &&
                        (r.right - r.left) == mon.width && (r.bottom - r.top) == mon.height) {
                        target_adapter = ad;
                        target_adapter->AddRef();
                        target_output = out;
                        break;
                    }
                }
                out->Release();
            }
            ad->Release();
        }
        factory->Release();

        if (!target_adapter || !target_output) {
            std::fprintf(stderr, "DXGI: no matching output for %dx%d at (%d,%d)\n",
                         mon.width, mon.height, mon.left, mon.top);
            safe_release(target_output);
            safe_release(target_adapter);
            return false;
        }

        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = D3D11CreateDevice(target_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, &fl, &ctx);
        safe_release(target_adapter);
        if (FAILED(hr)) {
            std::fprintf(stderr, "DXGI: D3D11CreateDevice failed (hr=0x%08lx)\n", (unsigned long)hr);
            safe_release(target_output);
            release_dxgi();
            return false;
        }

        IDXGIOutput1* out1 = nullptr;
        hr = target_output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&out1));
        safe_release(target_output);
        if (FAILED(hr)) {
            std::fprintf(stderr, "DXGI: QI IDXGIOutput1 failed (hr=0x%08lx)\n", (unsigned long)hr);
            release_dxgi();
            return false;
        }

        hr = out1->DuplicateOutput(device, &dupl);
        out1->Release();
        if (FAILED(hr)) {
            std::fprintf(stderr, "DXGI: DuplicateOutput failed (hr=0x%08lx), falling back to GDI\n",
                         (unsigned long)hr);
            release_dxgi();
            return false;
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(mon.width);
        td.Height = UINT(mon.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&td, nullptr, &staging);
        if (FAILED(hr)) {
            release_dxgi();
            return false;
        }

        using_dxgi = true;
        return true;
    }

    bool capture_dxgi(Image& out, int timeout_ms) {
        if (!dupl && !init_dxgi()) return false;

        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* res = nullptr;
        const HRESULT hr = dupl->AcquireNextFrame(UINT(std::max(0, timeout_ms)), &fi, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Desktop static: reuse cached frame, or grab initial frame once via GDI so we never stall.
            if (has_cached_frame) {
                out.order = PixelOrder::kBGRA;
                resize_bilinear_32bpp(cached_native.data.data(), cached_native.width, cached_native.height,
                                      ptrdiff_t(cached_native.stride), out);
                return true;
            }
            if (capture_gdi(out)) return true;
            return true;  // keep caller's existing buffer
        }

        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            // Resolution change / UAC / lock screen -> try recreating once, else fallback to GDI.
            if (init_dxgi()) return capture_gdi(out);
            return capture_gdi(out);
        }

        if (FAILED(hr) || !res) {
            return false;
        }

        ID3D11Texture2D* tex = nullptr;
        const HRESULT qhr = res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        res->Release();
        if (FAILED(qhr) || !tex) {
            dupl->ReleaseFrame();
            return false;
        }

        ctx->CopyResource(staging, tex);
        tex->Release();

        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT mhr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
        if (SUCCEEDED(mhr)) {
            if (cached_native.width != mon.width || cached_native.height != mon.height) {
                cached_native.allocate(mon.width, mon.height, PixelOrder::kBGRA);
            }
            const uint8_t* src = static_cast<const uint8_t*>(map.pData);
            for (int y = 0; y < mon.height; ++y) {
                std::memcpy(cached_native.row(y), src + size_t(y) * map.RowPitch, size_t(mon.width) * 4);
            }
            has_cached_frame = true;
            ctx->Unmap(staging, 0);

            out.order = PixelOrder::kBGRA;
            resize_bilinear_32bpp(cached_native.data.data(), mon.width, mon.height,
                                  ptrdiff_t(cached_native.stride), out);
        }
        dupl->ReleaseFrame();
        return SUCCEEDED(mhr);
    }
};

ScreenCapture::ScreenCapture() : impl_(std::make_unique<Impl>()) {}
ScreenCapture::~ScreenCapture() { close(); }

bool ScreenCapture::open(int monitor_index, CaptureBackend backend) {
    close();
    const auto mons = list_monitors();
    if (mons.empty() || monitor_index < 0 || monitor_index >= int(mons.size())) return false;
    impl_->mon = mons[size_t(monitor_index)];
    impl_->requested = backend;
    impl_->has_cached_frame = false;

    if (backend == CaptureBackend::kDxgi) {
        return impl_->init_dxgi();
    }
    if (backend == CaptureBackend::kGdi) {
        return impl_->init_gdi();
    }
    // kAuto: try DXGI first, transparently fall back to GDI.
    if (impl_->init_dxgi()) return true;
    return impl_->init_gdi();
}

void ScreenCapture::close() {
    if (!impl_) return;
    impl_->release_dxgi();
    impl_->release_gdi();
    impl_->has_cached_frame = false;
}

int ScreenCapture::native_width() const { return impl_ ? impl_->mon.width : 0; }
int ScreenCapture::native_height() const { return impl_ ? impl_->mon.height : 0; }
const char* ScreenCapture::active_backend_name() const {
    return (impl_ && impl_->using_dxgi) ? "dxgi" : "gdi";
}

bool ScreenCapture::capture(Image& out, int timeout_ms) {
    if (!impl_ || impl_->mon.width <= 0 || impl_->mon.height <= 0) return false;
    if (out.width <= 0 || out.height <= 0) {
        out.allocate(impl_->mon.width, impl_->mon.height, PixelOrder::kBGRA);
    }
    if (impl_->using_dxgi) {
        if (impl_->capture_dxgi(out, timeout_ms)) return true;
        if (impl_->requested == CaptureBackend::kAuto) {
            impl_->release_dxgi();
            return impl_->capture_gdi(out);
        }
        return false;
    }
    return impl_->capture_gdi(out);
}

}  // namespace lv

#else

namespace lv {
struct ScreenCapture::Impl {};
std::vector<MonitorInfo> list_monitors() { return {}; }
ScreenCapture::ScreenCapture() = default;
ScreenCapture::~ScreenCapture() = default;
bool ScreenCapture::open(int, CaptureBackend) { return false; }
void ScreenCapture::close() {}
int ScreenCapture::native_width() const { return 0; }
int ScreenCapture::native_height() const { return 0; }
const char* ScreenCapture::active_backend_name() const { return "none"; }
bool ScreenCapture::capture(Image&, int) { return false; }
}  // namespace lv

#endif
