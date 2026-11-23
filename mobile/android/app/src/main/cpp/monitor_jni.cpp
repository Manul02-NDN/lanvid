// LanVid Monitor - native engine (JNI): LanVid core C ABI (lv_monitor_client_*) + AAudio low-latency
// playback. The audio path has no Java at all: AAudio's real-time data callback pulls the finished stereo
// mix straight from the core's playout buffer.
//
//   network thread : lv_monitor_client_poll every 1 ms, channel names/VU 15 Hz, keepalive 1 s,
//                    re-opens the AAudio stream after a disconnect (headphones unplugged, route change)
//   AAudio callback: lv_monitor_client_pop_stereo (float, 2 ch, 48 kHz)

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lanvid/lanvid_sdk.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "LanVidMonitor", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "LanVidMonitor", __VA_ARGS__)

namespace {

constexpr int32_t kRate = 48000;

struct MonitorCtx {
    LvMonitorClientHandle* cli = nullptr;
    std::atomic<LvMonitorClientHandle*> rt_cli{nullptr};  // what the audio callback may use
    AAudioStream* stream = nullptr;
    std::mutex stream_mu;
    std::thread io;
    std::atomic<bool> running{false};
    std::atomic<bool> reopen_audio{false};

    std::mutex meta_mu;
    std::vector<LV_MonitorMetaChannel> meta;
    LV_MonitorTelemetry tel{};

    std::atomic<int32_t> burst{0}, buffer_frames{0}, device_rate{0}, exclusive{0};
    std::atomic<int32_t> xruns{0};
};

aaudio_data_callback_result_t on_audio(AAudioStream*, void* ud, void* data, int32_t frames) {
    auto* c = static_cast<MonitorCtx*>(ud);
    float* out = static_cast<float*>(data);
    LvMonitorClientHandle* cli = c->rt_cli.load(std::memory_order_acquire);
    if (cli && frames > 0) lv_monitor_client_pop_stereo(cli, out, uint32_t(frames));
    else std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void on_audio_error(AAudioStream*, void* ud, aaudio_result_t err) {
    // Not allowed to close/reopen here: the network thread does it.
    LOGW("AAudio stream error %d (%s) - reopening", int(err), AAudio_convertResultToText(err));
    static_cast<MonitorCtx*>(ud)->reopen_audio.store(true, std::memory_order_release);
}

AAudioStream* open_stream(MonitorCtx* c, aaudio_sharing_mode_t sharing) {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return nullptr;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, sharing);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, kRate);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, &on_audio, c);
    AAudioStreamBuilder_setErrorCallback(b, &on_audio_error, c);
    AAudioStream* s = nullptr;
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !s) return nullptr;
    if (AAudioStream_getSampleRate(s) != kRate) {  // exclusive MMAP at another rate: let SHARED convert
        AAudioStream_close(s);
        return nullptr;
    }
    return s;
}

void close_audio(MonitorCtx* c) {
    std::lock_guard<std::mutex> lk(c->stream_mu);
    if (c->stream) {
        AAudioStream_requestStop(c->stream);
        AAudioStream_close(c->stream);
        c->stream = nullptr;
    }
}

bool start_audio(MonitorCtx* c) {
    std::lock_guard<std::mutex> lk(c->stream_mu);
    if (c->stream) return true;
    AAudioStream* s = open_stream(c, AAUDIO_SHARING_MODE_EXCLUSIVE);
    c->exclusive.store(s ? 1 : 0);
    if (!s) s = open_stream(c, AAUDIO_SHARING_MODE_SHARED);
    if (!s) {
        LOGW("AAudio: cannot open a low-latency output stream");
        return false;
    }
    const int32_t burst = AAudioStream_getFramesPerBurst(s);
    // Two bursts: the minimum that survives scheduling jitter without underruns on most devices.
    AAudioStream_setBufferSizeInFrames(s, burst * 2);
    c->burst.store(burst);
    c->buffer_frames.store(AAudioStream_getBufferSizeInFrames(s));
    c->device_rate.store(AAudioStream_getSampleRate(s));
    if (AAudioStream_requestStart(s) != AAUDIO_OK) {
        AAudioStream_close(s);
        return false;
    }
    c->stream = s;
    LOGI("AAudio: %s, burst %d, buffer %d frames @ %d Hz, perf %d", c->exclusive.load() ? "EXCLUSIVE" : "SHARED",
         burst, int(c->buffer_frames.load()), int(c->device_rate.load()), int(AAudioStream_getPerformanceMode(s)));
    return true;
}

void io_loop(MonitorCtx* c) {
    using clk = std::chrono::steady_clock;
    auto last_meta = clk::now(), last_keep = clk::now(), last_xrun = clk::now();
    std::vector<LV_MonitorMetaChannel> buf(64);
    while (c->running.load(std::memory_order_acquire)) {
        lv_monitor_client_poll(c->cli);
        const auto now = clk::now();
        if (now - last_keep > std::chrono::seconds(1)) {
            lv_monitor_client_keepalive(c->cli);
            last_keep = now;
        }
        if (now - last_meta > std::chrono::milliseconds(66)) {
            lv_monitor_client_request_meta(c->cli);
            LV_MonitorTelemetry tel{};
            const int n = lv_monitor_client_get_meta(c->cli, buf.data(), int(buf.size()), &tel);
            {
                std::lock_guard<std::mutex> lk(c->meta_mu);
                c->meta.assign(buf.begin(), buf.begin() + std::max(0, std::min(n, int(buf.size()))));
                c->tel = tel;
            }
            last_meta = now;
        }
        if (now - last_xrun > std::chrono::milliseconds(500)) {
            std::lock_guard<std::mutex> lk(c->stream_mu);
            if (c->stream) c->xruns.store(AAudioStream_getXRunCount(c->stream));
            last_xrun = now;
        }
        if (c->reopen_audio.exchange(false)) {
            close_audio(c);
            start_audio(c);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

MonitorCtx* ctx(jlong h) { return reinterpret_cast<MonitorCtx*>(h); }

std::string str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

void stop_all(MonitorCtx* c) {
    c->rt_cli.store(nullptr, std::memory_order_release);
    close_audio(c);
    c->running.store(false, std::memory_order_release);
    if (c->io.joinable()) c->io.join();
    if (c->cli) {
        lv_monitor_client_disconnect(c->cli);
        lv_monitor_client_destroy(c->cli);
        c->cli = nullptr;
    }
    std::lock_guard<std::mutex> lk(c->meta_mu);
    c->meta.clear();
    c->tel = {};
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_lanvid_monitor_MonitorNative_nativeCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(new MonitorCtx());
}

JNIEXPORT void JNICALL Java_com_lanvid_monitor_MonitorNative_nativeDestroy(JNIEnv*, jclass, jlong h) {
    if (!h) return;
    stop_all(ctx(h));
    delete ctx(h);
}

// Returns 0 on success, otherwise a LanVid error code (< 0).
JNIEXPORT jint JNICALL Java_com_lanvid_monitor_MonitorNative_nativeConnect(JNIEnv* env, jclass, jlong h, jstring host,
                                                                          jint port, jstring name, jint bitrate,
                                                                          jint latency) {
    if (!h) return -1;
    MonitorCtx* c = ctx(h);
    stop_all(c);
    c->cli = lv_monitor_client_create(kRate);
    if (!c->cli) return -1;
    const int rc = lv_monitor_client_connect(c->cli, str(env, host).c_str(), uint16_t(port), str(env, name).c_str(),
                                             uint16_t(bitrate), uint16_t(latency));
    if (rc != LV_OK) {
        lv_monitor_client_destroy(c->cli);
        c->cli = nullptr;
        return rc;
    }
    c->running.store(true, std::memory_order_release);
    c->io = std::thread(io_loop, c);
    c->rt_cli.store(c->cli, std::memory_order_release);
    if (!start_audio(c)) return -2;
    return 0;
}

JNIEXPORT void JNICALL Java_com_lanvid_monitor_MonitorNative_nativeDisconnect(JNIEnv*, jclass, jlong h) {
    if (h) stop_all(ctx(h));
}

JNIEXPORT void JNICALL Java_com_lanvid_monitor_MonitorNative_nativeSetChannelMix(JNIEnv*, jclass, jlong h, jint ch,
                                                                                jfloat gain_db, jfloat pan,
                                                                                jboolean mute, jboolean solo) {
    if (h && ctx(h)->cli) lv_monitor_client_set_channel_mix(ctx(h)->cli, ch, gain_db, pan, mute ? 1 : 0, solo ? 1 : 0);
}

// p = [hpf_hz, eq_low, eq_mid_hz, eq_mid, eq_high, comp_thresh, comp_ratio, comp_attack, comp_release,
//      comp_makeup, master_volume]; flags: bit0 hpf, bit1 eq, bit2 comp, bit3 master mute.
JNIEXPORT void JNICALL Java_com_lanvid_monitor_MonitorNative_nativeSetDsp(JNIEnv* env, jclass, jlong h, jfloatArray arr,
                                                                         jint flags) {
    if (!h || !ctx(h)->cli || !arr || env->GetArrayLength(arr) < 11) return;
    float p[11];
    env->GetFloatArrayRegion(arr, 0, 11, p);
    LV_MonitorDspParams d{};
    d.hpf_enabled = (flags & 1) ? 1 : 0;
    d.hpf_hz = p[0];
    d.eq_enabled = (flags & 2) ? 1 : 0;
    d.eq_low_db = p[1];
    d.eq_mid_hz = p[2];
    d.eq_mid_db = p[3];
    d.eq_high_db = p[4];
    d.comp_enabled = (flags & 4) ? 1 : 0;
    d.comp_thresh_db = p[5];
    d.comp_ratio = p[6];
    d.comp_attack_ms = p[7];
    d.comp_release_ms = p[8];
    d.comp_makeup_db = p[9];
    d.master_volume_db = p[10];
    d.master_mute = (flags & 8) ? 1 : 0;
    lv_monitor_client_set_dsp(ctx(h)->cli, &d);
}

JNIEXPORT jint JNICALL Java_com_lanvid_monitor_MonitorNative_nativeSetStream(JNIEnv*, jclass, jlong h, jint bitrate,
                                                                            jint latency) {
    if (!h || !ctx(h)->cli) return -1;
    return lv_monitor_client_set_stream(ctx(h)->cli, uint16_t(bitrate), uint16_t(latency));
}

// Fills names[64 * 16] (raw UTF-8, NUL-padded - decoded in Kotlin, which tolerates a multi-byte character cut
// by the 15-byte wire limit), peaks[64] (dBFS) and tel[10] = {connected, gain_reduction_db, mix_peak_dbfs,
// buffer_ms, audio_packets, aaudio_burst, aaudio_buffer_frames, device_rate, exclusive, xruns}.
// Returns the channel count.
JNIEXPORT jint JNICALL Java_com_lanvid_monitor_MonitorNative_nativeGetMeta(JNIEnv* env, jclass, jlong h,
                                                                          jbyteArray names, jbyteArray peaks,
                                                                          jdoubleArray tel) {
    if (!h) return 0;
    MonitorCtx* c = ctx(h);
    std::vector<LV_MonitorMetaChannel> meta;
    LV_MonitorTelemetry t{};
    {
        std::lock_guard<std::mutex> lk(c->meta_mu);
        meta = c->meta;
        t = c->tel;
    }
    if (tel && env->GetArrayLength(tel) >= 10) {
        const jdouble v[10] = {double(t.connected), double(t.bus_gain_reduction_db), double(t.bus_peak_dbfs),
                               t.buffer_ms, double(t.audio_packets_rx), double(c->burst.load()),
                               double(c->buffer_frames.load()), double(c->device_rate.load()),
                               double(c->exclusive.load()), double(c->xruns.load())};
        env->SetDoubleArrayRegion(tel, 0, 10, v);
    }
    const int n = std::min<int>(int(meta.size()), 64);
    if (peaks && env->GetArrayLength(peaks) >= n && n > 0) {
        std::vector<jbyte> pk(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) pk[size_t(i)] = jbyte(meta[size_t(i)].peak_dbfs);
        env->SetByteArrayRegion(peaks, 0, n, pk.data());
    }
    if (names && env->GetArrayLength(names) >= n * 16 && n > 0) {
        std::vector<jbyte> nb(static_cast<size_t>(n) * 16, 0);
        for (int i = 0; i < n; ++i) std::memcpy(nb.data() + size_t(i) * 16, meta[size_t(i)].name, 16);
        env->SetByteArrayRegion(names, 0, n * 16, nb.data());
    }
    return n;
}

}  // extern "C"
