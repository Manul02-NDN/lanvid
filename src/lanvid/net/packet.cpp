#include "net/packet.hpp"

#include <cstring>

#include "common/crc32c.hpp"
#include "common/platform.hpp"

namespace lv {

namespace {

bool is_valid_lanvid_packet_type(PacketType t) {
    switch (t) {
        case PacketType::kVideoSlice:
        case PacketType::kPing:
        case PacketType::kAudio:
        case PacketType::kFeedback:
        case PacketType::kPong:
        case PacketType::kTallyState:
        case PacketType::kIntercomAudio:
        case PacketType::kCameraControl:
        case PacketType::kVideoConfigSync:
            return true;
    }
    return false;
}

}  // namespace

size_t write_packet(uint8_t* out, size_t cap, const PacketHeader& h, const uint8_t* payload,
                    size_t payload_len) {
    const size_t total = kPacketHeaderSize + payload_len;
    if (total > cap || payload_len > 0xFFFFu) return 0;

    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = uint8_t(((h.version & 0x0Fu) << 4) | (uint8_t(h.type) & 0x0Fu));
    out[3] = h.flags;
    store_le32(out + 4, h.frame_id);
    store_le32(out + 8, h.timestamp_us);
    store_le16(out + 12, h.slice_id);
    store_le16(out + 14, h.total_slices);
    store_le16(out + 16, h.frag_index);
    store_le16(out + 18, h.frag_count);
    store_le16(out + 20, uint16_t(payload_len));
    store_le16(out + 22, h.seq);

    if (payload_len && payload) {
        std::memcpy(out + kPacketHeaderSize, payload, payload_len);
    }

    uint32_t crc = 0;
    if (h.flags & kFlagHasCrc) {
        crc = crc32c_update(0, out, 24);
        if (payload_len) crc = crc32c_update(crc, out + kPacketHeaderSize, payload_len);
    }
    store_le32(out + 24, crc);
    return total;
}

PacketParseStatus parse_packet(const uint8_t* dgram, size_t len, PacketHeader& h,
                               const uint8_t** payload_out) {
    if (len < kPacketHeaderSize) return PacketParseStatus::kTooShort;
    if (dgram[0] != kMagic0 || dgram[1] != kMagic1) return PacketParseStatus::kBadMagic;

    h.version = uint8_t(dgram[2] >> 4);
    if (h.version != kProtocolVersion) return PacketParseStatus::kBadVersion;
    h.type = PacketType(dgram[2] & 0x0Fu);
    if (!is_valid_lanvid_packet_type(h.type)) return PacketParseStatus::kInvalidFields;

    h.flags = dgram[3];
    h.frame_id = load_le32(dgram + 4);
    h.timestamp_us = load_le32(dgram + 8);
    h.slice_id = load_le16(dgram + 12);
    h.total_slices = load_le16(dgram + 14);
    h.frag_index = load_le16(dgram + 16);
    h.frag_count = load_le16(dgram + 18);
    h.payload_size = load_le16(dgram + 20);
    h.seq = load_le16(dgram + 22);
    h.crc32c = load_le32(dgram + 24);

    if (kPacketHeaderSize + size_t(h.payload_size) > len) return PacketParseStatus::kBadLength;

    if (h.flags & kFlagHasCrc) {
        uint32_t calc = crc32c_update(0, dgram, 24);
        if (h.payload_size) calc = crc32c_update(calc, dgram + kPacketHeaderSize, h.payload_size);
        if (calc != h.crc32c) return PacketParseStatus::kBadCrc;
    }

    if (h.type == PacketType::kVideoSlice) {
        if (h.total_slices == 0 || h.slice_id >= h.total_slices) return PacketParseStatus::kInvalidFields;
        if (h.frag_count == 0) return PacketParseStatus::kInvalidFields;
        if (h.flags & kFlagFecParity) {
            if (h.frag_index < h.frag_count || h.payload_size <= kFecHeaderSize)
                return PacketParseStatus::kInvalidFields;
        } else {
            if (h.frag_index >= h.frag_count || h.payload_size == 0)
                return PacketParseStatus::kInvalidFields;
        }
    } else if (h.type == PacketType::kAudio) {
        if (h.payload_size <= kAudioHeaderSize) return PacketParseStatus::kInvalidFields;
    }

    if (payload_out) *payload_out = dgram + kPacketHeaderSize;
    return PacketParseStatus::kOk;
}

void write_fec_header(uint8_t* out, const FecHeader& f) {
    store_le16(out + 0, f.group_start_frag);
    store_le16(out + 2, f.group_data_count);
    store_le16(out + 4, f.xor_payload_len);
    store_le16(out + 6, f.reserved);
}

bool parse_fec_header(const uint8_t* in, size_t len, FecHeader& f) {
    if (len < kFecHeaderSize) return false;
    f.group_start_frag = load_le16(in + 0);
    f.group_data_count = load_le16(in + 2);
    f.xor_payload_len = load_le16(in + 4);
    f.reserved = load_le16(in + 6);
    return f.group_data_count > 0;
}

void write_audio_header(uint8_t* out, const AudioHeader& a) {
    store_le32(out + 0, a.sample_rate);
    out[4] = a.channels;
    out[5] = uint8_t(a.sample_format);
    store_le16(out + 6, a.num_frames);
}

bool parse_audio_header(const uint8_t* in, size_t len, AudioHeader& a) {
    if (len < kAudioHeaderSize) return false;
    a.sample_rate = load_le32(in + 0);
    a.channels = in[4];
    a.sample_format = AudioSampleFormat(in[5]);
    a.num_frames = load_le16(in + 6);
    if (a.sample_rate < 8000 || a.sample_rate > 192000) return false;
    if (a.channels < 1 || a.channels > kMaxStereoChannels) return false;
    if (uint8_t(a.sample_format) > 1 || a.num_frames == 0) return false;
    const size_t need =
        kAudioHeaderSize + size_t(a.num_frames) * a.channels * audio_bytes_per_sample(a.sample_format);
    return len >= need;
}

void write_ping_pong(uint8_t* out, const PingPongPayload& p) {
    store_le32(out + 0, p.t1_client_send_us);
    store_le32(out + 4, p.t2_server_recv_us);
    store_le32(out + 8, p.t3_server_send_us);
}

bool parse_ping_pong(const uint8_t* in, size_t len, PingPongPayload& p) {
    if (len < kPingPongSize) return false;
    p.t1_client_send_us = load_le32(in + 0);
    p.t2_server_recv_us = load_le32(in + 4);
    p.t3_server_send_us = load_le32(in + 8);
    return true;
}

void write_feedback(uint8_t* out, const FeedbackPayload& fb) {
    store_le32(out + 0, fb.last_completed_frame_id);
    const uint16_t packed_loss =
        uint16_t((fb.packet_loss_permille & 0x7FFFu) | (fb.request_idr ? 0x8000u : 0u));
    store_le16(out + 4, packed_loss);
    store_le16(out + 6, fb.fec_recovered_permille);
    store_le32(out + 8, fb.jitter_us);
    store_le32(out + 12, fb.decode_time_us);
}

bool parse_feedback(const uint8_t* in, size_t len, FeedbackPayload& fb) {
    if (len < kFeedbackSize) return false;
    fb.last_completed_frame_id = load_le32(in + 0);
    const uint16_t packed_loss = load_le16(in + 4);
    fb.packet_loss_permille = uint16_t(packed_loss & 0x7FFFu);
    fb.request_idr = (packed_loss & 0x8000u) != 0;
    fb.fec_recovered_permille = load_le16(in + 6);
    fb.jitter_us = load_le32(in + 8);
    fb.decode_time_us = load_le32(in + 12);
    return true;
}

void write_tally_state(uint8_t* out, const TallyStatePayload& t) {
    std::memset(out, 0, kTallyStateSize);
    store_le32(out + 0, t.camera_id);
    out[4] = t.tally_state;
    out[5] = t.flags;
    out[6] = t.battery_pct;
    out[7] = uint8_t(t.temp_c);
    store_le32(out + 8, t.revision);
    store_le32(out + 12, t.reserved);
    std::memcpy(out + 16, t.camera_label, 24);
    out[39] = '\0';
}

bool parse_tally_state(const uint8_t* in, size_t len, TallyStatePayload& t) {
    if (len < kTallyStateSize) return false;
    t = TallyStatePayload{};
    t.camera_id = load_le32(in + 0);
    t.tally_state = in[4] <= 2 ? in[4] : 0;
    t.flags = in[5];
    t.battery_pct = in[6];
    t.temp_c = int8_t(in[7]);
    t.revision = load_le32(in + 8);
    t.reserved = load_le32(in + 12);
    std::memcpy(t.camera_label, in + 16, 24);
    t.camera_label[23] = '\0';
    return true;
}

void write_camera_control(uint8_t* out, const CameraControlPayload& c) {
    store_le32(out + 0, c.camera_id);
    store_le32(out + 4, c.revision);
    out[8] = c.lens;
    out[9] = c.focus_auto;
    store_le16(out + 10, c.focus_pos);
    out[12] = c.quality;
    out[13] = c.flags;
    store_le16(out + 14, c.zoom_x100);
}

bool parse_camera_control(const uint8_t* in, size_t len, CameraControlPayload& c) {
    if (!in || len < kCameraControlSize) return false;
    c.camera_id = load_le32(in + 0);
    c.revision = load_le32(in + 4);
    c.lens = in[8];
    c.focus_auto = in[9];
    c.focus_pos = load_le16(in + 10);
    c.quality = in[12];
    c.flags = in[13];
    c.zoom_x100 = load_le16(in + 14);
    return c.lens <= kLensFront && c.quality <= kQuality4K && c.focus_pos <= 1000;
}

void write_video_config_sync(uint8_t* out, const VideoConfigSyncPayload& p) {
    std::memset(out, 0, kVideoConfigSyncSize);
    store_le64(out + 0, p.config_revision);
    store_le32(out + 8, p.origin_id);
    store_le32(out + 12, p.camera_id);
    out[16] = p.assigned_cam_number <= 16 ? p.assigned_cam_number : 16;
    out[17] = p.resolution_preset;
    out[18] = p.target_fps;
    out[19] = p.codec;
    store_le32(out + 20, p.bitrate_kbps);
    store_le16(out + 24, p.video_delay_ms);
    out[26] = p.fixed_delay_mode ? 1 : 0;
    out[27] = p.camera_lens;
    out[28] = p.flags;
    out[29] = p.badge_accent_rgba[0];
    out[30] = p.badge_accent_rgba[1];
    out[31] = p.badge_accent_rgba[2];
    out[32] = p.badge_accent_rgba[3];
    std::memcpy(out + 36, p.assigned_cam_tag, 16);
    out[51] = '\0';
}

bool parse_video_config_sync(const uint8_t* in, size_t len, VideoConfigSyncPayload& p) {
    if (!in || len < kVideoConfigSyncSize) return false;
    p = VideoConfigSyncPayload{};
    p.config_revision = load_le64(in + 0);
    p.origin_id = load_le32(in + 8);
    p.camera_id = load_le32(in + 12);
    p.assigned_cam_number = in[16];
    if (p.assigned_cam_number > 16) p.assigned_cam_number = 16;
    p.resolution_preset = in[17] <= kQuality4K ? in[17] : uint8_t(kQuality1080p);
    p.target_fps = in[18];
    p.codec = in[19] <= uint8_t(VideoCodecId::kH265) ? in[19] : uint8_t(VideoCodecId::kH264);
    p.bitrate_kbps = load_le32(in + 20);
    p.video_delay_ms = load_le16(in + 24);
    if (p.video_delay_ms < 5) p.video_delay_ms = 5;
    if (p.video_delay_ms > 250) p.video_delay_ms = 250;
    p.fixed_delay_mode = in[26] ? 1 : 0;
    p.camera_lens = in[27] <= kLensFront ? in[27] : uint8_t(kLensWide);
    p.flags = in[28];
    p.badge_accent_rgba[0] = in[29];
    p.badge_accent_rgba[1] = in[30];
    p.badge_accent_rgba[2] = in[31];
    p.badge_accent_rgba[3] = in[32];
    std::memcpy(p.assigned_cam_tag, in + 36, 16);
    p.assigned_cam_tag[15] = '\0';
    return true;
}

void write_intercom_header(uint8_t* out, const IntercomAudioHeader& ih) {
    store_le32(out + 0, ih.camera_id);
    store_le32(out + 4, ih.seq);
    store_le16(out + 8, ih.num_samples);
    out[10] = ih.codec;
    out[11] = ih.flags;
    store_le32(out + 12, ih.sample_rate);
}

bool parse_intercom_header(const uint8_t* in, size_t len, IntercomAudioHeader& ih) {
    if (len < kIntercomHeaderSize) return false;
    ih.camera_id = load_le32(in + 0);
    ih.seq = load_le32(in + 4);
    ih.num_samples = load_le16(in + 8);
    ih.codec = in[10];
    ih.flags = in[11];
    ih.sample_rate = load_le32(in + 12);
    if (ih.num_samples == 0 || ih.num_samples > 1920) return false;
    if (ih.sample_rate < 8000 || ih.sample_rate > 96000) return false;
    return true;
}

}  // namespace lv
