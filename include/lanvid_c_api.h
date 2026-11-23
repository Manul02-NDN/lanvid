#pragma once
/*
 * LanVid Core C ABI (lanvid_core.dll / liblanvid_core.so)
 * Shared low-latency video + audio streaming core for Windows, Linux, and Android.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef LV_API
#if defined(_WIN32)
#  if defined(LANVID_BUILD_SHARED)
#    define LV_API __declspec(dllexport)
#  elif defined(LANVID_USE_SHARED)
#    define LV_API __declspec(dllimport)
#  else
#    define LV_API
#  endif
#else
#  if defined(LANVID_BUILD_SHARED) || defined(__GNUC__) || defined(__clang__)
#    define LV_API __attribute__((visibility("default")))
#  else
#    define LV_API
#  endif
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel formats for lv_sender_push_raw_frame */
enum LvPixelFormat {
    LV_PIXFMT_RGBA = 0,
    LV_PIXFMT_BGRA = 1
};

/* Video source kinds for LvSenderConfig.source_type */
enum LvSourceType {
    LV_SOURCE_EXTERNAL = 0, /* Caller pushes frames via lv_sender_push_* */
    LV_SOURCE_PATTERN  = 1, /* Built-in animated test pattern */
    LV_SOURCE_SCREEN   = 2, /* Desktop screen/monitor capture */
    LV_SOURCE_CAMERA   = 3, /* Webcam / Media Foundation capture */
    LV_SOURCE_FILE     = 4  /* .y4m / .ppm / raw RGB/RGBA file loop */
};

/* Screen capture backend preference */
enum LvCaptureBackend {
    LV_CAP_AUTO = 0,
    LV_CAP_DXGI = 1,
    LV_CAP_GDI  = 2
};

/* Audio capture modes */
enum LvAudioMode {
    LV_AUDIO_NONE     = 0,
    LV_AUDIO_LOOPBACK = 1,
    LV_AUDIO_MIC      = 2
};

/* Receiver packet-loss concealment modes */
enum LvConcealMode {
    LV_CONCEAL_BLACK      = 0,
    LV_CONCEAL_PREV_FRAME = 1
};

#ifndef LV_CODEC_TYPE_DEFINED
#define LV_CODEC_TYPE_DEFINED
typedef enum LV_CodecType {
    LV_CODEC_LANVID_DCT = 0,
    LV_CODEC_H264       = 1,
    LV_CODEC_H265       = 2
} LV_CodecType;
#endif

typedef struct LvSenderHandle LvSenderHandle;
typedef struct LvReceiverHandle LvReceiverHandle;

typedef struct LvSenderConfig {
    char dest_ip[64];
    uint16_t dest_port;
    int source_type;       /* LvSourceType */
    int monitor_index;
    int capture_backend;   /* LvCaptureBackend */
    int camera_index;
    char file_path[512];
    int file_is_rgba;

    int width;             /* 0 = native for screen/camera/file */
    int height;            /* 0 = native */
    double fps;            /* default 60.0 */
    int quality;           /* 1..100, default 80 */
    int chroma;            /* 0=4:2:0, 1=4:2:2, 2=4:4:4 */
    int slice_height;      /* default 16 */
    int mtu;               /* default 1400 */
    int enable_crc;        /* default 1 */
    int fec_group_size;    /* 0=off, e.g. 6 */
    int adaptive_quality;  /* 0=off, 1=AIMD */
    int q_min;             /* default 35 */
    int q_max;             /* default 85 */
    int threads;           /* 0=auto */
    double pace_mbps;      /* 0=off */
    double sim_loss_pct;   /* 0=off */

    int audio_mode;        /* LvAudioMode */
    int audio_device_index;/* -1=system default */
    double audio_chunk_ms; /* 2.5 or 5.0 */

    int codec;             /* LV_CodecType: 0=DCT, 1=H264, 2=H265 */
    uint32_t bitrate_kbps; /* target bitrate for H.264/H.265 (default 8000) */
    uint32_t keyframe_interval; /* IDR interval in frames (default 120) */
    int audio_only;        /* 1=disable video capture/encode/TX (AoIP + MasterClock only) */
    int audio_codec;       /* 0=PCM32F, 1=PCM24, 2=PCM16, 3=192k, 4=256k, 5=192k, 6=128k, 7=96k, 8=64k */
    int audio_bitrate_kbps;/* 0=Lossless (use audio_codec), or 256/192/128/96/64 kbps per channel */
} LvSenderConfig;

typedef struct LvSenderStats {
    int running;
    int width;
    int height;
    double tx_fps;
    double tx_mbps;
    double encode_tx_ms;
    int active_quality;
    double rx_loss_pct;
    double rx_fec_pct;
    uint64_t frames_sent;
    uint64_t packets_sent;
    uint64_t send_errors;
    uint64_t audio_packets_sent;
    double audio_kbps;
    int active_codec;
    int codec_is_hardware;
    uint32_t target_bitrate_kbps;
    char codec_backend[64];
} LvSenderStats;

typedef struct LvReceiverConfig {
    char bind_ip[64];      /* default "0.0.0.0" */
    uint16_t bind_port;    /* default 5000 */
    int decode_threads;    /* 0=auto */
    int conceal_mode;      /* LvConcealMode */
    double frame_timeout_ms; /* default 25.0 */
    int enable_audio;      /* 1=start OS playback automatically (Windows WASAPI) */
    int external_audio_pull; /* 1=caller pulls via lv_receiver_pop_audio_pcm16 (Android AAudio) */
    double audio_target_ms;/* default 12.0 */
    int enable_discovery;  /* 1=respond to LAN discovery probes on UDP 5099 */
    uint16_t discovery_port; /* 0 -> default 5099 */
    char device_name[128]; /* optional friendly name for LAN discovery */
    int audio_only;        /* 1=drop video packets before reassembly/decoding (AoIP bridge) */
    int adaptive_jitter;   /* 1=Adaptive Wi-Fi jitter buffer (default; grows target..200 ms on bursts),
                              0=Fixed latency (wired Ethernet / live monitoring). Live: lv_receiver_set_adaptive_jitter */
} LvReceiverConfig;

typedef struct LvReceiverStats {
    int running;
    int width;
    int height;
    int total_slices;
    double fps;
    double rx_mbps;
    double latency_p50_ms;
    double latency_p99_ms;
    double loss_pct;
    uint64_t frames_complete;
    uint64_t frames_concealed;
    uint64_t fec_recovered;
    uint64_t bad_crc;
    uint64_t late_packets;
    int active_quality;
    uint64_t audio_packets_rx;
    uint64_t audio_packets_lost;
    double audio_kbps;
    double audio_buffer_ms;
    int active_codec;
    int codec_is_hardware;
    uint64_t pli_requests;
    char codec_backend[64];
    /* Latency mode telemetry (appended) */
    double effective_latency_ms;   /* measured 64ch jitter-buffer playout delay right now */
    uint32_t effective_target_ms;  /* delay the buffer steers to (== bus latency in Fixed mode) */
    int adaptive_jitter;           /* 1=Adaptive (Wi-Fi), 0=Fixed */
} LvReceiverStats;

/* ---- Core lifecycle & introspection ---- */
LV_API int lv_init(void);
LV_API void lv_shutdown(void);
LV_API void lv_free_string(char* str);
LV_API const char* lv_get_cpu_simd_info(void);
LV_API void lv_set_simd_enabled(int enabled);

/* Returns heap-allocated JSON describing monitors, cameras, audio devices, and network interfaces.
 * Free the returned string with lv_free_string(). */
LV_API char* lv_enumerate_devices(void);
LV_API char* lv_get_supported_codecs(void);

/* ---- Default config helpers ---- */
LV_API void lv_sender_config_default(LvSenderConfig* cfg);
LV_API void lv_receiver_config_default(LvReceiverConfig* cfg);

/* ---- Sender API ---- */
LV_API LvSenderHandle* lv_sender_create(void);
LV_API void lv_sender_destroy(LvSenderHandle* handle);
LV_API int lv_sender_start(LvSenderHandle* handle, const LvSenderConfig* cfg);
LV_API void lv_sender_stop(LvSenderHandle* handle);

/* Push an RGBA/BGRA frame from an external capture source (e.g. Android MediaProjection). */
LV_API int lv_sender_push_raw_frame(LvSenderHandle* handle, const uint8_t* data, int width, int height,
                                    int stride, int pixel_format, uint32_t timestamp_us);

/* Push a YUV_420_888 frame directly with zero RGB conversion (e.g. Android Camera2 / CameraX). */
LV_API int lv_sender_push_yuv420_frame(LvSenderHandle* handle, const uint8_t* y, const uint8_t* u,
                                       const uint8_t* v, int width, int height, int y_stride,
                                       int u_stride, int v_stride, int uv_pixel_stride,
                                       uint32_t timestamp_us);

/* Push a PCM16 audio chunk from an external capture source (e.g. Android AudioRecord). */
LV_API int lv_sender_push_audio_pcm16(LvSenderHandle* handle, const int16_t* pcm, int num_frames,
                                      int sample_rate, int channels, uint32_t timestamp_us);

/* Copies the latest transmitted frame (BGRA or RGBA) for GUI mini-preview.
 * Returns 1 if a new frame was copied, 0 if unchanged/unavailable, -1 if buffer too small. */
LV_API int lv_sender_get_preview_frame(LvSenderHandle* handle, uint8_t* out_buf, size_t cap,
                                       int* out_w, int* out_h, uint64_t* inout_frame_id,
                                       int want_rgba);

LV_API int lv_sender_get_stats(LvSenderHandle* handle, LvSenderStats* out);
LV_API int lv_sender_set_bitrate(LvSenderHandle* handle, uint32_t bitrate_kbps);
LV_API int lv_sender_force_keyframe(LvSenderHandle* handle);

/* ---- Receiver API ---- */
LV_API LvReceiverHandle* lv_receiver_create(void);
LV_API void lv_receiver_destroy(LvReceiverHandle* handle);
LV_API int lv_receiver_start(LvReceiverHandle* handle, const LvReceiverConfig* cfg);
LV_API void lv_receiver_stop(LvReceiverHandle* handle);
LV_API uint16_t lv_receiver_get_local_port(LvReceiverHandle* handle);

/* Copies the latest decoded frame into `out_buf` (BGRA when want_rgba=0, RGBA when want_rgba=1).
 * If `*inout_frame_id` equals the latest frame id, returns 0 (no new frame).
 * Returns 1 if copied, 0 if no new frame, -1 if `cap` is too small (still writes out_w/out_h). */
LV_API int lv_receiver_get_latest_frame(LvReceiverHandle* handle, uint8_t* out_buf, size_t cap,
                                        int* out_w, int* out_h, uint32_t* inout_frame_id,
                                        int want_rgba);

/* Pulls stereo 48kHz PCM16 frames from the lock-free audio jitter buffer (for AAudio / custom sinks). */
LV_API int lv_receiver_pop_audio_pcm16(LvReceiverHandle* handle, int16_t* out_stereo, int num_frames);

LV_API void lv_receiver_set_volume(LvReceiverHandle* handle, float volume);
LV_API void lv_receiver_set_concealment(LvReceiverHandle* handle, int mode);
LV_API int lv_receiver_save_screenshot(LvReceiverHandle* handle, const char* ppm_path);
LV_API int lv_receiver_get_stats(LvReceiverHandle* handle, LvReceiverStats* out);
/* Latency mode, live (no stream restart): 1=Adaptive Wi-Fi, 0=Fixed. Returns LV_OK (0) or a negative error. */
LV_API int lv_receiver_set_adaptive_jitter(LvReceiverHandle* handle, int enabled);

/* ---- LAN Auto-Discovery ---- */
/* Scans the LAN for active LanVid receivers and returns a heap-allocated JSON array.
 * Free the returned string with lv_free_string(). */
LV_API char* lv_discovery_scan(int timeout_ms);
LV_API char* lv_discovery_scan_port(int timeout_ms, uint16_t discovery_port);

#ifdef __cplusplus
}
#endif
