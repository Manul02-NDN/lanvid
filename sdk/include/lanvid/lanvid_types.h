#pragma once
/*
 * LanVid SDK — Public ABI Types, Enumerations & Fixed-Layout Structs
 * Zero dependencies on internal headers, STL, or OS GUI/capture frameworks.
 */

#include <stddef.h>
#include <stdint.h>

#include "lanvid_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pixel Formats ---- */
typedef enum LV_PixelFormat {
    LV_PIXEL_FORMAT_BGRA8888 = 0, /* 32-bit packed B,G,R,A (Windows D3D11 / Apple CoreVideo) */
    LV_PIXEL_FORMAT_RGBA8888 = 1, /* 32-bit packed R,G,B,A (OpenGL / Vulkan / Android Surface) */
    LV_PIXEL_FORMAT_I420     = 2, /* Planar Y, U, V 4:2:0 (uv_pixel_stride = 1) */
    LV_PIXEL_FORMAT_NV12     = 3, /* Bi-planar Y + interleaved UV 4:2:0 (Apple VideoToolbox / Android) */
    LV_PIXEL_FORMAT_NV21     = 4  /* Bi-planar Y + interleaved VU 4:2:0 (Android legacy camera) */
} LV_PixelFormat;

/* ---- Audio Sample Formats ---- */
typedef enum LV_AudioFormat {
    LV_AUDIO_FORMAT_PCM16 = 0, /* Interleaved signed 16-bit little-endian PCM */
    LV_AUDIO_FORMAT_F32   = 1  /* Interleaved 32-bit IEEE float [-1.0, 1.0] */
} LV_AudioFormat;

/* ---- SDK Error Codes ---- */
typedef enum LV_ErrorCode {
    LV_OK                   =  0,
    LV_ERR_INVALID_ARG      = -1,
    LV_ERR_NOT_INITIALIZED  = -2,
    LV_ERR_NOT_RUNNING      = -3,
    LV_ERR_SOCKET           = -4,
    LV_ERR_CODEC            = -5,
    LV_ERR_BUFFER_TOO_SMALL = -6,
    LV_ERR_UNSUPPORTED      = -7,
    LV_ERR_INTERNAL         = -8
} LV_ErrorCode;

/* ---- Stream Lifecycle States ---- */
typedef enum LV_StreamState {
    LV_STATE_STOPPED   = 0,
    LV_STATE_STARTING  = 1,
    LV_STATE_RUNNING   = 2, /* Socket bound / listening or sender active */
    LV_STATE_CONNECTED = 3, /* Actively exchanging frames / telemetry */
    LV_STATE_ERROR     = 4
} LV_StreamState;

/* ---- Chroma Subsampling ---- */
typedef enum LV_ChromaFormat {
    LV_CHROMA_420 = 0,
    LV_CHROMA_422 = 1,
    LV_CHROMA_444 = 2
} LV_ChromaFormat;

/* ---- Concealment Modes ---- */
typedef enum LV_ConcealmentMode {
    LV_CONCEALMENT_GRAY       = 0,
    LV_CONCEALMENT_PREV_FRAME = 1
} LV_ConcealmentMode;

/* ---- Zero-Copy Video Frame View (delivered to receiver video callback) ---- */
typedef struct LV_VideoFrameView {
    const uint8_t* data;
    size_t size;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t pixel_format; /* LV_PixelFormat (BGRA8888 or RGBA8888) */
    uint32_t frame_id;
    uint32_t timestamp_us;
    uint16_t slices_ok;
    uint16_t slices_total;
    uint8_t quality;
    uint8_t reserved[3];
} LV_VideoFrameView;

/* ---- Zero-Copy Audio Chunk View (delivered to receiver audio callback) ---- */
typedef struct LV_AudioChunkView {
    const void* data;
    int32_t num_frames;
    int32_t sample_rate;
    int32_t channels;
    int32_t audio_format; /* LV_AudioFormat */
    uint32_t seq_id;
    uint32_t timestamp_us;
} LV_AudioChunkView;

/* ---- Multi-Plane YUV Descriptor (I420 / NV12 / NV21 zero-copy input) ---- */
typedef struct LV_YuvPlanes {
    const uint8_t* plane_y;
    const uint8_t* plane_u; /* For NV12/NV21, may point to interleaved UV/VU plane */
    const uint8_t* plane_v; /* Optional for NV12/NV21 (derived automatically if NULL) */
    int32_t y_stride;
    int32_t u_stride;
    int32_t v_stride;
    int32_t uv_pixel_stride; /* 1 for I420, 2 for NV12/NV21 */
    int32_t pixel_format;    /* LV_PIXEL_FORMAT_I420 / NV12 / NV21 */
} LV_YuvPlanes;

/* ---- SDK Sender Configuration ---- */
typedef struct LV_SenderConfig {
    char dest_ip[64];
    uint16_t dest_port;
    uint16_t reserved_port;
    int32_t width;
    int32_t height;
    double fps;
    int32_t quality;          /* 1..100 (default 80) */
    int32_t chroma;           /* LV_ChromaFormat (default LV_CHROMA_420) */
    int32_t slice_height;     /* default 16 */
    int32_t mtu;              /* default 1400 */
    int32_t enable_crc;       /* default 1 */
    int32_t fec_group_size;   /* 0=off, 6..8 recommended for Wi-Fi */
    int32_t adaptive_quality; /* 0=off, 1=AIMD enabled */
    int32_t q_min;            /* default 35 */
    int32_t q_max;            /* default 85 */
    int32_t threads;          /* 0=auto */
    double pace_mbps;         /* 0=unpaced */
} LV_SenderConfig;

/* ---- SDK Receiver Configuration ---- */
typedef struct LV_ReceiverConfig {
    char bind_ip[64];         /* default "0.0.0.0" */
    uint16_t bind_port;       /* default 5000 (0 = OS ephemeral) */
    uint16_t discovery_port;  /* default 5099 */
    int32_t output_format;    /* LV_PIXEL_FORMAT_BGRA8888 or LV_PIXEL_FORMAT_RGBA8888 */
    int32_t decode_threads;   /* 0=auto */
    int32_t conceal_mode;     /* LV_ConcealmentMode */
    double frame_timeout_ms;  /* default 25.0 */
    double audio_target_ms;   /* default 12.0 */
    int32_t enable_os_audio;  /* 0=use audio callback/pull (SDK default), 1=OS audio playback */
    int32_t enable_discovery; /* 1=respond to LAN discovery scans */
    char device_name[128];    /* friendly name advertised on LAN */
} LV_ReceiverConfig;

/* ---- Unified Stream Telemetry Snapshot ---- */
typedef struct LV_StreamStats {
    int32_t state;            /* LV_StreamState */
    int32_t width;
    int32_t height;
    int32_t active_quality;
    double fps;
    double bitrate_mbps;
    double latency_p50_ms;
    double latency_p99_ms;
    double encode_ms;
    double packet_loss_pct;
    uint64_t frames_total;
    uint64_t frames_concealed;
    uint64_t packets_total;
    uint64_t fec_recovered;
    uint64_t crc_errors;
    uint64_t late_packets;
    uint64_t audio_packets;
    double audio_kbps;
    double audio_buffer_ms;
} LV_StreamStats;

/* ---- Discovered LAN Peer ---- */
typedef struct LV_DiscoveredPeer {
    char ip[64];
    uint16_t port;
    uint16_t reserved;
    uint32_t rtt_ms;
    char name[128];
    char os[32];
    char simd[32];
} LV_DiscoveredPeer;

/* Returns human-readable static string describing an LV_ErrorCode. */
LV_API const char* lv_error_string(int err);

#ifdef __cplusplus
}
#endif
