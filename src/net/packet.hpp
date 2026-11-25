#pragma once
// LVSP v1 binary packet header (28 bytes, little-endian) + XOR FEC, Audio, and Feedback payloads.

#include <cstddef>
#include <cstdint>

namespace lv {

constexpr uint8_t kMagic0 = 0x4C;  // 'L'
constexpr uint8_t kMagic1 = 0x56;  // 'V'
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kPacketHeaderSize = 28;
constexpr size_t kFecHeaderSize = 8;
constexpr size_t kAudioHeaderSize = 8;
constexpr size_t kFeedbackSize = 16;
constexpr size_t kDefaultMtu = 1400;
constexpr size_t kMaxDatagram = 65507;

enum class PacketType : uint8_t {
    kVideoSlice = 0x1,
    kPing = 0x2,
    kAudio = 0x3,
    kFeedback = 0x4,
    kRxReport = 0x4,  // alias
    kPong = 0x5,
    kAudio2 = 0x6,     // AudioHeader v2: independent per-channel stream (up to 64), media-clock timestamps
    kAudioNack = 0x7,  // Receiver -> Sender selective retransmission request (bitmask of missing seq)
    kMidiFrame = 0x8,      // MIDI2: sample-accurate MIDI batch synced to MasterClock + 16-event redundancy ring
    kControlSync = 0x9,    // Real-time bidirectional control/config sync (revision-guarded, 3x Wi-Fi redundant)
    kMonitorControl = 0xA, // Personal smartphone monitor mixer remote control & channel metadata/VU sync
    kTallyState = 0xB,     // Studio <-> Camera Tally (Standby / Preview / Program ON AIR) + REC trigger
    kIntercomAudio = 0xC,  // Isolated full-duplex Director <-> Camera Operator voice talkback / IFB
    kMonitorAudioFlow = 0xD, // Dante-style compact multi-channel monitor UDP flow (up to 16 ch in 1 packet)
};

enum class VideoCodecId : uint8_t {
    kLanVidDct = 0,  // Intra-only slice-parallel AVX2/NEON DCT/Huffman
    kH264      = 1,  // H.264 / AVC Annex-B zero-latency (no B-frames)
    kH265      = 2,  // H.265 / HEVC Annex-B zero-latency (no B-frames)
};

enum PacketFlags : uint8_t {
    kFlagHasCrc           = 0x01,
    kFlagLastFrag         = 0x02,
    kFlagLastSliceOfFrame = 0x04,
    kFlagFecParity        = 0x08,
    kFlagKeyFrame         = 0x10,  // IDR / Random Access Point (carries VPS/SPS/PPS)
    kFlagCodecConfig      = 0x20,  // Payload carries parameter sets (SPS/PPS/VPS)
    kFlagCodecMask        = 0xC0,  // Top 2 bits: VideoCodecId (0=DCT, 1=H264, 2=H265)
};

inline VideoCodecId packet_codec_id(uint8_t flags) {
    const uint8_t raw = uint8_t((flags & kFlagCodecMask) >> 6);
    return raw <= uint8_t(VideoCodecId::kH265) ? VideoCodecId(raw) : VideoCodecId::kLanVidDct;
}

inline uint8_t encode_codec_flags(VideoCodecId codec) {
    return uint8_t((uint8_t(codec) & 0x03u) << 6);
}

inline const char* video_codec_name(VideoCodecId codec) {
    switch (codec) {
        case VideoCodecId::kLanVidDct: return "DCT";
        case VideoCodecId::kH264:      return "H.264";
        case VideoCodecId::kH265:      return "H.265";
        default:                       return "Unknown";
    }
}

struct PacketHeader {
    uint8_t version = kProtocolVersion;
    PacketType type = PacketType::kVideoSlice;
    uint8_t flags = kFlagHasCrc;
    uint32_t frame_id = 0;      // video frame_id OR monotonic audio_seq_id for kAudio
    uint32_t timestamp_us = 0;  // shared steady_clock/QPC microsecond timestamp for A/V sync
    uint16_t slice_id = 0;
    uint16_t total_slices = 0;
    uint16_t frag_index = 0;
    uint16_t frag_count = 0;
    uint16_t payload_size = 0;
    uint16_t seq = 0;
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

// 8-byte sub-header prepended to XOR FEC parity payloads (flags & kFlagFecParity).
struct FecHeader {
    uint16_t group_start_frag = 0;
    uint16_t group_data_count = 0;
    uint16_t xor_payload_len = 0;
    uint16_t reserved = 0;
};

void write_fec_header(uint8_t* out, const FecHeader& f);
bool parse_fec_header(const uint8_t* in, size_t len, FecHeader& f);

// 8-byte sub-header prepended to PacketType::kAudio payloads (4-byte aligned).
enum class AudioSampleFormat : uint8_t {
    kPcm16 = 0,  // signed 16-bit interleaved little-endian PCM
    kFloat32 = 1,
};

struct AudioHeader {
    uint32_t sample_rate = 48000;
    uint8_t channels = 2;
    AudioSampleFormat sample_format = AudioSampleFormat::kPcm16;
    uint16_t num_frames = 0;  // samples per channel (e.g. 120 = 2.5 ms, 240 = 5 ms at 48 kHz)
};

inline size_t audio_bytes_per_sample(AudioSampleFormat fmt) {
    return fmt == AudioSampleFormat::kFloat32 ? 4 : 2;
}

void write_audio_header(uint8_t* out, const AudioHeader& a);
bool parse_audio_header(const uint8_t* in, size_t len, AudioHeader& a);

// ---------------------------------------------------------------------------------------------
// LVSP audio v2 (PacketType::kAudio2): 64-channel AoIP, master-clock aligned.
// ---------------------------------------------------------------------------------------------
constexpr size_t kAudioHeader2Size = 24;
constexpr size_t kAudioFecInfoSize = 4;
constexpr size_t kAudioNackSize = 16;
constexpr uint16_t kMaxAudioChannels = 64;

// NOTE: the wire values differ from the C ABI LV_AudioFormat (PCM16=0, F32=1): PCM16=1, PCM24=2, F32=3.
enum class AudioFormat2 : uint8_t {
    kPcm16 = 1,
    kPcm24Packed = 2,  // 3 bytes / sample, little-endian signed
    kFloat32 = 3,
    kAdpcm4 = 4,       // block-independent 4-bit IMA-ADPCM @ 48 kHz (192 kbps/ch)
    kAdpcm5 = 5,       // block-independent 5-bit IMA-ADPCM @ 48 kHz (256 kbps/ch)
    kAdpcm128 = 6,     // 2:1 decimated 5-bit IMA-ADPCM @ 24 kHz (128 kbps/ch)
    kAdpcm96 = 7,      // 2:1 decimated 4-bit IMA-ADPCM @ 24 kHz (96 kbps/ch)
    kAdpcm64 = 8,      // 3:1 decimated 4-bit IMA-ADPCM @ 16 kHz (64 kbps/ch)
    kOpus = 9,         // libopus CELT 2.5 ms (120 samples @ 48 kHz, fullband 20 Hz - 20 kHz)
};

enum AudioFlags2 : uint8_t {
    kAudioFlagFecParity = 0x01,   // payload = AudioFecInfo + XOR of the member payloads
    kAudioFlagRetransmit = 0x02,  // answer to a kAudioNack
    kAudioFlagDuplicate = 0x04,   // proactive duplicate (loss > 5% in high-reliability mode)
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
            return 0;  // variable-length Opus packet (determined by datagram payload_size)
    }
    return size_t(num_samples) * 4;
}

// 24-byte header (the requested fields sum to 24 bytes, not 16). Little-endian:
//  0 stream_id u16 | 2 channel_index u16 | 4 sample_rate u32 | 8 num_samples u16 |
// 10 sample_format u8 | 11 flags u8 | 12 media_timestamp_samples u64 | 20 wallclock_us u32
// PacketHeader.frame_id carries the per-stream packet sequence number (RFC 1982 wrap rules).
struct AudioHeader2 {
    uint16_t stream_id = 0;       // 0..63 independent transport stream (own seq/FEC/NACK state)
    uint16_t channel_index = 0;   // 0..63 destination slot in the receiver's 64-channel bus
    uint32_t sample_rate = 48000; // 48000 / 96000
    uint16_t num_samples = 0;     // samples of this one channel in this packet
    AudioFormat2 sample_format = AudioFormat2::kPcm16;
    uint8_t flags = 0;
    uint64_t media_timestamp_samples = 0;  // master-clock position of the first sample
    uint32_t wallclock_us = 0;             // sender steady clock at transmit (jitter / RTT)
};

void write_audio_header2(uint8_t* out, const AudioHeader2& a);
// Validates ranges and that `len` covers the header (+ payload for data packets).
bool parse_audio_header2(const uint8_t* in, size_t len, AudioHeader2& a);

// Parity packets: AudioHeader2 (FEC flag, media ts of first member) + 4-byte info + XOR bytes.
// Members are seq = first_seq + j * interleave, j = 0..k-1 (PacketHeader.frame_id = first_seq).
struct AudioFecInfo {
    uint8_t group_size = 1;  // k data packets protected by this parity
    uint8_t interleave = 1;  // stride between members (burst protection)
    uint16_t reserved = 0;
};
void write_audio_fec_info(uint8_t* out, const AudioFecInfo& f);
bool parse_audio_fec_info(const uint8_t* in, size_t len, AudioFecInfo& f);

// kAudioNack payload (16 bytes): stream_id u16 | reserved u16 | base_seq u32 | mask u64
// (bit i set => packet base_seq + i is missing).
struct AudioNack {
    uint16_t stream_id = 0;
    uint32_t base_seq = 0;
    uint64_t mask = 0;
};
void write_audio_nack(uint8_t* out, const AudioNack& n);
bool parse_audio_nack(const uint8_t* in, size_t len, AudioNack& n);

// ---------------------------------------------------------------------------------------------
// LVSP MIDI v2 (PacketType::kMidiFrame): sample-accurate MIDI over IP synced to MasterClock.
// ---------------------------------------------------------------------------------------------
constexpr size_t kMidiFrameHeaderSize = 20;
constexpr size_t kMidiEventPacketSize = 12;
constexpr uint8_t kMaxMidiPorts = 16;
constexpr uint16_t kMaxMidiEventsPerPacket = 64;

enum MidiFlags2 : uint16_t {
    kMidiFlagRedundant = 0x0001,  // carries recent history events for zero-stuck-note Wi-Fi recovery
    kMidiFlagHeartbeat = 0x0002,  // periodic heartbeat carrying recent events (prevents stuck Note Off)
};

// 20-byte header (little-endian):
//  0 stream_id u32 | 4 batch_seq u32 | 8 base_media_ts_us u64 | 16 event_count u16 | 18 flags u16
struct MidiFrameHeader {
    uint32_t stream_id = 0;
    uint32_t batch_seq = 0;
    uint64_t base_media_ts_us = 0;
    uint16_t event_count = 0;
    uint16_t flags = 0;
};

// 12-byte event (little-endian):
//  0 event_seq u32 | 4 offset_us i32 | 8 status u8 | 9 data1 u8 | 10 data2 u8 | 11 port_index u8
struct MidiEventPacket {
    uint32_t event_seq = 0;
    int32_t offset_us = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint8_t port_index = 0;  // 0..15
};

size_t write_midi_frame(uint8_t* out, size_t cap, const MidiFrameHeader& hdr,
                        const MidiEventPacket* events, uint16_t count);
bool parse_midi_frame(const uint8_t* in, size_t len, MidiFrameHeader& hdr,
                      MidiEventPacket* out_events, uint16_t max_events);

// Extended PONG tail (16 bytes, optional after the 12-byte PingPongPayload): sender master clock.
// t2/t3 are the sender's *master clock* microseconds (64-bit, since the sender's epoch).
constexpr size_t kPongMasterTailSize = 16;
void write_pong_master_tail(uint8_t* out, uint64_t t2_master_us, uint64_t t3_master_us);
bool parse_pong_master_tail(const uint8_t* in, size_t len, uint64_t& t2_master_us,
                            uint64_t& t3_master_us);

// PING / PONG control payloads (12 bytes: t1, t2, t3 in microseconds).
struct PingPongPayload {
    uint32_t t1_client_send_us = 0;
    uint32_t t2_server_recv_us = 0;
    uint32_t t3_server_send_us = 0;
};
constexpr size_t kPingPongSize = 12;

void write_ping_pong(uint8_t* out, const PingPongPayload& p);
bool parse_ping_pong(const uint8_t* in, size_t len, PingPongPayload& p);

// 16-byte Receiver -> Sender telemetry feedback + PLI (Picture Loss Indication) payload.
// `request_idr` is wire-packed into bit 15 (0x8000) of the 16-bit `packet_loss_permille` field
// (which only uses 0..1000), keeping kFeedbackSize == 16 and 100% wire compatibility.
struct FeedbackPayload {
    uint32_t last_completed_frame_id = 0;
    uint16_t packet_loss_permille = 0;    // 0..1000 (0.1% steps)
    uint16_t fec_recovered_permille = 0;  // 0..1000 (0.1% steps)
    uint32_t jitter_us = 0;
    uint32_t decode_time_us = 0;
    bool request_idr = false;             // PLI: sender immediately emits an IDR keyframe
};

void write_feedback(uint8_t* out, const FeedbackPayload& fb);
bool parse_feedback(const uint8_t* in, size_t len, FeedbackPayload& fb);

// RFC 1982 serial number arithmetic for wrap-safe comparisons.
inline int32_t frame_diff(uint32_t a, uint32_t b) { return int32_t(a - b); }
inline bool frame_newer(uint32_t a, uint32_t b) { return frame_diff(a, b) > 0; }
inline int16_t seq_diff(uint16_t a, uint16_t b) { return int16_t(a - b); }

// ---------------------------------------------------------------------------------------------
// Real-Time Bidirectional Control Sync (PacketType::kControlSync, 32-byte payload).
// ---------------------------------------------------------------------------------------------
constexpr size_t kControlSyncSize = 32;

enum ControlSyncFlags : uint8_t {
    kControlSyncMuteAll    = 0x01,  // PANIC / Mute All
    kControlSyncMatrix     = 0x02,  // Also sync matrix preset / routing
    kControlSyncAck        = 0x04,  // Reserved ACK flag
};

struct ControlSyncPayload {
    uint64_t revision = 0;           // monotonic revision (dedup + anti-loop)
    uint32_t origin_id = 0;          // random per-instance origin ID (echo guard)
    uint16_t audio_channels = 8;     // 2 / 8 / 16 / 32 / 64
    uint16_t audio_bitrate_kbps = 0; // 0=Lossless, 256 / 192 / 128 / 96 / 64
    uint16_t target_buffer_ms = 20;  // 2..2000 ms
    uint8_t  latency_mode = 1;       // 0=Fixed (Ethernet), 1=Adaptive (Wi-Fi)
    uint8_t  audio_only = 0;         // 0=AV, 1=Audio Only
    int8_t   active_preset_id = -1;  // -1=custom, 0/1/2/3=preset
    uint8_t  flags = 0;              // ControlSyncFlags
    int16_t  master_gain_cb = 0;     // 0.01 dB (-6000..+1200)
    uint8_t  audio_codec = 0;        // 0..8
};

void write_control_sync(uint8_t* out, const ControlSyncPayload& cs);
bool parse_control_sync(const uint8_t* in, size_t len, ControlSyncPayload& cs);

// ---------------------------------------------------------------------------------------------
// Personal Smartphone Monitor Mixer Remote Control (PacketType::kMonitorControl = 0xA).
// ---------------------------------------------------------------------------------------------
enum class MonitorControlOp : uint8_t {
    kHello = 1,          // Smartphone -> PC: register / keepalive (name, desired bitrate)
    kSyncMetaReq = 2,    // Smartphone -> PC: request active channel names + live VU meters
    kSyncMetaResp = 3,   // PC -> Smartphone: active channel count, names (16B each), VU dBFS (int8)
    kSetChannelMix = 4,  // Smartphone -> PC: gain_db, pan, mute, solo for channel 0..63
    kSetDsp = 5,         // Smartphone -> PC: HPF, 3-band EQ, Compressor, Master Volume/Mute
    kDisconnect = 6,     // Smartphone -> PC: clean disconnect
};

constexpr size_t kMonitorControlSize = 64;
// kHello.bitrate_kbps value requesting a lossless monitor stream (24-bit PCM, ~2.3 Mbps stereo).
constexpr uint16_t kMonitorBitrateLossless = 0xFFFF;
// Capability flags carried in MonitorControlPayload::reserved on kHello:
constexpr uint8_t kMonitorCapFlowV1 = 0x01;          // Client supports compact kMonitorAudioFlow multi-channel frames
constexpr uint8_t kMonitorCapRxSubMask = 0x02;       // Client sends active_channel_mask in kHello & 15 ms LVKA keep-alive
constexpr uint8_t kMonitorCapRedundancy1Plus1 = 0x04;// Client requests 1+1 Wi-Fi Redundant Flow (+1.5 ms backup copy)

struct MonitorControlPayload {
    MonitorControlOp op = MonitorControlOp::kHello;
    uint8_t channel_index = 0;       // 0..63 for kSetChannelMix
    uint8_t flags = 0;               // bit0=mute, bit1=solo, bit2=hpf_on, bit3=eq_on, bit4=comp_on, bit5=master_mute
    uint8_t reserved = 0;            // capability flags on kHello (kMonitorCapFlowV1 | kMonitorCapRxSubMask)
    uint32_t client_id = 0;          // unique smartphone session id
    uint16_t bitrate_kbps = 256;     // desired total stereo bitrate (e.g. 256 / 384 kbps)
    uint16_t target_latency_ms = 20; // desired monitor buffer (ms)

    // Channel mix parameters (kSetChannelMix)
    float gain_db = 0.0f;            // -60..+12 dB
    float pan = 0.0f;                // -1.0 (L) .. +1.0 (R)

    // Bus DSP parameters (kSetDsp)
    float hpf_hz = 80.0f;            // 20..250 Hz
    float eq_low_db = 0.0f;          // ±12 dB @ 100 Hz
    float eq_mid_hz = 1200.0f;       // 300..5000 Hz
    float eq_mid_db = 0.0f;          // ±12 dB
    float eq_high_db = 0.0f;         // ±12 dB @ 8 kHz
    float comp_thresh_db = -18.0f;   // -40..0 dB
    float comp_ratio = 3.0f;         // 1.0..10.0
    float comp_attack_ms = 5.0f;     // 0.5..50 ms
    float comp_release_ms = 80.0f;   // 10..500 ms
    float comp_makeup_db = 0.0f;     // 0..18 dB
    float master_volume_db = 0.0f;   // -60..+12 dB

    char client_name[16] = {};       // for kHello (null-terminated)
    uint64_t active_channel_mask = ~0ull;  // Receiver-Driven Subscription bitmask (kHello bytes 28..35)
};

// 16-byte compact keep-alive + Rx Subscription datagram ("LVKA" + client_id u32 + active_channel_mask u64)
constexpr size_t kMonitorKeepAliveSize = 16;
struct MonitorKeepAlivePayload {
    uint32_t client_id = 0;
    uint64_t active_channel_mask = ~0ull;
};
size_t write_monitor_keepalive(uint8_t* out, size_t cap, const MonitorKeepAlivePayload& ka);
bool parse_monitor_keepalive(const uint8_t* in, size_t len, MonitorKeepAlivePayload& ka);

// Dante-style Multi-Channel Monitor Audio Flow (PacketType::kMonitorAudioFlow = 0xD):
// Groups up to 16 active subscribed channels into ONE compact UDP datagram.
constexpr size_t kMonitorAudioFlowHeaderSize = 24;
constexpr uint8_t kMaxMonitorFlowChannels = 16;
constexpr uint8_t kMonitorFlowFlagVarLen = 0x01;   // payload begins with channel_count x u16 per-channel byte lengths
constexpr uint8_t kFlagRedundantCopy = kAudioFlagDuplicate;  // 0x04: proactive 1+1 Wi-Fi redundant copy (+1.5 ms)

struct MonitorAudioFlowHeader {
    uint32_t flow_seq = 0;
    uint64_t media_timestamp_samples = 0;
    uint32_t sample_rate = 48000;
    uint16_t num_samples = 240;
    uint16_t channel_mask = 0x0003;  // bit c set => channel c payload included (up to 16 channels)
    AudioFormat2 sample_format = AudioFormat2::kPcm16;
    uint8_t flags = 0;
    uint8_t channel_count = 0;       // 1..16 (number of set bits in channel_mask)
    uint8_t reserved = 0;
};

size_t write_monitor_audio_flow(uint8_t* out, size_t cap, const MonitorAudioFlowHeader& fh,
                                const uint8_t* const* ch_payloads, const uint16_t* ch_lengths);
bool parse_monitor_audio_flow(const uint8_t* in, size_t len, MonitorAudioFlowHeader& fh,
                              const uint8_t* out_ch_payloads[kMaxMonitorFlowChannels],
                              uint16_t out_ch_lengths[kMaxMonitorFlowChannels]);

void write_monitor_control(uint8_t* out, const MonitorControlPayload& mc);
bool parse_monitor_control(const uint8_t* in, size_t len, MonitorControlPayload& mc);

// Metadata + VU packet (kSyncMetaResp): up to 64 channels (16B name + 1B VU int8 dBFS each).
// Header bytes 2..3 (formerly reserved, 0) carry this client's bus telemetry: byte 2 = compressor+limiter
// gain reduction in quarter dB (0..255 -> 0..63.75 dB), byte 3 = int8 peak dBFS of the stereo mix sent to
// the phone. Older parsers ignore them; older servers send 0 / 0 (read as 0 dB GR, 0 dBFS -> treat
// peak 0 together with GR 0 as "unknown" only if needed).
struct MonitorMetaChannel {
    char name[16] = {};
    int8_t peak_dbfs = -120;  // -120..0 dBFS
};

struct MonitorBusTelemetry {
    float gain_reduction_db = 0.0f;  // <= 0 dB
    int8_t mix_peak_dbfs = -120;
};

size_t write_monitor_meta(uint8_t* out, size_t cap, uint32_t client_id, uint8_t num_channels,
                          const MonitorMetaChannel* channels, const MonitorBusTelemetry* bus = nullptr);
bool parse_monitor_meta(const uint8_t* in, size_t len, uint32_t& client_id, uint8_t& num_channels,
                        MonitorMetaChannel* out_channels, uint8_t max_channels,
                        MonitorBusTelemetry* bus = nullptr);

// ---------------------------------------------------------------------------------------------
// Tally State (PacketType::kTallyState = 0xB, 40-byte payload, 3x redundant + 250 ms heartbeat).
// ---------------------------------------------------------------------------------------------
constexpr size_t kTallyStateSize = 40;

enum TallyStateValue : uint8_t {
    kTallyStandby = 0,  // Neutral / off-air
    kTallyPreview = 1,  // Green (PVW — cued by director)
    kTallyProgram = 2,  // Red (PGM / ON AIR)
};

enum TallyFlags : uint8_t {
    kTallyFlagRecTrigger  = 0x01,  // Remote start/stop local recording on smartphone
    kTallyFlagHighBright  = 0x02,  // High-brightness tally border
    kTallyFlagFromCamera  = 0x04,  // Camera -> Studio heartbeat / battery / temp telemetry
};

struct TallyStatePayload {
    uint32_t camera_id = 0;       // unique camera / stream ID
    uint8_t tally_state = 0;      // TallyStateValue (0=Standby, 1=Preview, 2=Program)
    uint8_t flags = 0;            // TallyFlags
    uint8_t battery_pct = 100;    // 0..100% (camera telemetry)
    int8_t temp_c = 30;           // Celsius (camera telemetry)
    uint32_t revision = 0;        // monotonic revision
    uint32_t reserved = 0;
    char camera_label[24] = {};   // e.g. "CAM 1 - Wide"
};

void write_tally_state(uint8_t* out, const TallyStatePayload& t);
bool parse_tally_state(const uint8_t* in, size_t len, TallyStatePayload& t);

// ---------------------------------------------------------------------------------------------
// Duplex Intercom Voice (PacketType::kIntercomAudio = 0xC, 16-byte header + Opus/PCM16 payload).
// ---------------------------------------------------------------------------------------------
constexpr size_t kIntercomHeaderSize = 16;

enum IntercomFlags : uint8_t {
    kIntercomFlagStudioToCam = 0x01,  // Downlink: Director -> Camera IFB
    kIntercomFlagAllCall     = 0x02,  // Partyline broadcast to all cameras
    kIntercomFlagPttActive   = 0x04,  // Speaker actively talking
    kIntercomFlagDuckProgram = 0x08,  // Request smooth program ducking in operator headphones
};

struct IntercomAudioHeader {
    uint32_t camera_id = 0;       // 0 = All Call, or specific camera_id
    uint32_t seq = 0;             // monotonic sequence
    uint16_t num_samples = 480;   // 480 = 10 ms @ 48 kHz mono (or 240 = 5 ms)
    uint8_t codec = 1;            // 0 = PCM16, 1 = Opus 48 kHz mono
    uint8_t flags = 0;            // IntercomFlags
    uint32_t sample_rate = 48000; // 48000 / 16000
};

void write_intercom_header(uint8_t* out, const IntercomAudioHeader& ih);
bool parse_intercom_header(const uint8_t* in, size_t len, IntercomAudioHeader& ih);

}  // namespace lv
