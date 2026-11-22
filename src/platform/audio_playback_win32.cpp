// Windows WASAPI Shared Mode low-latency playback (IAudioClient + IAudioRenderClient,
// event-driven, AUTOCONVERTPCM from 48 kHz Stereo PCM16, pulling from lock-free AudioJitterBuffer).

#include "platform/audio_playback.hpp"

#include <atomic>
#include <future>
#include <thread>

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

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
    std::string out = "Default Audio Output";
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

}  // namespace

struct AudioPlayback::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    HANDLE wake_event = nullptr;
    std::string dev_name;
};

AudioPlayback::AudioPlayback() : impl_(std::make_unique<Impl>()) {}
AudioPlayback::~AudioPlayback() { stop(); }

bool AudioPlayback::start(AudioJitterBuffer* buffer) {
    stop();
    if (!buffer) return false;
    impl_->stop.store(false, std::memory_order_relaxed);

    std::promise<bool> ready;
    auto fut = ready.get_future();

    impl_->thread = std::thread([this, buffer, p = std::move(ready)]() mutable {
        const bool com_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        DWORD mmcss_task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        impl_->wake_event = evt;

        auto cleanup = [&]() {
            if (client) client->Stop();
            safe_release(render);
            safe_release(client);
            safe_release(device);
            safe_release(enumerator);
            impl_->wake_event = nullptr;
            if (evt) CloseHandle(evt);
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (com_ok) CoUninitialize();
        };

        if (!evt) {
            cleanup();
            p.set_value(false);
            return;
        }

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hr) && enumerator) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
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

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = buffer->sample_rate();
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 4;

        REFERENCE_TIME def_period = 100000;
        REFERENCE_TIME min_period = 100000;
        client->GetDevicePeriod(&def_period, &min_period);
        // Request minimal shared-mode buffer (device default period, typically 10 ms).
        const REFERENCE_TIME buf_dur = def_period > 0 ? def_period : 100000;

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buf_dur, 0, &wfx, nullptr);
        if (FAILED(hr)) {
            cleanup();
            p.set_value(false);
            return;
        }

        hr = client->SetEventHandle(evt);
        if (SUCCEEDED(hr)) {
            hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
        }
        UINT32 buf_frames = 0;
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&buf_frames);
        if (FAILED(hr) || !render || buf_frames == 0) {
            cleanup();
            p.set_value(false);
            return;
        }

        // Prime buffer with silence before starting.
        BYTE* init_buf = nullptr;
        if (SUCCEEDED(render->GetBuffer(buf_frames, &init_buf)) && init_buf) {
            render->ReleaseBuffer(buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        hr = client->Start();
        if (FAILED(hr)) {
            cleanup();
            p.set_value(false);
            return;
        }

        impl_->running.store(true, std::memory_order_release);
        p.set_value(true);

        while (!impl_->stop.load(std::memory_order_relaxed)) {
            WaitForSingleObject(evt, 20);
            if (impl_->stop.load(std::memory_order_relaxed)) break;

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            if (padding >= buf_frames) continue;
            const UINT32 to_write = buf_frames - padding;

            BYTE* out = nullptr;
            if (SUCCEEDED(render->GetBuffer(to_write, &out)) && out) {
                buffer->pop_pcm16(reinterpret_cast<int16_t*>(out), to_write);
                render->ReleaseBuffer(to_write, 0);
            }
        }

        impl_->running.store(false, std::memory_order_release);
        cleanup();
    });

    const bool ok = fut.get();
    if (!ok && impl_->thread.joinable()) impl_->thread.join();
    return ok;
}

void AudioPlayback::stop() {
    if (!impl_) return;
    impl_->stop.store(true, std::memory_order_release);
    if (impl_->wake_event) SetEvent(impl_->wake_event);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running.store(false, std::memory_order_release);
}

bool AudioPlayback::is_running() const { return impl_ && impl_->running.load(std::memory_order_acquire); }
std::string AudioPlayback::device_name() const { return impl_ ? impl_->dev_name : ""; }

}  // namespace lv
