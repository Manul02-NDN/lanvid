#include "platform/camera_capture.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>

namespace lv {

namespace {

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct ComMfScope {
    bool com_ok = false;
    bool mf_ok = false;
    ComMfScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ok = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE means already initialized in STA, still usable
        if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) mf_ok = true;
    }
    ~ComMfScope() {
        if (mf_ok) MFShutdown();
        if (com_ok) CoUninitialize();
    }
};

}  // namespace

std::vector<CameraInfo> list_cameras() {
    std::vector<CameraInfo> out;
    ComMfScope scope;
    if (!scope.mf_ok) return out;

    IMFAttributes* attr = nullptr;
    if (FAILED(MFCreateAttributes(&attr, 1))) return out;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attr, &devices, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            wchar_t* name = nullptr;
            UINT32 len = 0;
            CameraInfo ci;
            ci.index = int(i);
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) &&
                name) {
                ci.name = wide_to_utf8(name);
                CoTaskMemFree(name);
            } else {
                ci.name = "Camera #" + std::to_string(i);
            }
            out.push_back(std::move(ci));
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }
    attr->Release();
    return out;
}

struct CameraCapture::Impl {
    std::thread reader_thread;
    std::atomic<bool> stop{false};

    int width = 0;
    int height = 0;
    std::string name;

    std::mutex mu;
    std::condition_variable cv;
    Image latest;
    bool has_frame = false;
};

CameraCapture::CameraCapture() : impl_(std::make_unique<Impl>()) {}
CameraCapture::~CameraCapture() { close(); }

bool CameraCapture::open(int device_index, int req_w, int req_h, int req_fps) {
    close();
    impl_->stop.store(false, std::memory_order_relaxed);
    impl_->has_frame = false;

    std::promise<bool> ready;
    auto fut = ready.get_future();

    impl_->reader_thread = std::thread([this, device_index, req_w, req_h, req_fps, p = std::move(ready)]() mutable {
        ComMfScope scope;
        if (!scope.mf_ok) {
            p.set_value(false);
            return;
        }

        IMFAttributes* enum_attr = nullptr;
        if (FAILED(MFCreateAttributes(&enum_attr, 1))) {
            p.set_value(false);
            return;
        }
        enum_attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (FAILED(MFEnumDeviceSources(enum_attr, &devices, &count)) || device_index < 0 ||
            UINT32(device_index) >= count) {
            if (devices) {
                for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
                CoTaskMemFree(devices);
            }
            enum_attr->Release();
            p.set_value(false);
            return;
        }
        enum_attr->Release();

        wchar_t* wname = nullptr;
        UINT32 wlen = 0;
        if (SUCCEEDED(devices[device_index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                                &wname, &wlen)) &&
            wname) {
            impl_->name = wide_to_utf8(wname);
            CoTaskMemFree(wname);
        } else {
            impl_->name = "Camera #" + std::to_string(device_index);
        }

        IMFMediaSource* source = nullptr;
        HRESULT hr = devices[device_index]->ActivateObject(__uuidof(IMFMediaSource),
                                                           reinterpret_cast<void**>(&source));
        for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        if (FAILED(hr) || !source) {
            p.set_value(false);
            return;
        }

        IMFAttributes* r_attr = nullptr;
        MFCreateAttributes(&r_attr, 1);
        if (r_attr) r_attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        IMFSourceReader* reader = nullptr;
        hr = MFCreateSourceReaderFromMediaSource(source, r_attr, &reader);
        if (r_attr) r_attr->Release();
        source->Release();
        if (FAILED(hr) || !reader) {
            p.set_value(false);
            return;
        }

        // Pick closest native resolution/FPS before requesting RGB32 output.
        UINT32 best_w = 0, best_h = 0, best_fps_num = 30, best_fps_den = 1;
        double best_score = 1e18;
        for (DWORD idx = 0;; ++idx) {
            IMFMediaType* mt = nullptr;
            if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), idx, &mt))) {
                break;
            }
            UINT32 w = 0, h = 0, fn = 30, fd = 1;
            if (SUCCEEDED(MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
                MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &fn, &fd);
                const double fps = fd ? double(fn) / double(fd) : 30.0;
                const double dw = double(int(w) - req_w);
                const double dh = double(int(h) - req_h);
                const double df = std::max(0.0, double(req_fps) - fps) * 20.0;
                const double score = std::sqrt(dw * dw + dh * dh) + df;
                if (score < best_score) {
                    best_score = score;
                    best_w = w;
                    best_h = h;
                    best_fps_num = fn;
                    best_fps_den = fd ? fd : 1;
                }
            }
            mt->Release();
        }

        // Request RGB32 (BGRA8) output from MF video processor.
        IMFMediaType* out_mt = nullptr;
        MFCreateMediaType(&out_mt);
        out_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (best_w > 0 && best_h > 0) {
            MFSetAttributeSize(out_mt, MF_MT_FRAME_SIZE, best_w, best_h);
            MFSetAttributeRatio(out_mt, MF_MT_FRAME_RATE, best_fps_num, best_fps_den);
        }
        hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out_mt);
        out_mt->Release();
        if (FAILED(hr)) {
            // Retry without explicit frame size (let MF keep default resolution).
            MFCreateMediaType(&out_mt);
            out_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out_mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out_mt);
            out_mt->Release();
        }
        if (FAILED(hr)) {
            reader->Release();
            p.set_value(false);
            return;
        }

        // Read back actual negotiated dimensions & stride.
        IMFMediaType* cur_mt = nullptr;
        UINT32 act_w = 0, act_h = 0;
        LONG default_stride = 0;
        if (SUCCEEDED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur_mt))) {
            MFGetAttributeSize(cur_mt, MF_MT_FRAME_SIZE, &act_w, &act_h);
            UINT32 s_raw = 0;
            if (SUCCEEDED(cur_mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &s_raw))) {
                default_stride = LONG(INT32(s_raw));
            } else {
                MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, act_w, &default_stride);
            }
            cur_mt->Release();
        }
        if (act_w == 0 || act_h == 0) {
            reader->Release();
            p.set_value(false);
            return;
        }
        if (default_stride == 0) default_stride = LONG(act_w * 4);

        impl_->width = int(act_w);
        impl_->height = int(act_h);
        p.set_value(true);

        Image scratch;
        scratch.allocate(int(act_w), int(act_h), PixelOrder::kBGRA);

        while (!impl_->stop.load(std::memory_order_relaxed)) {
            DWORD stream_idx = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream_idx, &flags, &ts,
                                    &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                if (sample) sample->Release();
                break;
            }
            if (!sample) continue;

            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* data = nullptr;
                LONG pitch = default_stride;
                DWORD cur_len = 0;
                IMF2DBuffer* buf2d = nullptr;
                bool locked_2d = false;

                if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&buf2d))) &&
                    buf2d) {
                    if (SUCCEEDED(buf2d->Lock2D(&data, &pitch))) locked_2d = true;
                    else {
                        buf2d->Release();
                        buf2d = nullptr;
                    }
                }
                if (!locked_2d) {
                    if (FAILED(buf->Lock(&data, nullptr, &cur_len))) data = nullptr;
                }

                if (data) {
                    const uint8_t* top_row = data;
                    ptrdiff_t step = ptrdiff_t(pitch);
                    if (pitch < 0 && !locked_2d) {
                        // Contiguous bottom-up DIB: last row in memory is top scanline.
                        top_row = data + size_t(act_h - 1) * size_t(-pitch);
                        step = ptrdiff_t(pitch);
                    }
                    for (int y = 0; y < int(act_h); ++y) {
                        std::memcpy(scratch.row(y), top_row + ptrdiff_t(y) * step, size_t(act_w) * 4);
                    }
                    if (locked_2d) buf2d->Unlock2D();
                    else buf->Unlock();

                    {
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        std::swap(impl_->latest, scratch);
                        if (scratch.width != int(act_w) || scratch.height != int(act_h)) {
                            scratch.allocate(int(act_w), int(act_h), PixelOrder::kBGRA);
                        }
                        impl_->has_frame = true;
                    }
                    impl_->cv.notify_all();
                }

                if (buf2d) buf2d->Release();
                buf->Release();
            }
            sample->Release();
        }

        reader->Release();
    });

    const bool ok = fut.get();
    if (!ok && impl_->reader_thread.joinable()) impl_->reader_thread.join();
    return ok;
}

void CameraCapture::close() {
    if (!impl_) return;
    impl_->stop.store(true, std::memory_order_release);
    if (impl_->reader_thread.joinable()) impl_->reader_thread.join();
    impl_->width = 0;
    impl_->height = 0;
    impl_->has_frame = false;
}

int CameraCapture::native_width() const { return impl_ ? impl_->width : 0; }
int CameraCapture::native_height() const { return impl_ ? impl_->height : 0; }
std::string CameraCapture::device_name() const { return impl_ ? impl_->name : ""; }

bool CameraCapture::grab(Image& out, int wait_ms) {
    if (!impl_ || impl_->width <= 0) return false;
    std::unique_lock<std::mutex> lk(impl_->mu);
    if (!impl_->has_frame) {
        if (!impl_->cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] { return impl_->has_frame; })) {
            return false;
        }
    }
    if (out.width <= 0 || out.height <= 0) {
        out.allocate(impl_->latest.width, impl_->latest.height, PixelOrder::kBGRA);
    }
    out.order = PixelOrder::kBGRA;
    resize_bilinear_32bpp(impl_->latest.data.data(), impl_->latest.width, impl_->latest.height,
                          ptrdiff_t(impl_->latest.stride), out);
    return true;
}

}  // namespace lv

#else

namespace lv {
struct CameraCapture::Impl {};
std::vector<CameraInfo> list_cameras() { return {}; }
CameraCapture::CameraCapture() = default;
CameraCapture::~CameraCapture() = default;
bool CameraCapture::open(int, int, int, int) { return false; }
void CameraCapture::close() {}
int CameraCapture::native_width() const { return 0; }
int CameraCapture::native_height() const { return 0; }
std::string CameraCapture::device_name() const { return {}; }
bool CameraCapture::grab(Image&, int) { return false; }
}  // namespace lv

#endif
