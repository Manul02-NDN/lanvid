#pragma once
// CrossLink Audio Protocol v1 ("CL"):
// 24-byte audio-native binary header (little-endian, CRC32C protected) + payloads for:
//   - 64-channel sample-addressed AoIP audio (PCM16/PCM24/F32/IMA-ADPCM/Opus CELT + XOR FEC + NACK)
//   - 64-bit MasterClock PING/PONG synchronization
//   - MIDI2 sample-accurate transport with 16-event redundancy ring
//   - Real-time bidirectional Control Sync
//   - Personal Monitor Bus control, 15 ms CLKA keep-alive, and 16-channel Monitor Audio Flow

#include <cstddef>
#include <cstdint>

namespace cl {

constexpr uint8_t kMagic0 = 0x43;  // 'C'
constexpr uint8_t kMagic1 = 0x4C;  // 'L'
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kPacketHeaderSize = 24;
constexpr size_t kDefaultMtu = 1400;
constexpr size_t kMaxDatagram = 65507;

enum class PacketType : uint8_t {
    kAudioFrame       = 0x1,  // Per-channel AoIP stream (up to 64 ch, master-clock aligned)
    kAudioNack        = 0x2,  // Receiver -> Sender selective ARQ retransmission request
    kPing             = 0x3,  // Clock sync ping (client -> server)
    kPong             = 0x4,  // Clock sync pong + 64-bit MasterClock t2/t3 tail
    kMidiFrame        = 0x5,  // MIDI2 sample-accurate batch + 16-event redundancy ring
    kControlSync      = 0x6,  // Bidirectional audio config/control sync (3x redundant)
    kMonitorControl   = 0x7,  // Personal monitor mixer control & channel metadata/VU sync
    kMonitorAudioFlow = 0x8,  // Compact multi-channel monitor UDP flow (up to 16 ch in 1 packet)
};

enum PacketFlags : uint8_t {
    kFlagHasCrc = 0x01,
};

// 24-byte little-endian CrossLink v1 packet header:
//   0 magic[2] = {'C','L'}
//   2 ver_type = (version << 4) | (type & 0x0F)
//   3 flags
//   4 seq_id (u32)         - per-stream sequence / ping seq / flow_seq / control rev_lo
//   8 timestamp_us (u32)   - sender steady_clock wallclock_us
//  12 stream_id (u16)      - AoIP stream_id (0..63) or 0
//  14 stream_epoch (u16)   - non-zero random sender session epoch (for restart detection)
//  16 payload_size (u16)   - payload bytes following this header
//  18 seq16 (u16)          - low 16 bits of sequence
//  20 crc32c (u32)         - CRC32C over header[0..19] + payload
struct PacketHeader {
    uint8_t version = kProtocolVersion;
    PacketType type = PacketType::kAudioFrame;
    uint8_t flags = kFlagHasCrc;
    uint32_t seq_id = 0;
    uint32_t timestamp_us = 0;
    uint16_t stream_id = 0;
    uint16_t stream_epoch = 0;
    uint16_t payload_size = 0;
    uint16_t seq16 = 0;
    uint32_t crc32c = 0;
};

enum class PacketParseStatus {
    kOk,
    kTooShort,
    kBadMagic,
    kBadVersion,
    kBadLength,
    kBadCrc,
    kInvalidFields,
};

size_t write_packet(uint8_t* out, size_t cap, const PacketHeader& h, const uint8_t* payload,
                    size_t payload_len);

PacketParseStatus parse_packet(const uint8_t* dgram, size_t len, PacketHeader& h_out,
                               const uint8_t** payload_out = nullptr);

// RFC 1982 wrap-safe sequence arithmetic.
inline int32_t seq32_diff(uint32_t a, uint32_t b) { return int32_t(a - b); }
inline int16_t seq16_diff(uint16_t a, uint16_t b) { return int16_t(a - b); }

// ---------------------------------------------------------------------------------------------
// 64-Channel AoIP Audio (PacketType::kAudioFrame & kAudioNack)
// ---------------------------------------------------------------------------------------------
constexpr size_t kAudioHeader2Size = 24;
constexpr size_t kAudioFecInfoSize = 4;
constexpr size_t kAudioNackSize = 16;
constexpr uint16_t kMaxAudioChannels = 64;

enum class AudioFormat2 : uint8_t {
    kPcm16       = 1,  // 16-bit signed LE PCM
    kPcm24Packed = 2,  // 24-bit signed LE packed (3 bytes/sample)
    kFloat32     = 3,  // 32-bit IEEE float LE
    kAdpcm4      = 4,  // 4-bit IMA-ADPCM @ 48 kHz (192 kbps/ch)
    kAdpcm5      = 5,  // 5-bit IMA-ADPCM @ 48 kHz (256 kbps/ch)
    kAdpcm128    = 6,  // 2:1 decimated 5-bit IMA-ADPCM @ 24 kHz (128 kbps/ch)
    kAdpcm96     = 7,  // 2:1 decimated 4-bit IMA-ADPCM @ 24 kHz (96 kbps/ch)
    kAdpcm64     = 8,  // 3:1 decimated 4-bit IMA-ADPCM @ 16 kHz (64 kbps/ch)
    kOpus        = 9,  // libopus CELT 2.5 ms fullband (20 Hz - 20 kHz)
};

enum AudioFlags2 : uint8_t {
    kAudioFlagFecParity  = 0x01,  // payload = AudioFecInfo + XOR of member payloads
    kAudioFlagRetransmit = 0x02,  // answer to kAudioNack
    kAudioFlagDuplicate  = 0x04,  // proactive 1+1 Wi-Fi redundant copy
};

inline size_t audio2_bytes_per_sample(AudioFormat2 f) {
    switch (f) {
        case AudioFormat2::kPcm16:       return 2;
        case AudioFormat2::kPcm24Packed: return 3;
        case AudioFormat2::kFloat32:     return 4;
        default:                         return 1;
    }
}

inline size_t audio2_payload_bytes(AudioFormat2 f, uint32_t num_samples) {
    if (num_samples == 0) return 0;
    switch (f) {
        case AudioFormat2::kPcm16:       return size_t(num_samples) * 2;
        case AudioFormat2::kPcm24Packed: return size_t(num_samples) * 3;
        case AudioFormat2::kFloat32:     return size_t(num_samples) * 4;
        case AudioFormat2::kAdpcm4:      return 4u + (num_samples > 1 ? (size_t(num_samples - 1) + 1u) / 2u : 0u);
        case AudioFormat2::kAdpcm5:      return 4u + (num_samples > 1 ? (size_t(num_samples - 1) * 5u + 7u) / 8u : 0u);
        case AudioFormat2::kAdpcm128: {
            const uint32_t m = (num_samples + 1u) / 2u;
            return 4u + (m > 1 ? (size_t(m - 1) * 5u + 7u) / 8u : 0u);
        }
        case AudioFormat2::kAdpcm96: {
            const uint32_t m = (num_samples + 1u) / 2u;
            return 4u + (m > 1 ? (size_t(m - 1) + 1u) / 2u : 0u);
        }
        case AudioFormat2::kAdpcm64: {
            const uint32_t m = (num_samples + 2u) / 3u;
            return 4u + (m > 1 ? (size_t(m - 1) + 1u) / 2u : 0u);
        }
        case AudioFormat2::kOpus:
            return 0;  // variable-length Opus frame
    }
    return size_t(num_samples) * 4;
}

// 24-byte audio frame sub-header (little-endian):
//  0 stream_id u16 | 2 channel_index u16 | 4 sample_rate u32 | 8 num_samples u16 |
// 10 sample_format u8 | 11 flags u8 | 12 media_timestamp_samples u64 | 20 wallclock_us u32
struct AudioHeader2 {
    uint16_t stream_id = 0;
    uint16_t channel_index = 0;
    uint32_t sample_rate = 48000;
    uint16_t num_samples = 0;
    AudioFormat2 sample_format = AudioFormat2::kPcm16;
    uint8_t flags = 0;
    uint64_t media_timestamp_samples = 0;
    uint32_t wallclock_us = 0;
};

void write_audio_header2(uint8_t* out, const AudioHeader2& a);
bool parse_audio_header2(const uint8_t* in, size_t len, AudioHeader2& a);

struct AudioFecInfo {
    uint8_t group_size = 1;
    uint8_t interleave = 1;
    uint16_t reserved = 0;
};
void write_audio_fec_info(uint8_t* out, const AudioFecInfo& f);
bool parse_audio_fec_info(const uint8_t* in, size_t len, AudioFecInfo& f);

struct AudioNack {
    uint16_t stream_id = 0;
    uint32_t base_seq = 0;
    uint64_t mask = 0;
};
void write_audio_nack(uint8_t* out, const AudioNack& n);
bool parse_audio_nack(const uint8_t* in, size_t len, AudioNack& n);

// ---------------------------------------------------------------------------------------------
// MasterClock PING / PONG Payloads (12-byte base + 16-byte 64-bit MasterClock tail)
// ---------------------------------------------------------------------------------------------
constexpr size_t kPingPongSize = 12;
constexpr size_t kPongMasterTailSize = 16;

struct PingPongPayload {
    uint32_t t1_client_send_us = 0;
    uint32_t t2_server_recv_us = 0;
    uint32_t t3_server_send_us = 0;
};

void write_ping_pong(uint8_t* out, const PingPongPayload& p);
bool parse_ping_pong(const uint8_t* in, size_t len, PingPongPayload& p);
void write_pong_master_tail(uint8_t* out, uint64_t t2_master_us, uint64_t t3_master_us);
bool parse_pong_master_tail(const uint8_t* in, size_t len, uint64_t& t2_master_us,
                            uint64_t& t3_master_us);

// ---------------------------------------------------------------------------------------------
// MIDI2 over IP (PacketType::kMidiFrame)
// ---------------------------------------------------------------------------------------------
constexpr size_t kMidiFrameHeaderSize = 20;
constexpr size_t kMidiEventPacketSize = 12;
constexpr uint8_t kMaxMidiPorts = 16;
constexpr uint16_t kMaxMidiEventsPerPacket = 64;

enum MidiFlags2 : uint16_t {
    kMidiFlagRedundant = 0x0001,
    kMidiFlagHeartbeat = 0x0002,
};

struct MidiFrameHeader {
    uint32_t stream_id = 0;
    uint32_t batch_seq = 0;
    uint64_t base_media_ts_us = 0;
    uint16_t event_count = 0;
    uint16_t flags = 0;
};

struct MidiEventPacket {
    uint32_t event_seq = 0;
    int32_t offset_us = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint8_t port_index = 0;
};

size_t write_midi_frame(uint8_t* out, size_t cap, const MidiFrameHeader& hdr,
                        const MidiEventPacket* events, uint16_t count);
bool parse_midi_frame(const uint8_t* in, size_t len, MidiFrameHeader& hdr,
                      MidiEventPacket* out_events, uint16_t max_events);

// ---------------------------------------------------------------------------------------------
// Real-Time Bidirectional Audio Control Sync (PacketType::kControlSync, 32-byte payload)
// ---------------------------------------------------------------------------------------------
constexpr size_t kControlSyncSize = 32;

enum ControlSyncFlags : uint8_t {
    kControlSyncMuteAll = 0x01,  // PANIC / Mute All
    kControlSyncMatrix  = 0x02,  // Sync matrix preset / routing
    kControlSyncAck     = 0x04,  // Reserved ACK flag
};

struct ControlSyncPayload {
    uint64_t revision = 0;
    uint32_t origin_id = 0;
    uint16_t audio_channels = 8;      // 2 / 8 / 16 / 32 / 64
    uint16_t audio_bitrate_kbps = 0;  // 0=Lossless, 256 / 192 / 128 / 96 / 64
    uint16_t target_buffer_ms = 20;   // 2..2000 ms
    uint8_t  latency_mode = 1;        // 0=Fixed (Ethernet), 1=Adaptive (Wi-Fi)
    uint8_t  midi_only = 0;           // 0=Audio+MIDI, 1=MIDI Only
    int8_t   active_preset_id = -1;
    uint8_t  flags = 0;               // ControlSyncFlags
    int16_t  master_gain_cb = 0;      // 0.01 dB (-6000..+1200)
    uint8_t  audio_codec = 0;         // 0..8
};

void write_control_sync(uint8_t* out, const ControlSyncPayload& cs);
bool parse_control_sync(const uint8_t* in, size_t len, ControlSyncPayload& cs);

// ---------------------------------------------------------------------------------------------
// Personal Monitor Bus Control, CLKA Keep-Alive & 16-Channel Audio Flow
// ---------------------------------------------------------------------------------------------
enum class MonitorControlOp : uint8_t {
    kHello         = 1,
    kSyncMetaReq   = 2,
    kSyncMetaResp  = 3,
    kSetChannelMix = 4,
    kSetDsp        = 5,
    kDisconnect    = 6,
};

constexpr size_t kMonitorControlSize = 64;
constexpr uint16_t kMonitorBitrateLossless = 0xFFFF;
constexpr uint8_t kMonitorCapFlowV1 = 0x01;
constexpr uint8_t kMonitorCapRxSubMask = 0x02;
constexpr uint8_t kMonitorCapRedundancy1Plus1 = 0x04;

struct MonitorControlPayload {
    MonitorControlOp op = MonitorControlOp::kHello;
    uint8_t channel_index = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    uint32_t client_id = 0;
    uint16_t bitrate_kbps = 256;
    uint16_t target_latency_ms = 20;

    float gain_db = 0.0f;
    float pan = 0.0f;

    float hpf_hz = 80.0f;
    float eq_low_db = 0.0f;
    float eq_mid_hz = 1200.0f;
    float eq_mid_db = 0.0f;
    float eq_high_db = 0.0f;
    float comp_thresh_db = -18.0f;
    float comp_ratio = 3.0f;
    float comp_attack_ms = 5.0f;
    float comp_release_ms = 80.0f;
    float comp_makeup_db = 0.0f;
    float master_volume_db = 0.0f;

    char client_name[16] = {};
    uint64_t active_channel_mask = ~0ull;
};

// 16-byte compact keep-alive + Rx Subscription datagram ("CLKA" + client_id u32 + active_channel_mask u64)
constexpr size_t kMonitorKeepAliveSize = 16;
struct MonitorKeepAlivePayload {
    uint32_t client_id = 0;
    uint64_t active_channel_mask = ~0ull;
};
size_t write_monitor_keepalive(uint8_t* out, size_t cap, const MonitorKeepAlivePayload& ka);
bool parse_monitor_keepalive(const uint8_t* in, size_t len, MonitorKeepAlivePayload& ka);

// Multi-Channel Monitor Audio Flow (PacketType::kMonitorAudioFlow = 0x8)
constexpr size_t kMonitorAudioFlowHeaderSize = 24;
constexpr uint8_t kMaxMonitorFlowChannels = 16;
constexpr uint8_t kMonitorFlowFlagVarLen = 0x01;
constexpr uint8_t kFlagRedundantCopy = kAudioFlagDuplicate;

struct MonitorAudioFlowHeader {
    uint32_t flow_seq = 0;
    uint64_t media_timestamp_samples = 0;
    uint32_t sample_rate = 48000;
    uint16_t num_samples = 240;
    uint16_t channel_mask = 0x0003;
    AudioFormat2 sample_format = AudioFormat2::kPcm16;
    uint8_t flags = 0;
    uint8_t channel_count = 0;
    uint8_t reserved = 0;
};

size_t write_monitor_audio_flow(uint8_t* out, size_t cap, const MonitorAudioFlowHeader& fh,
                                const uint8_t* const* ch_payloads, const uint16_t* ch_lengths);
bool parse_monitor_audio_flow(const uint8_t* in, size_t len, MonitorAudioFlowHeader& fh,
                              const uint8_t* out_ch_payloads[kMaxMonitorFlowChannels],
                              uint16_t out_ch_lengths[kMaxMonitorFlowChannels]);

void write_monitor_control(uint8_t* out, const MonitorControlPayload& mc);
bool parse_monitor_control(const uint8_t* in, size_t len, MonitorControlPayload& mc);

struct MonitorMetaChannel {
    char name[16] = {};
    int8_t peak_dbfs = -120;
};

struct MonitorBusTelemetry {
    float gain_reduction_db = 0.0f;
    int8_t mix_peak_dbfs = -120;
};

size_t write_monitor_meta(uint8_t* out, size_t cap, uint32_t client_id, uint8_t num_channels,
                          const MonitorMetaChannel* channels, const MonitorBusTelemetry* bus = nullptr);
bool parse_monitor_meta(const uint8_t* in, size_t len, uint32_t& client_id, uint8_t& num_channels,
                        MonitorMetaChannel* out_channels, uint8_t max_channels,
                        MonitorBusTelemetry* bus = nullptr);

}  // namespace cl
