// Loads the built LanVidAudio.driver like coreaudiod does (factory -> AudioServerPlugIn interface),
// acting as a minimal HAL host: checks the device/stream property graph, sample-rate switching,
// and that DoIOOperation moves 64-channel audio through LanVidAudioBus_v1 in both directions.
#include "mini_test.hpp"

#if defined(__APPLE__) && defined(LV_HAL_DRIVER_BINARY)
#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <sys/mman.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "audio/shm_audio_bridge.hpp"

namespace {
AudioServerPlugInDriverRef g_drv = nullptr;
OSStatus host_props_changed(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress*) { return 0; }
OSStatus host_copy(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* out) { *out = nullptr; return 0; }
OSStatus host_write(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef) { return 0; }
OSStatus host_delete(AudioServerPlugInHostRef, CFStringRef) { return 0; }
OSStatus host_request(AudioServerPlugInHostRef, AudioObjectID dev, UInt64 action, void* info) {
    return (*g_drv)->PerformDeviceConfigurationChange(g_drv, dev, action, info);  // apply immediately
}
AudioServerPlugInHostInterface g_host = {host_props_changed, host_copy, host_write, host_delete, host_request};

template <class T>
T get_prop(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
    T v{};
    UInt32 used = 0;
    (*g_drv)->GetPropertyData(g_drv, obj, 0, &a, 0, nullptr, sizeof(T), &used, &v);
    return v;
}
std::string get_str(AudioObjectID obj, AudioObjectPropertySelector sel) {
    CFStringRef s = get_prop<CFStringRef>(obj, sel);
    if (!s) return {};
    char buf[256] = {};
    CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}
}  // namespace

TEST(hal_driver_bundle_loads_and_streams) {
    char bus_name[64];
    std::snprintf(bus_name, sizeof(bus_name), "LanVidAudioBus_hal_%u", unsigned(lv::shm_bus_now_ms() & 0xFFFFFF));
    setenv("LANVID_AUDIO_BUS_NAME", bus_name, 1);

    void* lib = dlopen(LV_HAL_DRIVER_BINARY, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(lib != nullptr);
    using Factory = void* (*)(CFAllocatorRef, CFUUIDRef);
    auto factory = reinterpret_cast<Factory>(dlsym(lib, "LanVidAudioDriverFactory"));
    REQUIRE(factory != nullptr);
    CHECK(factory(nullptr, kAudioServerPlugInDriverInterfaceUUID) == nullptr);  // wrong type rejected
    g_drv = static_cast<AudioServerPlugInDriverRef>(factory(nullptr, kAudioServerPlugInTypeUUID));
    REQUIRE(g_drv != nullptr);

    void* iface = nullptr;
    CHECK((*g_drv)->QueryInterface(g_drv, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &iface) == S_OK);
    CHECK(iface == g_drv);
    REQUIRE((*g_drv)->Initialize(g_drv, &g_host) == 0);

    // ---- property graph ----
    CHECK(get_prop<AudioObjectID>(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList) == 2);
    CHECK(get_str(2, kAudioObjectPropertyName) == "LanVid Virtual 64ch");
    CHECK(get_str(2, kAudioDevicePropertyDeviceUID) == "LanVidVirtual64ch_UID");
    CHECK(get_prop<UInt32>(2, kAudioDevicePropertyTransportType) == kAudioDeviceTransportTypeVirtual);
    {
        AudioObjectPropertyAddress a{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 sz = 0;
        (*g_drv)->GetPropertyDataSize(g_drv, 2, 0, &a, 0, nullptr, &sz);
        CHECK(sz == 2 * sizeof(AudioObjectID));
        AudioObjectID ids[2] = {};
        UInt32 used = 0;
        (*g_drv)->GetPropertyData(g_drv, 2, 0, &a, 0, nullptr, sizeof(ids), &used, ids);
        CHECK(used == sizeof(ids) && ids[0] == 3 && ids[1] == 4);
        a.mScope = kAudioObjectPropertyScopeInput;
        (*g_drv)->GetPropertyData(g_drv, 2, 0, &a, 0, nullptr, sizeof(ids), &used, ids);
        CHECK(used == sizeof(AudioObjectID) && ids[0] == 3);
    }
    CHECK(get_prop<UInt32>(3, kAudioStreamPropertyDirection) == 1);  // input
    CHECK(get_prop<UInt32>(4, kAudioStreamPropertyDirection) == 0);  // output
    const auto fmt = get_prop<AudioStreamBasicDescription>(4, kAudioStreamPropertyVirtualFormat);
    CHECK(fmt.mChannelsPerFrame == 64 && fmt.mBitsPerChannel == 32 && fmt.mSampleRate == 48000.0);
    CHECK((fmt.mFormatFlags & kAudioFormatFlagIsFloat) != 0);

    // ---- sample-rate switch 48k -> 96k -> 48k ----
    {
        AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        Boolean settable = false;
        (*g_drv)->IsPropertySettable(g_drv, 2, 0, &a, &settable);
        CHECK(settable);
        Float64 r = 96000.0;
        CHECK((*g_drv)->SetPropertyData(g_drv, 2, 0, &a, 0, nullptr, sizeof(r), &r) == 0);
        CHECK(get_prop<Float64>(2, kAudioDevicePropertyNominalSampleRate) == 96000.0);
        r = 44100.0;
        CHECK((*g_drv)->SetPropertyData(g_drv, 2, 0, &a, 0, nullptr, sizeof(r), &r) != 0);  // unsupported
        r = 48000.0;
        (*g_drv)->SetPropertyData(g_drv, 2, 0, &a, 0, nullptr, sizeof(r), &r);
    }

    // ---- IO through the shared-memory bus ----
    lv::ShmAudioBus host;
    REQUIRE(host.open(bus_name));
    CHECK(!host.driver_connected());
    REQUIRE((*g_drv)->StartIO(g_drv, 2, 1) == 0);
    CHECK(get_prop<UInt32>(2, kAudioDevicePropertyDeviceIsRunning) == 1);

    constexpr UInt32 kN = 256;
    // host -> RX (DAW input): channel c carries c + i/1000
    std::vector<std::vector<float>> rxp(64, std::vector<float>(kN));
    std::vector<const float*> rxpp(64);
    for (int c = 0; c < 64; ++c) {
        for (UInt32 i = 0; i < kN; ++i) rxp[c][i] = float(c) + float(i) / 1000.0f;
        rxpp[c] = rxp[c].data();
    }
    host.host_write_rx(rxpp.data(), 64, kN);

    std::vector<float> io(size_t(64) * kN);
    AudioServerPlugInIOCycleInfo cycle{};  // the HAL always passes a (non-null) cycle info
    (*g_drv)->DoIOOperation(g_drv, 2, 3, 1, kAudioServerPlugInIOOperationReadInput, kN, &cycle, io.data(), nullptr);
    int bad_in = 0;
    for (UInt32 i = 0; i < kN; ++i)
        for (int c = 0; c < 64; ++c)
            if (io[size_t(i) * 64 + c] != float(c) + float(i) / 1000.0f) ++bad_in;

    // DAW output -> TX -> host: channel c carries -(c + i/1000)
    for (UInt32 i = 0; i < kN; ++i)
        for (int c = 0; c < 64; ++c) io[size_t(i) * 64 + c] = -(float(c) + float(i) / 1000.0f);
    (*g_drv)->DoIOOperation(g_drv, 2, 4, 1, kAudioServerPlugInIOOperationWriteMix, kN, &cycle, io.data(), nullptr);
    CHECK(host.driver_connected());
    CHECK(host.header()->buffer_size.load() == kN);
    CHECK(std::string(host.header()->driver_name) == "CoreAudio HAL");
    std::vector<std::vector<float>> txp(64, std::vector<float>(kN));
    std::vector<float*> txpp(64);
    for (int c = 0; c < 64; ++c) txpp[c] = txp[c].data();
    CHECK(host.host_read_tx(txpp.data(), 64, kN) == kN);
    int bad_out = 0;
    for (int c = 0; c < 64; ++c)
        for (UInt32 i = 0; i < kN; ++i)
            if (txp[c][i] != -(float(c) + float(i) / 1000.0f)) ++bad_out;

    // Zero timestamps advance with the host clock.
    Float64 st = 0; UInt64 ht = 0, seed = 0;
    CHECK((*g_drv)->GetZeroTimeStamp(g_drv, 2, 1, &st, &ht, &seed) == 0);
    CHECK(seed == 1 && std::fmod(st, 16384.0) == 0.0);

    CHECK((*g_drv)->StopIO(g_drv, 2, 1) == 0);
    CHECK(!host.driver_connected());
    std::printf("    [HAL driver] props OK | ReadInput mismatches=%d | WriteMix mismatches=%d | 64ch x %u frames\n",
                bad_in, bad_out, unsigned(kN));
    CHECK(bad_in == 0);
    CHECK(bad_out == 0);
    host.close();
    char full[80];
    std::snprintf(full, sizeof(full), "/%s", bus_name);
    shm_unlink(full);
    unsetenv("LANVID_AUDIO_BUS_NAME");
}
#endif
