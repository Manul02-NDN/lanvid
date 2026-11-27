#pragma once
/*
 * CrossLink Audio SDK — Public ABI Types, Enumerations & Fixed-Layout Structs
 * Pure pro-audio protocol: 64-ch AoIP, MasterClock, Full-Duplex SHM Bus, MIDI2,
 * Personal Monitor Bus, ControlSync, and L2 Grid (zero video dependencies).
 */

#include <stddef.h>
#include <stdint.h>

#include "crosslink_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Audio Constants & Sample Formats ---- */
#define CL_AUDIO_MAX_CHANNELS    64
#define CL_AUDIO_LATENCY_MIN_MS  2
#define CL_AUDIO_LATENCY_MAX_MS  2000
#define CL_MEDIA_TS_AUTO         UINT64_MAX
#define CL_MONITOR_BITRATE_LOSSLESS 0xFFFF

typedef enum CL_AudioFormat {
    CL_AUDIO_FORMAT_PCM16        = 0, /* Signed 16-bit LE PCM */
    CL_AUDIO_FORMAT_F32          = 1, /* 32-bit IEEE float [-1.0, 1.0] */
    CL_AUDIO_FORMAT_PCM24_PACKED = 2  /* 3 bytes / sample, signed LE */
} CL_AudioFormat;

typedef enum CL_AudioLayout {
    CL_AUDIO_LAYOUT_INTERLEAVED = 0, /* data[frame * channels + ch] */
    CL_AUDIO_LAYOUT_PLANAR      = 1  /* data is `const void* const[channels]` or contiguous channel planes */
} CL_AudioLayout;

/* ---- SDK Error Codes ---- */
typedef enum CL_ErrorCode {
    CL_OK                   =  0,
    CL_ERR_INVALID_ARG      = -1,
    CL_ERR_NOT_INITIALIZED  = -2,
    CL_ERR_NOT_RUNNING      = -3,
    CL_ERR_SOCKET           = -4,
    CL_ERR_CODEC            = -5,
    CL_ERR_BUFFER_TOO_SMALL = -6,
    CL_ERR_UNSUPPORTED      = -7,
    CL_ERR_INTERNAL         = -8
} CL_ErrorCode;

/* ---- Stream Lifecycle States ---- */
typedef enum CL_StreamState {
    CL_STATE_STOPPED   = 0,
    CL_STATE_STARTING  = 1,
    CL_STATE_RUNNING   = 2,
    CL_STATE_CONNECTED = 3,
    CL_STATE_ERROR     = 4
} CL_StreamState;

/* ---- 64-Channel Audio Bus Configuration ---- */
typedef struct CL_AudioBusConfig {
    int32_t sample_rate;   /* 48000 (default) or 96000 */
    uint32_t latency_ms;   /* 2..2000 (default 20) */
    int32_t num_channels;  /* 1..64 (default 64) */
    uint32_t block_frames; /* receiver callback block size (default 240 = 5 ms @ 48 kHz) */
} CL_AudioBusConfig;

typedef struct CL_SenderConfig {
    char dest_ip[64];         /* default "127.0.0.1" */
    uint16_t dest_port;       /* default 5004 */
    uint16_t bind_port;       /* 0 = ephemeral */
    char bind_ip[64];         /* "" / "0.0.0.0" = auto */
    char secondary_bind_ip[64];
    int32_t mtu;              /* default 1400 */
    int32_t sample_rate;      /* 48000 / 96000 */
    uint32_t latency_ms;      /* 2..2000 (default 20) */
    int32_t audio_codec;      /* 0=PCM32F, 1=PCM24, 2=PCM16, 3..8=Opus/ADPCM */
    int32_t audio_bitrate_kbps; /* 0=Lossless, or 256/192/128/96/64 */
} CL_SenderConfig;

typedef struct CL_ReceiverConfig {
    char bind_ip[64];         /* default "0.0.0.0" */
    uint16_t bind_port;       /* default 5004 (0 = ephemeral) */
    uint16_t discovery_port;  /* default 5021 */
    int32_t sample_rate;      /* 48000 / 96000 */
    uint32_t latency_ms;      /* 2..2000 (default 20) */
    int32_t num_channels;     /* 1..64 (default 64) */
    uint32_t block_frames;    /* default 240 */
    int32_t adaptive_jitter;  /* 1=Adaptive Wi-Fi (default), 0=Fixed Ethernet */
    int32_t enable_discovery; /* 1=announce on UDP discovery port 5021 */
    char device_name[128];
} CL_ReceiverConfig;

typedef struct CL_MultichannelAudioBlock {
    const float* data;
    int32_t num_channels;
    int32_t num_frames;
    int32_t sample_rate;
    uint32_t reserved;
    uint64_t playout_position_samples;
    uint64_t block_index;
} CL_MultichannelAudioBlock;

typedef struct CL_AudioChannelStats {
    uint64_t packets_rx;
    uint64_t packets_lost;
    uint64_t fec_recovered;
    uint64_t nack_recovered;
    uint64_t nacks_sent;
    uint64_t underruns;
    uint64_t late_packets;
    float level_dbfs;
    int32_t active;
    uint32_t jitter_us;
    uint32_t reserved;
} CL_AudioChannelStats;

typedef struct CL_StreamStats {
    int32_t state;                /* CL_StreamState */
    int32_t clock_synced;         /* 1 when MasterClock locked */
    uint32_t rtt_us;
    double clock_skew_ppm;
    double audio_kbps;
    double effective_latency_ms;
    uint32_t effective_target_ms;
    int32_t adaptive_jitter;
    uint64_t audio_packets;
    uint64_t audio_bytes;
    uint64_t fec_packets;
    uint64_t retransmits;
    uint64_t midi_events;
    uint64_t stream_epoch_resets;
} CL_StreamStats;

/* ---- Full-Duplex 64x64 AoIP Transceiver ---- */
typedef enum CL_AudioDirection {
    CL_AUDIO_DIR_TX = 0, /* DAW outputs -> network (CrossLinkAudioBus_v1 TX ring) */
    CL_AUDIO_DIR_RX = 1  /* network -> DAW inputs  (CrossLinkAudioBus_v1 RX ring) */
} CL_AudioDirection;

typedef struct CL_DuplexConfig {
    char peer_host[64];
    uint16_t peer_port;          /* default 5004 */
    uint16_t listen_port;        /* default 5005 */
    char bind_ip[64];            /* default "0.0.0.0" */
    uint32_t latency_ms;         /* 2..2000 (default 20) */
    int32_t sample_rate;         /* 48000 / 96000 */
    int32_t num_channels;        /* 1..64 (default 64) */
    uint16_t discovery_port;     /* default 5021 */
    uint16_t reserved;
    int32_t enable_discovery;    /* 1 = announce AoIP node */
    char device_name[128];
    char bus_name[64];           /* "" = "CrossLinkAudioBus_v1" */
    uint32_t rx_headroom_frames; /* 0 = auto */
    int32_t audio_codec;         /* 0=PCM32F, 1=PCM24, 2=PCM16, 3..8=Opus/ADPCM */
    int32_t adaptive_jitter;     /* 1=Adaptive Wi-Fi, 0=Fixed Ethernet */
} CL_DuplexConfig;

typedef struct CL_DuplexStats {
    int32_t running;
    int32_t driver_connected;
    char driver_name[32];
    uint32_t sample_rate;
    uint32_t buffer_size;
    int32_t rx_clock_synced;
    uint32_t rx_rtt_us;
    double rx_clock_skew_ppm;
    uint64_t rx_packets;
    uint64_t rx_lost;
    uint64_t rx_frames_to_daw;
    uint64_t rx_underruns;
    uint64_t tx_packets;
    uint64_t tx_frames_from_daw;
    uint64_t tx_overruns;
    uint64_t tx_pongs_sent;
    uint64_t midi_tx_events;
    uint64_t midi_rx_events;
    uint64_t stream_epoch_resets;
    uint32_t active_midi_notes;
    uint32_t reserved_midi;
    double effective_latency_ms;
    uint32_t effective_target_ms;
    int32_t adaptive_jitter;
} CL_DuplexStats;

/* ---- MIDI2 ---- */
typedef struct CL_MidiEvent {
    uint64_t media_ts_us;
    uint32_t event_seq;
    int32_t block_offset_frames;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t port; /* 0..15 */
} CL_MidiEvent;

typedef struct CL_MidiDejitterStats {
    double target_buffer_ms;
    double playout_delay_ms;
    double absorbed_jitter_ms;
    int32_t fixed_mode;
    int32_t synced_with_audio;
    int32_t midi_only_mode;
    uint32_t queued_events;
    uint64_t total_enqueued;
    uint64_t total_dispatched;
    uint64_t late_events;
} CL_MidiDejitterStats;

/* ---- Discovered CrossLink Audio Node ---- */
typedef struct CL_AudioNode {
    char ip[64];
    char name[128];
    char hostname[128];
    char audio_driver[32];
    uint16_t rx_port;
    uint16_t tx_port;
    uint16_t channels;
    uint8_t duplex;
    uint8_t driver_connected;
    uint32_t rtt_ms;
    char os[32];
} CL_AudioNode;

/* ---- Personal Monitor Bus ---- */
typedef struct CL_MonitorDspParams {
    int32_t hpf_enabled;
    float hpf_hz;
    int32_t eq_enabled;
    float eq_low_db;
    float eq_mid_hz;
    float eq_mid_db;
    float eq_high_db;
    int32_t comp_enabled;
    float comp_thresh_db;
    float comp_ratio;
    float comp_attack_ms;
    float comp_release_ms;
    float comp_makeup_db;
    float master_volume_db;
    int32_t master_mute;
} CL_MonitorDspParams;

typedef struct CL_MonitorClientInfo {
    int32_t slot;
    uint32_t client_id;
    char name[32];
    char address[32];
    uint16_t bitrate_kbps;
    uint16_t latency_ms;
    int32_t muted;
    int32_t connected;
    uint64_t packets_tx;
} CL_MonitorClientInfo;

typedef struct CL_MonitorMetaChannel {
    char name[16];
    int8_t peak_dbfs;
    uint8_t reserved[3];
} CL_MonitorMetaChannel;

typedef struct CL_MonitorTelemetry {
    int32_t connected;
    int32_t num_channels;
    float bus_gain_reduction_db;
    float bus_peak_dbfs;
    double buffer_ms;
    uint64_t audio_packets_rx;
    uint64_t meta_packets_rx;
} CL_MonitorTelemetry;

typedef struct CL_MonitorFlowTelemetry {
    int32_t  clock_locked;
    uint32_t clock_offset_jitter_us;
    uint64_t duplicates_recovered;
    uint32_t active_flow_channels;
    double   effective_playout_ms;
    int32_t  wifi_redundancy_enabled;
    uint64_t flow_packets_rx;
    uint64_t nacks_sent;
} CL_MonitorFlowTelemetry;

/* ---- Real-Time Bidirectional Audio Control Sync ---- */
typedef struct CL_ControlSyncState {
    uint64_t revision;
    uint32_t origin_id;
    uint16_t audio_channels;
    uint16_t audio_bitrate_kbps;
    uint16_t target_buffer_ms;
    uint8_t  latency_mode;
    uint8_t  midi_only;
    int8_t   active_preset_id;
    uint8_t  flags;
    int16_t  master_gain_cb;
    uint8_t  audio_codec;
} CL_ControlSyncState;

/* ---- Network Interface Enumeration ---- */
typedef enum CL_NetInterfaceType {
    CL_NET_IFACE_OTHER        = 0,
    CL_NET_IFACE_ETHERNET     = 1,
    CL_NET_IFACE_WIFI         = 2,
    CL_NET_IFACE_USB_ETHERNET = 3,
    CL_NET_IFACE_VIRTUAL      = 4
} CL_NetInterfaceType;

typedef struct ClNetInterfaceInfo {
    char     id[128];
    char     system_name[64];
    char     friendly_name[128];
    uint8_t  type;
    uint8_t  is_up;
    uint16_t reserved;
    char     ipv4_address[32];
    char     subnet_mask[32];
    char     mac_address[24];
    uint32_t link_speed_mbps;
    uint32_t if_index;
} ClNetInterfaceInfo;

CL_API const char* cl_error_string(int err);

#ifdef __cplusplus
}
#endif
