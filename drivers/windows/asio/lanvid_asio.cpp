// LanVidASIO.dll — "LanVid Virtual ASIO": 64 inputs (network -> DAW) / 64 outputs (DAW -> network).
//
// In-process COM server implementing IASIO (see iasio_minimal.h). Audio is exchanged with
// LanVid Studio / CLI through the lock-free shared-memory bus LanVidAudioBus_v1.
// The device clock is a high-resolution waitable timer on QPC (the same system clock the LanVid
// sender uses as its Master Clock), so DAW timing and the network media timeline stay aligned.
//
// Sample formats: ASIOSTFloat32LSB (default) or ASIOSTInt32LSB
//   (HKCU\Software\LanVid\ASIO  SampleFormat = "int32").
// Registration: regsvr32 /s LanVidASIO.dll  ->  HKLM\SOFTWARE\ASIO\LanVid Virtual ASIO.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <olectl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "audio/shm_audio_bridge.hpp"
#include "iasio_minimal.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// {8B9F1C2D-4E5A-4B7C-9D1E-6F2A3B4C5D6E}
const CLSID CLSID_LanVidASIO = {0x8b9f1c2d, 0x4e5a, 0x4b7c, {0x9d, 0x1e, 0x6f, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e}};
const wchar_t* kClsidStr = L"{8B9F1C2D-4E5A-4B7C-9D1E-6F2A3B4C5D6E}";
const wchar_t* kDriverName = L"LanVid Virtual ASIO";
constexpr long kChannels = long(lv::kShmBusChannels);
constexpr long kBufferSizes[] = {64, 128, 256, 512, 1024};

HINSTANCE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

bool read_int32_preference() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LanVid\\ASIO", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[32] = {};
    DWORD sz = sizeof(buf), type = 0;
    const bool ok = RegQueryValueExW(k, L"SampleFormat", nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz) ==
                        ERROR_SUCCESS && type == REG_SZ && _wcsicmp(buf, L"int32") == 0;
    RegCloseKey(k);
    return ok;
}

class LanVidAsio final : public IASIO {
public:
    LanVidAsio() { ++g_objects; }
    ~LanVidAsio() {
        stop();
        disposeBuffers();
        bus_.close();
        --g_objects;
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        // ASIO hosts pass the driver CLSID as the IID.
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_LanVidASIO)) {
            *out = static_cast<IASIO*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --refs_;
        if (r == 0) delete this;
        return ULONG(r);
    }

    // ---- IASIO ----
    ASIOBool init(void*) override {
        int32_ = read_int32_preference();
        if (!bus_.open(nullptr, true)) {
            std::snprintf(err_, sizeof(err_), "Cannot open shared audio bus %s", lv::kShmBusName);
            return ASIOFalse;
        }
        return ASIOTrue;
    }
    void getDriverName(char* name) override { std::snprintf(name, 32, "LanVid Virtual ASIO"); }
    long getDriverVersion() override { return 0x00020301; }
    void getErrorMessage(char* s) override { std::snprintf(s, 124, "%s", err_[0] ? err_ : "No error"); }

    ASIOError start() override {
        if (!callbacks_ || bufsize_ == 0) return ASE_NotPresent;
        if (running_.load()) return ASE_OK;
        sample_pos_.store(0);
        stop_.store(false);
        bus_.driver_set_running(true, uint32_t(rate_), uint32_t(bufsize_), "ASIO");
        running_.store(true);
        thread_ = std::thread([this] { io_thread(); });
        return ASE_OK;
    }
    ASIOError stop() override {
        if (!running_.load()) return ASE_OK;
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        running_.store(false);
        bus_.driver_set_running(false, 0, 0, nullptr);
        return ASE_OK;
    }

    ASIOError getChannels(long* in, long* out) override {
        if (in) *in = kChannels;
        if (out) *out = kChannels;
        return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        const long b = bufsize_ ? bufsize_ : 256;
        if (in) *in = b;
        if (out) *out = b;
        return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        if (mn) *mn = kBufferSizes[0];
        if (mx) *mx = kBufferSizes[4];
        if (pref) *pref = 256;
        if (gran) *gran = -1;  // powers of two
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        return (r == 48000.0 || r == 96000.0) ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override {
        if (!r) return ASE_InvalidParameter;
        *r = rate_;
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (canSampleRate(r) != ASE_OK) return ASE_NoClock;
        if (r != rate_) {
            rate_ = r;
            if (auto* h = bus_.header()) h->sample_rate.store(uint32_t(r));
            if (callbacks_ && callbacks_->sampleRateDidChange) callbacks_->sampleRateDidChange(r);
        }
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override {
        if (!c || !n) return ASE_InvalidParameter;
        std::memset(c, 0, sizeof(*c));
        c->index = 0;
        c->associatedChannel = -1;
        c->associatedGroup = -1;
        c->isCurrentSource = ASIOTrue;
        std::snprintf(c->name, sizeof(c->name), "LanVid Master Clock");
        *n = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidMode; }
    ASIOError getSamplePosition(ASIOSamples* sp, ASIOTimeStamp* ts) override {
        if (!sp || !ts) return ASE_InvalidParameter;
        const uint64_t pos = sample_pos_.load();
        const uint64_t ns = time_ns_.load();
        sp->hi = static_cast<unsigned long>(pos >> 32);
        sp->lo = static_cast<unsigned long>(pos & 0xFFFFFFFFu);
        ts->hi = static_cast<unsigned long>(ns >> 32);
        ts->lo = static_cast<unsigned long>(ns & 0xFFFFFFFFu);
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info || info->channel < 0 || info->channel >= kChannels) return ASE_InvalidParameter;
        const long ch = info->channel;
        info->type = int32_ ? ASIOSTInt32LSB : ASIOSTFloat32LSB;
        info->channelGroup = 0;
        info->isActive = (info->isInput ? in_active_[ch] : out_active_[ch]) ? ASIOTrue : ASIOFalse;
        std::snprintf(info->name, sizeof(info->name), "LanVid %s %ld", info->isInput ? "In" : "Out", ch + 1);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long n, long size, ASIOCallbacks* cb) override {
        if (!infos || !cb || n <= 0) return ASE_InvalidParameter;
        if (std::find(std::begin(kBufferSizes), std::end(kBufferSizes), size) == std::end(kBufferSizes))
            return ASE_InvalidMode;
        disposeBuffers();
        bufsize_ = size;
        const size_t bytes = size_t(size) * 4;  // Float32LSB and Int32LSB are both 4 bytes
        for (long i = 0; i < n; ++i) {
            ASIOBufferInfo& b = infos[i];
            if (b.channelNum < 0 || b.channelNum >= kChannels) return ASE_InvalidParameter;
            auto& store = b.isInput ? in_buf_[b.channelNum] : out_buf_[b.channelNum];
            store.assign(2 * size_t(size), 0.0f);
            b.buffers[0] = store.data();
            b.buffers[1] = reinterpret_cast<uint8_t*>(store.data()) + bytes;
            (b.isInput ? in_active_ : out_active_)[b.channelNum] = true;
        }
        callbacks_ = cb;
        // Scratch for planar float exchange with the bus (allocated here, never on the IO thread).
        for (long c = 0; c < kChannels; ++c) scratch_[c].assign(size_t(size), 0.0f);
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        stop();
        for (long c = 0; c < kChannels; ++c) {
            in_buf_[c].clear();
            out_buf_[c].clear();
            in_active_[c] = out_active_[c] = false;
        }
        callbacks_ = nullptr;
        bufsize_ = 0;
        return ASE_OK;
    }
    ASIOError controlPanel() override {
        char msg[512];
        const bool host = bus_.host_connected();
        std::snprintf(msg, sizeof(msg),
                      "LanVid Virtual ASIO\n\n64 inputs (network -> DAW), 64 outputs (DAW -> network)\n"
                      "Sample rate: %.0f Hz | Buffer: %ld | Format: %s\n\nLanVid Studio / CLI: %s\n\n"
                      "Set HKCU\\Software\\LanVid\\ASIO\\SampleFormat = int32 for ASIOSTInt32LSB.",
                      rate_, bufsize_, int32_ ? "Int32LSB" : "Float32LSB",
                      host ? "connected" : "not running (start LanVid Studio or sender/receiver --audio-device virtual)");
        MessageBoxA(nullptr, msg, "LanVid Virtual ASIO", MB_OK | MB_ICONINFORMATION);
        return ASE_OK;
    }
    ASIOError future(long, void*) override { return ASE_InvalidParameter; }
    ASIOError outputReady() override { return ASE_NotPresent; }

private:
    void io_thread() {
        timeBeginPeriod(1);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        // Optional MMCSS "Pro Audio" class (avrt.dll loaded dynamically).
        HANDLE mmcss = nullptr;
        if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
            using AvSet = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
            if (auto fn = reinterpret_cast<AvSet>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))) {
                DWORD idx = 0;
                mmcss = fn(L"Pro Audio", &idx);
            }
        }

        LARGE_INTEGER freq, t0, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const double period_s = double(bufsize_) / rate_;
        uint64_t cycle = 0;
        long half = 0;
        const uint32_t n = uint32_t(bufsize_);
        float* planes[lv::kShmBusChannels];
        const float* cplanes[lv::kShmBusChannels];
        for (uint32_t c = 0; c < lv::kShmBusChannels; ++c) {
            planes[c] = scratch_[c].data();
            cplanes[c] = scratch_[c].data();
        }

        while (!stop_.load(std::memory_order_acquire)) {
            // Absolute schedule on QPC: cycle k fires at t0 + k * period (no cumulative drift).
            ++cycle;
            const double due_s = double(cycle) * period_s;
            QueryPerformanceCounter(&now);
            const double now_s = double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart);
            if (due_s > now_s && timer) {
                LARGE_INTEGER rel;
                rel.QuadPart = -LONGLONG((due_s - now_s) * 1e7);  // 100 ns units, relative
                SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            }

            // Inputs: network (RX ring) -> host buffers[half]
            bus_.driver_read_rx(planes, lv::kShmBusChannels, n);
            for (long c = 0; c < kChannels; ++c) {
                if (!in_active_[c]) continue;
                void* dst = reinterpret_cast<uint8_t*>(in_buf_[c].data()) + size_t(half) * n * 4;
                if (int32_) {
                    int32_t* d = static_cast<int32_t*>(dst);
                    for (uint32_t i = 0; i < n; ++i)
                        d[i] = int32_t(std::lrint(std::clamp(double(planes[c][i]), -1.0, 1.0) * 2147483647.0));
                } else {
                    std::memcpy(dst, planes[c], size_t(n) * 4);
                }
            }

            QueryPerformanceCounter(&now);
            time_ns_.store(uint64_t(double(now.QuadPart) * 1e9 / double(freq.QuadPart)));
            callbacks_->bufferSwitch(half, ASIOTrue);  // host processes in/out of buffers[half]

            // Outputs: host buffers[half] -> TX ring -> network
            for (long c = 0; c < kChannels; ++c) {
                if (!out_active_[c]) {
                    std::fill(scratch_[c].begin(), scratch_[c].end(), 0.0f);
                    continue;
                }
                const void* src = reinterpret_cast<const uint8_t*>(out_buf_[c].data()) + size_t(half) * n * 4;
                if (int32_) {
                    const int32_t* s = static_cast<const int32_t*>(src);
                    for (uint32_t i = 0; i < n; ++i) scratch_[c][i] = float(double(s[i]) / 2147483648.0);
                } else {
                    std::memcpy(scratch_[c].data(), src, size_t(n) * 4);
                }
            }
            bus_.driver_write_tx(cplanes, lv::kShmBusChannels, n);
            bus_.driver_heartbeat();
            sample_pos_.fetch_add(n);
            half ^= 1;
        }

        if (mmcss) {
            if (HMODULE avrt = GetModuleHandleW(L"avrt.dll")) {
                using AvRevert = BOOL(WINAPI*)(HANDLE);
                if (auto fn = reinterpret_cast<AvRevert>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"))) fn(mmcss);
            }
        }
        if (timer) CloseHandle(timer);
        timeEndPeriod(1);
    }

    std::atomic<long> refs_{1};
    lv::ShmAudioBus bus_;
    char err_[124] = {};
    bool int32_ = false;
    double rate_ = 48000.0;
    long bufsize_ = 0;
    ASIOCallbacks* callbacks_ = nullptr;
    std::vector<float> in_buf_[lv::kShmBusChannels];
    std::vector<float> out_buf_[lv::kShmBusChannels];
    std::vector<float> scratch_[lv::kShmBusChannels];
    bool in_active_[lv::kShmBusChannels] = {};
    bool out_active_[lv::kShmBusChannels] = {};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> sample_pos_{0};
    std::atomic<uint64_t> time_ns_{0};
};

class ClassFactory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *out = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }  // static instance
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* obj = new (std::nothrow) LanVidAsio();
        if (!obj) return E_OUTOFMEMORY;
        const HRESULT hr = obj->QueryInterface(riid, out);
        obj->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};
ClassFactory g_factory;

bool set_reg(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const LSTATUS st = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                      DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    if (!IsEqualCLSID(clsid, CLSID_LanVidASIO)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, out);
}

STDAPI DllCanUnloadNow() { return (g_objects.load() == 0 && g_locks.load() == 0) ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return SELFREG_E_CLASS;
    const std::wstring clsid_key = std::wstring(L"CLSID\\") + kClsidStr;
    const std::wstring asio_key = std::wstring(L"SOFTWARE\\ASIO\\") + kDriverName;
    const bool ok = set_reg(HKEY_CLASSES_ROOT, clsid_key, nullptr, kDriverName) &&
                    set_reg(HKEY_CLASSES_ROOT, clsid_key + L"\\InprocServer32", nullptr, path) &&
                    set_reg(HKEY_CLASSES_ROOT, clsid_key + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
                    set_reg(HKEY_LOCAL_MACHINE, asio_key, L"CLSID", kClsidStr) &&
                    set_reg(HKEY_LOCAL_MACHINE, asio_key, L"Description", kDriverName);
    return ok ? S_OK : SELFREG_E_CLASS;  // needs an elevated process (installer runs as admin)
}

STDAPI DllUnregisterServer() {
    const std::wstring clsid_key = std::wstring(L"CLSID\\") + kClsidStr;
    const std::wstring asio_key = std::wstring(L"SOFTWARE\\ASIO\\") + kDriverName;
    RegDeleteTreeW(HKEY_CLASSES_ROOT, clsid_key.c_str());
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_WRITE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        RegDeleteTreeW(k, kDriverName);
        RegCloseKey(k);
    }
    return S_OK;
}
