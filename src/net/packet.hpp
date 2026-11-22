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
};

enum AudioFlags2 : uint8_t {
    kAudioFlagFecParity = 0x01,   // payload = AudioFecInfo + XOR of the member payloads
    kAudioFlagRetransmit = 0x02,  // answer to a kAudioNack
    kAudioFlagDuplicate = 0x04,   // proactive duplicate (loss > 5% in high-reliability mode)
};

inline size_t audio2_bytes_per_sample(AudioFormat2 f) {
    return f == AudioFormat2::kPcm16 ? 2 : (f == AudioFormat2::kPcm24Packed ? 3 : 4);
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

}  // namespace lv
