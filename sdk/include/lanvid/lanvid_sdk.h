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
/* Live audio compression profile: 0=PCM32F, 1=PCM24 (-25%), 2=PCM16 (-50%), 3=Opus/ADPCM (-85%). */
LV_API int lv_sender_set_audio_codec(LvSenderHandle* handle, int codec);
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

/* ---- Jitter-buffer latency mode (live, no stream restart; additive) ----
 * enabled = 0: Fixed  - playout delay held exactly at the bus latency (wired Ethernet, live monitoring).
 * enabled = 1: Adaptive Wi-Fi (default) - grows within [latency .. 200 ms] on jitter bursts (<= 0.5 %
 *              time-stretch) and returns through +/-0.05 % micro-resampling. Works before or after start. */
LV_API int lv_receiver_set_adaptive_jitter(LvReceiverHandle* handle, int enabled);
/* Measured jitter-buffer playout delay in ms right now (0 when not running / not yet playing). */
LV_API double lv_receiver_get_effective_latency_ms(LvReceiverHandle* handle);
LV_API int lv_duplex_set_adaptive_jitter(LvDuplexHandle* handle, int enabled);

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
