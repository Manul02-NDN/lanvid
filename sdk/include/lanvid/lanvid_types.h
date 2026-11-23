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
    LV_AUDIO_FORMAT_F32   = 1, /* Interleaved 32-bit IEEE float [-1.0, 1.0] */
    LV_AUDIO_FORMAT_PCM24_PACKED = 2 /* 3 bytes / sample, little-endian signed (64-channel bus only) */
} LV_AudioFormat;

/* ---- 64-channel audio bus (LVSP audio v2) ---- */
#define LV_AUDIO_MAX_CHANNELS 64
#define LV_AUDIO_LATENCY_MIN_MS 2
#define LV_AUDIO_LATENCY_MAX_MS 2000
#define LV_MEDIA_TS_AUTO UINT64_MAX /* sender continues the channel / anchors to the master clock */

typedef enum LV_AudioLayout {
    LV_AUDIO_LAYOUT_INTERLEAVED = 0, /* data[frame * channels + ch] */
    LV_AUDIO_LAYOUT_PLANAR      = 1  /* data is `const void* const[channels]`, one buffer per channel */
} LV_AudioLayout;

/* Optional bus configuration; call BEFORE start (rate) - latency may also be changed live. */
typedef struct LV_AudioBusConfig {
    int32_t sample_rate;       /* 48000 (default) or 96000 */
    uint32_t latency_ms;       /* 2..2000 (default 20) */
    int32_t num_channels;      /* receiver: channels delivered by callback/pop (1..64, default 64) */
    uint32_t block_frames;     /* receiver callback block size in frames (default 240 = 5 ms @ 48k) */
} LV_AudioBusConfig;

/* Synchronised multichannel block delivered by the receiver callback (interleaved float32). */
typedef struct LV_MultichannelAudioBlock {
    const float* data;         /* num_frames * num_channels samples, valid during the callback only */
    int32_t num_channels;
    int32_t num_frames;
    int32_t sample_rate;
    uint32_t reserved;
    uint64_t playout_position_samples; /* master-clock position of the first frame */
    uint64_t block_index;
} LV_MultichannelAudioBlock;

/* Per-channel receiver telemetry (index 0..63). */
typedef struct LV_AudioChannelStats {
    uint64_t packets_rx;
    uint64_t packets_lost;      /* never recovered */
    uint64_t fec_recovered;
    uint64_t nack_recovered;
    uint64_t nacks_sent;
    uint64_t underruns;
    uint64_t late_packets;
    float level_dbfs;           /* peak with release, -120 = silence */
    int32_t active;             /* data received within the last second */
    uint32_t jitter_us;
    uint32_t reserved;
} LV_AudioChannelStats;

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

/* ---- Video Codecs ---- */
#ifndef LV_CODEC_TYPE_DEFINED
#define LV_CODEC_TYPE_DEFINED
typedef enum LV_CodecType {
    LV_CODEC_LANVID_DCT = 0, /* Intra-only slice-parallel AVX2/NEON DCT (< 2 ms latency) */
    LV_CODEC_H264       = 1, /* Zero-latency hardware/software H.264 (AVC) Annex-B */
    LV_CODEC_H265       = 2  /* Zero-latency hardware/software H.265 (HEVC) Annex-B */
} LV_CodecType;
#endif

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
    uint8_t codec;        /* LV_CodecType */
    uint8_t is_keyframe;  /* 1 for IDR / intra frame */
    uint8_t reserved;
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
    int32_t codec;            /* LV_CodecType (default LV_CODEC_LANVID_DCT) */
    uint32_t bitrate_kbps;    /* Target bitrate for H.264/H.265 (default 8000 kbps) */
    uint32_t keyframe_interval; /* IDR interval in frames for H.264/H.265 (default 120) */
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
    int32_t active_codec;        /* LV_CodecType */
    int32_t codec_is_hardware;   /* 1 if HW accelerated encoder/decoder active */
    uint32_t target_bitrate_kbps;/* Active target bitrate for H.264/H.265 */
    uint32_t reserved_u32;
    char codec_backend[64];      /* Active backend string (e.g. "AMDh264Encoder (HW)") */
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

/* ---- Full-duplex 64x64 AoIP transceiver (virtual device <-> peer node) ---- */
typedef enum LV_AudioDirection {
    LV_AUDIO_DIR_TX = 0, /* DAW outputs -> network (LanVidAudioBus_v1 TX ring)  */
    LV_AUDIO_DIR_RX = 1  /* network -> DAW inputs  (LanVidAudioBus_v1 RX ring)  */
} LV_AudioDirection;

typedef struct LV_DuplexConfig {
    char peer_host[64];       /* node we send our 64 channels to */
    uint16_t peer_port;       /* its listen port (default 5004) */
    uint16_t listen_port;     /* where we receive the peer's 64 channels (default 5005) */
    char bind_ip[64];         /* default "0.0.0.0" */
    uint32_t latency_ms;      /* 2..2000 (default 20) */
    int32_t sample_rate;      /* 48000 / 96000 */
    int32_t num_channels;     /* 1..64 (default 64) */
    uint16_t discovery_port;  /* default 5099 */
    uint16_t reserved;
    int32_t enable_discovery; /* 1 = announce this node as an AoIP node on the LAN */
    char device_name[128];    /* friendly name in discovery (default: host name) */
    char bus_name[64];        /* shared-memory bus (default "LanVidAudioBus_v1") */
    uint32_t rx_headroom_frames; /* frames kept queued for the DAW inputs (0 = auto: max(3 x DAW buffer, 512));
                                    raise on busy hosts to avoid underruns, at the cost of latency */
} LV_DuplexConfig;

typedef struct LV_DuplexStats {
    int32_t running;
    int32_t driver_connected;     /* a DAW has the virtual device open */
    char driver_name[32];
    uint32_t sample_rate;
    uint32_t buffer_size;
    /* peer -> us: measured locally (own MasterClock + RTT, independent of the TX direction) */
    int32_t rx_clock_synced;
    uint32_t rx_rtt_us;
    double rx_clock_skew_ppm;
    uint64_t rx_packets;
    uint64_t rx_lost;
    uint64_t rx_frames_to_daw;
    uint64_t rx_underruns;
    /* us -> peer (the peer measures this direction's clock/RTT on its side) */
    uint64_t tx_packets;
    uint64_t tx_frames_from_daw;
    uint64_t tx_overruns;
    uint64_t tx_pongs_sent;       /* clock-sync answers given to the peer's receiver */
} LV_DuplexStats;

/* Discovered AoIP node (LAN discovery, port 5099). */
typedef struct LV_AudioNode {
    char ip[64];
    char name[128];
    char hostname[128];
    char audio_driver[32];        /* "CoreAudio HAL" / "ASIO" / "JACK/PipeWire" / "" */
    uint16_t rx_port;             /* send to this port to reach the node */
    uint16_t tx_port;
    uint16_t channels;
    uint8_t duplex;
    uint8_t driver_connected;
    uint32_t rtt_ms;
    char os[32];
} LV_AudioNode;

/* Returns human-readable static string describing an LV_ErrorCode. */
LV_API const char* lv_error_string(int err);

#ifdef __cplusplus
}
#endif
