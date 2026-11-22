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

enum PacketFlags : uint8_t {
    kFlagHasCrc = 0x01,
    kFlagLastFrag = 0x02,
    kFlagLastSliceOfFrame = 0x04,
    kFlagFecParity = 0x08,
};

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

// 16-byte Receiver -> Sender telemetry feedback payload (sent every ~100 ms for AIMD).
struct FeedbackPayload {
    uint32_t last_completed_frame_id = 0;
    uint16_t packet_loss_permille = 0;    // 0..1000 (0.1% steps)
    uint16_t fec_recovered_permille = 0;  // 0..1000 (0.1% steps)
    uint32_t jitter_us = 0;
    uint32_t decode_time_us = 0;
};

void write_feedback(uint8_t* out, const FeedbackPayload& fb);
bool parse_feedback(const uint8_t* in, size_t len, FeedbackPayload& fb);

// RFC 1982 serial number arithmetic for wrap-safe comparisons.
inline int32_t frame_diff(uint32_t a, uint32_t b) { return int32_t(a - b); }
inline bool frame_newer(uint32_t a, uint32_t b) { return frame_diff(a, b) > 0; }
inline int16_t seq_diff(uint16_t a, uint16_t b) { return int16_t(a - b); }

}  // namespace lv
