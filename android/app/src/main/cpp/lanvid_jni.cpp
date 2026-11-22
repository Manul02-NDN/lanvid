// LanVid Android JNI Bridge:
//  - Zero-copy YUV_420_888 direct ByteBuffer encoding (Camera2) -> liblanvid_core.so
//  - Zero-copy RGBA_8888 direct ByteBuffer encoding (MediaProjection) -> liblanvid_core.so
//  - Low-latency ANativeWindow video rendering thread (hardware-composed SurfaceView)
//  - Low-latency native AAudio playback stream pulling directly from lock-free AudioJitterBuffer

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lanvid_c_api.h"

#define LV_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "LanVidJNI", __VA_ARGS__)
#define LV_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "LanVidJNI", __VA_ARGS__)

namespace {

std::mutex g_mu;
LvSenderHandle* g_sender = nullptr;
LvReceiverHandle* g_receiver = nullptr;

// Native Window surface renderer state
std::mutex g_surf_mu;
ANativeWindow* g_window = nullptr;
std::thread g_render_thread;
std::atomic<bool> g_render_stop{false};

// Native AAudio playback state
AAudioStream* g_audio_stream = nullptr;
std::atomic<bool> g_audio_active{false};

aaudio_data_callback_result_t aaudio_data_cb(AAudioStream*, void* user_data, void* audio_data,
                                             int32_t num_frames) {
    auto* rx = static_cast<LvReceiverHandle*>(user_data);
    auto* out = static_cast<int16_t*>(audio_data);
    if (!rx || !out || num_frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    lv_receiver_pop_audio_pcm16(rx, out, num_frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void start_aaudio(LvReceiverHandle* rx) {
    if (g_audio_stream || !rx) return;
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) return;

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, aaudio_data_cb, rx);

    AAudioStream* stream = nullptr;
    if (AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK && stream) {
        if (AAudioStream_requestStart(stream) == AAUDIO_OK) {
            g_audio_stream = stream;
            g_audio_active.store(true, std::memory_order_release);
            LV_LOGI("AAudio low-latency playback started (48kHz stereo PCM16)");
        } else {
            AAudioStream_close(stream);
        }
    }
    AAudioStreamBuilder_delete(builder);
}

void stop_aaudio() {
    g_audio_active.store(false, std::memory_order_release);
    if (g_audio_stream) {
        AAudioStream_requestStop(g_audio_stream);
        AAudioStream_close(g_audio_stream);
        g_audio_stream = nullptr;
    }
}

void stop_render_loop() {
    g_render_stop.store(true, std::memory_order_release);
    if (g_render_thread.joinable()) g_render_thread.join();
}

void start_render_loop(LvReceiverHandle* rx) {
    stop_render_loop();
    g_render_stop.store(false, std::memory_order_release);
    g_render_thread = std::thread([rx]() {
        std::vector<uint8_t> rgba;
        uint32_t last_seq = 0;
        int cur_w = 0, cur_h = 0;

        while (!g_render_stop.load(std::memory_order_relaxed)) {
            int w = 0, h = 0;
            int rc = lv_receiver_get_latest_frame(rx, rgba.data(), rgba.size(), &w, &h, &last_seq, 1);
            if (rc == -1 && w > 0 && h > 0) {
                rgba.resize(size_t(w) * size_t(h) * 4);
                rc = lv_receiver_get_latest_frame(rx, rgba.data(), rgba.size(), &w, &h, &last_seq, 1);
            }
            if (rc == 1 && w > 0 && h > 0) {
                std::lock_guard<std::mutex> slk(g_surf_mu);
                if (g_window) {
                    if (w != cur_w || h != cur_h) {
                        ANativeWindow_setBuffersGeometry(g_window, w, h, WINDOW_FORMAT_RGBA_8888);
                        cur_w = w;
                        cur_h = h;
                    }
                    ANativeWindow_Buffer wb{};
                    if (ANativeWindow_lock(g_window, &wb, nullptr) == 0 && wb.bits) {
                        const int copy_w = std::min(w, wb.width);
                        const int copy_h = std::min(h, wb.height);
                        const size_t row_bytes = size_t(copy_w) * 4;
                        auto* dst = static_cast<uint8_t*>(wb.bits);
                        for (int y = 0; y < copy_h; ++y) {
                            std::memcpy(dst + size_t(y) * size_t(wb.stride) * 4,
                                        rgba.data() + size_t(y) * size_t(w) * 4, row_bytes);
                        }
                        ANativeWindow_unlockAndPost(g_window);
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    });
}

std::string jstr_to_std(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string out(c ? c : "");
    if (c) env->ReleaseStringUTFChars(js, c);
    return out;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_com_lanvid_app_LanVidNative_nativeInit(JNIEnv*, jclass) {
    return lv_init();
}

JNIEXPORT jstring JNICALL Java_com_lanvid_app_LanVidNative_nativeGetSimdInfo(JNIEnv* env, jclass) {
    return env->NewStringUTF(lv_get_cpu_simd_info());
}

JNIEXPORT jstring JNICALL Java_com_lanvid_app_LanVidNative_nativeEnumerateDevices(JNIEnv* env, jclass) {
    char* j = lv_enumerate_devices();
    jstring out = env->NewStringUTF(j ? j : "{}");
    lv_free_string(j);
    return out;
}

JNIEXPORT jstring JNICALL Java_com_lanvid_app_LanVidNative_nativeScanLan(JNIEnv* env, jclass,
                                                                         jint timeout_ms) {
    char* j = lv_discovery_scan(timeout_ms);
    jstring out = env->NewStringUTF(j ? j : "[]");
    lv_free_string(j);
    return out;
}

JNIEXPORT jboolean JNICALL Java_com_lanvid_app_LanVidNative_nativeStartSender(
    JNIEnv* env, jclass, jstring dest_ip, jint dest_port, jint width, jint height, jint fps,
    jint quality, jint fec_group, jboolean adaptive) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_sender) g_sender = lv_sender_create();
    LvSenderConfig sc;
    lv_sender_config_default(&sc);
    const std::string ip = jstr_to_std(env, dest_ip);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "%s", ip.c_str());
    sc.dest_port = uint16_t(std::clamp(int(dest_port), 1024, 65535));
    sc.source_type = LV_SOURCE_EXTERNAL;
    sc.width = width;
    sc.height = height;
    sc.fps = double(fps > 0 ? fps : 60);
    sc.quality = std::clamp(int(quality), 10, 98);
    sc.fec_group_size = std::max(0, int(fec_group));
    sc.adaptive_quality = adaptive ? 1 : 0;
    sc.q_min = 35;
    sc.q_max = 85;
    sc.threads = 0;
    return lv_sender_start(g_sender, &sc) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_lanvid_app_LanVidNative_nativeStopSender(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_sender) lv_sender_stop(g_sender);
}

JNIEXPORT jboolean JNICALL Java_com_lanvid_app_LanVidNative_nativePushYuv420(
    JNIEnv* env, jclass, jobject y_buf, jobject u_buf, jobject v_buf, jint width, jint height,
    jint y_stride, jint u_stride, jint v_stride, jint uv_pixel_stride) {
    if (!g_sender || !y_buf || !u_buf || !v_buf) return JNI_FALSE;
    const auto* y = static_cast<const uint8_t*>(env->GetDirectBufferAddress(y_buf));
    const auto* u = static_cast<const uint8_t*>(env->GetDirectBufferAddress(u_buf));
    const auto* v = static_cast<const uint8_t*>(env->GetDirectBufferAddress(v_buf));
    if (!y || !u || !v) return JNI_FALSE;
    return lv_sender_push_yuv420_frame(g_sender, y, u, v, width, height, y_stride, u_stride,
                                       v_stride, uv_pixel_stride, 0)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_lanvid_app_LanVidNative_nativePushRgba(
    JNIEnv* env, jclass, jobject rgba_buf, jint width, jint height, jint stride) {
    if (!g_sender || !rgba_buf) return JNI_FALSE;
    const auto* ptr = static_cast<const uint8_t*>(env->GetDirectBufferAddress(rgba_buf));
    if (!ptr) return JNI_FALSE;
    return lv_sender_push_raw_frame(g_sender, ptr, width, height, stride, LV_PIXFMT_RGBA, 0)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_lanvid_app_LanVidNative_nativePushAudioPcm16(
    JNIEnv* env, jclass, jshortArray pcm, jint num_frames, jint sample_rate, jint channels) {
    if (!g_sender || !pcm || num_frames <= 0) return JNI_FALSE;
    jshort* data = env->GetShortArrayElements(pcm, nullptr);
    if (!data) return JNI_FALSE;
    const int ok = lv_sender_push_audio_pcm16(g_sender, reinterpret_cast<const int16_t*>(data),
                                              num_frames, sample_rate, channels, 0);
    env->ReleaseShortArrayElements(pcm, data, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_lanvid_app_LanVidNative_nativeGetSenderStatsJson(JNIEnv* env, jclass) {
    LvSenderStats st{};
    if (g_sender) lv_sender_get_stats(g_sender, &st);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"running\":%d,\"width\":%d,\"height\":%d,\"fps\":%.1f,\"mbps\":%.2f,"
                  "\"enc_ms\":%.2f,\"q\":%d,\"loss\":%.2f,\"audio_kbps\":%.0f,\"frames\":%llu}",
                  st.running, st.width, st.height, st.tx_fps, st.tx_mbps, st.encode_tx_ms,
                  st.active_quality, st.rx_loss_pct, st.audio_kbps,
                  static_cast<unsigned long long>(st.frames_sent));
    return env->NewStringUTF(buf);
}

JNIEXPORT jboolean JNICALL Java_com_lanvid_app_LanVidNative_nativeStartReceiver(
    JNIEnv* env, jclass, jint port, jdouble audio_target_ms, jstring device_name) {
    std::lock_guard<std::mutex> lk(g_mu);
    stop_render_loop();
    stop_aaudio();
    if (!g_receiver) g_receiver = lv_receiver_create();

    LvReceiverConfig rc;
    lv_receiver_config_default(&rc);
    rc.bind_port = uint16_t(std::clamp(int(port), 1024, 65535));
    rc.audio_target_ms = audio_target_ms > 0.0 ? audio_target_ms : 12.0;
    rc.enable_audio = 0;
    rc.external_audio_pull = 1;  // AAudio pulls directly via lv_receiver_pop_audio_pcm16
    rc.enable_discovery = 1;
    const std::string dn = jstr_to_std(env, device_name);
    if (!dn.empty()) std::snprintf(rc.device_name, sizeof(rc.device_name), "%s", dn.c_str());

    if (!lv_receiver_start(g_receiver, &rc)) return JNI_FALSE;
    start_aaudio(g_receiver);
    start_render_loop(g_receiver);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_lanvid_app_LanVidNative_nativeStopReceiver(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    stop_render_loop();
    stop_aaudio();
    if (g_receiver) lv_receiver_stop(g_receiver);
}

JNIEXPORT void JNICALL Java_com_lanvid_app_LanVidNative_nativeSetReceiverSurface(JNIEnv* env, jclass,
                                                                                 jobject surface) {
    std::lock_guard<std::mutex> slk(g_surf_mu);
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    if (surface) {
        g_window = ANativeWindow_fromSurface(env, surface);
    }
}

JNIEXPORT void JNICALL Java_com_lanvid_app_LanVidNative_nativeSetVolume(JNIEnv*, jclass, jfloat vol) {
    if (g_receiver) lv_receiver_set_volume(g_receiver, vol);
}

JNIEXPORT jstring JNICALL Java_com_lanvid_app_LanVidNative_nativeGetReceiverStatsJson(JNIEnv* env, jclass) {
    LvReceiverStats st{};
    if (g_receiver) lv_receiver_get_stats(g_receiver, &st);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"running\":%d,\"width\":%d,\"height\":%d,\"fps\":%.1f,\"mbps\":%.2f,"
                  "\"lat_p50\":%.2f,\"lat_p99\":%.2f,\"loss\":%.2f,\"fec\":%llu,"
                  "\"audio_kbps\":%.0f,\"audio_buf\":%.1f}",
                  st.running, st.width, st.height, st.fps, st.rx_mbps, st.latency_p50_ms,
                  st.latency_p99_ms, st.loss_pct, static_cast<unsigned long long>(st.fec_recovered),
                  st.audio_kbps, st.audio_buffer_ms);
    return env->NewStringUTF(buf);
}

}  // extern "C"
