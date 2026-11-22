// LanVid Android SDK — Multi-instance Handle-Based JNI Bridge
// Uses zero-copy lv_receiver_set_video_callback -> ANativeWindow + native AAudio playback.

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "lanvid/lanvid_sdk.h"

namespace {

struct SdkReceiverCtx {
    LvReceiverHandle* rx = nullptr;
    std::mutex surf_mu;
    ANativeWindow* window = nullptr;
    int win_w = 0;
    int win_h = 0;
    AAudioStream* audio = nullptr;
};

aaudio_data_callback_result_t aaudio_cb(AAudioStream*, void* ud, void* data, int32_t num_frames) {
    auto* rx = static_cast<LvReceiverHandle*>(ud);
    if (rx && data && num_frames > 0) {
        lv_receiver_pop_audio_pcm16(rx, static_cast<int16_t*>(data), num_frames);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void on_video_cb(const LV_VideoFrameView* f, void* ud) {
    auto* ctx = static_cast<SdkReceiverCtx*>(ud);
    if (!ctx || !f || !f->data || f->width <= 0 || f->height <= 0) return;

    std::lock_guard<std::mutex> lk(ctx->surf_mu);
    if (!ctx->window) return;
    if (ctx->win_w != f->width || ctx->win_h != f->height) {
        ANativeWindow_setBuffersGeometry(ctx->window, f->width, f->height, WINDOW_FORMAT_RGBA_8888);
        ctx->win_w = f->width;
        ctx->win_h = f->height;
    }
    ANativeWindow_Buffer wb{};
    if (ANativeWindow_lock(ctx->window, &wb, nullptr) == 0 && wb.bits) {
        const int cw = std::min(int(f->width), wb.width);
        const int ch = std::min(int(f->height), wb.height);
        const size_t row_bytes = size_t(cw) * 4;
        auto* dst = static_cast<uint8_t*>(wb.bits);
        for (int y = 0; y < ch; ++y) {
            std::memcpy(dst + size_t(y) * size_t(wb.stride) * 4,
                        f->data + size_t(y) * size_t(f->stride), row_bytes);
        }
        ANativeWindow_unlockAndPost(ctx->window);
    }
}

void start_aaudio(SdkReceiverCtx* ctx) {
    if (!ctx || ctx->audio || !ctx->rx) return;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, aaudio_cb, ctx->rx);
    AAudioStream* st = nullptr;
    if (AAudioStreamBuilder_openStream(b, &st) == AAUDIO_OK && st) {
        if (AAudioStream_requestStart(st) == AAUDIO_OK) ctx->audio = st;
        else AAudioStream_close(st);
    }
    AAudioStreamBuilder_delete(b);
}

void stop_aaudio(SdkReceiverCtx* ctx) {
    if (!ctx || !ctx->audio) return;
    AAudioStream_requestStop(ctx->audio);
    AAudioStream_close(ctx->audio);
    ctx->audio = nullptr;
}

std::string to_str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out(p ? p : "");
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_init(JNIEnv*, jclass) {
    return lv_init();
}

JNIEXPORT jstring JNICALL Java_com_lanvid_sdk_internal_NativeBridge_version(JNIEnv* env, jclass) {
    return env->NewStringUTF(lv_get_version_string());
}

JNIEXPORT jstring JNICALL Java_com_lanvid_sdk_internal_NativeBridge_simdInfo(JNIEnv* env, jclass) {
    return env->NewStringUTF(lv_get_cpu_simd_info());
}

JNIEXPORT jstring JNICALL Java_com_lanvid_sdk_internal_NativeBridge_scanLanJson(JNIEnv* env, jclass,
                                                                                jint timeout_ms) {
    char* j = lv_discovery_scan(timeout_ms);
    jstring out = env->NewStringUTF(j ? j : "[]");
    lv_free_string(j);
    return out;
}

/* ---- Sender ---- */
JNIEXPORT jlong JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(lv_sender_create());
}

JNIEXPORT void JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderDestroy(JNIEnv*, jclass,
                                                                               jlong ptr) {
    if (ptr) lv_sender_destroy(reinterpret_cast<LvSenderHandle*>(ptr));
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderStart(
    JNIEnv* env, jclass, jlong ptr, jstring dest_ip, jint dest_port, jint width, jint height,
    jint fps, jint quality, jint fec_group, jboolean adaptive) {
    if (!ptr) return LV_ERR_INVALID_ARG;
    LV_SenderConfig cfg{};
    lv_sdk_sender_config_default(&cfg);
    const std::string ip = to_str(env, dest_ip);
    std::snprintf(cfg.dest_ip, sizeof(cfg.dest_ip), "%s", ip.c_str());
    cfg.dest_port = uint16_t(std::clamp(int(dest_port), 1024, 65535));
    cfg.width = width;
    cfg.height = height;
    cfg.fps = double(fps > 0 ? fps : 60);
    cfg.quality = std::clamp(int(quality), 1, 100);
    cfg.fec_group_size = std::max(0, int(fec_group));
    cfg.adaptive_quality = adaptive ? 1 : 0;
    return lv_sdk_sender_start(reinterpret_cast<LvSenderHandle*>(ptr), &cfg);
}

JNIEXPORT void JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderStop(JNIEnv*, jclass,
                                                                            jlong ptr) {
    if (ptr) lv_sender_stop(reinterpret_cast<LvSenderHandle*>(ptr));
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderPushYuv420(
    JNIEnv* env, jclass, jlong ptr, jobject y_buf, jobject u_buf, jobject v_buf, jint width,
    jint height, jint y_stride, jint u_stride, jint v_stride, jint uv_pixel_stride) {
    if (!ptr || !y_buf || !u_buf || !v_buf) return LV_ERR_INVALID_ARG;
    LV_YuvPlanes yp{};
    yp.plane_y = static_cast<const uint8_t*>(env->GetDirectBufferAddress(y_buf));
    yp.plane_u = static_cast<const uint8_t*>(env->GetDirectBufferAddress(u_buf));
    yp.plane_v = static_cast<const uint8_t*>(env->GetDirectBufferAddress(v_buf));
    yp.y_stride = y_stride;
    yp.u_stride = u_stride;
    yp.v_stride = v_stride;
    yp.uv_pixel_stride = uv_pixel_stride > 0 ? uv_pixel_stride : 1;
    yp.pixel_format = (yp.uv_pixel_stride == 2) ? LV_PIXEL_FORMAT_NV12 : LV_PIXEL_FORMAT_I420;
    return lv_sender_push_yuv_planes(reinterpret_cast<LvSenderHandle*>(ptr), &yp, width, height, 0);
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderPushPacked(
    JNIEnv* env, jclass, jlong ptr, jobject buf, jint width, jint height, jint stride, jint fmt) {
    if (!ptr || !buf) return LV_ERR_INVALID_ARG;
    const auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buf));
    return lv_sender_push_packed_frame(reinterpret_cast<LvSenderHandle*>(ptr), data, width, height,
                                       stride, fmt, 0);
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderPushAudioPcm16(
    JNIEnv* env, jclass, jlong ptr, jshortArray pcm, jint frames, jint rate, jint channels) {
    if (!ptr || !pcm || frames <= 0) return LV_ERR_INVALID_ARG;
    jshort* p = env->GetShortArrayElements(pcm, nullptr);
    const int rc = lv_sender_push_audio_chunk(reinterpret_cast<LvSenderHandle*>(ptr), p, frames,
                                              rate, channels, LV_AUDIO_FORMAT_PCM16, 0);
    env->ReleaseShortArrayElements(pcm, p, JNI_ABORT);
    return rc;
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderSetQuality(
    JNIEnv*, jclass, jlong ptr, jint q) {
    return ptr ? lv_sender_request_keyframe_quality(reinterpret_cast<LvSenderHandle*>(ptr), q)
               : LV_ERR_INVALID_ARG;
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderSetFec(
    JNIEnv*, jclass, jlong ptr, jint g) {
    return ptr ? lv_sender_set_fec_group(reinterpret_cast<LvSenderHandle*>(ptr), g)
               : LV_ERR_INVALID_ARG;
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderSetAdaptive(
    JNIEnv*, jclass, jlong ptr, jboolean en, jint qmin, jint qmax) {
    return ptr ? lv_sender_set_adaptive_quality(reinterpret_cast<LvSenderHandle*>(ptr), en ? 1 : 0,
                                                qmin, qmax)
               : LV_ERR_INVALID_ARG;
}

JNIEXPORT jstring JNICALL Java_com_lanvid_sdk_internal_NativeBridge_senderStatsJson(JNIEnv* env, jclass,
                                                                                    jlong ptr) {
    LV_StreamStats s{};
    if (ptr) lv_sender_get_stream_stats(reinterpret_cast<LvSenderHandle*>(ptr), &s);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"state\":%d,\"width\":%d,\"height\":%d,\"q\":%d,\"fps\":%.1f,\"mbps\":%.2f,"
                  "\"enc_ms\":%.2f,\"loss\":%.2f,\"audio_kbps\":%.0f,\"frames\":%llu}",
                  s.state, s.width, s.height, s.active_quality, s.fps, s.bitrate_mbps, s.encode_ms,
                  s.packet_loss_pct, s.audio_kbps, static_cast<unsigned long long>(s.frames_total));
    return env->NewStringUTF(buf);
}

/* ---- Receiver ---- */
JNIEXPORT jlong JNICALL Java_com_lanvid_sdk_internal_NativeBridge_receiverCreate(JNIEnv*, jclass) {
    auto* ctx = new SdkReceiverCtx();
    ctx->rx = lv_receiver_create();
    lv_receiver_set_video_callback(ctx->rx, on_video_cb, ctx);
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT void JNICALL Java_com_lanvid_sdk_internal_NativeBridge_receiverDestroy(JNIEnv*, jclass,
                                                                                 jlong ptr) {
    if (!ptr) return;
    auto* ctx = reinterpret_cast<SdkReceiverCtx*>(ptr);
    stop_aaudio(ctx);
    if (ctx->rx) {
        lv_receiver_set_video_callback(ctx->rx, nullptr, nullptr);
        lv_receiver_destroy(ctx->rx);
    }
    std::lock_guard<std::mutex> lk(ctx->surf_mu);
    if (ctx->window) {
        ANativeWindow_release(ctx->window);
        ctx->window = nullptr;
    }
    delete ctx;
}

JNIEXPORT jint JNICALL Java_com_lanvid_sdk_internal_NativeBridge_receiverStart(
    JNIEnv* env, jclass, jlong ptr, jint port, jdouble audio_target_ms, jboolean enable_audio,
    jstring device_name) {
    if (!ptr) return LV_ERR_INVALID_ARG;
    auto* ctx = reinterpret_cast<SdkReceiverCtx*>(ptr);
    stop_aaudio(ctx);
    LV_ReceiverConfig cfg{};
    lv_sdk_receiver_config_default(&cfg);
    cfg.bind_port = uint16_t(std::clamp(int(port), 0, 65535));
    cfg.output_format = LV_PIXEL_FORMAT_RGBA8888;
    cfg.audio_target_ms = audio_target_ms > 0.0 ? audio_target_ms : 12.0;
    cfg.enable_os_audio = 0;
    cfg.enable_discovery = 1;
    const std::string dn = to_str(env, device_name);
    if (!dn.empty()) std::snprintf(cfg.device_name, sizeof(cfg.device_name), "%s", dn.c_str());

    const int rc = lv_sdk_receiver_start(ctx->rx, &cfg);
    if (rc == LV_OK && enable_audio) start_aaudio(ctx);
    return rc;
}

JNIEXPORT void JNICALL Java_com_lanvid_sdk_internal_NativeBridge_receiverStop(JNIEnv*, jclass,
                                                                              jlong ptr) {
    if (!ptr) return;
    auto* ctx = reinterpret_cast<SdkReceiverCtx*>(ptr);
    stop_aaudio(ctx);
    if (ctx->rx) lv_receiver_stop(ctx->rx);
}

JNIEXPORT void JNICALL Java_com_lanvid_sdk_internal_NativeBridge_receiverSetSurface(
    JNIEnv* env, jclass, jlong ptr, jobject surface) {
    if (!ptr) return;
    auto* ctx = reinterpret_cast<SdkReceiverCtx*>(ptr);
    std::lock_guard<std::mutex> lk(ctx->surf_mu);
    if (ctx->window) {
        ANativeWindow_release(ctx->window);
        ctx->window = nullptr;
    }
    ctx->win_w = ctx->win_h = 0;
    if (surface) ctx->window = ANativeWindow_fromSurface(env, surface);
}

JNIEXPORT void JNICALL Java_com_lanvid_sdk_internal_NativeBridge_receiverSetVolume(
    JNIEnv*, jclass, jlong ptr, jfloat vol) {
    if (!ptr) return;
    auto* ctx = reinterpret_cast<SdkReceiverCtx*>(ptr);
    if (ctx->rx) lv_receiver_set_volume(ctx->rx, vol);
}

JNIEXPORT jstring JNICALL Java_com_lanvid_sdk_internal_NativeBridge_receiverStatsJson(JNIEnv* env,
                                                                                      jclass,
                                                                                      jlong ptr) {
    LV_StreamStats s{};
    if (ptr) {
        auto* ctx = reinterpret_cast<SdkReceiverCtx*>(ptr);
        if (ctx->rx) lv_receiver_get_stream_stats(ctx->rx, &s);
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"state\":%d,\"width\":%d,\"height\":%d,\"q\":%d,\"fps\":%.1f,\"mbps\":%.2f,"
                  "\"lat_p50\":%.2f,\"lat_p99\":%.2f,\"loss\":%.2f,\"fec\":%llu,"
                  "\"audio_kbps\":%.0f,\"audio_buf\":%.1f}",
                  s.state, s.width, s.height, s.active_quality, s.fps, s.bitrate_mbps,
                  s.latency_p50_ms, s.latency_p99_ms, s.packet_loss_pct,
                  static_cast<unsigned long long>(s.fec_recovered), s.audio_kbps, s.audio_buffer_ms);
    return env->NewStringUTF(buf);
}

}  // extern "C"
