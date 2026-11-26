// LanVidVCam.dll - DirectShow virtual cameras "LanVid Cam 1..8" + "LanVid Program Out".
//
// Each device is a live capture source filter (CLSID_VideoInputDeviceCategory) with one output pin
// ("Capture", PIN_CATEGORY_CAPTURE) that reads the latest frame of its slot from the
// LanVidVideoBus_v1 shared memory published by LanVid Video. OBS Studio, Zoom, Teams (classic),
// Skype, Discord, vMix, Chrome/Edge (DirectShow fallback), ffmpeg -f dshow ... see it as a webcam.
//
// Formats (IAMStreamConfig): NV12 / YUY2 / RGB32 at 1920x1080, 1280x720, 3840x2160, 1080x1920 (9:16),
// 720x1280 (9:16), 640x360, 1..60 fps; any other even size between 160x120 and 3840x2160 requested
// through SetFormat is accepted too (frames are scaled). Without a fresh frame (camera gone, standby,
// LanVid Video closed) the lavender "LanVid Cam N - No Signal / Standby" card is shown, so the
// client application never freezes on the last frame or stalls.
//
// Implemented without the DirectShow BaseClasses (not part of the Windows SDK).
// Register (elevated): regsvr32 LanVidVCam.dll   Unregister: regsvr32 /u LanVidVCam.dll

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dshow.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lanvid_vcam_guids.h"
#include "video/video_bus.hpp"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// FOURCC subtypes: {XXXXXXXX-0000-0010-8000-00AA00389B71}
constexpr GUID fourcc_guid(uint32_t fcc) {
    return GUID{fcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}
const GUID kSubNV12 = fourcc_guid(0x3231564E);  // 'NV12'
const GUID kSubYUY2 = fourcc_guid(0x32595559);  // 'YUY2'
// MEDIASUBTYPE_RGB32 {e436eb7e-524f-11ce-9f53-0020af0ba770}
const GUID kSubRGB32 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};

constexpr REFERENCE_TIME kInterval30 = 333333;
constexpr REFERENCE_TIME kMinInterval = 166666;    // 60 fps
constexpr REFERENCE_TIME kMaxInterval = 10000000;  // 1 fps
constexpr wchar_t kPinName[] = L"Capture";

enum class Pix : int { kNV12 = 0, kYUY2 = 1, kRGB32 = 2 };

struct VFormat {
    Pix pix = Pix::kNV12;
    int w = 1920;
    int h = 1080;
    bool top_down = false;  // RGB32 only: negative biHeight
    REFERENCE_TIME interval = kInterval30;
};

constexpr int kRes[][2] = {{1920, 1080}, {1280, 720}, {3840, 2160}, {1080, 1920}, {720, 1280}, {640, 360}};
constexpr int kResCount = int(sizeof(kRes) / sizeof(kRes[0]));
constexpr int kPixCount = 3;
constexpr int kCapsCount = kResCount * kPixCount;

VFormat caps_format(int i) {
    VFormat f;
    f.w = kRes[i / kPixCount][0];
    f.h = kRes[i / kPixCount][1];
    f.pix = Pix(i % kPixCount);
    return f;
}

const GUID& pix_subtype(Pix p) { return p == Pix::kNV12 ? kSubNV12 : p == Pix::kYUY2 ? kSubYUY2 : kSubRGB32; }
WORD pix_bits(Pix p) { return p == Pix::kNV12 ? 12 : p == Pix::kYUY2 ? 16 : 32; }
DWORD pix_compression(Pix p) { return p == Pix::kNV12 ? 0x3231564E : p == Pix::kYUY2 ? 0x32595559 : BI_RGB; }
long image_bytes(const VFormat& f) {
    const long px = long(f.w) * long(f.h);
    return f.pix == Pix::kNV12 ? px * 3 / 2 : f.pix == Pix::kYUY2 ? px * 2 : px * 4;
}

// ---------------------------------------------------------------- AM_MEDIA_TYPE helpers
void free_mt(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void delete_mt(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    free_mt(*mt);
    CoTaskMemFree(mt);
}

HRESULT copy_mt(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src) {
    dst = src;
    if (src.cbFormat && src.pbFormat) {
        dst.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst.pbFormat) {
            dst.cbFormat = 0;
            dst.pUnk = nullptr;
            return E_OUTOFMEMORY;
        }
        std::memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    } else {
        dst.cbFormat = 0;
        dst.pbFormat = nullptr;
    }
    if (dst.pUnk) dst.pUnk->AddRef();
    return S_OK;
}

AM_MEDIA_TYPE* create_mt(const AM_MEDIA_TYPE& src) {
    auto* p = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!p) return nullptr;
    if (FAILED(copy_mt(*p, src))) {
        CoTaskMemFree(p);
        return nullptr;
    }
    return p;
}

HRESULT make_mt(const VFormat& f, AM_MEDIA_TYPE& mt) {
    std::memset(&mt, 0, sizeof(mt));
    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) return E_OUTOFMEMORY;
    std::memset(vih, 0, sizeof(*vih));
    const long bytes = image_bytes(f);
    vih->AvgTimePerFrame = f.interval;
    vih->dwBitRate = DWORD(std::min<uint64_t>(0xFFFFFFFFull, uint64_t(bytes) * 8ull * 10000000ull / uint64_t(f.interval)));
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = f.w;
    vih->bmiHeader.biHeight = (f.pix == Pix::kRGB32 && f.top_down) ? -f.h : f.h;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = pix_bits(f.pix);
    vih->bmiHeader.biCompression = pix_compression(f.pix);
    vih->bmiHeader.biSizeImage = DWORD(bytes);
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = pix_subtype(f.pix);
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = ULONG(bytes);
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = reinterpret_cast<BYTE*>(vih);
    return S_OK;
}

// Parses a (fully specified) media type we can produce.
bool parse_mt(const AM_MEDIA_TYPE& mt, VFormat& out) {
    if (!IsEqualGUID(mt.majortype, MEDIATYPE_Video)) return false;
    Pix pix;
    if (IsEqualGUID(mt.subtype, kSubNV12)) pix = Pix::kNV12;
    else if (IsEqualGUID(mt.subtype, kSubYUY2)) pix = Pix::kYUY2;
    else if (IsEqualGUID(mt.subtype, kSubRGB32)) pix = Pix::kRGB32;
    else return false;
    const BITMAPINFOHEADER* bih = nullptr;
    REFERENCE_TIME interval = 0;
    if (IsEqualGUID(mt.formattype, FORMAT_VideoInfo) && mt.cbFormat >= sizeof(VIDEOINFOHEADER) && mt.pbFormat) {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
        bih = &vih->bmiHeader;
        interval = vih->AvgTimePerFrame;
    } else {
        return false;
    }
    const int w = bih->biWidth;
    const int h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
    if (w < 160 || h < 120 || w > int(lv::kVideoBusMaxWidth) || h > int(lv::kVideoBusMaxHeight) ||
        std::max(w, h) > 3840 || (w & 1) || (h & 1))
        return false;
    if (pix != Pix::kRGB32 && bih->biHeight < 0) return false;  // YUV is always top-down, positive height
    out.pix = pix;
    out.w = w;
    out.h = h;
    out.top_down = pix == Pix::kRGB32 && bih->biHeight < 0;
    out.interval = interval <= 0 ? kInterval30 : std::clamp<REFERENCE_TIME>(interval, kMinInterval, kMaxInterval);
    return true;
}

// Partial type matching for IPin::Connect(pin, partial_mt).
bool partial_match(const AM_MEDIA_TYPE* want, const AM_MEDIA_TYPE& have) {
    if (!want) return true;
    if (!IsEqualGUID(want->majortype, GUID_NULL) && !IsEqualGUID(want->majortype, have.majortype)) return false;
    if (!IsEqualGUID(want->subtype, GUID_NULL) && !IsEqualGUID(want->subtype, have.subtype)) return false;
    if (!IsEqualGUID(want->formattype, GUID_NULL) && !IsEqualGUID(want->formattype, have.formattype)) return false;
    return true;
}

std::wstring widen(const char* s) {
    std::wstring w;
    for (; s && *s; ++s) w.push_back(wchar_t(static_cast<unsigned char>(*s)));
    return w;
}

class VCamFilter;

// ================================================================================================
// Output pin
// ================================================================================================
class VCamPin final : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    VCamPin(VCamFilter* f, uint32_t slot) : filter_(f), slot_(slot) {}
    ~VCamPin() {
        stop_thread();
        disconnect_internal();
        if (has_mt_) free_mt(mt_);
    }

    // IUnknown (lifetime = filter)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* receive_pin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override {
        if (!dir) return E_POINTER;
        *dir = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kPinName)));
        if (!*id) return E_OUTOFMEMORY;
        std::memcpy(*id, kPinName, sizeof(kPinName));
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override {
        VFormat f;
        return (pmt && parse_mt(*pmt, f)) ? S_OK : S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** e) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override {
        if (!count || !size) return E_POINTER;
        *count = kCapsCount;
        *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc) override;

    // IKsPropertySet (pin category = capture)
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD* returned) override {
        if (!IsEqualGUID(set, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        if (!data && !returned) return E_POINTER;
        if (returned) *returned = sizeof(GUID);
        if (!data) return S_OK;
        if (cb < sizeof(GUID)) return E_UNEXPECTED;
        *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE;
        return S_OK;
    }
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override {
        if (!IsEqualGUID(set, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        if (support) *support = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }

    // ---- filter-facing ----
    bool is_connected() const { return peer_ != nullptr; }
    HRESULT on_pause_from_stop();
    void on_run(REFERENCE_TIME start, IReferenceClock* clock);
    void on_pause_from_run() { deliver_.store(false, std::memory_order_release); }
    void on_stop();
    // Media type candidate i for enumeration (preferred format first).
    int type_count() const { return kCapsCount + (has_pref_ ? 1 : 0); }
    VFormat type_at(int i) const { return has_pref_ ? (i == 0 ? pref_ : caps_format(i - 1)) : caps_format(i); }

private:
    HRESULT try_connect(IPin* receive_pin, const AM_MEDIA_TYPE& mt);
    HRESULT decide_allocator(IMemInputPin* mem, long bytes);
    void disconnect_internal();
    void stop_thread();
    void stream_loop();
    long render(BYTE* dst, long cap);
    void compose(const uint8_t* src, const lv::VideoBus::FrameInfo& fi);
    void fill_background();
    bool slot_fresh(const lv::VideoBusSlot* s) const;

    VCamFilter* filter_;
    uint32_t slot_;
    IPin* peer_ = nullptr;
    IMemInputPin* mem_ = nullptr;
    IMemAllocator* alloc_ = nullptr;
    AM_MEDIA_TYPE mt_{};
    bool has_mt_ = false;
    VFormat cur_{};
    bool has_pref_ = false;
    VFormat pref_{};

    // streaming
    std::thread thread_;
    std::atomic<bool> thread_run_{false};
    std::atomic<bool> deliver_{false};
    std::atomic<REFERENCE_TIME> start_time_{0};
    std::atomic<IReferenceClock*> clock_{nullptr};
    std::atomic<bool> first_sample_{true};

    // frame source (LanVidVideoBus_v1) - touched only by the streaming thread
    lv::VideoBus bus_;
    uint64_t bus_retry_ms_ = 0;
    std::vector<uint8_t> src_;
    std::vector<uint8_t> bgra_;
    std::vector<uint8_t> out_;
    std::vector<uint8_t> native_;
    std::vector<uint8_t> card_;
    std::vector<uint32_t> xmap_;
    enum class OutState { kNone, kFrame, kPlaceholder } out_state_ = OutState::kNone;
    uint64_t out_seq_ = 0;
    uint64_t placeholder_ms_ = 0;
};

// ================================================================================================
// Filter
// ================================================================================================
class VCamFilter final : public IBaseFilter, public IAMFilterMiscFlags {
public:
    explicit VCamFilter(uint32_t slot) : slot_(slot), pin_(this, slot) {
        ++g_objects;
        const std::wstring n = widen(lv::video_bus_device_name(slot));
        wcsncpy_s(name_, n.c_str(), _TRUNCATE);
    }
    ~VCamFilter() {
        pin_.on_stop();
        if (clock_) clock_->Release();
        --g_objects;
    }

    std::recursive_mutex mu;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IBaseFilter)) ||
            IsEqualIID(riid, __uuidof(IMediaFilter)) || IsEqualIID(riid, __uuidof(IPersist))) {
            *ppv = static_cast<IBaseFilter*>(this);
        } else if (IsEqualIID(riid, __uuidof(IAMFilterMiscFlags))) {
            *ppv = static_cast<IAMFilterMiscFlags*>(this);
        } else if (IsEqualIID(riid, __uuidof(IAMStreamConfig)) || IsEqualIID(riid, __uuidof(IKsPropertySet))) {
            return pin_.QueryInterface(riid, ppv);  // some apps query these on the filter
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* clsid) override {
        if (!clsid) return E_POINTER;
        *clsid = lv::vcam::device_clsid(slot_);
        return S_OK;
    }

    // IMediaFilter
    STDMETHODIMP Stop() override {
        std::lock_guard<std::recursive_mutex> lk(mu);
        pin_.on_stop();
        state_ = State_Stopped;
        return S_OK;
    }
    STDMETHODIMP Pause() override {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (state_ == State_Stopped) {
            const HRESULT hr = pin_.on_pause_from_stop();
            if (FAILED(hr)) return hr;
        } else if (state_ == State_Running) {
            pin_.on_pause_from_run();
        }
        state_ = State_Paused;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME start) override {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (state_ == State_Stopped) {
            const HRESULT hr = Pause();
            if (FAILED(hr)) return hr;
        }
        pin_.on_run(start, clock_);
        state_ = State_Running;
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* st) override {
        if (!st) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(mu);
        *st = state_;
        return state_ == State_Paused ? VFW_S_CANT_CUE : S_OK;  // live source: no data while paused
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (clock) clock->AddRef();
        if (clock_) clock_->Release();
        clock_ = clock;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override {
        if (!clock) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(mu);
        *clock = clock_;
        if (clock_) clock_->AddRef();
        return S_OK;
    }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** e) override;
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override {
        if (!pin) return E_POINTER;
        *pin = nullptr;
        if (!id || (wcscmp(id, kPinName) != 0 && wcscmp(id, L"0") != 0)) return VFW_E_NOT_FOUND;
        *pin = &pin_;
        pin_.AddRef();
        return S_OK;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override {
        if (!info) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(mu);
        wcsncpy_s(info->achName, name_, _TRUNCATE);
        info->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override {
        std::lock_guard<std::recursive_mutex> lk(mu);
        graph_ = graph;  // weak reference (the graph owns the filter)
        if (graph && name && *name) wcsncpy_s(name_, name, _TRUNCATE);
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* info) override {
        if (!info) return E_POINTER;
        static const wchar_t kVendor[] = L"LanVid";
        *info = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kVendor)));
        if (!*info) return E_OUTOFMEMORY;
        std::memcpy(*info, kVendor, sizeof(kVendor));
        return S_OK;
    }

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

    FILTER_STATE state() {
        std::lock_guard<std::recursive_mutex> lk(mu);
        return state_;
    }
    IFilterGraph* graph() const { return graph_; }
    VCamPin& pin() { return pin_; }

private:
    std::atomic<ULONG> ref_{1};
    uint32_t slot_;
    FILTER_STATE state_ = State_Stopped;
    IReferenceClock* clock_ = nullptr;
    IFilterGraph* graph_ = nullptr;
    wchar_t name_[MAX_FILTER_NAME] = {};
    VCamPin pin_;
};

// ---------------------------------------------------------------- enumerators
class EnumPinsImpl final : public IEnumPins {
public:
    EnumPinsImpl(VCamFilter* f, ULONG pos) : f_(f), pos_(pos) { f_->AddRef(); }
    ~EnumPinsImpl() { f_->Release(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IEnumPins))) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG n, IPin** pins, ULONG* fetched) override {
        if (!pins) return E_POINTER;
        if (n > 1 && !fetched) return E_INVALIDARG;
        ULONG got = 0;
        if (n > 0 && pos_ == 0) {
            pins[0] = &f_->pin();
            pins[0]->AddRef();
            got = 1;
            pos_ = 1;
        }
        if (fetched) *fetched = got;
        return got == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG n) override {
        pos_ += n;
        return pos_ <= 1 ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** e) override {
        if (!e) return E_POINTER;
        *e = new EnumPinsImpl(f_, pos_);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    VCamFilter* f_;
    ULONG pos_;
};

class EnumTypesImpl final : public IEnumMediaTypes {
public:
    EnumTypesImpl(VCamPin* p, ULONG pos) : p_(p), pos_(pos) { p_->AddRef(); }
    ~EnumTypesImpl() { p_->Release(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IEnumMediaTypes))) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG n, AM_MEDIA_TYPE** types, ULONG* fetched) override {
        if (!types) return E_POINTER;
        if (n > 1 && !fetched) return E_INVALIDARG;
        ULONG got = 0;
        while (got < n && int(pos_) < p_->type_count()) {
            AM_MEDIA_TYPE mt;
            if (FAILED(make_mt(p_->type_at(int(pos_)), mt))) break;
            types[got] = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!types[got]) {
                free_mt(mt);
                break;
            }
            *types[got] = mt;  // ownership of pbFormat moves into the returned type
            ++got;
            ++pos_;
        }
        if (fetched) *fetched = got;
        return got == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG n) override {
        pos_ += n;
        return int(pos_) <= p_->type_count() ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** e) override {
        if (!e) return E_POINTER;
        *e = new EnumTypesImpl(p_, pos_);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    VCamPin* p_;
    ULONG pos_;
};

STDMETHODIMP VCamFilter::EnumPins(IEnumPins** e) {
    if (!e) return E_POINTER;
    *e = new EnumPinsImpl(this, 0);
    return S_OK;
}

// ================================================================================================
// VCamPin implementation
// ================================================================================================
STDMETHODIMP VCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IPin))) {
        *ppv = static_cast<IPin*>(this);
    } else if (IsEqualIID(riid, __uuidof(IAMStreamConfig))) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (IsEqualIID(riid, __uuidof(IKsPropertySet))) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) VCamPin::AddRef() { return filter_->AddRef(); }
STDMETHODIMP_(ULONG) VCamPin::Release() { return filter_->Release(); }

STDMETHODIMP VCamPin::EnumMediaTypes(IEnumMediaTypes** e) {
    if (!e) return E_POINTER;
    *e = new EnumTypesImpl(this, 0);
    return S_OK;
}

STDMETHODIMP VCamPin::Connect(IPin* receive_pin, const AM_MEDIA_TYPE* pmt) {
    if (!receive_pin) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(filter_->mu);
    if (peer_) return VFW_E_ALREADY_CONNECTED;
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;

    // 1. Fully specified type from the caller (e.g. after IAMStreamConfig::SetFormat).
    VFormat f;
    if (pmt && parse_mt(*pmt, f)) return try_connect(receive_pin, *pmt);

    // 2. Our types (preferred format first) matching the partial type.
    for (int i = 0; i < type_count(); ++i) {
        AM_MEDIA_TYPE mt;
        if (FAILED(make_mt(type_at(i), mt))) return E_OUTOFMEMORY;
        HRESULT hr = E_FAIL;
        if (partial_match(pmt, mt)) hr = try_connect(receive_pin, mt);
        free_mt(mt);
        if (SUCCEEDED(hr)) return hr;
    }
    // 3. Types proposed by the downstream pin that we can produce (e.g. other sizes).
    IEnumMediaTypes* e = nullptr;
    if (SUCCEEDED(receive_pin->EnumMediaTypes(&e)) && e) {
        AM_MEDIA_TYPE* t = nullptr;
        ULONG got = 0;
        while (e->Next(1, &t, &got) == S_OK && got == 1) {
            HRESULT hr = E_FAIL;
            if (partial_match(pmt, *t) && parse_mt(*t, f)) hr = try_connect(receive_pin, *t);
            delete_mt(t);
            if (SUCCEEDED(hr)) {
                e->Release();
                return hr;
            }
        }
        e->Release();
    }
    return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT VCamPin::try_connect(IPin* receive_pin, const AM_MEDIA_TYPE& mt) {
    VFormat f;
    if (!parse_mt(mt, f)) return VFW_E_TYPE_NOT_ACCEPTED;
    HRESULT hr = receive_pin->ReceiveConnection(this, &mt);
    if (FAILED(hr)) return hr;
    IMemInputPin* mem = nullptr;
    hr = receive_pin->QueryInterface(__uuidof(IMemInputPin), reinterpret_cast<void**>(&mem));
    if (SUCCEEDED(hr)) hr = decide_allocator(mem, image_bytes(f));
    if (FAILED(hr)) {
        if (mem) mem->Release();
        if (alloc_) {
            alloc_->Release();
            alloc_ = nullptr;
        }
        receive_pin->Disconnect();
        return hr;
    }
    peer_ = receive_pin;
    peer_->AddRef();
    mem_ = mem;
    if (has_mt_) free_mt(mt_);
    copy_mt(mt_, mt);
    has_mt_ = true;
    cur_ = f;
    out_state_ = OutState::kNone;
    return S_OK;
}

HRESULT VCamPin::decide_allocator(IMemInputPin* mem, long bytes) {
    ALLOCATOR_PROPERTIES req{}, act{};
    mem->GetAllocatorRequirements(&req);
    ALLOCATOR_PROPERTIES props{};
    props.cBuffers = std::max<long>(3, req.cBuffers);
    props.cbBuffer = bytes;
    props.cbAlign = std::max<long>(1, req.cbAlign);
    props.cbPrefix = req.cbPrefix;

    IMemAllocator* a = nullptr;
    if (SUCCEEDED(mem->GetAllocator(&a)) && a) {
        if (SUCCEEDED(a->SetProperties(&props, &act)) && act.cbBuffer >= bytes && SUCCEEDED(mem->NotifyAllocator(a, FALSE))) {
            alloc_ = a;
            return S_OK;
        }
        a->Release();
        a = nullptr;
    }
    HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMemAllocator),
                                  reinterpret_cast<void**>(&a));
    if (FAILED(hr)) return hr;
    hr = a->SetProperties(&props, &act);
    if (SUCCEEDED(hr) && act.cbBuffer < bytes) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = mem->NotifyAllocator(a, FALSE);
    if (FAILED(hr)) {
        a->Release();
        return hr;
    }
    alloc_ = a;
    return S_OK;
}

void VCamPin::disconnect_internal() {
    if (alloc_) {
        alloc_->Decommit();
        alloc_->Release();
        alloc_ = nullptr;
    }
    if (mem_) {
        mem_->Release();
        mem_ = nullptr;
    }
    if (peer_) {
        peer_->Release();
        peer_ = nullptr;
    }
}

STDMETHODIMP VCamPin::Disconnect() {
    std::lock_guard<std::recursive_mutex> lk(filter_->mu);
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
    if (!peer_) return S_FALSE;
    disconnect_internal();
    if (has_mt_) {
        free_mt(mt_);
        has_mt_ = false;
    }
    return S_OK;
}

STDMETHODIMP VCamPin::ConnectedTo(IPin** pin) {
    if (!pin) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(filter_->mu);
    *pin = peer_;
    if (!peer_) return VFW_E_NOT_CONNECTED;
    peer_->AddRef();
    return S_OK;
}

STDMETHODIMP VCamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(filter_->mu);
    if (!peer_ || !has_mt_) {
        std::memset(pmt, 0, sizeof(*pmt));
        return VFW_E_NOT_CONNECTED;
    }
    return copy_mt(*pmt, mt_);
}

STDMETHODIMP VCamPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    info->pFilter = filter_;
    filter_->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcsncpy_s(info->achName, kPinName, _TRUNCATE);
    return S_OK;
}

STDMETHODIMP VCamPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    std::lock_guard<std::recursive_mutex> lk(filter_->mu);
    if (!pmt) {
        has_pref_ = false;
        return S_OK;
    }
    VFormat f;
    if (!parse_mt(*pmt, f)) return VFW_E_INVALIDMEDIATYPE;
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
    pref_ = f;
    has_pref_ = true;
    if (peer_) {
        // Already connected: reconnect with the new format if the downstream accepts it.
        if (peer_->QueryAccept(pmt) != S_OK) return VFW_E_INVALIDMEDIATYPE;
        IFilterGraph* g = filter_->graph();
        if (g) {
            IFilterGraph2* g2 = nullptr;
            if (SUCCEEDED(g->QueryInterface(__uuidof(IFilterGraph2), reinterpret_cast<void**>(&g2))) && g2) {
                const HRESULT hr = g2->ReconnectEx(this, pmt);
                g2->Release();
                return hr;
            }
            return g->Reconnect(this);
        }
    }
    return S_OK;
}

STDMETHODIMP VCamPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(filter_->mu);
    AM_MEDIA_TYPE mt;
    const HRESULT hr = make_mt(peer_ ? cur_ : (has_pref_ ? pref_ : caps_format(0)), mt);
    if (FAILED(hr)) return hr;
    *ppmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!*ppmt) {
        free_mt(mt);
        return E_OUTOFMEMORY;
    }
    **ppmt = mt;
    return S_OK;
}

STDMETHODIMP VCamPin::GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc) {
    if (!ppmt || !scc) return E_POINTER;
    if (index < 0) return E_INVALIDARG;
    if (index >= kCapsCount) return S_FALSE;
    const VFormat f = caps_format(index);
    AM_MEDIA_TYPE mt;
    const HRESULT hr = make_mt(f, mt);
    if (FAILED(hr)) return hr;
    *ppmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!*ppmt) {
        free_mt(mt);
        return E_OUTOFMEMORY;
    }
    **ppmt = mt;
    auto* c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(scc);
    std::memset(c, 0, sizeof(*c));
    c->guid = FORMAT_VideoInfo;
    c->InputSize = SIZE{f.w, f.h};
    c->MinCroppingSize = c->MaxCroppingSize = SIZE{f.w, f.h};
    c->CropGranularityX = c->CropGranularityY = 1;
    c->CropAlignX = c->CropAlignY = 1;
    c->MinOutputSize = c->MaxOutputSize = SIZE{f.w, f.h};
    c->OutputGranularityX = c->OutputGranularityY = 2;
    c->MinFrameInterval = kMinInterval;
    c->MaxFrameInterval = kMaxInterval;
    const LONGLONG bits = LONGLONG(image_bytes(f)) * 8;
    c->MinBitsPerSecond = LONG(std::min<LONGLONG>(0x7FFFFFFF, bits));
    c->MaxBitsPerSecond = LONG(std::min<LONGLONG>(0x7FFFFFFF, bits * 60));
    return S_OK;
}

HRESULT VCamPin::on_pause_from_stop() {
    if (!alloc_) return S_OK;  // not connected: nothing to stream
    return alloc_->Commit();
}

void VCamPin::on_run(REFERENCE_TIME start, IReferenceClock* clock) {
    if (!alloc_ || !mem_) return;
    if (clock) clock->AddRef();
    IReferenceClock* old = clock_.exchange(clock);
    if (old) old->Release();
    start_time_.store(start);
    first_sample_ = true;
    deliver_.store(true, std::memory_order_release);
    if (!thread_run_.exchange(true)) thread_ = std::thread([this] { stream_loop(); });
}

void VCamPin::stop_thread() {
    deliver_.store(false, std::memory_order_release);
    thread_run_.store(false, std::memory_order_release);
    if (alloc_) alloc_->Decommit();  // unblocks GetBuffer()
    if (thread_.joinable()) thread_.join();
}

void VCamPin::on_stop() {
    stop_thread();
    IReferenceClock* old = clock_.exchange(nullptr);
    if (old) old->Release();
}

bool VCamPin::slot_fresh(const lv::VideoBusSlot* s) const {
    if (!s || !s->connected.load(std::memory_order_acquire) || s->standby.load(std::memory_order_acquire)) return false;
    const uint64_t now = lv::video_bus_now_ms(), wrote = s->last_write_ms.load(std::memory_order_relaxed);
    return (wrote >= now ? 0 : now - wrote) <= lv::kVideoBusStaleMs;
}

// Nearest-neighbour scale of a plane made of `unit`-byte elements (1 = Y, 2 = interleaved UV,
// 4 = BGRA pixel or YUY2 macropixel). Rows that map to the same source row are memcpy'd, so an
// upscale (720p -> 4K) touches each source row once.
void scale_plane(const uint8_t* src, uint32_t sw, uint32_t sh, ptrdiff_t sstride, uint8_t* dst, uint32_t dw,
                 uint32_t dh, ptrdiff_t dstride, uint32_t unit, std::vector<uint32_t>& xmap) {
    if (!sw || !sh || !dw || !dh) return;
    const size_t row_bytes = size_t(dw) * unit;
    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < dh; ++y) std::memcpy(dst + ptrdiff_t(y) * dstride, src + ptrdiff_t(y) * sstride, row_bytes);
        return;
    }
    xmap.resize(dw);
    for (uint32_t x = 0; x < dw; ++x) xmap[x] = uint32_t(uint64_t(x) * sw / dw) * unit;
    uint32_t prev = UINT32_MAX;
    const uint8_t* prev_row = nullptr;
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t sy = uint32_t(uint64_t(y) * sh / dh);
        uint8_t* d = dst + ptrdiff_t(y) * dstride;
        if (sy == prev) {
            std::memcpy(d, prev_row, row_bytes);
            continue;
        }
        const uint8_t* s = src + ptrdiff_t(sy) * sstride;
        if (unit == 1) {
            for (uint32_t x = 0; x < dw; ++x) d[x] = s[xmap[x]];
        } else if (unit == 2) {
            for (uint32_t x = 0; x < dw; ++x) std::memcpy(d + size_t(x) * 2, s + xmap[x], 2);
        } else {
            for (uint32_t x = 0; x < dw; ++x) std::memcpy(d + size_t(x) * 4, s + xmap[x], 4);
        }
        prev = sy;
        prev_row = d;
    }
}

void VCamPin::fill_background() {
    out_.resize(size_t(image_bytes(cur_)));
    const size_t px = size_t(cur_.w) * size_t(cur_.h);
    switch (cur_.pix) {
        case Pix::kNV12:
            std::memset(out_.data(), 16, px);
            std::memset(out_.data() + px, 128, px / 2);
            break;
        case Pix::kYUY2: {
            const uint8_t pat[4] = {16, 128, 16, 128};
            for (size_t i = 0; i < px * 2; i += 4) std::memcpy(out_.data() + i, pat, 4);
            break;
        }
        case Pix::kRGB32: {
            const uint8_t pat[4] = {0, 0, 0, 255};
            for (size_t i = 0; i < px * 4; i += 4) std::memcpy(out_.data() + i, pat, 4);
            break;
        }
    }
}

// Converts one bus frame (BGRA or NV12, any size) into out_ (cur_ format and size), preserving the
// aspect ratio (letter/pillar-box, e.g. a 16:9 phone into a 9:16 device). Pixel-format conversion runs
// at min(source, output) size and scaling runs in the output format.
void VCamPin::compose(const uint8_t* src, const lv::VideoBus::FrameInfo& fi) {
    const uint32_t w = uint32_t(cur_.w), h = uint32_t(cur_.h);
    const uint32_t sw = fi.width & ~1u, sh = fi.height & ~1u;
    if (sw < 2 || sh < 2) {
        fill_background();
        return;
    }
    uint32_t fw = w, fh = h;
    const uint64_t lhs = uint64_t(sw) * h, rhs = uint64_t(sh) * w;
    if (lhs > rhs + rhs / 100) fh = std::max<uint32_t>(2, uint32_t(uint64_t(w) * sh / sw) & ~1u);
    else if (rhs > lhs + lhs / 100) fw = std::max<uint32_t>(2, uint32_t(uint64_t(h) * sw / sh) & ~1u);
    const uint32_t ox = ((w - fw) / 2) & ~1u, oy = ((h - fh) / 2) & ~1u;
    const bool down = uint64_t(fw) * fh < uint64_t(sw) * sh;
    const uint32_t ww = down ? fw : sw, wh = down ? fh : sh;  // working size of the converted image

    const uint8_t* nat = nullptr;  // image in the output pixel format at the working size
    ptrdiff_t nat_stride = 0;
    const uint8_t* nat_uv = nullptr;
    if (fi.format == lv::VideoBusFormat::kNV12 && cur_.pix == Pix::kNV12 && !down) {
        nat = src;  // NV12 camera -> NV12 device: no colour conversion at all
        nat_stride = ptrdiff_t(fi.stride);
        nat_uv = src + size_t(fi.stride) * fi.height;
    } else {
        const uint8_t* bg = nullptr;
        ptrdiff_t bstride = 0;
        if (fi.format == lv::VideoBusFormat::kBGRA && ww == fi.width && wh == fi.height) {
            bg = src;
            bstride = ptrdiff_t(fi.stride);
        } else {
            bgra_.resize(size_t(ww) * wh * 4);
            lv::video_bus_convert_to_bgra(src, fi, bgra_.data(), ww, wh, ww * 4);
            bg = bgra_.data();
            bstride = ptrdiff_t(ww) * 4;
        }
        switch (cur_.pix) {
            case Pix::kNV12:
                native_.resize(size_t(ww) * wh * 3 / 2);
                lv::video_bus_bgra_to_nv12(bg, ww, wh, uint32_t(bstride), native_.data(), ww);
                nat = native_.data();
                nat_stride = ptrdiff_t(ww);
                nat_uv = native_.data() + size_t(ww) * wh;
                break;
            case Pix::kYUY2:
                native_.resize(size_t(ww) * wh * 2);
                lv::video_bus_bgra_to_yuy2(bg, ww, wh, uint32_t(bstride), native_.data(), ww * 2);
                nat = native_.data();
                nat_stride = ptrdiff_t(ww) * 2;
                break;
            case Pix::kRGB32:
                nat = bg;
                nat_stride = bstride;
                break;
        }
    }
    out_.resize(size_t(image_bytes(cur_)));
    if (fw != w || fh != h) fill_background();
    switch (cur_.pix) {
        case Pix::kNV12:
            scale_plane(nat, ww, wh, nat_stride, out_.data() + size_t(oy) * w + ox, fw, fh, ptrdiff_t(w), 1, xmap_);
            scale_plane(nat_uv, ww / 2, wh / 2, nat_stride, out_.data() + size_t(w) * h + size_t(oy / 2) * w + ox, fw / 2,
                        fh / 2, ptrdiff_t(w), 2, xmap_);
            break;
        case Pix::kYUY2:
            scale_plane(nat, ww / 2, wh, nat_stride, out_.data() + (size_t(oy) * w + ox) * 2, fw / 2, fh, ptrdiff_t(w) * 2, 4,
                        xmap_);
            break;
        case Pix::kRGB32: {
            // DirectShow RGB is bottom-up unless biHeight < 0
            uint8_t* d = cur_.top_down ? out_.data() + (size_t(oy) * w + ox) * 4
                                       : out_.data() + (size_t(h - 1 - oy) * w + ox) * 4;
            const ptrdiff_t ds = cur_.top_down ? ptrdiff_t(w) * 4 : -ptrdiff_t(w) * 4;
            scale_plane(nat, ww, wh, nat_stride, d, fw, fh, ds, 4, xmap_);
            break;
        }
    }
}

long VCamPin::render(BYTE* dst, long cap) {
    const long bytes = image_bytes(cur_);
    if (cap < bytes) return 0;
    const uint64_t now_ms = lv::video_bus_now_ms();
    if (!bus_.is_open() && now_ms >= bus_retry_ms_) {
        if (!bus_.open()) bus_retry_ms_ = now_ms + 1000;  // retry once a second until LanVid Video runs
        else src_.resize(lv::kVideoBusFrameBytes);
    }
    const lv::VideoBusSlot* s = bus_.slot(slot_);
    const lv::VideoBusHeader* hdr = bus_.header();
    const uint64_t hb = hdr ? hdr->host_heartbeat_ms.load(std::memory_order_relaxed) : 0;
    const bool host_alive = hdr && hb && (hb >= now_ms || now_ms - hb < 3000);
    const bool enabled = !host_alive || slot_ == lv::kVideoBusProgramSlot ||
                         slot_ < hdr->active_vcams.load(std::memory_order_relaxed);

    bool have_frame = false;
    if (s && enabled && slot_fresh(s)) {
        const uint64_t seq = s->frame_seq.load(std::memory_order_acquire);
        if (out_state_ == OutState::kFrame && seq == out_seq_) {
            have_frame = true;  // unchanged since the last sample: resend the converted frame
        } else {
            lv::VideoBus::FrameInfo fi;
            if (bus_.read_latest(slot_, src_.data(), src_.size(), fi)) {
                compose(src_.data(), fi);
                out_state_ = OutState::kFrame;
                out_seq_ = fi.frame_seq;
                have_frame = true;
            }
        }
    }
    if (!have_frame) {
        // Lavender "LanVid Cam N - No Signal / Standby" card (re-rendered twice a second for the pulse).
        if (out_state_ != OutState::kPlaceholder || now_ms - placeholder_ms_ >= 500) {
            const bool placeholder = !host_alive || hdr->placeholder_enabled.load(std::memory_order_relaxed) != 0;
            if (placeholder) {
                const char* line2 = !host_alive                                         ? "LanVid Video is not running"
                                    : !enabled                                          ? "Disabled in LanVid Video"
                                    : (s && s->standby.load(std::memory_order_relaxed)) ? "Standby"
                                                                                        : "No Signal / Standby";
                // Rendered at <= 1080p and scaled up, so a 4K device does not stall on the software text.
                uint32_t pw = uint32_t(cur_.w), ph = uint32_t(cur_.h);
                while (uint64_t(pw) * ph > 1920ull * 1080ull) {
                    pw = (pw / 2) & ~1u;
                    ph = (ph / 2) & ~1u;
                }
                card_.resize(size_t(pw) * ph * 4);
                lv::video_bus_render_placeholder_bgra(card_.data(), pw, ph, lv::video_bus_device_name(slot_), line2, now_ms);
                lv::VideoBus::FrameInfo fi;
                fi.width = pw;
                fi.height = ph;
                fi.stride = pw * 4;
                fi.format = lv::VideoBusFormat::kBGRA;
                compose(card_.data(), fi);
            } else {
                fill_background();  // host disabled the placeholder: black
            }
            out_state_ = OutState::kPlaceholder;
            placeholder_ms_ = now_ms;
        }
    }
    std::memcpy(dst, out_.data(), size_t(bytes));
    return bytes;
}

void VCamPin::stream_loop() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool raised_period = false;
    if (!timer) {
        timeBeginPeriod(1);
        raised_period = true;
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const int64_t interval_qpc = int64_t(double(cur_.interval) * double(freq.QuadPart) / 1e7);
    int64_t next = now.QuadPart;
    uint64_t frame_no = 0;

    while (thread_run_.load(std::memory_order_acquire)) {
        QueryPerformanceCounter(&now);
        const int64_t wait_qpc = next - now.QuadPart;
        if (wait_qpc > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -int64_t(double(wait_qpc) * 1e7 / double(freq.QuadPart));
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer, 1000);
            else
                Sleep(DWORD(std::max<int64_t>(1, wait_qpc * 1000 / freq.QuadPart)));
        }
        QueryPerformanceCounter(&now);
        next += interval_qpc;
        if (now.QuadPart - next > 2 * interval_qpc) next = now.QuadPart + interval_qpc;  // fell behind: resync

        if (!deliver_.load(std::memory_order_acquire) || !alloc_ || !mem_) continue;
        IMediaSample* sample = nullptr;
        if (FAILED(alloc_->GetBuffer(&sample, nullptr, nullptr, 0)) || !sample) continue;  // decommitted
        BYTE* p = nullptr;
        if (FAILED(sample->GetPointer(&p)) || !p) {
            sample->Release();
            continue;
        }
        const long used = render(p, sample->GetSize());
        sample->SetActualDataLength(used);
        REFERENCE_TIME st = REFERENCE_TIME(frame_no) * cur_.interval;
        if (IReferenceClock* clk = clock_.load()) {
            REFERENCE_TIME t = 0;
            if (SUCCEEDED(clk->GetTime(&t))) st = std::max<REFERENCE_TIME>(0, t - start_time_.load());
        }
        REFERENCE_TIME en = st + cur_.interval;
        sample->SetTime(&st, &en);
        sample->SetSyncPoint(TRUE);
        sample->SetPreroll(FALSE);
        sample->SetDiscontinuity(first_sample_.exchange(false) ? TRUE : FALSE);
        const HRESULT hr = used > 0 ? mem_->Receive(sample) : S_OK;
        sample->Release();
        ++frame_no;
        if (hr != S_OK) deliver_.store(false, std::memory_order_release);  // downstream asked to stop
    }
    if (timer) CloseHandle(timer);
    if (raised_period) timeEndPeriod(1);
}

// ================================================================================================
// Class factory
// ================================================================================================
class Factory final : public IClassFactory {
public:
    explicit Factory(uint32_t slot) : slot_(slot) { ++g_objects; }
    ~Factory() { --g_objects; }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IClassFactory))) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* f = new (std::nothrow) VCamFilter(slot_);
        if (!f) return E_OUTOFMEMORY;
        const HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    uint32_t slot_;
};

// ---------------------------------------------------------------- registration helpers
bool set_reg(const std::wstring& key, const wchar_t* value, const std::wstring& data) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG r = RegSetValueExW(k, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                                  DWORD((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

std::wstring guid_string(const GUID& g) {
    wchar_t buf[64] = {};
    StringFromGUID2(g, buf, 64);
    return buf;
}

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

}  // namespace

// ================================================================================================
// DLL exports
// ================================================================================================
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    const int slot = lv::vcam::device_slot(clsid);
    if (slot < 0) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) Factory(uint32_t(slot));
    if (!f) return E_OUTOFMEMORY;
    const HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return (g_objects.load() == 0 && g_locks.load() == 0) ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return SELFREG_E_CLASS;
    ComInit com;
    IFilterMapper2* fm = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFilterMapper2),
                                  reinterpret_cast<void**>(&fm));
    if (FAILED(hr)) return hr;
    for (uint32_t i = 0; i < lv::vcam::kDeviceCount && SUCCEEDED(hr); ++i) {
        const GUID clsid = lv::vcam::device_clsid(i);
        const std::wstring name = widen(lv::video_bus_device_name(i));
        const std::wstring key = L"SOFTWARE\\Classes\\CLSID\\" + guid_string(clsid);
        if (!set_reg(key, nullptr, name) || !set_reg(key + L"\\InprocServer32", nullptr, path) ||
            !set_reg(key + L"\\InprocServer32", L"ThreadingModel", L"Both")) {
            hr = SELFREG_E_CLASS;  // needs an elevated process (installer / "Register virtual cameras")
            break;
        }
        REGPINTYPES pin_type = {&MEDIATYPE_Video, &MEDIASUBTYPE_NULL};
        REGFILTERPINS pin = {};
        pin.strName = nullptr;
        pin.bRendered = FALSE;
        pin.bOutput = TRUE;
        pin.bZero = FALSE;
        pin.bMany = FALSE;
        pin.clsConnectsToFilter = &CLSID_NULL;
        pin.strConnectsToPin = nullptr;
        pin.nMediaTypes = 1;
        pin.lpMediaType = &pin_type;
        REGFILTER2 rf2 = {};
        rf2.dwVersion = 1;
        rf2.dwMerit = MERIT_DO_NOT_USE;
        rf2.cPins = 1;
        rf2.rgPins = &pin;
        IMoniker* mon = nullptr;
        hr = fm->RegisterFilter(clsid, name.c_str(), &mon, &CLSID_VideoInputDeviceCategory, nullptr, &rf2);
        if (mon) mon->Release();
    }
    fm->Release();
    return hr;
}

STDAPI DllUnregisterServer() {
    ComInit com;
    IFilterMapper2* fm = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFilterMapper2),
                                   reinterpret_cast<void**>(&fm)))) {
        for (uint32_t i = 0; i < lv::vcam::kDeviceCount; ++i)
            fm->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, lv::vcam::device_clsid(i));
        fm->Release();
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID", 0, KEY_WRITE | KEY_READ, &k) == ERROR_SUCCESS) {
        for (uint32_t i = 0; i < lv::vcam::kDeviceCount; ++i)
            RegDeleteTreeW(k, guid_string(lv::vcam::device_clsid(i)).c_str());
        RegCloseKey(k);
    }
    return S_OK;
}
