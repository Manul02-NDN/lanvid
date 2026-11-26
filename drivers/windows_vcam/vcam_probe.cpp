// lanvid_vcam_probe.exe - end-to-end check of the registered LanVid DirectShow virtual cameras.
//
// Acts like a client application (OBS / Zoom / ffmpeg -f dshow): enumerates
// CLSID_VideoInputDeviceCategory, builds a capture graph (ICaptureGraphBuilder2::RenderStream with
// PIN_CATEGORY_CAPTURE -> Sample Grabber -> Null Renderer), selects formats through IAMStreamConfig
// and validates the delivered pixels while this process publishes test frames into
// LanVidVideoBus_v1 (as LanVid Video does):
//   slot 0 "LanVid Cam 1"       : BGRA 1280x720, top half colour A, bottom half colour B
//   slot 1 "LanVid Cam 2"       : nothing -> lavender placeholder
//   slot 8 "LanVid Program Out" : NV12 1920x1080 (zero-conversion path)
// Exit code 0 = all checks passed.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "video/video_bus.hpp"

// ---- qedit.h (Sample Grabber) is no longer in the Windows SDK; the COM classes still ship with Windows.
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double t, IMediaSample* s) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double t, BYTE* buf, long len) = 0;
};
struct __declspec(uuid("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")) ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL one_shot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* cb, long which) = 0;
};
static const CLSID kClsidSampleGrabber = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const CLSID kClsidNullRenderer = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID kSubNV12 = {0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSubYUY2 = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSubRGB32 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};

namespace {

int g_failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            std::printf("  [ok]   ");                     \
        } else {                                          \
            std::printf("  [FAIL] ");                     \
            ++g_failures;                                 \
        }                                                 \
        std::printf(__VA_ARGS__);                         \
        std::printf("\n");                                \
    } while (0)

template <class T>
void safe_release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct Grabber final : public ISampleGrabberCB {
    std::atomic<long> frames{0};
    std::vector<uint8_t> last;
    std::atomic<long> last_len{0};
    CRITICAL_SECTION cs;
    Grabber() { InitializeCriticalSection(&cs); }
    ~Grabber() { DeleteCriticalSection(&cs); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(ISampleGrabberCB))) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE* buf, long len) override {
        EnterCriticalSection(&cs);
        last.assign(buf, buf + len);
        LeaveCriticalSection(&cs);
        last_len = len;
        ++frames;
        return S_OK;
    }
};

std::string friendly_name(IMoniker* m) {
    IPropertyBag* bag = nullptr;
    std::string out;
    if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_IPropertyBag, reinterpret_cast<void**>(&bag)))) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) {
            char buf[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, buf, sizeof(buf), nullptr, nullptr);
            out = buf;
        }
        VariantClear(&v);
        bag->Release();
    }
    return out;
}

IBaseFilter* find_device(const std::string& name, std::vector<std::string>* all = nullptr) {
    ICreateDevEnum* de = nullptr;
    IEnumMoniker* em = nullptr;
    IBaseFilter* result = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                                reinterpret_cast<void**>(&de))))
        return nullptr;
    if (de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0) == S_OK && em) {
        IMoniker* m = nullptr;
        while (em->Next(1, &m, nullptr) == S_OK) {
            const std::string n = friendly_name(m);
            if (all) all->push_back(n);
            if (!result && n == name) m->BindToObject(nullptr, nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&result));
            m->Release();
        }
        em->Release();
    }
    de->Release();
    return result;
}

struct CaptureResult {
    bool ok = false;
    long frames = 0;
    long len = 0;
    int width = 0, height = 0;
    GUID subtype{};
    std::vector<uint8_t> frame;
};

// Captures `seconds` from device `name` in format (subtype, w, h); w == 0 keeps the default format.
CaptureResult capture(const std::string& name, const GUID& subtype, int w, int h, double seconds,
                      REFERENCE_TIME interval = 333333) {
    CaptureResult r;
    IGraphBuilder* graph = nullptr;
    ICaptureGraphBuilder2* cgb = nullptr;
    IBaseFilter* src = find_device(name);
    IBaseFilter* grab_f = nullptr;
    IBaseFilter* null_f = nullptr;
    ISampleGrabber* grab = nullptr;
    IAMStreamConfig* sc = nullptr;
    IMediaControl* mc = nullptr;
    Grabber cb;
    if (!src) {
        std::printf("  device '%s' not found\n", name.c_str());
        return r;
    }
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder,
                                  reinterpret_cast<void**>(&graph));
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2,
                              reinterpret_cast<void**>(&cgb));
    if (SUCCEEDED(hr)) hr = cgb->SetFiltergraph(graph);
    if (SUCCEEDED(hr)) hr = graph->AddFilter(src, L"LanVid source");
    if (SUCCEEDED(hr) && w > 0) {
        hr = cgb->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, src, IID_IAMStreamConfig,
                                reinterpret_cast<void**>(&sc));
        if (SUCCEEDED(hr)) {
            int count = 0, size = 0;
            sc->GetNumberOfCapabilities(&count, &size);
            bool set = false;
            for (int i = 0; i < count && !set; ++i) {
                AM_MEDIA_TYPE* mt = nullptr;
                VIDEO_STREAM_CONFIG_CAPS caps;
                if (sc->GetStreamCaps(i, &mt, reinterpret_cast<BYTE*>(&caps)) != S_OK || !mt) continue;
                auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
                if (IsEqualGUID(mt->subtype, subtype) && vih->bmiHeader.biWidth == w && std::abs(vih->bmiHeader.biHeight) == h) {
                    vih->AvgTimePerFrame = interval;
                    hr = sc->SetFormat(mt);
                    set = SUCCEEDED(hr);
                }
                CoTaskMemFree(mt->pbFormat);
                CoTaskMemFree(mt);
            }
            if (!set) {
                // Not in the caps list: request it directly (dynamic resolution).
                AM_MEDIA_TYPE* mt = nullptr;
                if (SUCCEEDED(sc->GetFormat(&mt)) && mt) {
                    auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
                    const int bpp = IsEqualGUID(subtype, kSubNV12) ? 12 : IsEqualGUID(subtype, kSubYUY2) ? 16 : 32;
                    mt->subtype = subtype;
                    vih->bmiHeader.biWidth = w;
                    vih->bmiHeader.biHeight = h;
                    vih->bmiHeader.biBitCount = WORD(bpp);
                    vih->bmiHeader.biCompression = IsEqualGUID(subtype, kSubNV12)   ? 0x3231564E
                                                   : IsEqualGUID(subtype, kSubYUY2) ? 0x32595559
                                                                                    : BI_RGB;
                    vih->bmiHeader.biSizeImage = DWORD(w * h * bpp / 8);
                    mt->lSampleSize = vih->bmiHeader.biSizeImage;
                    vih->AvgTimePerFrame = interval;
                    hr = sc->SetFormat(mt);
                    CoTaskMemFree(mt->pbFormat);
                    CoTaskMemFree(mt);
                }
            }
        }
    }
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                              reinterpret_cast<void**>(&grab_f));
    if (SUCCEEDED(hr)) hr = grab_f->QueryInterface(__uuidof(ISampleGrabber), reinterpret_cast<void**>(&grab));
    if (SUCCEEDED(hr)) {
        AM_MEDIA_TYPE want{};
        want.majortype = MEDIATYPE_Video;
        if (w > 0) want.subtype = subtype;
        grab->SetMediaType(&want);
        grab->SetBufferSamples(FALSE);
        grab->SetCallback(&cb, 1);
        hr = graph->AddFilter(grab_f, L"Grabber");
    }
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                              reinterpret_cast<void**>(&null_f));
    if (SUCCEEDED(hr)) hr = graph->AddFilter(null_f, L"Null");
    if (SUCCEEDED(hr)) hr = cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, src, grab_f, null_f);
    if (FAILED(hr)) std::printf("  graph build failed: 0x%08lX\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        AM_MEDIA_TYPE mt{};
        if (SUCCEEDED(grab->GetConnectedMediaType(&mt))) {
            r.subtype = mt.subtype;
            if (mt.pbFormat) {
                auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt.pbFormat);
                r.width = vih->bmiHeader.biWidth;
                r.height = vih->bmiHeader.biHeight;
                CoTaskMemFree(mt.pbFormat);
            }
        }
        hr = graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&mc));
    }
    if (SUCCEEDED(hr)) {
        hr = mc->Run();
        if (SUCCEEDED(hr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(int(seconds * 1000)));
            mc->Stop();
            r.ok = true;
        }
    }
    r.frames = cb.frames.load();
    r.len = cb.last_len.load();
    EnterCriticalSection(&cb.cs);
    r.frame = cb.last;
    LeaveCriticalSection(&cb.cs);
    safe_release(mc);
    safe_release(sc);
    safe_release(grab);
    safe_release(null_f);
    safe_release(grab_f);
    safe_release(src);
    safe_release(cgb);
    safe_release(graph);
    return r;
}

// Test pattern colours (BGRA)
constexpr uint8_t kTopB = 200, kTopG = 60, kTopR = 120;
constexpr uint8_t kBotB = 30, kBotG = 180, kBotR = 40;

void publisher(std::atomic<bool>& stop) {
    lv::VideoBus bus;
    if (!bus.open()) {
        std::printf("  [FAIL] cannot open LanVidVideoBus_v1\n");
        ++g_failures;
        return;
    }
    bus.set_active_vcams(8, true);
    bus.set_slot_info(0, "CAM 1 - Probe", "lanvid_vcam_probe", true, false);
    bus.set_slot_info(1, "CAM 2", "", false, false);
    bus.set_slot_info(lv::kVideoBusProgramSlot, "PGM", "lanvid_vcam_probe", true, false);
    const uint32_t w = 1280, h = 720;
    std::vector<uint8_t> bgra(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &bgra[(size_t(y) * w + x) * 4];
            const bool top = y < h / 2;
            p[0] = top ? kTopB : kBotB;
            p[1] = top ? kTopG : kBotG;
            p[2] = top ? kTopR : kBotR;
            p[3] = 255;
        }
    const uint32_t pw = 1920, ph = 1080;
    std::vector<uint8_t> nv12(size_t(pw) * ph * 3 / 2);
    std::memset(nv12.data(), 81, size_t(pw) * ph);
    for (size_t i = size_t(pw) * ph; i + 1 < nv12.size(); i += 2) {
        nv12[i] = 90;
        nv12[i + 1] = 240;
    }
    uint64_t ts = 0;
    while (!stop.load()) {
        bus.publish(0, lv::VideoBusFormat::kBGRA, w, h, bgra.data(), 0, ts, 3000);
        bus.publish(lv::kVideoBusProgramSlot, lv::VideoBusFormat::kNV12, pw, ph, nv12.data(), 0, ts, 3000);
        bus.heartbeat();
        ts += 33333;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    bus.set_slot_info(0, "CAM 1", "", false, false);
    bus.set_slot_info(lv::kVideoBusProgramSlot, "PGM", "", false, false);
}

// BT.601 limited-range luma of a BGRA colour (matches video_bus rgb_to_yuv within rounding).
int luma(int r, int g, int b) { return ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16; }

}  // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    std::printf("LanVid virtual camera probe\n");

    std::vector<std::string> devices;
    if (IBaseFilter* f = find_device("<none>", &devices)) f->Release();
    int lanvid = 0;
    for (const auto& d : devices) {
        std::printf("  device: %s\n", d.c_str());
        if (d.rfind("LanVid Cam ", 0) == 0 || d == "LanVid Program Out") ++lanvid;
    }
    CHECK(lanvid == 9, "9 LanVid devices registered in CLSID_VideoInputDeviceCategory (found %d)", lanvid);

    std::atomic<bool> stop{false};
    std::thread pub([&] { publisher(stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 1) RGB32 1920x1080 from a 1280x720 BGRA camera: scaling + bottom-up orientation + colours.
    {
        auto r = capture("LanVid Cam 1", kSubRGB32, 1920, 1080, 2.0);
        CHECK(r.ok && r.frames >= 45, "Cam 1 RGB32 1920x1080: %ld frames in 2 s", r.frames);
        CHECK(r.width == 1920 && std::abs(r.height) == 1080 && r.len == 1920 * 1080 * 4, "Cam 1 RGB32 size %dx%d len %ld",
              r.width, r.height, r.len);
        if (r.len == 1920 * 1080 * 4 && !r.frame.empty()) {
            // bottom-up buffer: row 270 = image row 809 (bottom half), row 810 = image row 269 (top half)
            const bool bottom_up = r.height > 0;
            const uint8_t* lo = &r.frame[(size_t(270) * 1920 + 960) * 4];
            const uint8_t* hi = &r.frame[(size_t(810) * 1920 + 960) * 4];
            const uint8_t* img_top = bottom_up ? hi : lo;
            const uint8_t* img_bot = bottom_up ? lo : hi;
            CHECK(std::abs(img_top[0] - kTopB) <= 2 && std::abs(img_top[1] - kTopG) <= 2 && std::abs(img_top[2] - kTopR) <= 2,
                  "Cam 1 top colour BGR=(%d,%d,%d)", img_top[0], img_top[1], img_top[2]);
            CHECK(std::abs(img_bot[0] - kBotB) <= 2 && std::abs(img_bot[1] - kBotG) <= 2 && std::abs(img_bot[2] - kBotR) <= 2,
                  "Cam 1 bottom colour BGR=(%d,%d,%d)", img_bot[0], img_bot[1], img_bot[2]);
        }
    }
    // 2) NV12 1080x1920 (9:16 vertical) from a 16:9 camera: letterboxed, luma of the picture area.
    {
        auto r = capture("LanVid Cam 1", kSubNV12, 1080, 1920, 1.5);
        CHECK(r.ok && r.frames >= 30 && r.len == 1080 * 1920 * 3 / 2, "Cam 1 NV12 9:16 1080x1920: %ld frames, len %ld",
              r.frames, r.len);
        if (r.len == 1080 * 1920 * 3 / 2) {
            const int y_top = r.frame[size_t(900) * 1080 + 540];   // inside the 1080x608 picture, top half
            const int y_bar = r.frame[size_t(100) * 1080 + 540];   // letterbox bar
            CHECK(std::abs(y_top - luma(kTopR, kTopG, kTopB)) <= 3 && y_bar <= 20, "Cam 1 9:16 luma picture=%d bar=%d", y_top,
                  y_bar);
        }
    }
    // 3) YUY2 1280x720 on Cam 2 (no camera): lavender placeholder, never black, never stalls.
    {
        auto r = capture("LanVid Cam 2", kSubYUY2, 1280, 720, 1.5);
        long sum = 0;
        for (size_t i = 0; i + 1 < r.frame.size(); i += 2 * 97) sum += r.frame[i];
        const long samples = long(r.frame.size() / (2 * 97));
        const long avg_y = samples ? sum / samples : 0;
        CHECK(r.ok && r.frames >= 30 && r.len == 1280 * 720 * 2, "Cam 2 YUY2 placeholder: %ld frames, len %ld", r.frames, r.len);
        CHECK(avg_y > 60, "Cam 2 placeholder is not black (avg Y %ld)", avg_y);
    }
    // 4) Program Out NV12 1920x1080: zero-conversion NV12 path.
    {
        auto r = capture("LanVid Program Out", kSubNV12, 1920, 1080, 1.5);
        CHECK(r.ok && r.frames >= 30 && r.len == 1920 * 1080 * 3 / 2, "Program Out NV12 1080p: %ld frames", r.frames);
        if (r.len == 1920 * 1080 * 3 / 2) {
            const int y = r.frame[size_t(540) * 1920 + 960];
            const int u = r.frame[size_t(1920) * 1080 + size_t(270) * 1920 + 960];
            const int v = r.frame[size_t(1920) * 1080 + size_t(270) * 1920 + 961];
            CHECK(y == 81 && u == 90 && v == 240, "Program Out NV12 passthrough Y/U/V = %d/%d/%d", y, u, v);
        }
    }
    // 5) 4K60 NV12 on Cam 1: throughput.
    {
        auto r = capture("LanVid Cam 1", kSubNV12, 3840, 2160, 2.0, 166666);
        CHECK(r.ok && r.frames >= 60, "Cam 1 NV12 4K60: %ld frames in 2 s", r.frames);
    }
    // 6) Default format (no IAMStreamConfig::SetFormat), like a naive client.
    {
        auto r = capture("LanVid Cam 3", GUID_NULL, 0, 0, 1.0);
        CHECK(r.ok && r.frames >= 15, "Cam 3 default format %dx%d: %ld frames", r.width, r.height, r.frames);
    }

    stop = true;
    pub.join();
    CoUninitialize();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
