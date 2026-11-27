#pragma once
/*
 * LanVid Video SDK — Main Umbrella Header (v3.0.0)
 *
 * Self-contained C ABI for ultra-low-latency LAN video + 2-channel stereo audio streaming,
 * Multi-Cam reception, ISO recording, virtual camera VideoBus, and camera Tally/Intercom.
 *
 * For 64-channel AoIP, MIDI2, Personal Monitor Bus, and L2 Grid audio, use the CrossLink Audio SDK
 * (<crosslink/crosslink_sdk.h>).
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

/* ---- Sender Lifecycle & Zero-Copy Frame / Stereo Audio Submission ---- */
LV_API LvSenderHandle* lv_sender_create(void);
LV_API void lv_sender_destroy(LvSenderHandle* handle);

/* Starts sender in external zero-copy push mode. Returns LV_OK (0) or negative LV_ErrorCode. */
LV_API int lv_sdk_sender_start(LvSenderHandle* handle, const LV_SenderConfig* cfg);
LV_API void lv_sender_stop(LvSenderHandle* handle);

/* Pushes packed 32-bit frame (LV_PIXEL_FORMAT_BGRA8888 or LV_PIXEL_FORMAT_RGBA8888). */
LV_API int lv_sender_push_packed_frame(LvSenderHandle* handle, const uint8_t* data, int width,
                                       int height, int stride, int pixel_format,
                                       uint32_t timestamp_us);

/* Pushes planar/bi-planar YUV 4:2:0 frame (I420 / NV12 / NV21) with zero RGB conversion. */
LV_API int lv_sender_push_yuv_planes(LvSenderHandle* handle, const LV_YuvPlanes* planes, int width,
                                     int height, uint32_t timestamp_us);

/* Extended YUV 4:2:0 push supporting clockwise rotation (0/90/180/270 deg) and horizontal mirror. */
LV_API int lv_sender_push_yuv_planes_ex(LvSenderHandle* handle, const LV_YuvPlanes* planes,
                                        int width, int height, int rotation_deg, int mirror_h,
                                        uint32_t timestamp_us);

/* Zero-copy entry point for Apple CVPixelBuffer planes (32BGRA or NV12). */
LV_API int lv_sender_push_cvpixelbuffer_planes(LvSenderHandle* handle, const uint8_t* plane0,
                                               int stride0, const uint8_t* plane1, int stride1,
                                               int width, int height, int pixel_format,
                                               uint32_t timestamp_us);

/* True zero-copy entry point for Apple CVPixelBufferRef (IOSurface-backed NV12/BGRA -> VideoToolbox). */
LV_API int lv_sender_push_cvpixelbuffer(LvSenderHandle* handle, const void* cv_pixel_buffer_ref,
                                        int width, int height, uint32_t timestamp_us);

/* Pushes a 1-2 channel stereo audio chunk (LV_AUDIO_FORMAT_PCM16 or LV_AUDIO_FORMAT_F32) alongside video. */
LV_API int lv_sender_push_audio_chunk(LvSenderHandle* handle, const void* data, int num_frames,
                                      int sample_rate, int channels, int audio_format,
                                      uint32_t timestamp_us);

/* Physical NIC selection & Dual-NIC 1+1 failover (empty/NULL/"auto" = OS default route). */
LV_API int lv_sender_set_network_interfaces(LvSenderHandle* handle, const char* primary_ip,
                                            const char* secondary_ip);
LV_API int lv_sender_get_failover_stats(LvSenderHandle* handle, uint64_t* out_failover_packets,
                                        uint64_t* out_secondary_bytes);

/* Live runtime encoding & transport controls (lock-free / thread-safe): */
LV_API int lv_sender_request_keyframe_quality(LvSenderHandle* handle, int quality);
LV_API int lv_sender_set_bitrate(LvSenderHandle* handle, uint32_t bitrate_kbps);
LV_API int lv_sender_force_keyframe(LvSenderHandle* handle);
LV_API int lv_sender_set_fec_group(LvSenderHandle* handle, int fec_group_size);
LV_API int lv_sender_set_adaptive_quality(LvSenderHandle* handle, int enabled, int q_min, int q_max);

LV_API int lv_sender_get_stream_stats(LvSenderHandle* handle, LV_StreamStats* out);

/* ---- Receiver Lifecycle, Zero-Copy Callbacks & Stereo Audio Pull ---- */
LV_API LvReceiverHandle* lv_receiver_create(void);
LV_API void lv_receiver_destroy(LvReceiverHandle* handle);

/* Register zero-copy push callbacks (may be called before or after start; pass NULL to clear). */
LV_API void lv_receiver_set_video_callback(LvReceiverHandle* handle, LV_VideoFrameCallback cb,
                                           void* user_data);
LV_API void lv_receiver_set_audio_callback(LvReceiverHandle* handle, LV_AudioSamplesCallback cb,
                                           void* user_data);
LV_API void lv_receiver_set_state_callback(LvReceiverHandle* handle, LV_StateChangedCallback cb,
                                           void* user_data);

/* Starts UDP receiver + optional LAN video discovery responder. Returns LV_OK (0) or negative LV_ErrorCode. */
LV_API int lv_sdk_receiver_start(LvReceiverHandle* handle, const LV_ReceiverConfig* cfg);
LV_API void lv_receiver_stop(LvReceiverHandle* handle);
LV_API uint16_t lv_receiver_get_local_port(LvReceiverHandle* handle);

/* Pulls stereo 48 kHz PCM16 frames from the lock-free stereo audio jitter buffer. */
LV_API int lv_receiver_pop_audio_pcm16(LvReceiverHandle* handle, int16_t* out_stereo, int num_frames);

/* Adjusts stereo audio jitter buffer target latency live (5..60 ms). */
LV_API int lv_receiver_set_audio_target_ms(LvReceiverHandle* handle, double target_ms);

/* Optional polling helper for clients that prefer pull over video callbacks. */
LV_API int lv_receiver_get_latest_frame(LvReceiverHandle* handle, uint8_t* out_buf, size_t cap,
                                        int* out_w, int* out_h, uint32_t* inout_frame_id,
                                        int want_rgba);

LV_API void lv_receiver_set_volume(LvReceiverHandle* handle, float volume);
LV_API void lv_receiver_set_concealment(LvReceiverHandle* handle, int mode);
LV_API int lv_receiver_save_screenshot(LvReceiverHandle* handle, const char* ppm_path);
LV_API int lv_receiver_get_stream_stats(LvReceiverHandle* handle, LV_StreamStats* out);
LV_API int lv_receiver_request_keyframe(LvReceiverHandle* handle);

/* ---- Tally (Standby / Preview / Program ON AIR) & Full-Duplex Intercom (Director <-> Camera) ---- */
typedef struct LvTallyIntercomHandle LvTallyIntercomHandle;

typedef struct LV_TallyState {
    uint32_t camera_id;
    uint8_t tally_state;     /* 0 = Standby, 1 = Preview (Green), 2 = Program / ON AIR (Red) */
    uint8_t flags;           /* bit0 = rec_trigger, bit1 = high_bright */
    uint8_t battery_pct;
    int8_t temp_c;
    uint32_t revision;
    char camera_label[24];
} LV_TallyState;

typedef void (*LV_TallyCallback)(const LV_TallyState* state, void* user_data);

LV_API LvTallyIntercomHandle* lv_tally_intercom_create_studio(uint16_t bind_port);
LV_API LvTallyIntercomHandle* lv_tally_intercom_create_camera(const char* studio_ip, uint16_t studio_port,
                                                              uint32_t camera_id, const char* camera_label);
LV_API void lv_tally_intercom_destroy(LvTallyIntercomHandle* handle);
LV_API uint16_t lv_tally_intercom_port(LvTallyIntercomHandle* handle);
LV_API void lv_tally_intercom_poll(LvTallyIntercomHandle* handle);

LV_API int lv_studio_set_camera_tally(uint32_t stream_id, uint8_t tally_state, const char* camera_label,
                                      int rec_trigger);
LV_API int lv_tally_intercom_set_camera_tally(LvTallyIntercomHandle* studio, uint32_t stream_id,
                                              uint8_t tally_state, const char* camera_label, int rec_trigger);
LV_API void lv_camera_set_tally_callback(LvTallyIntercomHandle* handle, LV_TallyCallback cb, void* user_data);
LV_API void lv_tally_intercom_update_camera_telemetry(LvTallyIntercomHandle* handle, uint8_t battery_pct,
                                                      int8_t temp_c);

/* Remote camera control (Studio -> Camera): lens, focus, quality preset. */
typedef struct LV_CameraControl {
    uint32_t camera_id;
    uint32_t revision;
    uint8_t lens;           /* 0 = Wide, 1 = Ultra Wide, 2 = Tele, 3 = Front */
    uint8_t focus_auto;     /* 1 = continuous auto focus, 0 = manual focus_pos */
    uint16_t focus_pos;     /* 0 (near) .. 1000 (infinity) */
    uint8_t quality;        /* 0 = 360p, 1 = 720p, 2 = 1080p, 3 = 4K */
    uint8_t flags;
    uint16_t zoom_x100;     /* 100 = 1.0x */
} LV_CameraControl;
typedef void (*LV_CameraControlCallback)(const LV_CameraControl* control, void* user_data);
LV_API int lv_tally_intercom_set_camera_control(LvTallyIntercomHandle* studio, const LV_CameraControl* control);
LV_API void lv_camera_set_control_callback(LvTallyIntercomHandle* handle, LV_CameraControlCallback cb, void* user_data);

/* Bidirectional Video Config Sync + Camera Number Assignment + Smart IDR Keyframe Request. */
typedef struct LV_VideoConfigSync {
    uint64_t config_revision;
    uint32_t origin_id;
    uint32_t camera_id;
    uint8_t assigned_cam_number;  /* 0 = unassigned, 1..16 */
    uint8_t resolution_preset;    /* 0=360p, 1=720p, 2=1080p, 3=4K */
    uint8_t target_fps;           /* 24 / 25 / 30 / 50 / 60 */
    uint8_t codec;                /* 0=DCT, 1=H.264, 2=HEVC */
    uint32_t bitrate_kbps;        /* 5000..150000 */
    uint16_t video_delay_ms;      /* 5..250 ms */
    uint8_t fixed_delay_mode;     /* 1=Fixed, 0=Adaptive Wi-Fi */
    uint8_t camera_lens;          /* 0=Wide, 1=UltraWide, 2=Tele, 3=Front */
    uint8_t flags;                /* bit0=RequestIdr, bit1=FromCamera, bit2=HasBadgeColor, bit3=SyncAck */
    uint8_t badge_accent_rgba[4];
    char assigned_cam_tag[16];    /* "CAM 1", "CAM 2", "A", "B" */
} LV_VideoConfigSync;
typedef void (*LV_VideoConfigSyncCallback)(const LV_VideoConfigSync* cfg, void* user_data);
LV_API int lv_tally_intercom_send_video_config_sync(LvTallyIntercomHandle* handle, const LV_VideoConfigSync* cfg);
LV_API void lv_tally_intercom_set_video_config_sync_callback(LvTallyIntercomHandle* handle,
                                                             LV_VideoConfigSyncCallback cb, void* user_data);
LV_API int lv_tally_intercom_request_keyframe(LvTallyIntercomHandle* handle, uint32_t camera_id);

/* Duplex Intercom voice I/O (48 kHz mono float32, Director <-> Camera Operator). */
LV_API int lv_intercom_push_mic_pcm(LvTallyIntercomHandle* handle, const float* samples, int frames);
LV_API int lv_intercom_pull_rx_pcm(LvTallyIntercomHandle* handle, float* out_samples, int frames);
LV_API void lv_intercom_set_state(LvTallyIntercomHandle* handle, int talk_active, int listen_muted,
                                  float rx_gain, float sidetone_gain);
LV_API int lv_studio_set_camera_intercom(uint32_t camera_id, int talk_to_cam, int listen_from_cam, int all_call);
LV_API int lv_tally_intercom_set_camera_intercom(LvTallyIntercomHandle* studio, uint32_t camera_id,
                                                 int talk_to_cam, int listen_from_cam, int all_call);
LV_API int lv_tally_intercom_is_director_speaking(LvTallyIntercomHandle* handle);
LV_API float lv_tally_intercom_ducking_gain(LvTallyIntercomHandle* handle);

/* Extended Studio Tally & Intercom introspection + Wi-Fi 1+1 redundancy (v3.0.1 ABI-compatible). */
typedef struct LV_StudioCameraInfo {
    uint32_t camera_id;
    char label[64];
    char address[64];
    uint8_t tally_state;
    uint8_t rec_trigger;
    uint8_t talk_to_cam;
    uint8_t listen_from_cam;
    uint8_t battery_pct;
    int8_t temp_c;
    uint8_t operator_speaking;
    uint8_t connected;
    float voice_peak_dbfs;
    uint64_t tally_tx_count;
    uint64_t intercom_rx_pkts;
    uint64_t intercom_tx_pkts;
    uint8_t assigned_cam_number;
    uint8_t config_synced;
    uint16_t reserved;
    char assigned_cam_tag[16];
} LV_StudioCameraInfo;

typedef struct LV_IntercomStats {
    int32_t running;
    uint16_t bound_port;
    uint8_t director_speaking;
    uint8_t wifi_redundancy;
    float ducking_gain;
    float tx_peak_dbfs;
    float rx_peak_dbfs;
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t duplicates_dropped;
} LV_IntercomStats;

LV_API int lv_tally_intercom_list_cameras(LvTallyIntercomHandle* studio, LV_StudioCameraInfo* out_cams,
                                          int max_count);
LV_API int lv_tally_intercom_get_stats(LvTallyIntercomHandle* handle, LV_IntercomStats* out);
LV_API void lv_tally_intercom_set_wifi_redundancy(LvTallyIntercomHandle* handle, int enabled,
                                                  uint32_t stagger_us);

/* ---- Network Interface Enumeration ---- */
LV_API int lv_net_list_interfaces(LvNetInterfaceInfo* out_list, int max_count);
LV_API int lv_net_list_interfaces_ex(LvNetInterfaceInfo* out_list, int max_count,
                                     int include_disconnected, int show_all);

/* ---- Multi-Cam (LanVid Video): up to 16 cameras, per-camera ISO recording, OS virtual cameras ---- */
typedef struct LvMultiCamHandle LvMultiCamHandle;

typedef struct LV_MultiCamConfig {
    char bind_ip[64];          /* "0.0.0.0" */
    uint16_t base_port;        /* 5006 */
    int32_t port_mode;         /* 0 = all phones on base_port (demux by sender), 1 = base_port + 2*slot */
    uint32_t max_slots;        /* 1..16 */
    int32_t auto_assign;       /* bind new senders to the first free slot */
    int32_t publish_video_bus; /* feed LanVidVideoBus_v1 ("LanVid Cam 1..8" + "LanVid Program Out") */
    uint32_t vcam_count;       /* virtual cameras exposed to the OS (0..8) */
    int32_t vcam_placeholder;  /* standby card when a camera is gone */
    char video_bus_name[64];   /* "" = LanVidVideoBus_v1 */
} LV_MultiCamConfig;

typedef struct LV_MultiCamSlotInfo {
    int32_t index;             /* 0-based ("Cam index+1") */
    uint32_t camera_id;
    char label[64];
    char source_name[64];
    char sender_ip[48];
    uint16_t listen_port;
    int32_t bound, streaming, width, height;
    double fps;
    char codec[16];            /* "H.264" / "H.265" / "DCT" */
    double bitrate_mbps, latency_ms;
    int32_t battery_pct, temp_c;
    float audio_peak_dbfs;
    uint8_t tally;             /* 0 = STBY, 1 = PVW, 2 = PGM */
    int32_t recording, vcam_active;
    uint64_t frames;
} LV_MultiCamSlotInfo;

typedef struct LV_IsoSessionConfig {
    char output_dir[512];
    char session_name[64];
    int32_t container;         /* 0 = MP4, 1 = MKV, 2 = MOV */
    int32_t record_program;    /* extra PGM_Master file */
    uint32_t slot_mask;        /* cameras included in REC ALL */
} LV_IsoSessionConfig;

typedef struct LV_IsoSlotStats {
    int32_t recording, waiting_for_keyframe;
    char path[512];
    uint64_t bytes, duration_us, frames;
    double write_mbps;
} LV_IsoSlotStats;

LV_API void lv_multicam_config_default(LV_MultiCamConfig* cfg);
LV_API LvMultiCamHandle* lv_multicam_create(void);
LV_API void lv_multicam_destroy(LvMultiCamHandle* mc);
LV_API int lv_multicam_start(LvMultiCamHandle* mc, const LV_MultiCamConfig* cfg);
LV_API void lv_multicam_stop(LvMultiCamHandle* mc);
LV_API uint32_t lv_multicam_slot_count(LvMultiCamHandle* mc);
LV_API int lv_multicam_get_slot_info(LvMultiCamHandle* mc, uint32_t slot, LV_MultiCamSlotInfo* out);
LV_API int lv_multicam_set_slot_label(LvMultiCamHandle* mc, uint32_t slot, const char* label);
LV_API int lv_multicam_pin_slot(LvMultiCamHandle* mc, uint32_t slot, const char* ip);
LV_API void lv_multicam_release_slot(LvMultiCamHandle* mc, uint32_t slot);
LV_API int lv_multicam_get_latest_frame_bgra(LvMultiCamHandle* mc, uint32_t slot, uint8_t* buf, size_t cap,
                                             int* width, int* height, uint64_t* seq);
LV_API int lv_multicam_set_program(LvMultiCamHandle* mc, int slot);
LV_API int lv_multicam_rec_start_all(LvMultiCamHandle* mc, const LV_IsoSessionConfig* session);
LV_API void lv_multicam_rec_stop_all(LvMultiCamHandle* mc);
LV_API int lv_multicam_rec_slot(LvMultiCamHandle* mc, uint32_t slot, int on);
LV_API int lv_multicam_get_rec_stats(LvMultiCamHandle* mc, uint32_t slot, LV_IsoSlotStats* out);
LV_API void lv_multicam_set_vcam(LvMultiCamHandle* mc, uint32_t count, int placeholder);
LV_API int lv_multicam_set_slot_playout(LvMultiCamHandle* mc, uint32_t slot, uint32_t delay_ms, int fixed_mode);
LV_API int lv_multicam_force_idr(LvMultiCamHandle* mc, uint32_t slot);

/* Extended Multi-Cam controls, stereo audio pull, playout stats, and session info (v3.0.1). */
typedef struct LV_MultiCamSlotExt {
    char pinned_ip[48];
    uint32_t playout_delay_ms;
    double playout_effective_ms;
    int32_t playout_fixed_mode;
    double playout_jitter_ms;
    uint64_t playout_drops;
    uint64_t playout_holds;
    uint8_t assigned_cam_number;
    uint8_t config_synced;
    uint8_t badge_rgba[4];
    uint16_t reserved;
    char assigned_cam_tag[16];
    uint64_t packets_total;
    uint64_t fec_recovered;
    uint64_t crc_errors;
    uint64_t late_packets;
    uint64_t frames_concealed;
    uint64_t audio_packets;
    double audio_buffer_ms;
    double packet_loss_pct;
    int32_t decoder_is_hw;
    char decoder_backend[64];
} LV_MultiCamSlotExt;

typedef struct LV_MultiCamSessionInfo {
    int32_t recording_any;
    int32_t video_bus_open;
    char session_dir[512];
    char session_timecode[32];
    uint64_t disk_free_bytes;
    LV_IsoSlotStats program_stats;
} LV_MultiCamSessionInfo;

LV_API int lv_multicam_set_preview(LvMultiCamHandle* mc, int slot);
LV_API int lv_multicam_sync_tally_telemetry(LvMultiCamHandle* mc, LvTallyIntercomHandle* studio);
LV_API int lv_multicam_get_slot_ext(LvMultiCamHandle* mc, uint32_t slot, LV_MultiCamSlotExt* out);
LV_API int lv_multicam_get_session_info(LvMultiCamHandle* mc, const char* disk_path, LV_MultiCamSessionInfo* out);
LV_API int lv_multicam_pop_slot_audio_pcm16(LvMultiCamHandle* mc, uint32_t slot, int16_t* out_stereo, int num_frames);
LV_API int lv_multicam_set_slot_audio_config(LvMultiCamHandle* mc, uint32_t slot, double target_ms, float volume);
LV_API int lv_multicam_set_slot_concealment(LvMultiCamHandle* mc, uint32_t slot, int conceal_mode);
LV_API int lv_multicam_set_slot_assigned_camera(LvMultiCamHandle* mc, uint32_t slot, uint8_t cam_number,
                                                const char* tag, const uint8_t rgba[4]);

/* ---- LAN Video Auto-Discovery ---- */
/* Scans the LAN for active video receivers (discovery_port=0 -> default 5099). */
LV_API int lv_discovery_scan_peers(int timeout_ms, uint16_t discovery_port,
                                   LV_DiscoveredPeer* out_peers, int max_peers);

/* JSON discovery helpers (free returned string with lv_free_string()). */
LV_API char* lv_discovery_scan(int timeout_ms);
LV_API char* lv_discovery_scan_port(int timeout_ms, uint16_t discovery_port);

#ifdef __cplusplus
}
#endif
