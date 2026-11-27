// CrossLink Android Audio SDK — JNI Bridge (64-ch AoIP + MIDI 2.0 + Discovery)
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "crosslink/crosslink_sdk.h"

namespace {
std::string to_str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out(p ? p : "");
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}
uint64_t jni_ts(jlong ts) { return ts < 0 ? CL_MEDIA_TS_AUTO : uint64_t(ts); }
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_init(JNIEnv*, jclass) {
    return cl_init();
}

JNIEXPORT jstring JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_version(JNIEnv* env, jclass) {
    return env->NewStringUTF(cl_get_version_string());
}

JNIEXPORT jstring JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_scanNodesJson(
    JNIEnv* env, jclass, jint timeout_ms, jint discovery_port) {
    char* j = cl_discovery_scan_json(timeout_ms, uint16_t(std::clamp(int(discovery_port), 0, 65535)));
    jstring out = env->NewStringUTF(j ? j : "[]");
    cl_free_string(j);
    return out;
}

/* ---- Sender ---- */
JNIEXPORT jlong JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(cl_sender_create());
}

JNIEXPORT void JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderDestroy(JNIEnv*, jclass, jlong ptr) {
    if (ptr) cl_sender_destroy(reinterpret_cast<ClSenderHandle*>(ptr));
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderStart(
    JNIEnv* env, jclass, jlong ptr, jstring dest_ip, jint dest_port, jint sample_rate, jint latency_ms, jint codec) {
    if (!ptr) return CL_ERR_INVALID_ARG;
    CL_SenderConfig cfg{};
    cl_sender_config_default(&cfg);
    const std::string ip = to_str(env, dest_ip);
    std::snprintf(cfg.dest_ip, sizeof(cfg.dest_ip), "%s", ip.c_str());
    cfg.dest_port = uint16_t(std::clamp(int(dest_port), 1024, 65535));
    cfg.sample_rate = sample_rate > 0 ? sample_rate : 48000;
    cfg.latency_ms = uint32_t(std::clamp(int(latency_ms), CL_AUDIO_LATENCY_MIN_MS, CL_AUDIO_LATENCY_MAX_MS));
    cfg.audio_codec = codec;
    return cl_sender_start(reinterpret_cast<ClSenderHandle*>(ptr), &cfg);
}

JNIEXPORT void JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderStop(JNIEnv*, jclass, jlong ptr) {
    if (ptr) cl_sender_stop(reinterpret_cast<ClSenderHandle*>(ptr));
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderPushChannelF32(
    JNIEnv* env, jclass, jlong ptr, jint ch, jfloatArray samples, jint count, jlong ts) {
    if (!ptr || !samples || ch < 0 || ch >= CL_AUDIO_MAX_CHANNELS ||
        count <= 0 || count > env->GetArrayLength(samples))
        return CL_ERR_INVALID_ARG;
    jfloat* p = env->GetFloatArrayElements(samples, nullptr);
    if (!p) return CL_ERR_INTERNAL;
    const int rc = cl_sender_push_audio_channel(reinterpret_cast<ClSenderHandle*>(ptr), uint16_t(ch), p,
                                                uint32_t(count), CL_AUDIO_FORMAT_F32, jni_ts(ts));
    env->ReleaseFloatArrayElements(samples, p, JNI_ABORT);
    return rc;
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderPushMultichannelF32(
    JNIEnv* env, jclass, jlong ptr, jfloatArray data, jint channels, jint frames, jlong ts) {
    if (!ptr || !data || channels <= 0 || frames <= 0 ||
        jlong(channels) * frames > env->GetArrayLength(data))
        return CL_ERR_INVALID_ARG;
    jfloat* p = env->GetFloatArrayElements(data, nullptr);
    if (!p) return CL_ERR_INTERNAL;
    const int rc = cl_sender_push_audio_multichannel(reinterpret_cast<ClSenderHandle*>(ptr), p, uint16_t(channels),
                                                     uint32_t(frames), CL_AUDIO_FORMAT_F32,
                                                     CL_AUDIO_LAYOUT_INTERLEAVED, jni_ts(ts));
    env->ReleaseFloatArrayElements(data, p, JNI_ABORT);
    return rc;
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderPushMidi(
    JNIEnv*, jclass, jlong ptr, jint status, jint d1, jint d2, jint port, jlong media_ts_us) {
    if (!ptr) return CL_ERR_INVALID_ARG;
    return cl_sender_push_midi(reinterpret_cast<ClSenderHandle*>(ptr), uint8_t(status), uint8_t(d1),
                               uint8_t(d2), uint8_t(port), uint64_t(std::max<jlong>(0, media_ts_us)));
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderSetLatency(
    JNIEnv*, jclass, jlong ptr, jint ms) {
    return (ptr && ms > 0) ? cl_sender_set_audio_latency_ms(reinterpret_cast<ClSenderHandle*>(ptr), uint32_t(ms))
                           : CL_ERR_INVALID_ARG;
}

JNIEXPORT jlong JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_senderMasterClockSamples(
    JNIEnv*, jclass, jlong ptr) {
    return ptr ? jlong(cl_sender_get_master_clock_samples(reinterpret_cast<ClSenderHandle*>(ptr))) : 0;
}

/* ---- Receiver ---- */
JNIEXPORT jlong JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(cl_receiver_create());
}

JNIEXPORT void JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverDestroy(JNIEnv*, jclass, jlong ptr) {
    if (ptr) cl_receiver_destroy(reinterpret_cast<ClReceiverHandle*>(ptr));
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverStart(
    JNIEnv* env, jclass, jlong ptr, jint bind_port, jint sample_rate, jint latency_ms, jint channels,
    jboolean adaptive_jitter, jstring device_name) {
    if (!ptr) return CL_ERR_INVALID_ARG;
    CL_ReceiverConfig cfg{};
    cl_receiver_config_default(&cfg);
    cfg.bind_port = uint16_t(std::clamp(int(bind_port), 0, 65535));
    cfg.sample_rate = sample_rate > 0 ? sample_rate : 48000;
    cfg.latency_ms = uint32_t(std::clamp(int(latency_ms), CL_AUDIO_LATENCY_MIN_MS, CL_AUDIO_LATENCY_MAX_MS));
    cfg.num_channels = std::clamp(int(channels), 1, CL_AUDIO_MAX_CHANNELS);
    cfg.adaptive_jitter = adaptive_jitter ? 1 : 0;
    const std::string dn = to_str(env, device_name);
    if (!dn.empty()) std::snprintf(cfg.device_name, sizeof(cfg.device_name), "%s", dn.c_str());
    return cl_receiver_start(reinterpret_cast<ClReceiverHandle*>(ptr), &cfg);
}

JNIEXPORT void JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverStop(JNIEnv*, jclass, jlong ptr) {
    if (ptr) cl_receiver_stop(reinterpret_cast<ClReceiverHandle*>(ptr));
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverSetLatency(
    JNIEnv*, jclass, jlong ptr, jint ms) {
    return (ptr && ms > 0) ? cl_receiver_set_audio_latency_ms(reinterpret_cast<ClReceiverHandle*>(ptr), uint32_t(ms))
                           : CL_ERR_INVALID_ARG;
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverSetAdaptiveJitter(
    JNIEnv*, jclass, jlong ptr, jboolean enabled) {
    return ptr ? cl_receiver_set_adaptive_jitter(reinterpret_cast<ClReceiverHandle*>(ptr), enabled ? 1 : 0)
               : CL_ERR_INVALID_ARG;
}

JNIEXPORT jdouble JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverGetEffectiveLatencyMs(
    JNIEnv*, jclass, jlong ptr) {
    return ptr ? cl_receiver_get_effective_latency_ms(reinterpret_cast<ClReceiverHandle*>(ptr)) : 0.0;
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverPopMultichannel(
    JNIEnv* env, jclass, jlong ptr, jfloatArray out, jint channels, jint frames) {
    if (!ptr || !out || channels <= 0 || frames <= 0 || jlong(channels) * frames > env->GetArrayLength(out))
        return CL_ERR_INVALID_ARG;
    jfloat* p = env->GetFloatArrayElements(out, nullptr);
    if (!p) return CL_ERR_INTERNAL;
    const int rc = cl_receiver_pop_audio_multichannel(reinterpret_cast<ClReceiverHandle*>(ptr), p,
                                                      uint16_t(channels), uint32_t(frames),
                                                      CL_AUDIO_LAYOUT_INTERLEAVED);
    env->ReleaseFloatArrayElements(out, p, 0);
    return rc;
}

JNIEXPORT jint JNICALL Java_com_crosslink_sdk_internal_CrossLinkNativeBridge_receiverChannelStats(
    JNIEnv* env, jclass, jlong ptr, jint ch, jlongArray out) {
    if (!ptr || !out || ch < 0 || ch >= CL_AUDIO_MAX_CHANNELS || env->GetArrayLength(out) < 10)
        return CL_ERR_INVALID_ARG;
    CL_AudioChannelStats st{};
    const int rc = cl_receiver_get_channel_stats(reinterpret_cast<ClReceiverHandle*>(ptr), uint16_t(ch), &st);
    const jlong v[10] = {jlong(st.packets_rx), jlong(st.packets_lost), jlong(st.fec_recovered),
                         jlong(st.nack_recovered), jlong(st.nacks_sent), jlong(st.underruns),
                         jlong(st.late_packets), jlong(st.jitter_us), jlong(st.active),
                         jlong(st.level_dbfs * 100.0f)};
    env->SetLongArrayRegion(out, 0, 10, v);
    return rc;
}

}  // extern "C"
