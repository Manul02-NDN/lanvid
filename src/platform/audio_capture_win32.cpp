// Windows WASAPI Audio Capture (System Loopback + Microphone) with AUTOCONVERTPCM -> 48kHz Stereo PCM16.

#include "platform/audio_capture.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include "common/clock.hpp"

namespace lv {

namespace {

template <class T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string get_device_friendly_name(IMMDevice* dev) {
    IPropertyStore* store = nullptr;
    std::string out = "Unknown Audio Device";
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            out = wide_to_utf8(pv.pwszVal);
        }
        PropVariantClear(&pv);
        store->Release();
    }
    return out;
}

std::string get_device_id(IMMDevice* dev) {
    wchar_t* id = nullptr;
    std::string out;
    if (dev && SUCCEEDED(dev->GetId(&id)) && id) {
        out = wide_to_utf8(id);
        CoTaskMemFree(id);
    }
    return out;
}

struct ComScope {
    bool ok = false;
    ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() {
        if (ok) CoUninitialize();
    }
};

}  // namespace

std::vector<AudioDeviceInfo> list_audio_devices(bool loopback) {
    std::vector<AudioDeviceInfo> out;
    ComScope com;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) ||
        !enumerator) {
        return out;
    }

    const EDataFlow flow = loopback ? eRender : eCapture;
    IMMDevice* def_dev = nullptr;
    std::string def_id;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &def_dev)) && def_dev) {
        def_id = get_device_id(def_dev);
        def_dev->Release();
    }

    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(col->Item(i, &dev)) && dev) {
                AudioDeviceInfo info;
                info.index = int(i);
                info.name = get_device_friendly_name(dev);
                info.is_loopback = loopback;
                info.is_default = (!def_id.empty() && get_device_id(dev) == def_id);
                out.push_back(std::move(info));
                dev->Release();
            }
        }
        col->Release();
    }
    enumerator->Release();
    return out;
}

struct AudioCapture::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::string dev_name;
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { stop(); }

bool AudioCapture::start(AudioCaptureMode mode, int device_index, uint16_t chunk_frames,
                         ChunkCallback on_chunk) {
    stop();
    if (mode == AudioCaptureMode::kNone || !on_chunk) return false;
    chunk_frames = std::clamp<uint16_t>(chunk_frames ? chunk_frames : 240, 60, 480);

    impl_->stop.store(false, std::memory_order_relaxed);
    std::promise<bool> ready;
    auto fut = ready.get_future();

    impl_->thread = std::thread([this, mode, device_index, chunk_frames, cb = std::move(on_chunk),
                                 p = std::move(ready)]() mutable {
        ComScope com;
        DWORD mmcss_task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap_client = nullptr;
        IAudioClient* silence_client = nullptr;
        IAudioRenderClient* silence_render = nullptr;
        UINT32 silence_buf_frames = 0;

        auto cleanup = [&]() {
            if (silence_client) silence_client->Stop();
            safe_release(silence_render);
            safe_release(silence_client);
            if (client) client->Stop();
            safe_release(cap_client);
            safe_release(client);
            safe_release(device);
            safe_release(enumerator);
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            cleanup();
            p.set_value(false);
            return;
        }

        const bool is_loopback = (mode == AudioCaptureMode::kLoopback);
        const EDataFlow flow = is_loopback ? eRender : eCapture;
        if (device_index < 0) {
            hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
        } else {
            IMMDeviceCollection* col = nullptr;
            hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col);
            if (SUCCEEDED(hr) && col) {
                UINT count = 0;
                col->GetCount(&count);
                if (UINT(device_index) < count) hr = col->Item(UINT(device_index), &device);
                else hr = E_INVALIDARG;
                col->Release();
            }
        }
        if (FAILED(hr) || !device) {
            cleanup();
            p.set_value(false);
            return;
        }

        impl_->dev_name = get_device_friendly_name(device);

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (FAILED(hr) || !client) {
            cleanup();
            p.set_value(false);
            return;
        }

        // Request 48 kHz, 2-ch, 16-bit PCM via WASAPI AUTOCONVERTPCM.
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = 48000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        DWORD stream_flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (is_loopback) stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

        const REFERENCE_TIME buf_duration = 200000;  // 20 ms (100-ns units)
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, buf_duration, 0, &wfx, nullptr);
        if (FAILED(hr)) {
            cleanup();
            p.set_value(false);
            return;
        }

        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap_client));
        if (FAILED(hr) || !cap_client) {
            cleanup();
            p.set_value(false);
            return;
        }

        // Windows WASAPI Loopback pauses packet delivery whenever the render endpoint is completely idle.
        // Opening a silent render stream (AUDCLNT_BUFFERFLAGS_SILENT) on the same endpoint keeps the
        // loopback clock running smoothly during silence without producing any audible output.
        if (is_loopback) {
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&silence_client))) &&
                silence_client) {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(silence_client->GetMixFormat(&mix)) && mix) {
                    if (SUCCEEDED(silence_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix,
                                                             nullptr)) &&
                        SUCCEEDED(silence_client->GetBufferSize(&silence_buf_frames)) &&
                        SUCCEEDED(silence_client->GetService(__uuidof(IAudioRenderClient),
                                                             reinterpret_cast<void**>(&silence_render))) &&
                        silence_render) {
                        BYTE* sbuf = nullptr;
                        if (SUCCEEDED(silence_render->GetBuffer(silence_buf_frames, &sbuf))) {
                            silence_render->ReleaseBuffer(silence_buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                        if (FAILED(silence_client->Start())) {
                            safe_release(silence_render);
                            safe_release(silence_client);
                        }
                    } else {
                        safe_release(silence_render);
                        safe_release(silence_client);
                    }
                    CoTaskMemFree(mix);
                } else {
                    safe_release(silence_client);
                }
            }
        }

        hr = client->Start();
        if (FAILED(hr)) {
            cleanup();
            p.set_value(false);
            return;
        }

        impl_->running.store(true, std::memory_order_release);
        p.set_value(true);

        std::vector<int16_t> accum;
        accum.reserve(size_t(chunk_frames) * 4);
        uint32_t seq_id = 0;

        while (!impl_->stop.load(std::memory_order_relaxed)) {
            if (silence_client && silence_render && silence_buf_frames > 0) {
                UINT32 pad = 0;
                if (SUCCEEDED(silence_client->GetCurrentPadding(&pad)) && pad < silence_buf_frames / 2) {
                    const UINT32 avail_s = silence_buf_frames - pad;
                    BYTE* sbuf = nullptr;
                    if (SUCCEEDED(silence_render->GetBuffer(avail_s, &sbuf))) {
                        silence_render->ReleaseBuffer(avail_s, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                }
            }

            UINT32 packet_frames = 0;
            if (FAILED(cap_client->GetNextPacketSize(&packet_frames))) break;

            if (packet_frames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            while (packet_frames > 0 && !impl_->stop.load(std::memory_order_relaxed)) {
                BYTE* data = nullptr;
                UINT32 frames_avail = 0;
                DWORD flags = 0;
                hr = cap_client->GetBuffer(&data, &frames_avail, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                const size_t samples = size_t(frames_avail) * 2;
                const size_t old_sz = accum.size();
                accum.resize(old_sz + samples);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    std::memset(accum.data() + old_sz, 0, samples * sizeof(int16_t));
                } else {
                    std::memcpy(accum.data() + old_sz, data, samples * sizeof(int16_t));
                }
                cap_client->ReleaseBuffer(frames_avail);

                const size_t chunk_samples = size_t(chunk_frames) * 2;
                size_t offset = 0;
                while (accum.size() - offset >= chunk_samples) {
                    cb(seq_id++, now_us32(), accum.data() + offset, chunk_frames);
                    offset += chunk_samples;
                }
                if (offset > 0) {
                    accum.erase(accum.begin(), accum.begin() + ptrdiff_t(offset));
                }

                if (FAILED(cap_client->GetNextPacketSize(&packet_frames))) break;
            }
        }

        impl_->running.store(false, std::memory_order_release);
        cleanup();
    });

    const bool ok = fut.get();
    if (!ok && impl_->thread.joinable()) impl_->thread.join();
    return ok;
}

void AudioCapture::stop() {
    if (!impl_) return;
    impl_->stop.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running.store(false, std::memory_order_release);
}

bool AudioCapture::is_running() const { return impl_ && impl_->running.load(std::memory_order_acquire); }
std::string AudioCapture::device_name() const { return impl_ ? impl_->dev_name : ""; }

}  // namespace lv
