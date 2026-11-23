// LanVid Virtual 64ch — CoreAudio user-space AudioServerPlugIn (no kernel extension).
//
// Publishes one device with a 64-channel Float32 input stream (network -> DAW) and a 64-channel
// Float32 output stream (DAW -> network) at 48 / 96 kHz. Audio is exchanged with LanVid Studio /
// CLI through the lock-free shared-memory bus LanVidAudioBus_v1 (src/audio/shm_audio_bridge.*).
// The IO path only touches atomics and memcpy. Installed to /Library/Audio/Plug-Ins/HAL/.
//
// Object graph: PlugIn(1) -> Device(2) -> InputStream(3), OutputStream(4).

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "audio/shm_audio_bridge.hpp"

namespace {

constexpr AudioObjectID kObjPlugIn = kAudioObjectPlugInObject;
constexpr AudioObjectID kObjDevice = 2;
constexpr AudioObjectID kObjStreamIn = 3;
constexpr AudioObjectID kObjStreamOut = 4;

constexpr UInt32 kChannels = lv::kShmBusChannels;  // 64
constexpr UInt32 kZeroTsPeriod = 16384;           // frames per HAL timeline period
constexpr Float64 kRates[] = {48000.0, 96000.0};

const CFStringRef kDeviceUID = CFSTR("LanVidVirtual64ch_UID");
const CFStringRef kModelUID = CFSTR("LanVidVirtual64ch_Model");

AudioServerPlugInHostRef g_host = nullptr;
std::atomic<UInt32> g_refcount{1};
std::atomic<Float64> g_rate{48000.0};
std::atomic<UInt32> g_io_count{0};
std::atomic<UInt32> g_buffer_frames{0};
pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;  // non-IO state only

// Zero-timestamp timeline (written in StartIO, read on the IO thread).
std::atomic<UInt64> g_anchor_host{0};
std::atomic<Float64> g_ticks_per_frame{0.0};

lv::ShmAudioBus g_bus;

Float64 host_ticks_per_second() {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1e9 * Float64(tb.denom) / Float64(tb.numer);
}

AudioStreamBasicDescription make_asbd(Float64 rate) {
    AudioStreamBasicDescription d{};
    d.mSampleRate = rate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = AudioFormatFlags(kAudioFormatFlagIsFloat) | AudioFormatFlags(kAudioFormatFlagsNativeEndian) |
                     AudioFormatFlags(kAudioFormatFlagIsPacked);
    d.mBytesPerPacket = 4 * kChannels;
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = 4 * kChannels;
    d.mChannelsPerFrame = kChannels;
    d.mBitsPerChannel = 32;
    return d;
}

bool is_stream(AudioObjectID id) { return id == kObjStreamIn || id == kObjStreamOut; }

template <class T>
OSStatus put(void* out, UInt32 cap, UInt32* used, const T& v) {
    if (cap < sizeof(T)) return kAudioHardwareBadPropertySizeError;
    std::memcpy(out, &v, sizeof(T));
    *used = sizeof(T);
    return kAudioHardwareNoError;
}

OSStatus put_cfstring(void* out, UInt32 cap, UInt32* used, CFStringRef s) {
    if (cap < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
    CFRetain(s);  // caller releases
    *static_cast<CFStringRef*>(out) = s;
    *used = sizeof(CFStringRef);
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------------------------
// IUnknown + driver interface

HRESULT QueryInterface(void* drv, REFIID iid, LPVOID* out);
ULONG AddRef(void*) { return ++g_refcount; }
ULONG Release(void*) {
    UInt32 v = g_refcount.load();
    while (v > 0 && !g_refcount.compare_exchange_weak(v, v - 1)) {}
    return v > 0 ? v - 1 : 0;
}

OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host) {
    g_host = host;
    g_ticks_per_frame.store(host_ticks_per_second() / g_rate.load());
    g_bus.open(nullptr, true);  // creates LanVidAudioBus_v1 if LanVid Studio is not running yet
    return kAudioHardwareNoError;
}

OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*,
                      AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID dev, const AudioServerPlugInClientInfo*) {
    return dev == kObjDevice ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID dev, const AudioServerPlugInClientInfo*) {
    return dev == kObjDevice ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID dev, UInt64 action, void*) {
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    const Float64 rate = Float64(action);
    if (rate != kRates[0] && rate != kRates[1]) return kAudioHardwareBadPropertySizeError;
    pthread_mutex_lock(&g_state_mu);
    g_rate.store(rate);
    g_ticks_per_frame.store(host_ticks_per_second() / rate);
    pthread_mutex_unlock(&g_state_mu);
    return kAudioHardwareNoError;
}
OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) {
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------------------------
// Properties

Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a) {
    const AudioObjectPropertySelector s = a->mSelector;
    switch (obj) {
        case kObjPlugIn:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner: case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice: case kAudioPlugInPropertyResourceBundle:
                    return true;
            }
            return false;
        case kObjDevice:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner: case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType: case kAudioDevicePropertyRelatedDevices:
                case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams: case kAudioObjectPropertyControlList:
                case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyPreferredChannelsForStereo:
                    return true;
            }
            return false;
        case kObjStreamIn:
        case kObjStreamOut:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner: case kAudioObjectPropertyOwnedObjects:
                case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency: case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
            }
            return false;
    }
    return false;
}

OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                            const AudioObjectPropertyAddress* a, Boolean* out) {
    if (!HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = a->mSelector;
    *out = (obj == kObjDevice && s == kAudioDevicePropertyNominalSampleRate) ||
           (is_stream(obj) && (s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat ||
                               s == kAudioStreamPropertyIsActive));
    return kAudioHardwareNoError;
}

UInt32 device_stream_count(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeGlobal ? 2u : 1u;
}

OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                             const AudioObjectPropertyAddress* a, UInt32, const void*, UInt32* out) {
    if (!HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = a->mSelector;
    switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
        case kAudioPlugInPropertyResourceBundle:
            *out = sizeof(CFStringRef); return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice: *out = sizeof(AudioObjectID); return 0;
        case kAudioPlugInPropertyDeviceList: case kAudioDevicePropertyRelatedDevices:
            *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects:
            if (obj == kObjPlugIn) *out = sizeof(AudioObjectID);
            else if (obj == kObjDevice) *out = device_stream_count(a->mScope) * sizeof(AudioObjectID);
            else *out = 0;
            return 0;
        case kAudioDevicePropertyStreams: *out = device_stream_count(a->mScope) * sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyControlList: *out = 0; return 0;
        case kAudioDevicePropertyNominalSampleRate: *out = sizeof(Float64); return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: *out = 2 * sizeof(AudioValueRange); return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo: *out = 2 * sizeof(UInt32); return 0;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            *out = sizeof(AudioStreamBasicDescription); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            *out = 2 * sizeof(AudioStreamRangedDescription); return 0;
        default: *out = sizeof(UInt32); return 0;
    }
}

OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                         const AudioObjectPropertyAddress* a, UInt32 qsize, const void* qdata,
                         UInt32 cap, UInt32* used, void* out) {
    if (!HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = a->mSelector;
    const Float64 rate = g_rate.load();

    if (obj == kObjPlugIn) {
        switch (s) {
            case kAudioObjectPropertyBaseClass: return put<AudioClassID>(out, cap, used, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return put<AudioClassID>(out, cap, used, kAudioPlugInClassID);
            case kAudioObjectPropertyOwner: return put<AudioObjectID>(out, cap, used, kAudioObjectUnknown);
            case kAudioObjectPropertyManufacturer: return put_cfstring(out, cap, used, CFSTR("LanVid"));
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList:
                if (cap < sizeof(AudioObjectID)) { *used = 0; return 0; }
                return put<AudioObjectID>(out, cap, used, kObjDevice);
            case kAudioPlugInPropertyTranslateUIDToDevice: {
                if (qsize < sizeof(CFStringRef) || !qdata) return kAudioHardwareBadPropertySizeError;
                const CFStringRef uid = *static_cast<const CFStringRef*>(qdata);
                const bool match = uid && CFStringCompare(uid, kDeviceUID, 0) == kCFCompareEqualTo;
                return put<AudioObjectID>(out, cap, used, match ? kObjDevice : kAudioObjectUnknown);
            }
            case kAudioPlugInPropertyResourceBundle: return put_cfstring(out, cap, used, CFSTR(""));
        }
    } else if (obj == kObjDevice) {
        switch (s) {
            case kAudioObjectPropertyBaseClass: return put<AudioClassID>(out, cap, used, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return put<AudioClassID>(out, cap, used, kAudioDeviceClassID);
            case kAudioObjectPropertyOwner: return put<AudioObjectID>(out, cap, used, kObjPlugIn);
            case kAudioObjectPropertyName: return put_cfstring(out, cap, used, CFSTR("LanVid Virtual 64ch"));
            case kAudioObjectPropertyManufacturer: return put_cfstring(out, cap, used, CFSTR("LanVid"));
            case kAudioDevicePropertyDeviceUID: return put_cfstring(out, cap, used, kDeviceUID);
            case kAudioDevicePropertyModelUID: return put_cfstring(out, cap, used, kModelUID);
            case kAudioDevicePropertyTransportType: return put<UInt32>(out, cap, used, kAudioDeviceTransportTypeVirtual);
            case kAudioDevicePropertyRelatedDevices: return put<AudioObjectID>(out, cap, used, kObjDevice);
            case kAudioDevicePropertyClockDomain: return put<UInt32>(out, cap, used, 0);
            case kAudioDevicePropertyDeviceIsAlive: return put<UInt32>(out, cap, used, 1);
            case kAudioDevicePropertyDeviceIsRunning: return put<UInt32>(out, cap, used, g_io_count.load() > 0 ? 1 : 0);
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: return put<UInt32>(out, cap, used, 1);
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return put<UInt32>(out, cap, used, 1);
            case kAudioDevicePropertyLatency: return put<UInt32>(out, cap, used, 0);
            case kAudioDevicePropertySafetyOffset: return put<UInt32>(out, cap, used, 0);
            case kAudioDevicePropertyIsHidden: return put<UInt32>(out, cap, used, 0);
            case kAudioDevicePropertyZeroTimeStampPeriod: return put<UInt32>(out, cap, used, kZeroTsPeriod);
            case kAudioObjectPropertyControlList: *used = 0; return 0;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams: {
                AudioObjectID ids[2];
                UInt32 n = 0;
                if (a->mScope != kAudioObjectPropertyScopeOutput) ids[n++] = kObjStreamIn;
                if (a->mScope != kAudioObjectPropertyScopeInput) ids[n++] = kObjStreamOut;
                n = std::min<UInt32>(n, cap / sizeof(AudioObjectID));
                std::memcpy(out, ids, n * sizeof(AudioObjectID));
                *used = n * sizeof(AudioObjectID);
                return 0;
            }
            case kAudioDevicePropertyNominalSampleRate: return put<Float64>(out, cap, used, rate);
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange r[2] = {{kRates[0], kRates[0]}, {kRates[1], kRates[1]}};
                const UInt32 n = std::min<UInt32>(2, cap / sizeof(AudioValueRange));
                std::memcpy(out, r, n * sizeof(AudioValueRange));
                *used = n * sizeof(AudioValueRange);
                return 0;
            }
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                if (cap < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                UInt32 ch[2] = {1, 2};
                std::memcpy(out, ch, sizeof(ch));
                *used = sizeof(ch);
                return 0;
            }
        }
    } else if (is_stream(obj)) {
        switch (s) {
            case kAudioObjectPropertyBaseClass: return put<AudioClassID>(out, cap, used, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return put<AudioClassID>(out, cap, used, kAudioStreamClassID);
            case kAudioObjectPropertyOwner: return put<AudioObjectID>(out, cap, used, kObjDevice);
            case kAudioObjectPropertyOwnedObjects: *used = 0; return 0;
            case kAudioStreamPropertyIsActive: return put<UInt32>(out, cap, used, 1);
            case kAudioStreamPropertyDirection: return put<UInt32>(out, cap, used, obj == kObjStreamIn ? 1 : 0);
            case kAudioStreamPropertyTerminalType: return put<UInt32>(out, cap, used, kAudioStreamTerminalTypeLine);
            case kAudioStreamPropertyStartingChannel: return put<UInt32>(out, cap, used, 1);
            case kAudioStreamPropertyLatency: return put<UInt32>(out, cap, used, 0);
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: return put(out, cap, used, make_asbd(rate));
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription r[2];
                for (int i = 0; i < 2; ++i) {
                    r[i].mFormat = make_asbd(kRates[i]);
                    r[i].mSampleRateRange = {kRates[i], kRates[i]};
                }
                const UInt32 n = std::min<UInt32>(2, cap / sizeof(AudioStreamRangedDescription));
                std::memcpy(out, r, n * sizeof(AudioStreamRangedDescription));
                *used = n * sizeof(AudioStreamRangedDescription);
                return 0;
            }
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus request_rate(Float64 rate) {
    if (rate != kRates[0] && rate != kRates[1]) return kAudioDeviceUnsupportedFormatError;
    if (rate == g_rate.load()) return kAudioHardwareNoError;
    // The HAL calls PerformDeviceConfigurationChange when it is safe to switch.
    return g_host->RequestDeviceConfigurationChange(g_host, kObjDevice, UInt64(rate), nullptr);
}

OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a,
                         UInt32, const void*, UInt32 size, const void* data) {
    if (obj == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (size < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return request_rate(*static_cast<const Float64*>(data));
    }
    if (is_stream(obj) && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                           a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (size < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const auto* f = static_cast<const AudioStreamBasicDescription*>(data);
        if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != kChannels || f->mBitsPerChannel != 32)
            return kAudioDeviceUnsupportedFormatError;
        return request_rate(f->mSampleRate);
    }
    if (is_stream(obj) && a->mSelector == kAudioStreamPropertyIsActive) return kAudioHardwareNoError;
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------------------------
// IO

OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID dev, UInt32) {
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    if (g_io_count.fetch_add(1) == 0) {
        g_anchor_host.store(mach_absolute_time());
        g_bus.open(nullptr, true);
        g_bus.driver_set_running(true, UInt32(g_rate.load()), g_buffer_frames.load(), "CoreAudio HAL");
    }
    return kAudioHardwareNoError;
}

OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID dev, UInt32) {
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    UInt32 v = g_io_count.load();
    while (v > 0 && !g_io_count.compare_exchange_weak(v, v - 1)) {}
    if (v == 1) g_bus.driver_set_running(false, 0, 0, nullptr);
    return kAudioHardwareNoError;
}

OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64* sample_time,
                          UInt64* host_time, UInt64* seed) {
    // The device clock is the host clock: one zero timestamp every kZeroTsPeriod frames.
    const Float64 ticks_per_period = g_ticks_per_frame.load() * kZeroTsPeriod;
    const UInt64 anchor = g_anchor_host.load();
    const UInt64 now = mach_absolute_time();
    const UInt64 periods = UInt64(Float64(now - anchor) / ticks_per_period);
    *sample_time = Float64(periods) * kZeroTsPeriod;
    *host_time = anchor + UInt64(Float64(periods) * ticks_per_period);
    *seed = 1;
    return kAudioHardwareNoError;
}

OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 op, Boolean* will,
                           Boolean* in_place) {
    *will = (op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix);
    *in_place = true;
    return kAudioHardwareNoError;
}

OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                          const AudioServerPlugInIOCycleInfo*) {
    return kAudioHardwareNoError;
}

OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID dev, AudioObjectID stream, UInt32, UInt32 op,
                       UInt32 frames, const AudioServerPlugInIOCycleInfo*, void* main_buf, void*) {
    if (dev != kObjDevice || !main_buf) return kAudioHardwareNoError;
    if (g_buffer_frames.load(std::memory_order_relaxed) != frames) {
        g_buffer_frames.store(frames, std::memory_order_relaxed);
        if (auto* h = g_bus.header()) h->buffer_size.store(frames, std::memory_order_relaxed);
    }
    float* buf = static_cast<float*>(main_buf);
    if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjStreamIn) {
        g_bus.driver_read_rx_interleaved(buf, kChannels, frames);  // zero-filled on underrun
    } else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjStreamOut) {
        g_bus.driver_write_tx_interleaved(buf, kChannels, frames);  // drops when the host is away
        g_bus.driver_heartbeat();
    }
    return kAudioHardwareNoError;
}

OSStatus EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                        const AudioServerPlugInIOCycleInfo*) {
    return kAudioHardwareNoError;
}

AudioServerPlugInDriverInterface g_iface = {
    nullptr, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice,
    AddDeviceClient, RemoveDeviceClient, PerformDeviceConfigurationChange, AbortDeviceConfigurationChange,
    HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation,
};
AudioServerPlugInDriverInterface* g_iface_ptr = &g_iface;
AudioServerPlugInDriverRef g_driver = &g_iface_ptr;

HRESULT QueryInterface(void* drv, REFIID iid, LPVOID* out) {
    if (drv != g_driver || !out) return kAudioHardwareIllegalOperationError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(nullptr, iid);
    const bool ok = CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(req);
    if (!ok) {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ++g_refcount;
    *out = g_driver;
    return S_OK;
}

}  // namespace

// CFPlugIn factory named in Info.plist (CFPlugInFactories).
extern "C" __attribute__((visibility("default"))) void* LanVidAudioDriverFactory(CFAllocatorRef, CFUUIDRef type) {
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? g_driver : nullptr;
}
