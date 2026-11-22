#pragma once
/*
 * LanVid SDK — Main Umbrella Header
 *
 * Self-contained C ABI for ultra-low-latency LAN video & audio streaming across
 * macOS, iOS, Android, Windows, and Linux.
 *
 * Typical receiver integration (zero-copy callback mode):
 *   lv_init();
 *   LV_ReceiverConfig cfg;
 *   lv_sdk_receiver_config_default(&cfg);
 *   LvReceiverHandle* rx = lv_receiver_create();
 *   lv_receiver_set_video_callback(rx, on_frame, ctx);
 *   lv_receiver_set_audio_callback(rx, on_audio, ctx);
 *   lv_sdk_receiver_start(rx, &cfg);
 */

#include "lanvid_export.h"
#include "lanvid_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LvSenderHandle LvSenderHandle;
typedef struct LvReceiverHandle LvReceiverHandle;

/* ---- Zero-Copy Callback Prototypes ---- */
typedef void (*LV_VideoFrameCallback)(const LV_VideoFrameView* frame, void* user_data);
typedef void (*LV_AudioSamplesCallback)(const LV_AudioChunkView* audio, void* user_data);
typedef void (*LV_StateChangedCallback)(int new_state, int error_code, const char* message,
                                        void* user_data);

/* ---- Core Lifecycle & Runtime Introspection ---- */
LV_API int lv_init(void);
LV_API void lv_shutdown(void);
LV_API void lv_free_string(char* str);
LV_API const char* lv_get_cpu_simd_info(void);
LV_API void lv_set_simd_enabled(int enabled);

/* Returns heap-allocated JSON enumerating network interfaces (and OS capture devices when enabled).
 * Free the returned string with lv_free_string(). */
LV_API char* lv_enumerate_devices(void);

/* Returns heap-allocated JSON array listing supported video codecs (DCT, H.264, H.265) and HW/SW status.
 * Free the returned string with lv_free_string(). */
LV_API char* lv_get_supported_codecs(void);

/* ---- SDK Default Configuration Initializers ---- */
LV_API void lv_sdk_sender_config_default(LV_SenderConfig* cfg);
LV_API void lv_sdk_receiver_config_default(LV_ReceiverConfig* cfg);

/* ---- Sender Lifecycle & Zero-Copy Frame/Audio Submission ---- */
LV_API LvSenderHandle* lv_sender_create(void);
LV_API void lv_sender_destroy(LvSenderHandle* handle);

/* Starts sender in external zero-copy push mode. Returns LV_OK (0) or negative LV_ErrorCode. */
LV_API int lv_sdk_sender_start(LvSenderHandle* handle, const LV_SenderConfig* cfg);
LV_API void lv_sender_stop(LvSenderHandle* handle);

/* Pushes packed 32-bit frame (LV_PIXEL_FORMAT_BGRA8888 or LV_PIXEL_FORMAT_RGBA8888).
 * Returns LV_OK (0) on success or negative LV_ErrorCode. */
LV_API int lv_sender_push_packed_frame(LvSenderHandle* handle, const uint8_t* data, int width,
                                       int height, int stride, int pixel_format,
                                       uint32_t timestamp_us);

/* Pushes planar/bi-planar YUV 4:2:0 frame (I420 / NV12 / NV21) with zero RGB conversion.
 * Ideal for Android ImageReader, FFmpeg AVFrame, and OBS obs_source_frame. */
LV_API int lv_sender_push_yuv_planes(LvSenderHandle* handle, const LV_YuvPlanes* planes, int width,
                                     int height, uint32_t timestamp_us);

/* Zero-copy convenience entry point for Apple CVPixelBuffer (32BGRA or 420YpCbCr8BiPlanar NV12)
 * after CVPixelBufferLockBaseAddress. For BGRA pass plane1=NULL. */
LV_API int lv_sender_push_cvpixelbuffer_planes(LvSenderHandle* handle, const uint8_t* plane0,
                                               int stride0, const uint8_t* plane1, int stride1,
                                               int width, int height, int pixel_format,
                                               uint32_t timestamp_us);

/* Pushes an audio chunk (LV_AUDIO_FORMAT_PCM16 or LV_AUDIO_FORMAT_F32) over the same UDP socket. */
LV_API int lv_sender_push_audio_chunk(LvSenderHandle* handle, const void* data, int num_frames,
                                      int sample_rate, int channels, int audio_format,
                                      uint32_t timestamp_us);

/* Live runtime encoding & transport controls (lock-free / thread-safe): */
LV_API int lv_sender_request_keyframe_quality(LvSenderHandle* handle, int quality);
LV_API int lv_sender_set_bitrate(LvSenderHandle* handle, uint32_t bitrate_kbps);
LV_API int lv_sender_force_keyframe(LvSenderHandle* handle);
LV_API int lv_sender_set_fec_group(LvSenderHandle* handle, int fec_group_size);
LV_API int lv_sender_set_adaptive_quality(LvSenderHandle* handle, int enabled, int q_min, int q_max);

LV_API int lv_sender_get_stream_stats(LvSenderHandle* handle, LV_StreamStats* out);

/* ---- Receiver Lifecycle, Zero-Copy Callbacks & Audio Pull ---- */
LV_API LvReceiverHandle* lv_receiver_create(void);
LV_API void lv_receiver_destroy(LvReceiverHandle* handle);

/* Register zero-copy push callbacks (may be called before or after start; pass NULL to clear). */
LV_API void lv_receiver_set_video_callback(LvReceiverHandle* handle, LV_VideoFrameCallback cb,
                                           void* user_data);
LV_API void lv_receiver_set_audio_callback(LvReceiverHandle* handle, LV_AudioSamplesCallback cb,
                                           void* user_data);
LV_API void lv_receiver_set_state_callback(LvReceiverHandle* handle, LV_StateChangedCallback cb,
                                           void* user_data);

/* Starts UDP receiver + optional LAN discovery responder. Returns LV_OK (0) or negative LV_ErrorCode. */
LV_API int lv_sdk_receiver_start(LvReceiverHandle* handle, const LV_ReceiverConfig* cfg);
LV_API void lv_receiver_stop(LvReceiverHandle* handle);
LV_API uint16_t lv_receiver_get_local_port(LvReceiverHandle* handle);

/* Pulls stereo 48 kHz PCM16 frames from the lock-free audio jitter buffer (AAudio / AVAudioEngine). */
LV_API int lv_receiver_pop_audio_pcm16(LvReceiverHandle* handle, int16_t* out_stereo, int num_frames);

/* Optional polling helper for clients that prefer pull over video callbacks (e.g. Python/OpenGL loops). */
LV_API int lv_receiver_get_latest_frame(LvReceiverHandle* handle, uint8_t* out_buf, size_t cap,
                                        int* out_w, int* out_h, uint32_t* inout_frame_id,
                                        int want_rgba);

LV_API void lv_receiver_set_volume(LvReceiverHandle* handle, float volume);
LV_API void lv_receiver_set_concealment(LvReceiverHandle* handle, int mode);
LV_API int lv_receiver_save_screenshot(LvReceiverHandle* handle, const char* ppm_path);
LV_API int lv_receiver_get_stream_stats(LvReceiverHandle* handle, LV_StreamStats* out);

/* ---- LAN Auto-Discovery ---- */
/* Scans the LAN for active receivers and populates `out_peers` (up to `max_peers`).
 * Returns number of discovered peers (>= 0). Pass discovery_port=0 for default (5099). */
LV_API int lv_discovery_scan_peers(int timeout_ms, uint16_t discovery_port,
                                   LV_DiscoveredPeer* out_peers, int max_peers);

/* JSON discovery helpers (free returned string with lv_free_string()). */
LV_API char* lv_discovery_scan(int timeout_ms);
LV_API char* lv_discovery_scan_port(int timeout_ms, uint16_t discovery_port);

#ifdef __cplusplus
}
#endif
