#pragma once
/*
 * CrossLink Audio SDK — Main Umbrella Header (v1.0.0)
 *
 * Self-contained C ABI for ultra-low-latency 64-channel Audio-over-IP (AoIP),
 * MasterClock synchronization, Full-Duplex 64x64 DAW Shared-Memory Audio Bus,
 * Channel Patchbay, MIDI2, ControlSync, Personal Monitor Bus, and L2 Grid audio.
 *
 * For low-latency video + 2-channel stereo audio streaming, use the LanVid Video SDK
 * (<lanvid/lanvid_sdk.h>).
 */

#include "crosslink_export.h"
#include "crosslink_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClSenderHandle ClSenderHandle;
typedef struct ClReceiverHandle ClReceiverHandle;
typedef struct ClDuplexHandle ClDuplexHandle;
typedef struct ClMonitorServerHandle ClMonitorServerHandle;
typedef struct ClMonitorClientHandle ClMonitorClientHandle;

/* ---- Callback Prototypes ---- */
typedef void (*CL_MultichannelAudioCallback)(const CL_MultichannelAudioBlock* block, void* user_data);
typedef void (*CL_MidiEventCallback)(const CL_MidiEvent* ev, void* user_data);
typedef void (*CL_ControlSyncCallback)(const CL_ControlSyncState* state, void* user_data);
typedef void (*CL_StateChangedCallback)(int new_state, int error_code, const char* message,
                                        void* user_data);

/* ---- Core Lifecycle & Introspection ---- */
CL_API int cl_init(void);
CL_API void cl_shutdown(void);
CL_API void cl_free_string(char* str);
CL_API const char* cl_get_cpu_simd_info(void);
CL_API void cl_set_simd_enabled(int enabled);

/* ---- Default Configuration Initializers ---- */
CL_API void cl_audio_bus_config_default(CL_AudioBusConfig* cfg);
CL_API void cl_sender_config_default(CL_SenderConfig* cfg);
CL_API void cl_receiver_config_default(CL_ReceiverConfig* cfg);
CL_API void cl_duplex_config_default(CL_DuplexConfig* cfg);

/* ---- 64-Channel AoIP Sender API ---- */
CL_API ClSenderHandle* cl_sender_create(void);
CL_API void cl_sender_destroy(ClSenderHandle* handle);
CL_API int cl_sender_configure_audio_bus(ClSenderHandle* handle, const CL_AudioBusConfig* cfg);
CL_API int cl_sender_start(ClSenderHandle* handle, const CL_SenderConfig* cfg);
CL_API void cl_sender_stop(ClSenderHandle* handle);
CL_API uint16_t cl_sender_get_local_port(ClSenderHandle* handle);

CL_API int cl_sender_push_audio_channel(ClSenderHandle* handle, uint16_t channel_index,
                                        const void* samples, uint32_t num_samples,
                                        CL_AudioFormat format, uint64_t media_timestamp_samples);
CL_API int cl_sender_push_audio_multichannel(ClSenderHandle* handle, const void* data,
                                             uint16_t num_channels, uint32_t num_samples,
                                             CL_AudioFormat format, CL_AudioLayout layout,
                                             uint64_t media_timestamp_samples);
CL_API int cl_sender_set_audio_latency_ms(ClSenderHandle* handle, uint32_t latency_ms);
CL_API int cl_sender_set_audio_codec(ClSenderHandle* handle, int codec);
CL_API int cl_sender_set_audio_bitrate(ClSenderHandle* handle, int kbps_per_ch);
CL_API int cl_sender_set_wifi_redundancy(ClSenderHandle* handle, int enabled, uint32_t stagger_us);
CL_API int cl_sender_set_network_interfaces(ClSenderHandle* handle, const char* primary_ip,
                                            const char* secondary_ip);
CL_API int cl_sender_get_failover_stats(ClSenderHandle* handle, uint64_t* out_failover_packets,
                                        uint64_t* out_secondary_bytes);
CL_API uint64_t cl_sender_get_master_clock_samples(ClSenderHandle* handle);
CL_API int cl_sender_get_stream_stats(ClSenderHandle* handle, CL_StreamStats* out);

/* ---- 64-Channel AoIP Receiver API ---- */
CL_API ClReceiverHandle* cl_receiver_create(void);
CL_API void cl_receiver_destroy(ClReceiverHandle* handle);
CL_API int cl_receiver_configure_audio_bus(ClReceiverHandle* handle, const CL_AudioBusConfig* cfg);
CL_API void cl_receiver_set_state_callback(ClReceiverHandle* handle, CL_StateChangedCallback cb,
                                           void* user_data);
CL_API int cl_receiver_start(ClReceiverHandle* handle, const CL_ReceiverConfig* cfg);
CL_API void cl_receiver_stop(ClReceiverHandle* handle);
CL_API uint16_t cl_receiver_get_local_port(ClReceiverHandle* handle);

CL_API int cl_receiver_set_audio_latency_ms(ClReceiverHandle* handle, uint32_t latency_ms);
CL_API int cl_receiver_set_adaptive_jitter(ClReceiverHandle* handle, int enabled);
CL_API double cl_receiver_get_effective_latency_ms(ClReceiverHandle* handle);
CL_API int cl_receiver_set_multichannel_audio_callback(ClReceiverHandle* handle,
                                                       CL_MultichannelAudioCallback cb,
                                                       void* user_data);
CL_API int cl_receiver_pop_audio_multichannel(ClReceiverHandle* handle, float* out,
                                              uint16_t num_channels, uint32_t num_frames,
                                              CL_AudioLayout layout);
CL_API int cl_receiver_get_channel_stats(ClReceiverHandle* handle, uint16_t channel_index,
                                         CL_AudioChannelStats* out);
CL_API int cl_receiver_get_stream_stats(ClReceiverHandle* handle, CL_StreamStats* out);

/* ---- Full-Duplex 64x64 AoIP Transceiver (CrossLinkAudioBus_v1 SHM + Patchbay) ---- */
CL_API ClDuplexHandle* cl_duplex_create(void);
CL_API void cl_duplex_destroy(ClDuplexHandle* handle);
CL_API int cl_duplex_start(ClDuplexHandle* handle, const CL_DuplexConfig* cfg);
CL_API void cl_duplex_stop(ClDuplexHandle* handle);
CL_API int cl_duplex_get_stats(ClDuplexHandle* handle, CL_DuplexStats* out);
CL_API int cl_duplex_set_latency_ms(ClDuplexHandle* handle, uint32_t latency_ms);
CL_API int cl_duplex_set_adaptive_jitter(ClDuplexHandle* handle, int enabled);
CL_API int cl_duplex_set_audio_codec(ClDuplexHandle* handle, int codec);
CL_API int cl_duplex_set_audio_bitrate(ClDuplexHandle* handle, int kbps_per_ch);
CL_API int cl_duplex_set_wifi_redundancy(ClDuplexHandle* handle, int enabled, uint32_t stagger_us);
CL_API int cl_duplex_set_network_interfaces(ClDuplexHandle* handle, const char* primary_ip,
                                            const char* secondary_ip);

CL_API int cl_audio_set_tx_routing(ClDuplexHandle* handle, const int8_t dst_of_src[64]);
CL_API int cl_audio_set_rx_routing(ClDuplexHandle* handle, const int8_t dst_of_src[64]);
CL_API int cl_audio_get_routing(ClDuplexHandle* handle, CL_AudioDirection dir, int8_t dst_of_src[64]);
CL_API int cl_audio_set_channel_mute_solo(ClDuplexHandle* handle, CL_AudioDirection dir, uint16_t channel,
                                          int mute, int solo);

/* ---- MIDI2 over IP ---- */
CL_API int cl_sender_push_midi(ClSenderHandle* handle, uint8_t status, uint8_t data1, uint8_t data2,
                               uint8_t port, uint64_t media_ts_us);
CL_API void cl_receiver_set_midi_callback(ClReceiverHandle* handle, CL_MidiEventCallback cb, void* user_data);
CL_API int cl_receiver_get_stream_epoch_info(ClReceiverHandle* handle, uint64_t* epoch_resets,
                                             uint32_t* active_midi_notes, uint64_t* midi_events_rx);
CL_API int cl_duplex_send_midi(ClDuplexHandle* handle, uint8_t status, uint8_t data1, uint8_t data2,
                               uint8_t port, uint64_t media_ts_us);
CL_API void cl_duplex_set_midi_callback(ClDuplexHandle* handle, CL_MidiEventCallback cb, void* user_data);

CL_API int cl_receiver_get_midi_dejitter_stats(ClReceiverHandle* handle, CL_MidiDejitterStats* out);
CL_API int cl_duplex_get_midi_dejitter_stats(ClDuplexHandle* handle, CL_MidiDejitterStats* out);
CL_API int cl_sender_set_midi_only(ClSenderHandle* handle, int enabled);
CL_API int cl_receiver_set_midi_only(ClReceiverHandle* handle, int enabled);
CL_API int cl_duplex_set_midi_only(ClDuplexHandle* handle, int enabled);

/* ---- ControlSync ---- */
CL_API int cl_sender_send_control_sync(ClSenderHandle* handle, const CL_ControlSyncState* state);
CL_API void cl_sender_set_control_sync_callback(ClSenderHandle* handle, CL_ControlSyncCallback cb,
                                                void* user_data);
CL_API int cl_receiver_send_control_sync(ClReceiverHandle* handle, const CL_ControlSyncState* state);
CL_API void cl_receiver_set_control_sync_callback(ClReceiverHandle* handle, CL_ControlSyncCallback cb,
                                                  void* user_data);
CL_API int cl_duplex_send_control_sync(ClDuplexHandle* handle, const CL_ControlSyncState* state);
CL_API void cl_duplex_set_control_sync_callback(ClDuplexHandle* handle, CL_ControlSyncCallback cb,
                                                void* user_data);

/* ---- Personal Monitor Bus Server & Client ---- */
CL_API ClMonitorServerHandle* cl_monitor_server_create(uint32_t sample_rate);
CL_API void cl_monitor_server_destroy(ClMonitorServerHandle* srv);
CL_API int cl_monitor_server_start(ClMonitorServerHandle* srv, uint16_t bind_port);
CL_API void cl_monitor_server_stop(ClMonitorServerHandle* srv);
CL_API uint16_t cl_monitor_server_port(ClMonitorServerHandle* srv);
CL_API void cl_monitor_server_set_channel_name(ClMonitorServerHandle* srv, int ch, const char* name);
CL_API void cl_monitor_server_set_client_mute(ClMonitorServerHandle* srv, int slot, int muted);
CL_API int cl_monitor_server_get_clients(ClMonitorServerHandle* srv, CL_MonitorClientInfo* out,
                                         int max_clients);
CL_API void cl_monitor_server_push_audio_planar(ClMonitorServerHandle* srv, const float* const* planes,
                                                int num_channels, uint32_t frames);
CL_API int cl_monitor_server_set_wifi_redundancy(ClMonitorServerHandle* srv, int enabled, uint32_t stagger_us);
CL_API int cl_monitor_server_set_multichannel_flow(ClMonitorServerHandle* srv, int enabled,
                                                   uint8_t flow_channels);

CL_API ClMonitorClientHandle* cl_monitor_client_create(uint32_t sample_rate);
CL_API void cl_monitor_client_destroy(ClMonitorClientHandle* cli);
CL_API int cl_monitor_client_connect(ClMonitorClientHandle* cli, const char* server_ip, uint16_t server_port,
                                     const char* client_name, uint16_t bitrate_kbps, uint16_t latency_ms);
CL_API void cl_monitor_client_disconnect(ClMonitorClientHandle* cli);
CL_API int cl_monitor_client_request_meta(ClMonitorClientHandle* cli);
CL_API int cl_monitor_client_set_channel_mix(ClMonitorClientHandle* cli, int ch, float gain_db, float pan,
                                             int mute, int solo);
CL_API int cl_monitor_client_set_dsp(ClMonitorClientHandle* cli, const CL_MonitorDspParams* dsp);
CL_API void cl_monitor_client_poll(ClMonitorClientHandle* cli);
CL_API void cl_monitor_client_pop_stereo(ClMonitorClientHandle* cli, float* out_interleaved, uint32_t frames);
CL_API int cl_monitor_client_get_meta(ClMonitorClientHandle* cli, CL_MonitorMetaChannel* out, int max_channels,
                                      CL_MonitorTelemetry* tel);
CL_API int cl_monitor_client_set_stream(ClMonitorClientHandle* cli, uint16_t bitrate_kbps, uint16_t latency_ms);
CL_API int cl_monitor_client_keepalive(ClMonitorClientHandle* cli);
CL_API int cl_monitor_client_set_wifi_redundancy(ClMonitorClientHandle* cli, int enabled);
CL_API int cl_monitor_client_set_subscribed_mask(ClMonitorClientHandle* cli, uint64_t channel_mask);
CL_API int cl_monitor_client_get_flow_telemetry(ClMonitorClientHandle* cli, CL_MonitorFlowTelemetry* out);
CL_API int cl_monitor_client_set_network_interfaces(ClMonitorClientHandle* cli, const char* primary_ip,
                                                    const char* secondary_ip);

/* ---- Network Interface Enumeration & Audio Node Discovery ---- */
CL_API int cl_net_list_interfaces(ClNetInterfaceInfo* out_list, int max_count);
CL_API int cl_net_list_interfaces_ex(ClNetInterfaceInfo* out_list, int max_count,
                                     int include_disconnected, int show_all);
CL_API int cl_discovery_scan_audio_nodes(int timeout_ms, uint16_t discovery_port, CL_AudioNode* out_nodes,
                                         int max_nodes);
CL_API char* cl_discovery_scan_json(int timeout_ms, uint16_t discovery_port);

#ifdef __cplusplus
}
#endif
