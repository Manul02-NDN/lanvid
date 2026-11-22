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
