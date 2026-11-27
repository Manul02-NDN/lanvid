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
typedef void (*LV_MultichannelAudioCallback)(const LV_MultichannelAudioBlock* block, void* user_data);
typedef void (*LV_MidiEventCallback)(const LV_MidiEvent* ev, void* user_data);
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

/* Extended YUV 4:2:0 push supporting clockwise rotation (0/90/180/270 deg) and horizontal mirror
 * (e.g. Android front camera SENSOR_ORIENTATION=270 + LENS_FACING_FRONT) with strict rowStride and
 * pixelStride bounds safety. Output dimensions are swapped automatically for 90/270 deg. */
LV_API int lv_sender_push_yuv_planes_ex(LvSenderHandle* handle, const LV_YuvPlanes* planes,
                                        int width, int height, int rotation_deg, int mirror_h,
                                        uint32_t timestamp_us);

/* Zero-copy convenience entry point for Apple CVPixelBuffer (32BGRA or 420YpCbCr8BiPlanar NV12)
 * after CVPixelBufferLockBaseAddress. For BGRA pass plane1=NULL. */
LV_API int lv_sender_push_cvpixelbuffer_planes(LvSenderHandle* handle, const uint8_t* plane0,
                                               int stride0, const uint8_t* plane1, int stride1,
                                               int width, int height, int pixel_format,
                                               uint32_t timestamp_us);

/* True zero-copy entry point for Apple CVPixelBufferRef (IOSurface-backed NV12/BGRA).
 * Passes CVPixelBufferRef directly into VideoToolbox without locking, allocating, or copying pixels.
 * Returns LV_ERR_UNSUPPORTED on non-Apple platforms or non-VT codecs. */
LV_API int lv_sender_push_cvpixelbuffer(LvSenderHandle* handle, const void* cv_pixel_buffer_ref,
                                        int width, int height, uint32_t timestamp_us);

/* Pushes an audio chunk (LV_AUDIO_FORMAT_PCM16 or LV_AUDIO_FORMAT_F32) over the same UDP socket. */
LV_API int lv_sender_push_audio_chunk(LvSenderHandle* handle, const void* data, int num_frames,
                                      int sample_rate, int channels, int audio_format,
                                      uint32_t timestamp_us);

/* ---- 64-channel AoIP bus (additive; LV_SDK_ABI_VERSION unchanged) ---- */
/* Fills defaults: 48 kHz, 20 ms, 64 channels, 240-frame blocks. */
LV_API void lv_audio_bus_config_default(LV_AudioBusConfig* cfg);
/* Must be called before lv_sdk_sender_start() (sample rate); latency can be changed any time. */
LV_API int lv_sender_configure_audio_bus(LvSenderHandle* handle, const LV_AudioBusConfig* cfg);
/* Independent push of ONE channel (0..63). media_timestamp_samples = master-clock position of the
 * first sample, or LV_MEDIA_TS_AUTO. Any thread; each channel may be fed from its own thread. */
LV_API int lv_sender_push_audio_channel(LvSenderHandle* handle, uint16_t channel_index,
                                        const void* samples, uint32_t num_samples,
                                        LV_AudioFormat format, uint64_t media_timestamp_samples);
/* Pushes num_channels channels at once (ASIO / CoreAudio / JACK / DAW). For LV_AUDIO_LAYOUT_PLANAR
 * `data` is an array of num_channels buffer pointers. All channels share one timestamp. */
LV_API int lv_sender_push_audio_multichannel(LvSenderHandle* handle, const void* data,
                                             uint16_t num_channels, uint32_t num_samples,
                                             LV_AudioFormat format, LV_AudioLayout layout,
                                             uint64_t media_timestamp_samples);
/* Sender-side latency budget: selects chunk size, FEC and retransmission policy (2..2000 ms). */
LV_API int lv_sender_set_audio_latency_ms(LvSenderHandle* handle, uint32_t latency_ms);
/* Live audio compression profile: 0=PCM32F, 1=PCM24 (-25%), 2=PCM16 (-50%), 3=Opus CELT (-85%; IMA-ADPCM fallback for non-Opus frame sizes). */
LV_API int lv_sender_set_audio_codec(LvSenderHandle* handle, int codec);
/* Live per-channel audio bitrate target: 0 = Lossless (PCM), or 256 / 192 / 128 / 96 / 64 kbps/ch. */
LV_API int lv_sender_set_audio_bitrate(LvSenderHandle* handle, int kbps_per_ch);
/* Glitch-Free Wi-Fi Redundancy (1+1 Duplicate): sends backup UDP audio packets staggered by stagger_us (default 1500 us). */
LV_API int lv_sender_set_wifi_redundancy(LvSenderHandle* handle, int enabled, uint32_t stagger_us);
/* Physical NIC selection & Dual-NIC 1+1 failover (empty/NULL/"auto" = OS default route). */
LV_API int lv_sender_set_network_interfaces(LvSenderHandle* handle, const char* primary_ip,
                                            const char* secondary_ip);
LV_API int lv_sender_get_failover_stats(LvSenderHandle* handle, uint64_t* out_failover_packets,
                                        uint64_t* out_secondary_bytes);
/* Current master-clock position in samples (use to timestamp pushes from a DAW callback). */
LV_API uint64_t lv_sender_get_master_clock_samples(LvSenderHandle* handle);

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

/* ---- 64-channel phase-coherent receiver bus ---- */
LV_API int lv_receiver_configure_audio_bus(LvReceiverHandle* handle, const LV_AudioBusConfig* cfg);
/* Dynamic playout latency 2..2000 ms, applied seamlessly while streaming. */
LV_API int lv_receiver_set_audio_latency_ms(LvReceiverHandle* handle, uint32_t latency_ms);
/* Registers a callback receiving synchronised blocks (paced internally every block_frames).
 * While registered, lv_receiver_pop_audio_multichannel() returns LV_ERR_UNSUPPORTED. NULL clears. */
LV_API int lv_receiver_set_multichannel_audio_callback(LvReceiverHandle* handle,
                                                       LV_MultichannelAudioCallback cb,
                                                       void* user_data);
/* Pull mode for audio-device callbacks: renders num_frames sample-aligned float32 frames for
 * channels [0, num_channels). Returns frames rendered (>= 0) or a negative LV_ErrorCode. */
LV_API int lv_receiver_pop_audio_multichannel(LvReceiverHandle* handle, float* out,
                                              uint16_t num_channels, uint32_t num_frames,
                                              LV_AudioLayout layout);
LV_API int lv_receiver_get_channel_stats(LvReceiverHandle* handle, uint16_t channel_index,
                                         LV_AudioChannelStats* out);

/* Optional polling helper for clients that prefer pull over video callbacks (e.g. Python/OpenGL loops). */
LV_API int lv_receiver_get_latest_frame(LvReceiverHandle* handle, uint8_t* out_buf, size_t cap,
                                        int* out_w, int* out_h, uint32_t* inout_frame_id,
                                        int want_rgba);

LV_API void lv_receiver_set_volume(LvReceiverHandle* handle, float volume);
LV_API void lv_receiver_set_concealment(LvReceiverHandle* handle, int mode);
LV_API int lv_receiver_save_screenshot(LvReceiverHandle* handle, const char* ppm_path);
LV_API int lv_receiver_get_stream_stats(LvReceiverHandle* handle, LV_StreamStats* out);

/* ---- Full-duplex 64x64 AoIP transceiver (desktop builds with the virtual device bridge) ----
 * Reads the DAW outputs from the LanVidAudioBus_v1 TX ring and streams them to peer_host:peer_port,
 * while receiving the peer's channels on listen_port, aligning them with the AudioSyncMatrix /
 * MasterClock, and writing them to the RX ring (DAW inputs). TX and RX run on separate threads. */
typedef struct LvDuplexHandle LvDuplexHandle;
LV_API void lv_duplex_config_default(LV_DuplexConfig* cfg);
LV_API LvDuplexHandle* lv_duplex_create(void);
LV_API void lv_duplex_destroy(LvDuplexHandle* handle);
LV_API int lv_duplex_start(LvDuplexHandle* handle, const LV_DuplexConfig* cfg);
LV_API void lv_duplex_stop(LvDuplexHandle* handle);
LV_API int lv_duplex_get_stats(LvDuplexHandle* handle, LV_DuplexStats* out);
LV_API int lv_duplex_set_latency_ms(LvDuplexHandle* handle, uint32_t latency_ms);
LV_API int lv_duplex_set_audio_codec(LvDuplexHandle* handle, int codec);
/* Live per-channel audio bitrate target for Duplex TX: 0 = Lossless, or 256 / 192 / 128 / 96 / 64 kbps/ch. */
LV_API int lv_duplex_set_audio_bitrate(LvDuplexHandle* handle, int kbps_per_ch);
/* Glitch-Free Wi-Fi Redundancy (1+1 Duplicate) for Duplex TX audio stream. */
LV_API int lv_duplex_set_wifi_redundancy(LvDuplexHandle* handle, int enabled, uint32_t stagger_us);
/* Physical NIC selection & Dual-NIC 1+1 failover for Duplex TX. */
LV_API int lv_duplex_set_network_interfaces(LvDuplexHandle* handle, const char* primary_ip,
                                            const char* secondary_ip);

/* 64x64 patchbay of the duplex bridge, live (5 ms crossfade). dst_of_src[s] = destination channel
 * (0..63) of source channel s, or -1 for unrouted; NULL resets to 1:1. */
LV_API int lv_audio_set_tx_routing(LvDuplexHandle* handle, const int8_t dst_of_src[64]);
LV_API int lv_audio_set_rx_routing(LvDuplexHandle* handle, const int8_t dst_of_src[64]);
LV_API int lv_audio_get_routing(LvDuplexHandle* handle, LV_AudioDirection dir, int8_t dst_of_src[64]);
/* Mute / Solo of a SOURCE channel (pre-routing) with a 5 ms gain ramp. mute/solo: 0/1, -1 = keep. */
LV_API int lv_audio_set_channel_mute_solo(LvDuplexHandle* handle, LV_AudioDirection dir, uint16_t channel,
                                          int mute, int solo);

/* Scans for AoIP nodes (duplex transceivers and receivers announcing the bus). Returns count >= 0. */
LV_API int lv_discovery_scan_audio_nodes(int timeout_ms, uint16_t discovery_port, LV_AudioNode* out_nodes,
                                         int max_nodes);

/* ---- MIDI2 over IP (same UDP stream as the audio, MasterClock-stamped, Note-Off redundancy) ----
 * media_ts_us = 0 stamps the event with the sender's MasterClock "now". On the receiver the callback
 * fires from inside lv_receiver_pop_audio_multichannel() for events within the returned block
 * (block_offset_frames = sample position), or from the network thread when no audio is pulled. */
LV_API int lv_sender_push_midi(LvSenderHandle* handle, uint8_t status, uint8_t data1, uint8_t data2, uint8_t port,
                               uint64_t media_ts_us);
LV_API void lv_receiver_set_midi_callback(LvReceiverHandle* handle, LV_MidiEventCallback cb, void* user_data);
/* Sender-restart handling and held notes on the receive side (any argument may be NULL). */
LV_API int lv_receiver_get_stream_epoch_info(LvReceiverHandle* handle, uint64_t* epoch_resets, uint32_t* active_midi_notes,
                                             uint64_t* midi_events_rx);
LV_API int lv_duplex_send_midi(LvDuplexHandle* handle, uint8_t status, uint8_t data1, uint8_t data2, uint8_t port,
                               uint64_t media_ts_us);
LV_API void lv_duplex_set_midi_callback(LvDuplexHandle* handle, LV_MidiEventCallback cb, void* user_data);

/* Sample-accurate MIDI De-Jitter Playout Queue telemetry (synchronized with audio latency settings). */
typedef struct LV_MidiDejitterStats {
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
} LV_MidiDejitterStats;

LV_API int lv_receiver_get_midi_dejitter_stats(LvReceiverHandle* handle, LV_MidiDejitterStats* out);
LV_API int lv_duplex_get_midi_dejitter_stats(LvDuplexHandle* handle, LV_MidiDejitterStats* out);

/* MIDI Only mode: 0 audio/video TX packets, ultra-low latency (~1.5 ms) MIDI playout. */
LV_API int lv_sender_set_midi_only(LvSenderHandle* handle, int enabled);
LV_API int lv_receiver_set_midi_only(LvReceiverHandle* handle, int enabled);
LV_API int lv_duplex_set_midi_only(LvDuplexHandle* handle, int enabled);

/* ---- Server-side Personal Smartphone Monitor Bus (64ch -> Stereo DSP + AUDIO2 2ch) ---- */
typedef struct LvMonitorServerHandle LvMonitorServerHandle;
typedef struct LvMonitorClientHandle LvMonitorClientHandle;

typedef struct LV_MonitorDspParams {
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
} LV_MonitorDspParams;

typedef struct LV_MonitorClientInfo {
    int32_t slot;
    uint32_t client_id;
    char name[32];
    char address[32];
    uint16_t bitrate_kbps;
    uint16_t latency_ms;
    int32_t muted;
    int32_t connected;
    uint64_t packets_tx;
} LV_MonitorClientInfo;

LV_API LvMonitorServerHandle* lv_monitor_server_create(uint32_t sample_rate);
LV_API void lv_monitor_server_destroy(LvMonitorServerHandle* srv);
LV_API int lv_monitor_server_start(LvMonitorServerHandle* srv, uint16_t bind_port);
LV_API void lv_monitor_server_stop(LvMonitorServerHandle* srv);
LV_API uint16_t lv_monitor_server_port(LvMonitorServerHandle* srv);
LV_API void lv_monitor_server_set_channel_name(LvMonitorServerHandle* srv, int ch, const char* name);
LV_API void lv_monitor_server_set_client_mute(LvMonitorServerHandle* srv, int slot, int muted);
LV_API int lv_monitor_server_get_clients(LvMonitorServerHandle* srv, LV_MonitorClientInfo* out, int max_clients);
LV_API void lv_monitor_server_push_audio_planar(LvMonitorServerHandle* srv, const float* const* planes,
                                                int num_channels, uint32_t frames);

LV_API LvMonitorClientHandle* lv_monitor_client_create(uint32_t sample_rate);
LV_API void lv_monitor_client_destroy(LvMonitorClientHandle* cli);
LV_API int lv_monitor_client_connect(LvMonitorClientHandle* cli, const char* server_ip, uint16_t server_port,
                                     const char* client_name, uint16_t bitrate_kbps, uint16_t latency_ms);
LV_API void lv_monitor_client_disconnect(LvMonitorClientHandle* cli);
LV_API int lv_monitor_client_request_meta(LvMonitorClientHandle* cli);
LV_API int lv_monitor_client_set_channel_mix(LvMonitorClientHandle* cli, int ch, float gain_db, float pan,
                                             int mute, int solo);
LV_API int lv_monitor_client_set_dsp(LvMonitorClientHandle* cli, const LV_MonitorDspParams* dsp);
LV_API void lv_monitor_client_poll(LvMonitorClientHandle* cli);
LV_API void lv_monitor_client_pop_stereo(LvMonitorClientHandle* cli, float* out_interleaved, uint32_t frames);

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

/* Tally control & callback */
LV_API int lv_studio_set_camera_tally(uint32_t stream_id, uint8_t tally_state, const char* camera_label,
                                      int rec_trigger);
LV_API int lv_tally_intercom_set_camera_tally(LvTallyIntercomHandle* studio, uint32_t stream_id,
                                              uint8_t tally_state, const char* camera_label, int rec_trigger);
LV_API void lv_camera_set_tally_callback(LvTallyIntercomHandle* handle, LV_TallyCallback cb, void* user_data);
LV_API void lv_tally_intercom_update_camera_telemetry(LvTallyIntercomHandle* handle, uint8_t battery_pct,
                                                      int8_t temp_c);

/* Remote camera control (LanVid Video -> phone): lens, focus, quality preset (additive). */
typedef struct LV_CameraControl {
    uint32_t camera_id;
    uint32_t revision;      /* monotonic; duplicates of the 3x redundant send are filtered */
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

/* Bidirectional Video Config Sync + Camera Number Assignment + Smart IDR Keyframe Request (additive). */
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

/* Duplex Intercom voice I/O (48 kHz mono float32, isolated from program audio) */
LV_API int lv_intercom_push_mic_pcm(LvTallyIntercomHandle* handle, const float* samples, int frames);
LV_API int lv_intercom_pull_rx_pcm(LvTallyIntercomHandle* handle, float* out_samples, int frames);
LV_API void lv_intercom_set_state(LvTallyIntercomHandle* handle, int talk_active, int listen_muted,
                                  float rx_gain, float sidetone_gain);
LV_API int lv_studio_set_camera_intercom(uint32_t camera_id, int talk_to_cam, int listen_from_cam, int all_call);
LV_API int lv_tally_intercom_set_camera_intercom(LvTallyIntercomHandle* studio, uint32_t camera_id,
                                                 int talk_to_cam, int listen_from_cam, int all_call);
LV_API int lv_tally_intercom_is_director_speaking(LvTallyIntercomHandle* handle);
LV_API float lv_tally_intercom_ducking_gain(LvTallyIntercomHandle* handle);

/* ---- Mobile monitor app helpers (additive) ---- */
/* bitrate_kbps for connect / set_stream: total stereo kbps (128..512 = 64..256 per channel, Opus CELT) or: */
#define LV_MONITOR_BITRATE_LOSSLESS 0xFFFF /* 24-bit PCM, ~2.3 Mbps stereo */

typedef struct LV_MonitorMetaChannel {
    char name[16];     /* channel name from LanVid Studio (null-terminated) */
    int8_t peak_dbfs;  /* live input VU, -120..0 dBFS */
    uint8_t reserved[3];
} LV_MonitorMetaChannel;

typedef struct LV_MonitorTelemetry {
    int32_t connected;
    int32_t num_channels;           /* active channels on the PC */
    float bus_gain_reduction_db;    /* this phone's compressor + limiter GR, <= 0 dB */
    float bus_peak_dbfs;            /* peak of the stereo mix the PC sends to this phone */
    double buffer_ms;               /* measured playout buffer on the phone */
    uint64_t audio_packets_rx;
    uint64_t meta_packets_rx;
} LV_MonitorTelemetry;

/* Copies the last channel list (names + VU) received from the PC; returns the channel count (may exceed
 * max_channels). `tel` (optional) receives bus telemetry. Call lv_monitor_client_request_meta() ~10-20x/s. */
LV_API int lv_monitor_client_get_meta(LvMonitorClientHandle* cli, LV_MonitorMetaChannel* out, int max_channels,
                                      LV_MonitorTelemetry* tel);
/* Live change of stream quality / buffer without reconnecting (also re-registers with the PC). */
LV_API int lv_monitor_client_set_stream(LvMonitorClientHandle* cli, uint16_t bitrate_kbps, uint16_t latency_ms);
/* Keepalive: the PC drops phones it has not heard from for 15 s. Call every 1-3 s. */
LV_API int lv_monitor_client_keepalive(LvMonitorClientHandle* cli);

/* ---- Dante-style Wi-Fi Redundancy, Rx Subscriptions & Telemetry (additive) ---- */
typedef struct LV_MonitorDanteTelemetry {
    int32_t  clock_locked;            /* 1 once min-delay clock offset filter locked, 0 otherwise */
    uint32_t clock_offset_jitter_us;  /* filtered one-way clock offset jitter (us) */
    uint64_t duplicates_recovered;    /* lost Wi-Fi primary packets rescued by +1.5 ms 1+1 copy (0 NACK) */
    uint32_t active_flow_channels;    /* active channels carried in latest kMonitorAudioFlow packet */
    double   effective_playout_ms;    /* actual deterministic playout deadline delay (ms) */
    int32_t  wifi_redundancy_enabled; /* 1 if 1+1 Wi-Fi Redundancy is active */
    uint64_t flow_packets_rx;         /* total unique kMonitorAudioFlow datagrams received */
    uint64_t nacks_sent;              /* total NACK retransmission requests sent */
} LV_MonitorDanteTelemetry;

/* Toggles per-client Dante 1+1 Wi-Fi Redundancy (+1.5 ms backup copy + deterministic playout) live. */
LV_API int lv_monitor_client_set_wifi_redundancy(LvMonitorClientHandle* cli, int enabled);
/* Sends Receiver-Driven Subscription bitmask (bit c = channel c active) in 15 ms LVKA keep-alive. */
LV_API int lv_monitor_client_set_subscribed_mask(LvMonitorClientHandle* cli, uint64_t channel_mask);
/* Returns Dante clock-lock, effective playout ms, and 1+1 recovery telemetry. */
LV_API int lv_monitor_client_get_dante_telemetry(LvMonitorClientHandle* cli, LV_MonitorDanteTelemetry* out);
/* Server-side controls for Dante multi-channel flows and global 1+1 Wi-Fi redundancy. */
LV_API int lv_monitor_server_set_wifi_redundancy(LvMonitorServerHandle* srv, int enabled, uint32_t stagger_us);
LV_API int lv_monitor_server_set_multichannel_flow(LvMonitorServerHandle* srv, int enabled, uint8_t flow_channels);
/* Physical NIC selection on mobile/desktop monitor client (e.g. Wi-Fi + USB-C Ethernet 1+1). */
LV_API int lv_monitor_client_set_network_interfaces(LvMonitorClientHandle* cli, const char* primary_ip,
                                                    const char* secondary_ip);

/* ---- Cross-Platform Network Interface Enumeration (Ethernet + Wi-Fi + USB-LAN) ---- */
typedef enum LV_NetInterfaceType {
    LV_NET_IFACE_OTHER        = 0,
    LV_NET_IFACE_ETHERNET     = 1,
    LV_NET_IFACE_WIFI         = 2,
    LV_NET_IFACE_USB_ETHERNET = 3,
    LV_NET_IFACE_VIRTUAL      = 4
} LV_NetInterfaceType;

typedef struct LvNetInterfaceInfo {
    char     id[128];
    char     system_name[64];
    char     friendly_name[128];
    uint8_t  type;               /* LV_NetInterfaceType */
    uint8_t  is_up;              /* 1 = active connected link with valid IPv4 */
    uint16_t reserved;
    char     ipv4_address[32];
    char     subnet_mask[32];
    char     mac_address[24];
    uint32_t link_speed_mbps;
    uint32_t if_index;
} LvNetInterfaceInfo;

/* Populates up to `max_count` connected Ethernet / Wi-Fi / USB-LAN interfaces. Returns total count found. */
LV_API int lv_net_list_interfaces(LvNetInterfaceInfo* out_list, int max_count);
LV_API int lv_net_list_interfaces_ex(LvNetInterfaceInfo* out_list, int max_count,
                                     int include_disconnected, int show_all);

/* ---- Real-Time Bidirectional Config Sync (PacketType::kControlSync) ---- */
typedef struct LV_ControlSyncState {
    uint64_t revision;           /* monotonic version (anti-loop / dedup) */
    uint32_t origin_id;          /* per-instance origin ID (echo guard) */
    uint16_t audio_channels;     /* 2 / 8 / 16 / 32 / 64 */
    uint16_t audio_bitrate_kbps; /* 0 = Lossless, 256 / 192 / 128 / 96 / 64 */
    uint16_t target_buffer_ms;   /* 2..2000 ms */
    uint8_t  latency_mode;       /* 0 = Fixed (Ethernet), 1 = Adaptive (Wi-Fi) */
    uint8_t  audio_only;         /* 0 = AV, 1 = Audio Only */
    int8_t   active_preset_id;   /* -1 = custom, 0/1/2/3 = preset */
    uint8_t  flags;              /* bit0 = mute_all/PANIC, bit1 = sync_matrix */
    int16_t  master_gain_cb;     /* 0.01 dB (-6000..+1200) */
    uint8_t  audio_codec;        /* 0..8 */
} LV_ControlSyncState;

typedef void (*LV_ControlSyncCallback)(const LV_ControlSyncState* state, void* user_data);

LV_API int lv_sender_send_control_sync(LvSenderHandle* handle, const LV_ControlSyncState* state);
LV_API void lv_sender_set_control_sync_callback(LvSenderHandle* handle, LV_ControlSyncCallback cb, void* user_data);
LV_API int lv_receiver_send_control_sync(LvReceiverHandle* handle, const LV_ControlSyncState* state);
LV_API void lv_receiver_set_control_sync_callback(LvReceiverHandle* handle, LV_ControlSyncCallback cb, void* user_data);
LV_API int lv_duplex_send_control_sync(LvDuplexHandle* handle, const LV_ControlSyncState* state);
LV_API void lv_duplex_set_control_sync_callback(LvDuplexHandle* handle, LV_ControlSyncCallback cb, void* user_data);

/* ---- Jitter-buffer latency mode (live, no stream restart; additive) ----
 * enabled = 0: Fixed  - playout delay held exactly at the bus latency (wired Ethernet, live monitoring).
 * enabled = 1: Adaptive Wi-Fi (default) - grows within [latency .. 200 ms] on jitter bursts (<= 0.5 %
 *              time-stretch) and returns through +/-0.05 % micro-resampling. Works before or after start. */
LV_API int lv_receiver_set_adaptive_jitter(LvReceiverHandle* handle, int enabled);
/* Measured jitter-buffer playout delay in ms right now (0 when not running / not yet playing). */
LV_API double lv_receiver_get_effective_latency_ms(LvReceiverHandle* handle);
LV_API int lv_duplex_set_adaptive_jitter(LvDuplexHandle* handle, int enabled);

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
    int32_t vcam_placeholder;  /* lavender "No Signal / Standby" card when a camera is gone */
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
    char output_dir[512];      /* "" = ~/Movies/LanVid, %USERPROFILE%\Videos\LanVid, ~/Videos/LanVid */
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
LV_API int lv_multicam_start(LvMultiCamHandle* mc, const LV_MultiCamConfig* cfg); /* LV_OK or < 0 */
LV_API void lv_multicam_stop(LvMultiCamHandle* mc);
LV_API uint32_t lv_multicam_slot_count(LvMultiCamHandle* mc);
LV_API int lv_multicam_get_slot_info(LvMultiCamHandle* mc, uint32_t slot, LV_MultiCamSlotInfo* out);
LV_API int lv_multicam_set_slot_label(LvMultiCamHandle* mc, uint32_t slot, const char* label);
LV_API int lv_multicam_pin_slot(LvMultiCamHandle* mc, uint32_t slot, const char* ip); /* "" clears */
LV_API void lv_multicam_release_slot(LvMultiCamHandle* mc, uint32_t slot);
/* Latest decoded frame as BGRA (width*4 stride). Returns 1 = new frame copied, 0 = nothing new since *seq,
 * LV_ERR_BUFFER_TOO_SMALL (width/height filled) when `cap` is too small. */
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
