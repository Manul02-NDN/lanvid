#include "audio/multi_device_matrix.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "common/clock.hpp"
#include "common/rt_thread.hpp"
#include "audio/stream_resampler.hpp"
#include "midi/midi_device_manager.hpp"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
#define LV_MATRIX_COREAUDIO 1
#include <CoreAudio/CoreAudio.h>
#else
#define LV_MATRIX_COREAUDIO 0
#endif
#ifdef LV_HAVE_SHM_BUS
#include "audio/shm_audio_bridge.hpp"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <objbase.h>
#include <functiondiscoverykeys_devpkey.h>

#include "../drivers/windows/asio/iasio_minimal.h"
#endif

namespace lv {

namespace {

constexpr uint32_t kRingCap = 16384;  // power of two (~341 ms @ 48 kHz)

float linear_to_dbfs(float peak) {
    if (peak <= 1e-6f) return -120.0f;
    return std::max(-120.0f, 20.0f * std::log10(peak));
}

float db_to_linear(float db) {
    if (db <= -96.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

// Lock-free SPSC atomic multi-channel frame-block ring buffer (interleaved [frame][ch], up to 64 ch x kRingCap).
// Guarantees whole multi-channel frames are published/consumed atomically so even/odd channels never skew on underrun.
class LockFreePlanarRing {
public:
    void configure(int channels) {
        channels_ = std::clamp(channels, 1, kMatrixMaxChannels);
        data_.assign(size_t(kRingCap) * size_t(channels_), 0.0f);
        wr_.store(0, std::memory_order_relaxed);
        rd_.store(0, std::memory_order_relaxed);
        xruns_.store(0, std::memory_order_relaxed);
    }

    int channels() const { return channels_; }

    uint32_t available() const {
        const uint64_t w = wr_.load(std::memory_order_acquire);
        const uint64_t r = rd_.load(std::memory_order_relaxed);
        return w > r ? uint32_t(std::min<uint64_t>(w - r, kRingCap)) : 0u;
    }

    uint32_t write(const float* const* planes, uint32_t ch, uint32_t frames) {
        if (data_.empty() || frames == 0) return 0;
        const uint64_t w = wr_.load(std::memory_order_relaxed);
        const uint64_t r = rd_.load(std::memory_order_acquire);
        const uint32_t free_n = uint32_t(kRingCap - std::min<uint64_t>(w - r, kRingCap));
        uint32_t n = frames;
        if (n > free_n) {
            n = free_n;
            xruns_.fetch_add(1, std::memory_order_relaxed);
        }
        const int copy_ch = std::min(int(ch), channels_);
        for (uint32_t i = 0; i < n; ++i) {
            float* frame_dst = data_.data() + size_t((w + i) & (kRingCap - 1)) * size_t(channels_);
            for (int c = 0; c < channels_; ++c) {
                frame_dst[c] = (c < copy_ch && planes && planes[c]) ? planes[c][i] : 0.0f;
            }
        }
        // Single atomic release publishes all `channels_` for all `n` frames together.
        wr_.store(w + n, std::memory_order_release);
        return n;
    }

    uint32_t read(float* const* planes, uint32_t ch, uint32_t frames, bool zero_pad = true) {
        if (data_.empty() || frames == 0) return 0;
        const uint64_t w = wr_.load(std::memory_order_acquire);
        const uint64_t r = rd_.load(std::memory_order_relaxed);
        const uint32_t avail = w > r ? uint32_t(std::min<uint64_t>(w - r, kRingCap)) : 0u;
        const uint32_t n = std::min(frames, avail);
        if (n < frames) {
            xruns_.fetch_add(1, std::memory_order_relaxed);
        }
        const int out_ch = int(ch);
        for (uint32_t i = 0; i < n; ++i) {
            const float* frame_src = data_.data() + size_t((r + i) & (kRingCap - 1)) * size_t(channels_);
            for (int c = 0; c < out_ch; ++c) {
                if (planes && planes[c]) planes[c][i] = (c < channels_) ? frame_src[c] : 0.0f;
            }
        }
        if (zero_pad && n < frames) {
            for (int c = 0; c < out_ch; ++c) {
                if (planes && planes[c]) std::memset(planes[c] + n, 0, size_t(frames - n) * sizeof(float));
            }
        }
        // Single atomic release advances all `channels_` together by `n` whole frames.
        rd_.store(r + n, std::memory_order_release);
        return n;
    }

    uint64_t xruns() const { return xruns_.load(std::memory_order_relaxed); }

private:
    int channels_ = 2;
    std::vector<float> data_;
    alignas(64) std::atomic<uint64_t> wr_{0};
    alignas(64) std::atomic<uint64_t> rd_{0};
    std::atomic<uint64_t> xruns_{0};
};

#if defined(_WIN32)
std::string wide_to_utf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

// ---- Windows WASAPI Shared Mode Render/Capture Backend with 44.1/96 -> 48 kHz Resampler ----
class WasapiSlotBackend {
public:
    ~WasapiSlotBackend() { stop(); }

    bool start(MatrixSlotRole role, int device_index, const std::string& device_name, int channels,
               LockFreePlanarRing* in_ring, LockFreePlanarRing* out_ring) {
        stop();
        role_ = role;
        device_index_ = device_index;
        device_name_ = device_name;
        channels_ = std::clamp(channels, 1, 8);
        in_ring_ = in_ring;
        out_ring_ = out_ring;
        stop_.store(false, std::memory_order_relaxed);
        connected_.store(false, std::memory_order_relaxed);
        thread_ = std::thread([this]() { run(); });
        return true;
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        connected_.store(false, std::memory_order_relaxed);
    }

    bool connected() const { return connected_.load(std::memory_order_acquire); }
    uint32_t sample_rate() const { return dev_rate_.load(std::memory_order_acquire); }

private:
    IMMDevice* pick_device(IMMDeviceEnumerator* en, EDataFlow flow) {
        IMMDevice* dev = nullptr;
        if (device_index_ < 0 && device_name_.empty()) {
            en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
            return dev;
        }
        IMMDeviceCollection* col = nullptr;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || !col) {
            en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
            return dev;
        }
        UINT count = 0;
        col->GetCount(&count);
        if (!device_name_.empty()) {
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* cand = nullptr;
                if (SUCCEEDED(col->Item(i, &cand)) && cand) {
                    IPropertyStore* ps = nullptr;
                    if (SUCCEEDED(cand->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                            if (wide_to_utf8(pv.pwszVal) == device_name_) dev = cand;
                        }
                        PropVariantClear(&pv);
                        ps->Release();
                    }
                    if (dev) break;
                    cand->Release();
                }
            }
        }
        if (!dev && device_index_ >= 0 && UINT(device_index_) < count) {
            col->Item(UINT(device_index_), &dev);
        }
        col->Release();
        if (!dev) en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
        return dev;
    }

    void run() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool need_uninit = SUCCEEDED(hr);
        IMMDeviceEnumerator* en = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
        if (FAILED(hr) || !en) {
            if (need_uninit) CoUninitialize();
            return;
        }

        const bool is_capture = (role_ == MatrixSlotRole::kInput);
        IMMDevice* dev = pick_device(en, is_capture ? eCapture : eRender);
        en->Release();
        if (!dev) {
            if (need_uninit) CoUninitialize();
            return;
        }

        IAudioClient* client = nullptr;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        dev->Release();
        if (FAILED(hr) || !client) {
            if (need_uninit) CoUninitialize();
            return;
        }

        WAVEFORMATEX* wfx = nullptr;
        if (FAILED(client->GetMixFormat(&wfx)) || !wfx) {
            client->Release();
            if (need_uninit) CoUninitialize();
            return;
        }

        const uint32_t dev_sr = wfx->nSamplesPerSec ? wfx->nSamplesPerSec : 48000;
        const uint32_t dev_ch = std::clamp<uint32_t>(wfx->nChannels, 1, 8);
        dev_rate_.store(dev_sr, std::memory_order_release);

        bool is_float = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
            is_float = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        }

        const REFERENCE_TIME hns_dur = 200000;  // 20 ms shared buffer
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hns_dur, 0, wfx, nullptr);
        CoTaskMemFree(wfx);
        if (FAILED(hr)) {
            client->Release();
            if (need_uninit) CoUninitialize();
            return;
        }

        UINT32 buf_frames = 0;
        client->GetBufferSize(&buf_frames);

        IAudioRenderClient* render_cli = nullptr;
        IAudioCaptureClient* cap_cli = nullptr;
        if (is_capture) {
            hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap_cli));
        } else {
            hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_cli));
        }
        if (FAILED(hr) || (!render_cli && !cap_cli)) {
            client->Release();
            if (need_uninit) CoUninitialize();
            return;
        }

        if (FAILED(client->Start())) {
            if (render_cli) render_cli->Release();
            if (cap_cli) cap_cli->Release();
            client->Release();
            if (need_uninit) CoUninitialize();
            return;
        }

        connected_.store(true, std::memory_order_release);
        timeBeginPeriod(1);

        // Linear resampler state between 48000 Hz matrix and dev_sr (handles 44.1 kHz / 96 kHz + drift)
        std::vector<std::vector<float>> tmp_planes(dev_ch, std::vector<float>(2048, 0.0f));
        std::vector<float*> ptrs(dev_ch);
        for (uint32_t c = 0; c < dev_ch; ++c) ptrs[c] = tmp_planes[c].data();

        double phase = 0.0;
        std::vector<float> prev_samp(dev_ch, 0.0f);

        while (!stop_.load(std::memory_order_acquire)) {
            if (!is_capture && render_cli && out_ring_) {
                UINT32 pad = 0;
                if (FAILED(client->GetCurrentPadding(&pad))) break;
                const UINT32 avail_out = buf_frames > pad ? (buf_frames - pad) : 0;
                if (avail_out >= 64) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(render_cli->GetBuffer(avail_out, &dst)) && dst) {
                        // Adaptive drift trim (±0.05%) around 512-frame target fill in out_ring_
                        const uint32_t ring_fill = out_ring_->available();
                        double drift = 1.0;
                        if (ring_fill > 1536) drift = 1.0005;
                        else if (ring_fill < 256) drift = 0.9995;
                        const double step = (double(kMatrixSampleRate) / double(dev_sr)) * drift;

                        const uint32_t need_in = std::min<uint32_t>(
                            2040, uint32_t(std::ceil(phase + double(avail_out) * step)) + 2);
                        const uint32_t got = out_ring_->read(ptrs.data(), dev_ch, need_in, true);
                        (void)got;

                        for (UINT32 i = 0; i < avail_out; ++i) {
                            const uint32_t idx0 = uint32_t(phase);
                            const float frac = float(phase - double(idx0));
                            const uint32_t idx1 = std::min(idx0 + 1, need_in - 1);
                            for (uint32_t c = 0; c < dev_ch; ++c) {
                                const float s0 = (idx0 < need_in) ? tmp_planes[c][idx0] : prev_samp[c];
                                const float s1 = (idx1 < need_in) ? tmp_planes[c][idx1] : s0;
                                const float v = std::clamp(s0 + (s1 - s0) * frac, -1.0f, 1.0f);
                                if (is_float) {
                                    reinterpret_cast<float*>(dst)[size_t(i) * dev_ch + c] = v;
                                } else {
                                    reinterpret_cast<int16_t*>(dst)[size_t(i) * dev_ch + c] =
                                        int16_t(std::lrint(v * 32767.0f));
                                }
                            }
                            phase += step;
                        }
                        const uint32_t consumed = uint32_t(phase);
                        phase -= double(consumed);
                        if (consumed > 0 && consumed <= need_in) {
                            for (uint32_t c = 0; c < dev_ch; ++c) prev_samp[c] = tmp_planes[c][consumed - 1];
                        }
                        render_cli->ReleaseBuffer(avail_out, 0);
                    }
                }
            } else if (is_capture && cap_cli && in_ring_) {
                UINT32 pkt_len = 0;
                while (SUCCEEDED(cap_cli->GetNextPacketSize(&pkt_len)) && pkt_len > 0) {
                    BYTE* src = nullptr;
                    UINT32 nframes = 0;
                    DWORD flags = 0;
                    if (FAILED(cap_cli->GetBuffer(&src, &nframes, &flags, nullptr, nullptr))) break;
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    const double step = double(dev_sr) / double(kMatrixSampleRate);
                    const uint32_t out_n = std::min<uint32_t>(2040, uint32_t(double(nframes) / step));
                    for (uint32_t i = 0; i < out_n; ++i) {
                        const double pos = double(i) * step;
                        const uint32_t i0 = std::min<uint32_t>(uint32_t(pos), nframes > 0 ? nframes - 1 : 0);
                        const uint32_t i1 = std::min<uint32_t>(i0 + 1, nframes > 0 ? nframes - 1 : 0);
                        const float frac = float(pos - double(i0));
                        for (uint32_t c = 0; c < dev_ch; ++c) {
                            float v = 0.0f;
                            if (!silent && src) {
                                if (is_float) {
                                    const auto* fp = reinterpret_cast<const float*>(src);
                                    const float s0 = fp[size_t(i0) * dev_ch + c];
                                    const float s1 = fp[size_t(i1) * dev_ch + c];
                                    v = s0 + (s1 - s0) * frac;
                                } else {
                                    const auto* sp = reinterpret_cast<const int16_t*>(src);
                                    const float s0 = float(sp[size_t(i0) * dev_ch + c]) / 32768.0f;
                                    const float s1 = float(sp[size_t(i1) * dev_ch + c]) / 32768.0f;
                                    v = s0 + (s1 - s0) * frac;
                                }
                            }
                            tmp_planes[c][i] = v;
                        }
                    }
                    std::vector<const float*> cptrs(dev_ch);
                    for (uint32_t c = 0; c < dev_ch; ++c) cptrs[c] = tmp_planes[c].data();
                    in_ring_->write(cptrs.data(), dev_ch, out_n);
                    cap_cli->ReleaseBuffer(nframes);
                }
            }
            Sleep(2);
        }

        timeEndPeriod(1);
        client->Stop();
        if (render_cli) render_cli->Release();
        if (cap_cli) cap_cli->Release();
        client->Release();
        connected_.store(false, std::memory_order_release);
        if (need_uninit) CoUninitialize();
    }

    MatrixSlotRole role_ = MatrixSlotRole::kOutput;
    int device_index_ = -1;
    std::string device_name_;
    int channels_ = 2;
    LockFreePlanarRing* in_ring_ = nullptr;
    LockFreePlanarRing* out_ring_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> dev_rate_{kMatrixSampleRate};
};

// ---- Windows External ASIO Host Backend (loads any installed 64-bit ASIO driver via COM) ----
class ExternalAsioSlotBackend {
public:
    ~ExternalAsioSlotBackend() { stop(); }

    bool start(const std::string& driver_name, int channels, int buffer_size,
               LockFreePlanarRing* in_ring, LockFreePlanarRing* out_ring) {
        stop();
        if (driver_name.empty()) return false;
        // Find CLSID in HKLM\SOFTWARE\ASIO\<driver_name>
        const std::wstring sub = L"SOFTWARE\\ASIO\\" + utf8_to_wide(driver_name);
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) {
            return false;
        }
        wchar_t clsid_buf[128] = {};
        DWORD sz = sizeof(clsid_buf), type = 0;
        const bool ok_reg = RegQueryValueExW(hk, L"CLSID", nullptr, &type,
                                             reinterpret_cast<BYTE*>(clsid_buf), &sz) == ERROR_SUCCESS &&
                            type == REG_SZ;
        RegCloseKey(hk);
        if (!ok_reg) return false;

        CLSID clsid{};
        if (FAILED(CLSIDFromString(clsid_buf, &clsid))) return false;

        in_ring_ = in_ring;
        out_ring_ = out_ring;
        want_ch_ = std::clamp(channels, 2, kMatrixMaxChannels);
        want_buf_ = std::clamp(buffer_size, 64, 1024);
        clsid_ = clsid;
        stop_.store(false, std::memory_order_relaxed);
        thread_ = std::thread([this]() { run(); });
        return true;
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        connected_.store(false, std::memory_order_relaxed);
    }

    bool connected() const { return connected_.load(std::memory_order_acquire); }
    uint32_t sample_rate() const { return dev_rate_.load(std::memory_order_acquire); }

private:
    static inline ExternalAsioSlotBackend* s_active_ = nullptr;

    static void cb_bufferSwitch(long idx, ASIOBool) {
        auto* self = s_active_;
        if (!self) return;
        self->on_switch(idx);
    }
    static void cb_rateChanged(ASIOSampleRate) {}
    static long cb_message(long sel, long val, void*, double*) {
        if (sel == kAsioSelectorSupported) return (val == kAsioEngineVersion) ? 1 : 0;
        if (sel == kAsioEngineVersion) return 2;
        return 0;
    }
    static ASIOTime* cb_switchTime(ASIOTime* p, long idx, ASIOBool d) {
        cb_bufferSwitch(idx, d);
        return p;
    }

    void on_switch(long idx) {
        const uint32_t n = uint32_t(buf_size_);
        // Inputs from external ASIO -> matrix in_ring_
        if (in_ch_ > 0 && in_ring_) {
            for (long c = 0; c < in_ch_; ++c) {
                const void* src = buf_infos_[size_t(c)].buffers[idx];
                float* dst = scratch_in_[size_t(c)].data();
                if (!src) { std::memset(dst, 0, n * sizeof(float)); continue; }
                if (in_types_[size_t(c)] == ASIOSTFloat32LSB) {
                    std::memcpy(dst, src, n * sizeof(float));
                } else {
                    const auto* s32 = static_cast<const int32_t*>(src);
                    for (uint32_t i = 0; i < n; ++i) dst[i] = float(double(s32[i]) / 2147483648.0);
                }
            }
            in_ring_->write(in_ptrs_.data(), uint32_t(in_ch_), n);
        }
        // Outputs from matrix out_ring_ -> external ASIO
        if (out_ch_ > 0 && out_ring_) {
            out_ring_->read(out_ptrs_.data(), uint32_t(out_ch_), n, true);
            for (long c = 0; c < out_ch_; ++c) {
                void* dst = buf_infos_[size_t(in_ch_ + c)].buffers[idx];
                const float* src = scratch_out_[size_t(c)].data();
                if (!dst) continue;
                if (out_types_[size_t(c)] == ASIOSTFloat32LSB) {
                    std::memcpy(dst, src, n * sizeof(float));
                } else {
                    auto* d32 = static_cast<int32_t*>(dst);
                    for (uint32_t i = 0; i < n; ++i)
                        d32[i] = int32_t(std::lrint(std::clamp(double(src[i]), -1.0, 1.0) * 2147483647.0));
                }
            }
        }
    }

    void run() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool need_uninit = SUCCEEDED(hr);
        IASIO* asio = nullptr;
        hr = CoCreateInstance(clsid_, nullptr, CLSCTX_INPROC_SERVER, clsid_, reinterpret_cast<void**>(&asio));
        if (FAILED(hr) || !asio) {
            if (need_uninit) CoUninitialize();
            return;
        }
        if (!asio->init(nullptr)) {
            asio->Release();
            if (need_uninit) CoUninitialize();
            return;
        }
        long n_in = 0, n_out = 0;
        asio->getChannels(&n_in, &n_out);
        in_ch_ = std::min<long>(n_in, want_ch_);
        out_ch_ = std::min<long>(n_out, want_ch_);
        long mn = 64, mx = 1024, pref = 256, gran = 64;
        asio->getBufferSize(&mn, &mx, &pref, &gran);
        buf_size_ = (want_buf_ >= mn && want_buf_ <= mx) ? want_buf_ : pref;
        ASIOSampleRate sr = 48000.0;
        if (asio->canSampleRate(48000.0) == ASE_OK) asio->setSampleRate(48000.0);
        asio->getSampleRate(&sr);
        dev_rate_.store(uint32_t(sr), std::memory_order_release);

        buf_infos_.assign(size_t(in_ch_ + out_ch_), ASIOBufferInfo{});
        in_types_.assign(size_t(in_ch_), ASIOSTInt32LSB);
        out_types_.assign(size_t(out_ch_), ASIOSTInt32LSB);
        scratch_in_.assign(size_t(in_ch_), std::vector<float>(size_t(buf_size_), 0.0f));
        scratch_out_.assign(size_t(out_ch_), std::vector<float>(size_t(buf_size_), 0.0f));
        in_ptrs_.resize(size_t(in_ch_));
        out_ptrs_.resize(size_t(out_ch_));

        for (long c = 0; c < in_ch_; ++c) {
            buf_infos_[size_t(c)].isInput = ASIOTrue;
            buf_infos_[size_t(c)].channelNum = c;
            ASIOChannelInfo ci{};
            ci.channel = c;
            ci.isInput = ASIOTrue;
            if (asio->getChannelInfo(&ci) == ASE_OK) in_types_[size_t(c)] = ci.type;
            in_ptrs_[size_t(c)] = scratch_in_[size_t(c)].data();
        }
        for (long c = 0; c < out_ch_; ++c) {
            buf_infos_[size_t(in_ch_ + c)].isInput = ASIOFalse;
            buf_infos_[size_t(in_ch_ + c)].channelNum = c;
            ASIOChannelInfo ci{};
            ci.channel = c;
            ci.isInput = ASIOFalse;
            if (asio->getChannelInfo(&ci) == ASE_OK) out_types_[size_t(c)] = ci.type;
            out_ptrs_[size_t(c)] = scratch_out_[size_t(c)].data();
        }

        ASIOCallbacks cbs{&cb_bufferSwitch, &cb_rateChanged, &cb_message, &cb_switchTime};
        s_active_ = this;
        if (asio->createBuffers(buf_infos_.data(), in_ch_ + out_ch_, buf_size_, &cbs) == ASE_OK &&
            asio->start() == ASE_OK) {
            connected_.store(true, std::memory_order_release);
            while (!stop_.load(std::memory_order_acquire)) Sleep(10);
            asio->stop();
            asio->disposeBuffers();
        }
        connected_.store(false, std::memory_order_release);
        if (s_active_ == this) s_active_ = nullptr;
        asio->Release();
        if (need_uninit) CoUninitialize();
    }

    CLSID clsid_{};
    int want_ch_ = 2;
    int want_buf_ = 256;
    long in_ch_ = 0, out_ch_ = 0, buf_size_ = 256;
    LockFreePlanarRing* in_ring_ = nullptr;
    LockFreePlanarRing* out_ring_ = nullptr;
    std::vector<ASIOBufferInfo> buf_infos_;
    std::vector<ASIOSampleType> in_types_, out_types_;
    std::vector<std::vector<float>> scratch_in_, scratch_out_;
    std::vector<const float*> in_ptrs_;
    std::vector<float*> out_ptrs_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> dev_rate_{kMatrixSampleRate};
};
#endif

#if LV_MATRIX_COREAUDIO
// ---------------------------------------------------------------------------------------------
// macOS CoreAudio HAL backend for kSystemAudio slots (built-in speakers / headphones / USB, etc.).
// An AudioDeviceIOProc runs on the device's real-time thread and moves audio between the device
// (any nominal rate, any channel layout) and the slot's 48 kHz lock-free rings. Everything the
// IOProc touches is preallocated; it only uses atomics, ring copies and arithmetic.

const char* kLanVidHalUid = "LanVidVirtual64ch_UID";

std::string cf_to_utf8(CFStringRef s) {
    if (!s) return {};
    char buf[512] = {};
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return {};
}

std::string ca_device_string(AudioObjectID dev, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a{sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = nullptr;
    UInt32 sz = sizeof(s);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &s) != noErr || !s) return {};
    std::string out = cf_to_utf8(s);
    CFRelease(s);
    return out;
}

int ca_device_channels(AudioObjectID dev, bool output) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration,
                                 output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput,
                                 kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz == 0) return 0;
    std::vector<uint8_t> mem(sz);
    auto* abl = reinterpret_cast<AudioBufferList*>(mem.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, abl) != noErr) return 0;
    int ch = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) ch += int(abl->mBuffers[i].mNumberChannels);
    return ch;
}

double ca_device_rate(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    Float64 r = 0;
    UInt32 sz = sizeof(r);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &r) != noErr) return 0.0;
    return r;
}

AudioObjectID ca_default_device(bool output) {
    AudioObjectPropertyAddress a{output ? kAudioHardwarePropertyDefaultOutputDevice : kAudioHardwarePropertyDefaultInputDevice,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 sz = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &dev);
    return dev;
}

AudioObjectID ca_device_for_uid(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;
    CFStringRef cf = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    AudioObjectID dev = kAudioObjectUnknown;
    AudioValueTranslation t{&cf, sizeof(cf), &dev, sizeof(dev)};
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    UInt32 sz = sizeof(t);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &t);
    CFRelease(cf);
    return dev;
}

class CoreAudioSlotBackend {
public:
    static constexpr uint32_t kMaxBlock = 4096;

    ~CoreAudioSlotBackend() { stop(); }

    // `uid` empty = current default device for the role. Duplex opens both directions of one device.
    bool start(MatrixSlotRole role, const std::string& uid, int channels, int buffer_size,
               LockFreePlanarRing* in_ring, LockFreePlanarRing* out_ring) {
        stop();
        role_ = role;
        want_out_ = role != MatrixSlotRole::kInput;
        want_in_ = role != MatrixSlotRole::kOutput;
        ch_ = std::clamp(channels, 1, kMatrixMaxChannels);
        block_ = uint32_t(std::clamp(buffer_size, 32, 2048));
        in_ring_ = in_ring;
        out_ring_ = out_ring;
        dev_ = uid.empty() ? ca_default_device(want_out_) : ca_device_for_uid(uid);
        if (dev_ == kAudioObjectUnknown) { status_ = "CoreAudio: device not found"; return false; }
        if (ca_device_string(dev_, kAudioDevicePropertyDeviceUID) == kLanVidHalUid) {
            status_ = "CoreAudio: LanVid Virtual 64ch is the Virtual Bus slot";
            dev_ = kAudioObjectUnknown;
            return false;
        }
        name_ = ca_device_string(dev_, kAudioObjectPropertyName);
        const double r = ca_device_rate(dev_);
        rate_.store(r > 1000.0 ? r : 48000.0, std::memory_order_release);

        // Preallocate everything the IOProc needs.
        for (auto& v : tmp_a_) v.assign(kMaxBlock * 3, 0.0f);
        for (auto& v : tmp_b_) v.assign(kMaxBlock * 3, 0.0f);
        rs_out_.configure(ch_, kMaxBlock * 3);
        rs_in_.configure(ch_, kMaxBlock * 3);

        UInt32 bs = block_;
        AudioObjectPropertyAddress ba{kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain};
        AudioObjectSetPropertyData(dev_, &ba, 0, nullptr, sizeof(bs), &bs);  // best effort

        AudioObjectPropertyAddress ra{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain};
        AudioObjectAddPropertyListener(dev_, &ra, &CoreAudioSlotBackend::on_rate_changed, this);
        listening_ = true;

        if (AudioDeviceCreateIOProcID(dev_, &CoreAudioSlotBackend::io_proc, this, &proc_) != noErr || !proc_) {
            status_ = "CoreAudio: cannot create IOProc";
            stop();
            return false;
        }
        if (AudioDeviceStart(dev_, proc_) != noErr) {
            status_ = "CoreAudio: device start failed";
            stop();
            return false;
        }
        running_.store(true, std::memory_order_release);
        status_ = "CoreAudio Active";
        return true;
    }

    void stop() {
        if (dev_ != kAudioObjectUnknown && proc_) {
            AudioDeviceStop(dev_, proc_);
            AudioDeviceDestroyIOProcID(dev_, proc_);
        }
        if (dev_ != kAudioObjectUnknown && listening_) {
            AudioObjectPropertyAddress ra{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
            AudioObjectRemovePropertyListener(dev_, &ra, &CoreAudioSlotBackend::on_rate_changed, this);
        }
        listening_ = false;
        proc_ = nullptr;
        dev_ = kAudioObjectUnknown;
        running_.store(false, std::memory_order_release);
        callbacks_.store(0, std::memory_order_relaxed);
    }

    // Connected = the device is actually calling us back.
    bool connected() const { return running_.load(std::memory_order_acquire) && callbacks_.load() > 0; }
    uint32_t sample_rate() const { return uint32_t(rate_.load(std::memory_order_acquire) + 0.5); }
    const std::string& device_name() const { return name_; }
    const std::string& status() const { return status_; }
    uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }

private:
    static OSStatus on_rate_changed(AudioObjectID dev, UInt32, const AudioObjectPropertyAddress*, void* ud) {
        auto* self = static_cast<CoreAudioSlotBackend*>(ud);
        const double r = ca_device_rate(dev);
        if (r > 1000.0) self->rate_.store(r, std::memory_order_release);
        return noErr;
    }

    static OSStatus io_proc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                            AudioBufferList* out, const AudioTimeStamp*, void* ud) {
        auto* self = static_cast<CoreAudioSlotBackend*>(ud);
        self->callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (self->want_out_ && out && out->mNumberBuffers) self->render(out);
        if (self->want_in_ && in && in->mNumberBuffers) self->capture(in);
        return noErr;
    }

    // matrix (48 kHz) -> device
    void render(AudioBufferList* out) {
        const UInt32 nc0 = std::max<UInt32>(1, out->mBuffers[0].mNumberChannels);
        const uint32_t n = std::min<uint32_t>(kMaxBlock, out->mBuffers[0].mDataByteSize / (sizeof(float) * nc0));
        double ratio = double(kMatrixSampleRate) / rate_.load(std::memory_order_relaxed);
        // Drift trim: keep the matrix->device ring around 2 blocks.
        const uint32_t fill = out_ring_ ? out_ring_->available() : 0;
        if (fill > 3 * block_) ratio *= 1.0005;
        else if (fill < block_ / 2) ratio *= 0.9995;
        float* a[kMatrixMaxChannels];
        float* b[kMatrixMaxChannels];
        for (int c = 0; c < ch_; ++c) { a[c] = tmp_a_[c].data(); b[c] = tmp_b_[c].data(); }
        const uint32_t need = rs_out_.needed_for(n, ratio);
        if (rs_out_.buffered() < need && out_ring_) {
            const uint32_t pull = std::min<uint32_t>(need - rs_out_.buffered(), rs_out_.space());
            out_ring_->read(a, uint32_t(ch_), pull, true);  // zero-padded on underrun
            const float* ca[kMatrixMaxChannels];
            for (int c = 0; c < ch_; ++c) ca[c] = a[c];
            rs_out_.push(ca, ch_, pull);
        }
        const uint32_t got = rs_out_.produce(b, ch_, n, ratio);
        for (int c = 0; c < ch_; ++c) if (got < n) std::memset(b[c] + got, 0, size_t(n - got) * sizeof(float));
        // Scatter into the device's buffers (interleaved per buffer); extra device channels get silence.
        int dev_ch = 0;
        for (UInt32 bi = 0; bi < out->mNumberBuffers; ++bi) {
            AudioBuffer& ab = out->mBuffers[bi];
            float* d = static_cast<float*>(ab.mData);
            const UInt32 nc = ab.mNumberChannels;
            if (!d || nc == 0) continue;
            const uint32_t frames = std::min<uint32_t>(n, ab.mDataByteSize / (sizeof(float) * nc));
            for (UInt32 k = 0; k < nc; ++k, ++dev_ch) {
                const float* src = dev_ch < ch_ ? b[dev_ch] : nullptr;
                for (uint32_t i = 0; i < frames; ++i) d[size_t(i) * nc + k] = src ? src[i] : 0.0f;
            }
        }
    }

    // device -> matrix (48 kHz)
    void capture(const AudioBufferList* in) {
        const UInt32 nc0 = std::max<UInt32>(1, in->mBuffers[0].mNumberChannels);
        const uint32_t n = std::min<uint32_t>(kMaxBlock, in->mBuffers[0].mDataByteSize / (sizeof(float) * nc0));
        float* a[kMatrixMaxChannels];
        float* b[kMatrixMaxChannels];
        for (int c = 0; c < ch_; ++c) {
            a[c] = tmp_a_[c].data() + kMaxBlock;  // separate region from render()'s use
            b[c] = tmp_b_[c].data() + kMaxBlock;
        }
        // Gather slot channels from the device's (possibly several) interleaved buffers.
        int dev_ch = 0;
        for (int c = 0; c < ch_; ++c) std::memset(a[c], 0, size_t(n) * sizeof(float));
        for (UInt32 bi = 0; bi < in->mNumberBuffers; ++bi) {
            const AudioBuffer& ab = in->mBuffers[bi];
            const float* d = static_cast<const float*>(ab.mData);
            const UInt32 nc = ab.mNumberChannels;
            if (nc == 0) continue;
            const uint32_t frames = std::min<uint32_t>(n, ab.mDataByteSize / (sizeof(float) * nc));
            for (UInt32 k = 0; k < nc; ++k, ++dev_ch) {
                if (dev_ch >= ch_ || !d) continue;
                for (uint32_t i = 0; i < frames; ++i) a[dev_ch][i] = d[size_t(i) * nc + k];
            }
        }
        const float* ca[kMatrixMaxChannels];
        for (int c = 0; c < ch_; ++c) ca[c] = a[c];
        rs_in_.push(ca, ch_, n);
        double ratio = rate_.load(std::memory_order_relaxed) / double(kMatrixSampleRate);
        const uint32_t fill = in_ring_ ? in_ring_->available() : 0;
        if (fill > 4 * block_) ratio *= 1.0005;       // mixer is slower than the device: emit less
        else if (fill < block_) ratio *= 0.9995;      // mixer is faster: emit a little more
        const uint32_t got = rs_in_.produce(b, ch_, kMaxBlock, ratio);
        if (got && in_ring_) {
            const float* cb[kMatrixMaxChannels];
            for (int c = 0; c < ch_; ++c) cb[c] = b[c];
            in_ring_->write(cb, uint32_t(ch_), got);
        }
    }

    MatrixSlotRole role_ = MatrixSlotRole::kOutput;
    bool want_out_ = true, want_in_ = false;
    int ch_ = 2;
    uint32_t block_ = 256;
    LockFreePlanarRing* in_ring_ = nullptr;
    LockFreePlanarRing* out_ring_ = nullptr;
    AudioObjectID dev_ = kAudioObjectUnknown;
    AudioDeviceIOProcID proc_ = nullptr;
    bool listening_ = false;
    std::string name_;
    std::string status_ = "CoreAudio Stopped";
    std::atomic<double> rate_{48000.0};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> callbacks_{0};
    StreamResampler rs_out_, rs_in_;
    std::array<std::vector<float>, kMatrixMaxChannels> tmp_a_, tmp_b_;
};
#endif  // LV_MATRIX_COREAUDIO

}  // namespace

std::vector<SystemAudioDeviceEntry> list_system_audio_devices(bool output, bool exclude_lanvid) {
    std::vector<SystemAudioDeviceEntry> out;
#if LV_MATRIX_COREAUDIO
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr || sz == 0) return out;
    std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr) return out;
    const AudioObjectID def = ca_default_device(output);
    for (AudioObjectID dev : ids) {
        const int ch = ca_device_channels(dev, output);
        if (ch <= 0) continue;
        SystemAudioDeviceEntry e;
        e.uid = ca_device_string(dev, kAudioDevicePropertyDeviceUID);
        if (exclude_lanvid && e.uid == kLanVidHalUid) continue;
        e.name = ca_device_string(dev, kAudioObjectPropertyName);
        e.channels = ch;
        e.sample_rate = uint32_t(ca_device_rate(dev) + 0.5);
        e.is_default = dev == def;
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.is_default > y.is_default; });
#else
    (void)output;
    (void)exclude_lanvid;
#endif
    return out;
}

std::vector<AsioDriverEntry> list_external_asio_drivers(bool exclude_lanvid) {
    std::vector<AsioDriverEntry> out;
#if defined(_WIN32)
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return out;
    }
    DWORD idx = 0;
    wchar_t subname[256];
    DWORD sublen = 256;
    while (RegEnumKeyExW(root, idx++, subname, &sublen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        const std::string name = wide_to_utf8(subname);
        if (!exclude_lanvid || name != "LanVid Virtual ASIO") {
            HKEY sub = nullptr;
            if (RegOpenKeyExW(root, subname, 0, KEY_READ | KEY_WOW64_64KEY, &sub) == ERROR_SUCCESS) {
                wchar_t clsid_w[128] = {};
                DWORD sz = sizeof(clsid_w), type = 0;
                if (RegQueryValueExW(sub, L"CLSID", nullptr, &type,
                                     reinterpret_cast<BYTE*>(clsid_w), &sz) == ERROR_SUCCESS &&
                    type == REG_SZ) {
                    out.push_back({name, wide_to_utf8(clsid_w)});
                }
                RegCloseKey(sub);
            }
        }
        sublen = 256;
    }
    RegCloseKey(root);
#else
    (void)exclude_lanvid;
#endif
    return out;
}

struct MultiDeviceAudioMatrix::Impl {
    struct SlotState {
        bool used = false;
        MatrixSlotConfig cfg{};
        LockFreePlanarRing in_ring;   // device/source -> matrix
        LockFreePlanarRing out_ring;  // matrix -> device/sink
        std::atomic<float> peak_in_dbfs{-120.0f};
        std::atomic<float> peak_out_dbfs{-120.0f};
#ifdef LV_HAVE_SHM_BUS
        std::unique_ptr<ShmAudioBus> shm_bus;
#endif
#if defined(_WIN32)
        std::unique_ptr<WasapiSlotBackend> wasapi;
        std::unique_ptr<ExternalAsioSlotBackend> asio;
#endif
#if LV_MATRIX_COREAUDIO
        std::unique_ptr<CoreAudioSlotBackend> coreaudio;
#endif
    };

    struct MidiSlotState {
        bool used = false;
        MidiSlotConfig cfg{};
        std::unique_ptr<MidiInputPort> in_port;
        std::unique_ptr<MidiOutputPort> out_port;
        std::atomic<uint64_t> events_count{0};
        std::atomic<uint64_t> last_activity_us{0};
    };

    mutable std::mutex mu;
    std::array<SlotState, kMatrixMaxSlots> slots{};
    std::vector<MatrixCrosspoint> crosspoints;
    std::array<MidiSlotState, kMatrixMaxMidiSlots> midi_slots{};
    std::vector<MidiCrosspoint> midi_crosspoints;
    MidiOutSinkFn midi_sink;
    bool feedback_guard = true;
    int buffer_size = 256;

    NetworkPushFn net_push;
    NetworkPopFn net_pop;

    std::thread mix_thread;
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> running{false};

    // Scratch buffers owned exclusively by mixer thread (or process_block_sync)
    std::array<std::vector<std::vector<float>>, kMatrixMaxSlots> src_bufs;
    std::array<std::vector<std::vector<float>>, kMatrixMaxSlots> dst_bufs;

    void ensure_scratch(uint32_t frames) {
        for (int s = 0; s < kMatrixMaxSlots; ++s) {
            if (int(src_bufs[s].size()) < kMatrixMaxChannels) {
                src_bufs[s].resize(kMatrixMaxChannels);
                dst_bufs[s].resize(kMatrixMaxChannels);
            }
            for (int c = 0; c < kMatrixMaxChannels; ++c) {
                if (src_bufs[s][c].size() < frames) src_bufs[s][c].resize(frames, 0.0f);
                if (dst_bufs[s][c].size() < frames) dst_bufs[s][c].resize(frames, 0.0f);
            }
        }
    }

    void start_slot_backend(SlotState& st) {
        if (!st.used || !st.cfg.active) return;
        st.in_ring.configure(st.cfg.channels);
        st.out_ring.configure(st.cfg.channels);
#ifdef LV_HAVE_SHM_BUS
        if (st.cfg.type == MatrixSlotType::kVirtualBus) {
            st.shm_bus = std::make_unique<ShmAudioBus>();
            st.shm_bus->open();
        }
#endif
#if defined(_WIN32)
        if (st.cfg.type == MatrixSlotType::kSystemAudio) {
            st.wasapi = std::make_unique<WasapiSlotBackend>();
            st.wasapi->start(st.cfg.role, st.cfg.device_index, st.cfg.device_id, st.cfg.channels,
                             &st.in_ring, &st.out_ring);
        } else if (st.cfg.type == MatrixSlotType::kExternalAsio) {
            st.asio = std::make_unique<ExternalAsioSlotBackend>();
            st.asio->start(st.cfg.device_id, st.cfg.channels, st.cfg.buffer_size, &st.in_ring, &st.out_ring);
        }
#endif
#if LV_MATRIX_COREAUDIO
        if (st.cfg.type == MatrixSlotType::kSystemAudio) {
            st.coreaudio = std::make_unique<CoreAudioSlotBackend>();
            st.coreaudio->start(st.cfg.role, st.cfg.device_id, st.cfg.channels, st.cfg.buffer_size,
                                &st.in_ring, &st.out_ring);
        }
#endif
    }

    void stop_slot_backend(SlotState& st) {
#if LV_MATRIX_COREAUDIO
        if (st.coreaudio) { st.coreaudio->stop(); st.coreaudio.reset(); }
#endif
#if defined(_WIN32)
        if (st.wasapi) { st.wasapi->stop(); st.wasapi.reset(); }
        if (st.asio) { st.asio->stop(); st.asio.reset(); }
#endif
#ifdef LV_HAVE_SHM_BUS
        if (st.shm_bus) { st.shm_bus->close(); st.shm_bus.reset(); }
#endif
    }

    void process_block(uint32_t frames) {
        ensure_scratch(frames);

        // Snapshot routing + active slots briefly under lock
        std::vector<MatrixCrosspoint> cps;
        std::array<bool, kMatrixMaxSlots> active{};
        std::array<MatrixSlotConfig, kMatrixMaxSlots> cfgs{};
        NetworkPushFn npush;
        NetworkPopFn npop;
        {
            std::lock_guard<std::mutex> lk(mu);
            cps = crosspoints;
            npush = net_push;
            npop = net_pop;
            for (int s = 0; s < kMatrixMaxSlots; ++s) {
                active[s] = slots[s].used && slots[s].cfg.active;
                if (active[s]) cfgs[s] = slots[s].cfg;
            }
        }

        float* ptrs[kMatrixMaxChannels];
        const float* cptrs[kMatrixMaxChannels];

        // 1. Pull audio from every active source slot
        for (int s = 0; s < kMatrixMaxSlots; ++s) {
            if (!active[s]) continue;
            const int ch = std::clamp(cfgs[s].channels, 1, kMatrixMaxChannels);
            for (int c = 0; c < ch; ++c) {
                ptrs[c] = src_bufs[s][c].data();
                std::memset(ptrs[c], 0, frames * sizeof(float));
            }
            if (cfgs[s].role != MatrixSlotRole::kOutput) {
                if (cfgs[s].type == MatrixSlotType::kNetworkAoip && npop) {
                    npop(ptrs, uint32_t(ch), frames);
                }
#ifdef LV_HAVE_SHM_BUS
                else if (cfgs[s].type == MatrixSlotType::kVirtualBus && slots[s].shm_bus &&
                         slots[s].shm_bus->is_open()) {
                    slots[s].shm_bus->host_read_tx(ptrs, uint32_t(ch), frames);
                    slots[s].shm_bus->host_heartbeat();
                }
#endif
                else {
                    slots[s].in_ring.read(ptrs, uint32_t(ch), frames, true);
                }
            }
            float peak = 0.0f;
            for (int c = 0; c < ch; ++c) {
                for (uint32_t i = 0; i < frames; ++i) {
                    const float a = std::fabs(src_bufs[s][c][i]);
                    if (a > peak) peak = a;
                }
            }
            slots[s].peak_in_dbfs.store(linear_to_dbfs(peak), std::memory_order_relaxed);
        }

        // 2. Zero destination accumulators
        for (int s = 0; s < kMatrixMaxSlots; ++s) {
            if (!active[s]) continue;
            const int ch = std::clamp(cfgs[s].channels, 1, kMatrixMaxChannels);
            for (int c = 0; c < ch; ++c) {
                std::memset(dst_bufs[s][c].data(), 0, frames * sizeof(float));
            }
        }

        // 3. Crosspoint routing & summing (1-to-many + many-to-1 with gain_db & mute)
        for (const auto& cp : cps) {
            if (cp.mute) continue;
            if (cp.src_slot < 0 || cp.src_slot >= kMatrixMaxSlots || !active[cp.src_slot]) continue;
            if (cp.dst_slot < 0 || cp.dst_slot >= kMatrixMaxSlots || !active[cp.dst_slot]) continue;
            if (cp.src_ch < 0 || cp.src_ch >= cfgs[cp.src_slot].channels) continue;
            if (cp.dst_ch < 0 || cp.dst_ch >= cfgs[cp.dst_slot].channels) continue;
            const float g = db_to_linear(cp.gain_db);
            if (g <= 0.0f) continue;
            const float* src = src_bufs[cp.src_slot][cp.src_ch].data();
            float* dst = dst_bufs[cp.dst_slot][cp.dst_ch].data();
            for (uint32_t i = 0; i < frames; ++i) {
                dst[i] += src[i] * g;
            }
        }

        // 4. Clamp, compute output VU meters, and push to every active destination slot
        for (int s = 0; s < kMatrixMaxSlots; ++s) {
            if (!active[s]) continue;
            const int ch = std::clamp(cfgs[s].channels, 1, kMatrixMaxChannels);
            float peak = 0.0f;
            for (int c = 0; c < ch; ++c) {
                float* d = dst_bufs[s][c].data();
                for (uint32_t i = 0; i < frames; ++i) {
                    d[i] = std::clamp(d[i], -1.0f, 1.0f);
                    const float a = std::fabs(d[i]);
                    if (a > peak) peak = a;
                }
                cptrs[c] = d;
            }
            slots[s].peak_out_dbfs.store(linear_to_dbfs(peak), std::memory_order_relaxed);

            if (cfgs[s].role != MatrixSlotRole::kInput) {
                if (cfgs[s].type == MatrixSlotType::kNetworkAoip && npush) {
                    npush(cptrs, uint32_t(ch), frames);
                }
#ifdef LV_HAVE_SHM_BUS
                else if (cfgs[s].type == MatrixSlotType::kVirtualBus && slots[s].shm_bus &&
                         slots[s].shm_bus->is_open()) {
                    slots[s].shm_bus->host_write_rx(cptrs, uint32_t(ch), frames);
                }
#endif
                else {
                    slots[s].out_ring.write(cptrs, uint32_t(ch), frames);
                }
            }
        }
    }
};

MultiDeviceAudioMatrix::MultiDeviceAudioMatrix() : impl_(std::make_unique<Impl>()) {}
MultiDeviceAudioMatrix::~MultiDeviceAudioMatrix() { stop(); }

int MultiDeviceAudioMatrix::add_slot(const MatrixSlotConfig& cfg) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    int target = -1;
    if (cfg.id >= 0 && cfg.id < kMatrixMaxSlots && !impl_->slots[cfg.id].used) {
        target = cfg.id;
    } else {
        for (int i = 0; i < kMatrixMaxSlots; ++i) {
            if (!impl_->slots[i].used) { target = i; break; }
        }
    }
    if (target < 0) return -1;
    auto& st = impl_->slots[target];
    st.used = true;
    st.cfg = cfg;
    st.cfg.id = target;
    st.cfg.channels = std::clamp(cfg.channels, 1, kMatrixMaxChannels);
    st.cfg.buffer_size = std::clamp(cfg.buffer_size, 64, 1024);
    st.in_ring.configure(st.cfg.channels);
    st.out_ring.configure(st.cfg.channels);
    if (impl_->running.load(std::memory_order_acquire)) {
        impl_->start_slot_backend(st);
    }
    return target;
}

bool MultiDeviceAudioMatrix::update_slot(int slot_id, const MatrixSlotConfig& cfg) {
    if (slot_id < 0 || slot_id >= kMatrixMaxSlots) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& st = impl_->slots[slot_id];
    if (!st.used) return false;
    const bool was_running = impl_->running.load(std::memory_order_acquire);
    if (was_running) impl_->stop_slot_backend(st);
    st.cfg = cfg;
    st.cfg.id = slot_id;
    st.cfg.channels = std::clamp(cfg.channels, 1, kMatrixMaxChannels);
    st.cfg.buffer_size = std::clamp(cfg.buffer_size, 64, 1024);
    st.in_ring.configure(st.cfg.channels);
    st.out_ring.configure(st.cfg.channels);
    if (was_running) impl_->start_slot_backend(st);
    return true;
}

bool MultiDeviceAudioMatrix::remove_slot(int slot_id) {
    if (slot_id < 0 || slot_id >= kMatrixMaxSlots) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& st = impl_->slots[slot_id];
    if (!st.used) return false;
    impl_->stop_slot_backend(st);
    st.used = false;
    auto& cps = impl_->crosspoints;
    cps.erase(std::remove_if(cps.begin(), cps.end(), [slot_id](const MatrixCrosspoint& c) {
                  return c.src_slot == slot_id || c.dst_slot == slot_id;
              }),
              cps.end());
    return true;
}

void MultiDeviceAudioMatrix::clear_slots() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (int i = 0; i < kMatrixMaxSlots; ++i) {
        if (impl_->slots[i].used) {
            impl_->stop_slot_backend(impl_->slots[i]);
            impl_->slots[i].used = false;
        }
    }
    impl_->crosspoints.clear();
}

std::vector<MatrixSlotConfig> MultiDeviceAudioMatrix::slots() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<MatrixSlotConfig> out;
    for (int i = 0; i < kMatrixMaxSlots; ++i) {
        if (impl_->slots[i].used) out.push_back(impl_->slots[i].cfg);
    }
    return out;
}

CrosspointResult MultiDeviceAudioMatrix::set_crosspoint(int src_slot, int src_ch, int dst_slot, int dst_ch,
                                                        float gain_db, bool mute, bool allow_feedback) {
    if (src_slot < 0 || src_slot >= kMatrixMaxSlots || dst_slot < 0 || dst_slot >= kMatrixMaxSlots ||
        src_ch < 0 || src_ch >= kMatrixMaxChannels || dst_ch < 0 || dst_ch >= kMatrixMaxChannels) {
        return CrosspointResult::kInvalidSlotOrChannel;
    }
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->slots[src_slot].used || !impl_->slots[dst_slot].used) {
        return CrosspointResult::kInvalidSlotOrChannel;
    }
    if (src_ch >= impl_->slots[src_slot].cfg.channels || dst_ch >= impl_->slots[dst_slot].cfg.channels) {
        return CrosspointResult::kInvalidSlotOrChannel;
    }
    if (src_slot == dst_slot && impl_->feedback_guard && !allow_feedback) {
        return CrosspointResult::kBlockedFeedbackLoop;
    }
    for (auto& cp : impl_->crosspoints) {
        if (cp.src_slot == src_slot && cp.src_ch == src_ch && cp.dst_slot == dst_slot && cp.dst_ch == dst_ch) {
            cp.gain_db = std::clamp(gain_db, -96.0f, 12.0f);
            cp.mute = mute;
            return CrosspointResult::kOk;
        }
    }
    impl_->crosspoints.push_back({src_slot, src_ch, dst_slot, dst_ch, std::clamp(gain_db, -96.0f, 12.0f), mute});
    return CrosspointResult::kOk;
}

bool MultiDeviceAudioMatrix::remove_crosspoint(int src_slot, int src_ch, int dst_slot, int dst_ch) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& cps = impl_->crosspoints;
    const size_t before = cps.size();
    cps.erase(std::remove_if(cps.begin(), cps.end(), [&](const MatrixCrosspoint& c) {
                  return c.src_slot == src_slot && c.src_ch == src_ch && c.dst_slot == dst_slot && c.dst_ch == dst_ch;
              }),
              cps.end());
    return cps.size() != before;
}

bool MultiDeviceAudioMatrix::has_crosspoint(int src_slot, int src_ch, int dst_slot, int dst_ch,
                                            MatrixCrosspoint* out) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (const auto& cp : impl_->crosspoints) {
        if (cp.src_slot == src_slot && cp.src_ch == src_ch && cp.dst_slot == dst_slot && cp.dst_ch == dst_ch) {
            if (out) *out = cp;
            return true;
        }
    }
    return false;
}

void MultiDeviceAudioMatrix::clear_crosspoints() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->crosspoints.clear();
}

std::vector<MatrixCrosspoint> MultiDeviceAudioMatrix::crosspoints() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->crosspoints;
}

void MultiDeviceAudioMatrix::set_feedback_guard(bool enabled) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->feedback_guard = enabled;
}

bool MultiDeviceAudioMatrix::feedback_guard() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->feedback_guard;
}

void MultiDeviceAudioMatrix::apply_preset(int preset_index, int net_slot_id, int daw_slot_id, int monitor_slot_id) {
    clear_crosspoints();
    if (preset_index == 0) {
        // Preset 1: "Mac/Net -> DAW (64ch) + Headphones (Ch 1/2)"
        if (net_slot_id >= 0 && daw_slot_id >= 0) {
            for (int c = 0; c < kMatrixMaxChannels; ++c) set_crosspoint(net_slot_id, c, daw_slot_id, c);
        }
        if (net_slot_id >= 0 && monitor_slot_id >= 0) {
            set_crosspoint(net_slot_id, 0, monitor_slot_id, 0);
            set_crosspoint(net_slot_id, 1, monitor_slot_id, 1);
        }
    } else if (preset_index == 1) {
        // Preset 2: "Full-Duplex 64x64 (DAW <-> Net) + Local Monitor"
        if (net_slot_id >= 0 && daw_slot_id >= 0) {
            for (int c = 0; c < kMatrixMaxChannels; ++c) {
                set_crosspoint(net_slot_id, c, daw_slot_id, c);
                set_crosspoint(daw_slot_id, c, net_slot_id, c);
            }
        }
        if (net_slot_id >= 0 && monitor_slot_id >= 0) {
            set_crosspoint(net_slot_id, 0, monitor_slot_id, 0);
            set_crosspoint(net_slot_id, 1, monitor_slot_id, 1);
        }
    } else {
        // Preset 3: "1:1 Direct Patch"
        if (net_slot_id >= 0 && daw_slot_id >= 0) {
            for (int c = 0; c < kMatrixMaxChannels; ++c) {
                set_crosspoint(net_slot_id, c, daw_slot_id, c);
                set_crosspoint(daw_slot_id, c, net_slot_id, c);
            }
        }
    }
}

void MultiDeviceAudioMatrix::set_network_io(NetworkPushFn push_tx, NetworkPopFn pop_rx) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->net_push = std::move(push_tx);
    impl_->net_pop = std::move(pop_rx);
}

void MultiDeviceAudioMatrix::set_buffer_size(int frames) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->buffer_size = (frames <= 128) ? 128 : (frames <= 256 ? 256 : 512);
}

int MultiDeviceAudioMatrix::buffer_size() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->buffer_size;
}

bool MultiDeviceAudioMatrix::start() {
    if (impl_->running.load(std::memory_order_acquire)) return true;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (int i = 0; i < kMatrixMaxSlots; ++i) {
            if (impl_->slots[i].used && impl_->slots[i].cfg.active) {
                impl_->start_slot_backend(impl_->slots[i]);
            }
        }
    }
    impl_->stop_flag.store(false, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);
    impl_->mix_thread = std::thread([this]() {
        // Real-time scheduling for the mixer: period = one block at 48 kHz (128 -> 2.67 ms, 256 -> 5.33 ms).
        const uint32_t rt_period_us = uint32_t(uint64_t(buffer_size()) * 1000000ull / kMatrixSampleRate);
        RealtimeThreadScope rt(rt_period_us);
#if defined(_WIN32)
        timeBeginPeriod(1);
#endif
        uint64_t next = now_us();
        while (!impl_->stop_flag.load(std::memory_order_acquire)) {
            const uint32_t blk = uint32_t(buffer_size());
            const uint64_t period_us = uint64_t(blk) * 1000000ull / kMatrixSampleRate;
            next += period_us;
            impl_->process_block(blk);
            const uint64_t now = now_us();
            if (next > now) {
                sleep_until_us(next, [] {});
            } else if (now - next > period_us * 4) {
                next = now;
            }
        }
#if defined(_WIN32)
        timeEndPeriod(1);
#endif
    });
    return true;
}

void MultiDeviceAudioMatrix::stop() {
    impl_->stop_flag.store(true, std::memory_order_release);
    if (impl_->mix_thread.joinable()) impl_->mix_thread.join();
    impl_->running.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (int i = 0; i < kMatrixMaxSlots; ++i) {
        if (impl_->slots[i].used) impl_->stop_slot_backend(impl_->slots[i]);
    }
}

bool MultiDeviceAudioMatrix::is_running() const {
    return impl_->running.load(std::memory_order_acquire);
}

MatrixSlotRuntimeStatus MultiDeviceAudioMatrix::slot_status(int slot_id) const {
    MatrixSlotRuntimeStatus out{};
    if (slot_id < 0 || slot_id >= kMatrixMaxSlots) return out;
    std::lock_guard<std::mutex> lk(impl_->mu);
    const auto& st = impl_->slots[slot_id];
    out.id = slot_id;
    out.active = st.used && st.cfg.active;
    out.buffer_size = uint32_t(st.cfg.buffer_size);
    out.peak_in_dbfs = st.peak_in_dbfs.load(std::memory_order_relaxed);
    out.peak_out_dbfs = st.peak_out_dbfs.load(std::memory_order_relaxed);
    out.xruns = st.in_ring.xruns() + st.out_ring.xruns();
    if (!st.used) {
        out.status_text = "Unused";
        return out;
    }
    if (st.cfg.type == MatrixSlotType::kVirtualBus) {
#ifdef LV_HAVE_SHM_BUS
        out.connected = st.shm_bus && st.shm_bus->is_open() && st.shm_bus->driver_connected();
        out.status_text = out.connected ? "DAW Connected (SHM v2)" : "Waiting for DAW (LanVid Virtual ASIO)";
#else
        out.status_text = "SHM unavailable";
#endif
    } else if (st.cfg.type == MatrixSlotType::kNetworkAoip) {
        out.connected = (impl_->net_push != nullptr || impl_->net_pop != nullptr);
        out.status_text = out.connected ? "AoIP Stream Linked" : "Idle (start Sender/Receiver/Duplex)";
    } else if (st.cfg.type == MatrixSlotType::kSystemAudio) {
#if defined(_WIN32)
        out.connected = st.wasapi && st.wasapi->connected();
        out.device_sample_rate = st.wasapi ? st.wasapi->sample_rate() : kMatrixSampleRate;
        out.status_text = out.connected ? "WASAPI Active" : "WASAPI Stopped";
#elif LV_MATRIX_COREAUDIO
        out.connected = st.coreaudio && st.coreaudio->connected();
        out.device_sample_rate = st.coreaudio ? st.coreaudio->sample_rate() : kMatrixSampleRate;
        if (!st.coreaudio) out.status_text = "CoreAudio Stopped";
        else if (out.connected) out.status_text = "CoreAudio Active: " + st.coreaudio->device_name() + " @ " +
                                                  std::to_string(out.device_sample_rate) + " Hz -> 48 kHz";
        else out.status_text = st.coreaudio->status();
#else
        out.status_text = "System Audio (stub)";
#endif
    } else if (st.cfg.type == MatrixSlotType::kExternalAsio) {
#if defined(_WIN32)
        out.connected = st.asio && st.asio->connected();
        out.device_sample_rate = st.asio ? st.asio->sample_rate() : kMatrixSampleRate;
        out.status_text = out.connected ? "ASIO Active" : "ASIO Stopped";
#else
        out.status_text = "ASIO (Windows only)";
#endif
    }
    return out;
}

uint32_t MultiDeviceAudioMatrix::inject_slot_input(int slot_id, const float* const* planes,
                                                   uint32_t channels, uint32_t frames) {
    if (slot_id < 0 || slot_id >= kMatrixMaxSlots) return 0;
    return impl_->slots[slot_id].in_ring.write(planes, channels, frames);
}

uint32_t MultiDeviceAudioMatrix::extract_slot_output(int slot_id, float* const* planes,
                                                     uint32_t channels, uint32_t frames) {
    if (slot_id < 0 || slot_id >= kMatrixMaxSlots) return 0;
    return impl_->slots[slot_id].out_ring.read(planes, channels, frames, true);
}

void MultiDeviceAudioMatrix::process_block_sync(uint32_t frames) {
    impl_->process_block(frames);
}

// ---------------------------------------------------------------- MIDI Slots & Crosspoint Routing

int MultiDeviceAudioMatrix::add_midi_slot(const MidiSlotConfig& cfg) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    int target = -1;
    if (cfg.id >= 0 && cfg.id < kMatrixMaxMidiSlots && !impl_->midi_slots[cfg.id].used) {
        target = cfg.id;
    } else {
        for (int i = 0; i < kMatrixMaxMidiSlots; ++i) {
            if (!impl_->midi_slots[i].used) { target = i; break; }
        }
    }
    if (target < 0) return -1;
    auto& st = impl_->midi_slots[target];
    st.used = true;
    st.cfg = cfg;
    st.cfg.id = target;
    st.cfg.port_index = cfg.port_index < 16 ? cfg.port_index : uint8_t(target & 0x0F);
    st.events_count.store(0, std::memory_order_relaxed);
    st.last_activity_us.store(0, std::memory_order_relaxed);
    if (st.cfg.active) {
        if (st.cfg.type == MidiSlotType::kMidiInput) {
            st.in_port = std::make_unique<MidiInputPort>();
            st.in_port->open(st.cfg.device_index, st.cfg.device_name,
                             [this, target](uint8_t s, uint8_t d1, uint8_t d2, uint64_t ts) {
                                 inject_midi_event(target, s, d1, d2, ts);
                             });
        } else if (st.cfg.type == MidiSlotType::kMidiOutput) {
            st.out_port = std::make_unique<MidiOutputPort>();
            st.out_port->open(st.cfg.device_index, st.cfg.device_name);
        }
    }
    return target;
}

bool MultiDeviceAudioMatrix::update_midi_slot(int slot_id, const MidiSlotConfig& cfg) {
    if (slot_id < 0 || slot_id >= kMatrixMaxMidiSlots) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& st = impl_->midi_slots[slot_id];
    if (!st.used) return false;
    st.in_port.reset();
    st.out_port.reset();
    st.cfg = cfg;
    st.cfg.id = slot_id;
    if (st.cfg.active) {
        if (st.cfg.type == MidiSlotType::kMidiInput) {
            st.in_port = std::make_unique<MidiInputPort>();
            st.in_port->open(st.cfg.device_index, st.cfg.device_name,
                             [this, slot_id](uint8_t s, uint8_t d1, uint8_t d2, uint64_t ts) {
                                 inject_midi_event(slot_id, s, d1, d2, ts);
                             });
        } else if (st.cfg.type == MidiSlotType::kMidiOutput) {
            st.out_port = std::make_unique<MidiOutputPort>();
            st.out_port->open(st.cfg.device_index, st.cfg.device_name);
        }
    }
    return true;
}

bool MultiDeviceAudioMatrix::remove_midi_slot(int slot_id) {
    if (slot_id < 0 || slot_id >= kMatrixMaxMidiSlots) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& st = impl_->midi_slots[slot_id];
    if (!st.used) return false;
    st.in_port.reset();
    st.out_port.reset();
    st.used = false;
    auto& cps = impl_->midi_crosspoints;
    cps.erase(std::remove_if(cps.begin(), cps.end(), [slot_id](const MidiCrosspoint& c) {
                  return c.src_slot == slot_id || c.dst_slot == slot_id;
              }),
              cps.end());
    return true;
}

void MultiDeviceAudioMatrix::clear_midi_slots() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (auto& st : impl_->midi_slots) {
        st.in_port.reset();
        st.out_port.reset();
        st.used = false;
    }
    impl_->midi_crosspoints.clear();
}

std::vector<MidiSlotConfig> MultiDeviceAudioMatrix::midi_slots() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<MidiSlotConfig> out;
    for (const auto& st : impl_->midi_slots) {
        if (st.used) out.push_back(st.cfg);
    }
    return out;
}

bool MultiDeviceAudioMatrix::set_midi_crosspoint(int src_slot, int dst_slot, int channel_filter, bool mute) {
    if (src_slot < 0 || src_slot >= kMatrixMaxMidiSlots || dst_slot < 0 || dst_slot >= kMatrixMaxMidiSlots ||
        src_slot == dst_slot) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->midi_slots[src_slot].used || !impl_->midi_slots[dst_slot].used) return false;
    channel_filter = std::clamp(channel_filter, 0, 16);
    for (auto& c : impl_->midi_crosspoints) {
        if (c.src_slot == src_slot && c.dst_slot == dst_slot) {
            c.channel_filter = channel_filter;
            c.mute = mute;
            return true;
        }
    }
    impl_->midi_crosspoints.push_back({src_slot, dst_slot, channel_filter, mute});
    return true;
}

bool MultiDeviceAudioMatrix::remove_midi_crosspoint(int src_slot, int dst_slot) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& cps = impl_->midi_crosspoints;
    const size_t before = cps.size();
    cps.erase(std::remove_if(cps.begin(), cps.end(), [&](const MidiCrosspoint& c) {
                  return c.src_slot == src_slot && c.dst_slot == dst_slot;
              }),
              cps.end());
    return cps.size() != before;
}

bool MultiDeviceAudioMatrix::has_midi_crosspoint(int src_slot, int dst_slot, MidiCrosspoint* out) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (const auto& c : impl_->midi_crosspoints) {
        if (c.src_slot == src_slot && c.dst_slot == dst_slot) {
            if (out) *out = c;
            return true;
        }
    }
    return false;
}

void MultiDeviceAudioMatrix::clear_midi_crosspoints() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->midi_crosspoints.clear();
}

std::vector<MidiCrosspoint> MultiDeviceAudioMatrix::midi_crosspoints() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->midi_crosspoints;
}

void MultiDeviceAudioMatrix::set_midi_sink(MidiOutSinkFn sink) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->midi_sink = std::move(sink);
}

void MultiDeviceAudioMatrix::inject_midi_event(int src_slot_id, uint8_t status, uint8_t d1, uint8_t d2,
                                               uint64_t ts_us) {
    if (src_slot_id < 0 || src_slot_id >= kMatrixMaxMidiSlots) return;
    const uint64_t act_now = now_us();
    std::vector<std::pair<int, uint8_t>> targets;
    MidiOutSinkFn sink;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto& src = impl_->midi_slots[src_slot_id];
        if (!src.used || !src.cfg.active) return;
        src.events_count.fetch_add(1, std::memory_order_relaxed);
        src.last_activity_us.store(act_now, std::memory_order_relaxed);
        sink = impl_->midi_sink;

        const bool is_channel_msg = (status >= 0x80u && status < 0xF0u);
        const int midi_ch = is_channel_msg ? int((status & 0x0Fu) + 1) : 0;

        for (const auto& cp : impl_->midi_crosspoints) {
            if (cp.mute || cp.src_slot != src_slot_id) continue;
            if (cp.channel_filter > 0 && is_channel_msg && midi_ch != cp.channel_filter) continue;
            auto& dst = impl_->midi_slots[cp.dst_slot];
            if (!dst.used || !dst.cfg.active) continue;
            if (dst.out_port && dst.out_port->is_open()) {
                dst.out_port->send_short(status, d1, d2);
            }
            dst.events_count.fetch_add(1, std::memory_order_relaxed);
            dst.last_activity_us.store(act_now, std::memory_order_relaxed);
            targets.emplace_back(cp.dst_slot, dst.cfg.port_index);
        }
    }
    if (sink) {
        for (const auto& t : targets) {
            sink(t.first, status, d1, d2, t.second, ts_us);
        }
    }
}

void MultiDeviceAudioMatrix::send_midi_panic(int slot_id) {
    const uint64_t act_now = now_us();
    MidiOutSinkFn sink;
    std::vector<std::pair<int, uint8_t>> targets;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        sink = impl_->midi_sink;
        for (int i = 0; i < kMatrixMaxMidiSlots; ++i) {
            if (slot_id >= 0 && i != slot_id) continue;
            auto& st = impl_->midi_slots[i];
            if (!st.used || !st.cfg.active) continue;
            if (st.cfg.type == MidiSlotType::kMidiOutput || st.cfg.type == MidiSlotType::kNetworkMidiTx) {
                if (st.out_port && st.out_port->is_open()) st.out_port->send_panic();
                st.last_activity_us.store(act_now, std::memory_order_relaxed);
                targets.emplace_back(i, st.cfg.port_index);
            }
        }
    }
    if (sink) {
        for (const auto& t : targets) {
            for (uint8_t ch = 0; ch < 16; ++ch) {
                sink(t.first, uint8_t(0xB0u | ch), 123, 0, t.second, act_now);
                sink(t.first, uint8_t(0xB0u | ch), 120, 0, t.second, act_now);
            }
        }
    }
}

MidiSlotRuntimeStatus MultiDeviceAudioMatrix::midi_slot_status(int slot_id) const {
    MidiSlotRuntimeStatus out{};
    if (slot_id < 0 || slot_id >= kMatrixMaxMidiSlots) return out;
    std::lock_guard<std::mutex> lk(impl_->mu);
    const auto& st = impl_->midi_slots[slot_id];
    out.id = slot_id;
    out.active = st.used && st.cfg.active;
    out.events_count = st.events_count.load(std::memory_order_relaxed);
    out.last_activity_us = st.last_activity_us.load(std::memory_order_relaxed);
    if (!st.used) return out;
    if (st.cfg.type == MidiSlotType::kMidiInput) {
        out.connected = st.in_port && st.in_port->is_open();
    } else if (st.cfg.type == MidiSlotType::kMidiOutput) {
        out.connected = st.out_port && st.out_port->is_open();
    } else {
        out.connected = st.cfg.active;
    }
    return out;
}

}  // namespace lv
