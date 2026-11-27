#pragma once
// LanVid Video Slice Protocol (LVSP v1): 28-byte binary packet header (little-endian),
// video slice XOR FEC, 2-channel stereo audio (kAudio), AIMD/PLI feedback, and camera Tally/Intercom/Sync.

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
    kVideoSlice      = 0x1,
    kPing            = 0x2,
    kAudio           = 0x3,  // 1-2 channel stereo audio (48 kHz PCM16 / Float32)
    kFeedback        = 0x4,
    kRxReport        = 0x4,  // alias
    kPong            = 0x5,
    kTallyState      = 0xB,  // Studio <-> Camera Tally (Standby / Preview / Program ON AIR) + REC trigger
    kIntercomAudio   = 0xC,  // Isolated full-duplex Director <-> Camera Operator voice talkback / IFB
    kCameraControl   = 0xE,  // Studio -> Camera remote control (lens, focus, quality preset)
    kVideoConfigSync = 0xF,  // Bidirectional Video Config Sync + Camera Badge + Smart IDR Keyframe Req
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

// 8-byte sub-header prepended to PacketType::kAudio payloads (1-2 channel stereo audio, 4-byte aligned).
constexpr uint8_t kMaxStereoChannels = 2;

enum class AudioSampleFormat : uint8_t {
    kPcm16   = 0,  // signed 16-bit interleaved little-endian PCM
    kFloat32 = 1,  // 32-bit IEEE float interleaved little-endian PCM
};

struct AudioHeader {
    uint32_t sample_rate = 48000;
    uint8_t channels = 2;  // 1 (mono) or 2 (stereo)
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
// `request_idr` is wire-packed into bit 15 (0x8000) of `packet_loss_permille`.
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
// Camera Remote Control (PacketType::kCameraControl = 0xE, 16 bytes), Studio -> Camera.
// ---------------------------------------------------------------------------------------------
enum CameraLens : uint8_t { kLensWide = 0, kLensUltraWide = 1, kLensTele = 2, kLensFront = 3 };
enum CameraQualityPreset : uint8_t { kQuality360p = 0, kQuality720p = 1, kQuality1080p = 2, kQuality4K = 3 };
constexpr size_t kCameraControlSize = 16;

struct CameraControlPayload {
    uint32_t camera_id = 0;
    uint32_t revision = 0;
    uint8_t lens = kLensWide;
    uint8_t focus_auto = 1;       // 1 = continuous autofocus, 0 = manual `focus_pos`
    uint16_t focus_pos = 0;       // 0 (near) .. 1000 (infinity)
    uint8_t quality = kQuality1080p;
    uint8_t flags = 0;            // bit0 = torch, bit1 = lock exposure
    uint16_t zoom_x100 = 100;     // 1.00x .. 10.00x
};

void write_camera_control(uint8_t* out, const CameraControlPayload& c);
bool parse_camera_control(const uint8_t* in, size_t len, CameraControlPayload& c);

// ---------------------------------------------------------------------------------------------
// Bidirectional Video Config Sync + Camera Number Assignment (PacketType::kVideoConfigSync = 0xF).
// 64-byte payload with revision + origin_id Echo Guard + Smart IDR Request flag.
// ---------------------------------------------------------------------------------------------
constexpr size_t kVideoConfigSyncSize = 64;

enum VideoConfigSyncFlags : uint8_t {
    kVideoConfigFlagRequestIdr    = 0x01,  // Studio -> Camera immediate IDR request
    kVideoConfigFlagFromCamera    = 0x02,  // Sent by mobile camera (local encoder/lens update)
    kVideoConfigFlagHasBadgeColor = 0x04,  // badge_accent_rgba[4] carries an explicit accent color
    kVideoConfigFlagSyncAck       = 0x08,  // Acknowledgement / state confirmation
};
constexpr uint8_t kVideoKeyFrameReqFlag = kVideoConfigFlagRequestIdr;

struct VideoConfigSyncPayload {
    uint64_t config_revision = 0;
    uint32_t origin_id = 0;
    uint32_t camera_id = 0;
    uint8_t assigned_cam_number = 0;    // 0 = unassigned, 1..16 = studio slot / badge number
    uint8_t resolution_preset = kQuality1080p;
    uint8_t target_fps = 60;
    uint8_t codec = uint8_t(VideoCodecId::kH264);
    uint32_t bitrate_kbps = 25000;
    uint16_t video_delay_ms = 33;       // VideoPlayoutBuffer delay (5..250 ms)
    uint8_t fixed_delay_mode = 1;       // 1 = Fixed Delay, 0 = Adaptive Wi-Fi
    uint8_t camera_lens = kLensWide;
    uint8_t flags = 0;
    uint8_t badge_accent_rgba[4] = {0, 0, 0, 0};
    char assigned_cam_tag[16] = {};
};

inline VideoConfigSyncPayload make_video_keyframe_req(uint32_t camera_id, uint64_t rev, uint32_t origin_id) {
    VideoConfigSyncPayload p{};
    p.config_revision = rev;
    p.origin_id = origin_id;
    p.camera_id = camera_id;
    // Clear encoder/lens defaults so IDR-only requests can never be mistaken for a config push.
    p.assigned_cam_number = 0;
    p.resolution_preset = 0;
    p.target_fps = 0;
    p.codec = 0;
    p.bitrate_kbps = 0;
    p.video_delay_ms = 0;
    p.fixed_delay_mode = 0;
    p.camera_lens = 0;
    p.flags = kVideoConfigFlagRequestIdr;
    return p;
}

void write_video_config_sync(uint8_t* out, const VideoConfigSyncPayload& p);
bool parse_video_config_sync(const uint8_t* in, size_t len, VideoConfigSyncPayload& p);

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
    uint32_t camera_id = 0;
    uint32_t seq = 0;
    uint16_t num_samples = 480;   // 480 = 10 ms @ 48 kHz mono (or 240 = 5 ms)
    uint8_t codec = 1;            // 0 = PCM16, 1 = Opus 48 kHz mono
    uint8_t flags = 0;
    uint32_t sample_rate = 48000;
};

void write_intercom_header(uint8_t* out, const IntercomAudioHeader& ih);
bool parse_intercom_header(const uint8_t* in, size_t len, IntercomAudioHeader& ih);

}  // namespace lv
