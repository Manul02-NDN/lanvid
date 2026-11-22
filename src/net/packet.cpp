#include "net/packet.hpp"

#include <cstring>

#include "common/crc32c.hpp"
#include "common/platform.hpp"

namespace lv {

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
    if (a.channels < 1 || a.channels > 2) return false;
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
    store_le16(out + 4, fb.packet_loss_permille);
    store_le16(out + 6, fb.fec_recovered_permille);
    store_le32(out + 8, fb.jitter_us);
    store_le32(out + 12, fb.decode_time_us);
}

bool parse_feedback(const uint8_t* in, size_t len, FeedbackPayload& fb) {
    if (len < kFeedbackSize) return false;
    fb.last_completed_frame_id = load_le32(in + 0);
    fb.packet_loss_permille = load_le16(in + 4);
    fb.fec_recovered_permille = load_le16(in + 6);
    fb.jitter_us = load_le32(in + 8);
    fb.decode_time_us = load_le32(in + 12);
    return true;
}

}  // namespace lv
