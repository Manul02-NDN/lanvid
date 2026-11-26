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

static void store_le64(uint8_t* p, uint64_t v) {
    store_le32(p, uint32_t(v));
    store_le32(p + 4, uint32_t(v >> 32));
}
static uint64_t load_le64(const uint8_t* p) {
    return uint64_t(load_le32(p)) | (uint64_t(load_le32(p + 4)) << 32);
}

void write_audio_header2(uint8_t* out, const AudioHeader2& a) {
    store_le16(out + 0, a.stream_id);
    store_le16(out + 2, a.channel_index);
    store_le32(out + 4, a.sample_rate);
    store_le16(out + 8, a.num_samples);
    out[10] = uint8_t(a.sample_format);
    out[11] = a.flags;
    store_le64(out + 12, a.media_timestamp_samples);
    store_le32(out + 20, a.wallclock_us);
}

bool parse_audio_header2(const uint8_t* in, size_t len, AudioHeader2& a) {
    if (len < kAudioHeader2Size) return false;
    a.stream_id = load_le16(in + 0);
    a.channel_index = load_le16(in + 2);
    a.sample_rate = load_le32(in + 4);
    a.num_samples = load_le16(in + 8);
    a.sample_format = AudioFormat2(in[10]);
    a.flags = in[11];
    a.media_timestamp_samples = load_le64(in + 12);
    a.wallclock_us = load_le32(in + 20);
    if (a.stream_id >= kMaxAudioChannels || a.channel_index >= kMaxAudioChannels) return false;
    if (a.sample_rate < 8000 || a.sample_rate > 192000) return false;
    if (uint8_t(a.sample_format) < 1 || uint8_t(a.sample_format) > 9) return false;
    if (a.num_samples == 0) return false;
    if (a.sample_format == AudioFormat2::kOpus) {
        if (a.flags & kAudioFlagFecParity) return len > kAudioHeader2Size + kAudioFecInfoSize;
        return len > kAudioHeader2Size;
    }
    const size_t body = audio2_payload_bytes(a.sample_format, a.num_samples);
    if (a.flags & kAudioFlagFecParity) return len >= kAudioHeader2Size + kAudioFecInfoSize + body;
    return len >= kAudioHeader2Size + body;
}

void write_audio_fec_info(uint8_t* out, const AudioFecInfo& f) {
    out[0] = f.group_size;
    out[1] = f.interleave;
    store_le16(out + 2, f.reserved);
}

bool parse_audio_fec_info(const uint8_t* in, size_t len, AudioFecInfo& f) {
    if (len < kAudioFecInfoSize) return false;
    f.group_size = in[0];
    f.interleave = in[1];
    f.reserved = load_le16(in + 2);
    return f.group_size >= 1 && f.interleave >= 1;
}

void write_audio_nack(uint8_t* out, const AudioNack& n) {
    store_le16(out + 0, n.stream_id);
    store_le16(out + 2, 0);
    store_le32(out + 4, n.base_seq);
    store_le64(out + 8, n.mask);
}

bool parse_audio_nack(const uint8_t* in, size_t len, AudioNack& n) {
    if (len < kAudioNackSize) return false;
    n.stream_id = load_le16(in + 0);
    n.base_seq = load_le32(in + 4);
    n.mask = load_le64(in + 8);
    return n.stream_id < kMaxAudioChannels && n.mask != 0;
}

size_t write_midi_frame(uint8_t* out, size_t cap, const MidiFrameHeader& hdr,
                        const MidiEventPacket* events, uint16_t count) {
    if (!out || count > kMaxMidiEventsPerPacket) return 0;
    const size_t total = kMidiFrameHeaderSize + size_t(count) * kMidiEventPacketSize;
    if (cap < total) return 0;
    store_le32(out + 0, hdr.stream_id);
    store_le32(out + 4, hdr.batch_seq);
    store_le64(out + 8, hdr.base_media_ts_us);
    store_le16(out + 16, count);
    store_le16(out + 18, hdr.flags);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t* ep = out + kMidiFrameHeaderSize + size_t(i) * kMidiEventPacketSize;
        store_le32(ep + 0, events[i].event_seq);
        store_le32(ep + 4, uint32_t(events[i].offset_us));
        ep[8] = events[i].status;
        ep[9] = events[i].data1;
        ep[10] = events[i].data2;
        ep[11] = events[i].port_index;
    }
    return total;
}

bool parse_midi_frame(const uint8_t* in, size_t len, MidiFrameHeader& hdr,
                      MidiEventPacket* out_events, uint16_t max_events) {
    if (!in || len < kMidiFrameHeaderSize) return false;
    hdr.stream_id = load_le32(in + 0);
    hdr.batch_seq = load_le32(in + 4);
    hdr.base_media_ts_us = load_le64(in + 8);
    hdr.event_count = load_le16(in + 16);
    hdr.flags = load_le16(in + 18);
    if (hdr.event_count > kMaxMidiEventsPerPacket) return false;
    const size_t need = kMidiFrameHeaderSize + size_t(hdr.event_count) * kMidiEventPacketSize;
    if (len < need) return false;
    if (out_events) {
        const uint16_t n = hdr.event_count < max_events ? hdr.event_count : max_events;
        for (uint16_t i = 0; i < n; ++i) {
            const uint8_t* ep = in + kMidiFrameHeaderSize + size_t(i) * kMidiEventPacketSize;
            out_events[i].event_seq = load_le32(ep + 0);
            out_events[i].offset_us = int32_t(load_le32(ep + 4));
            out_events[i].status = ep[8];
            out_events[i].data1 = ep[9];
            out_events[i].data2 = ep[10];
            out_events[i].port_index = ep[11];
            if (out_events[i].port_index >= kMaxMidiPorts) return false;
        }
    }
    return true;
}

void write_pong_master_tail(uint8_t* out, uint64_t t2_master_us, uint64_t t3_master_us) {
    store_le64(out + 0, t2_master_us);
    store_le64(out + 8, t3_master_us);
}

bool parse_pong_master_tail(const uint8_t* in, size_t len, uint64_t& t2_master_us,
                            uint64_t& t3_master_us) {
    if (len < kPongMasterTailSize) return false;
    t2_master_us = load_le64(in + 0);
    t3_master_us = load_le64(in + 8);
    return true;
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

void write_control_sync(uint8_t* out, const ControlSyncPayload& cs) {
    std::memset(out, 0, kControlSyncSize);
    store_le64(out + 0, cs.revision);
    store_le32(out + 8, cs.origin_id);
    store_le16(out + 12, cs.audio_channels);
    store_le16(out + 14, cs.audio_bitrate_kbps);
    store_le16(out + 16, cs.target_buffer_ms);
    out[18] = cs.latency_mode;
    out[19] = cs.audio_only;
    out[20] = uint8_t(cs.active_preset_id);
    out[21] = cs.flags;
    store_le16(out + 22, uint16_t(cs.master_gain_cb));
    out[24] = cs.audio_codec;
}

bool parse_control_sync(const uint8_t* in, size_t len, ControlSyncPayload& cs) {
    if (len < kControlSyncSize) return false;
    cs.revision = load_le64(in + 0);
    cs.origin_id = load_le32(in + 8);
    cs.audio_channels = load_le16(in + 12);
    cs.audio_bitrate_kbps = load_le16(in + 14);
    cs.target_buffer_ms = load_le16(in + 16);
    cs.latency_mode = in[18];
    cs.audio_only = in[19];
    cs.active_preset_id = int8_t(in[20]);
    cs.flags = in[21];
    cs.master_gain_cb = int16_t(load_le16(in + 22));
    cs.audio_codec = in[24];
    if (cs.audio_channels == 0 || cs.audio_channels > kMaxAudioChannels) return false;
    if (cs.target_buffer_ms < 2 || cs.target_buffer_ms > 2000) return false;
    return true;
}

namespace {
inline void store_f32_le(uint8_t* p, float v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, 4);
    store_le32(p, u);
}
inline float load_f32_le(const uint8_t* p) {
    const uint32_t u = load_le32(p);
    float v = 0.0f;
    std::memcpy(&v, &u, 4);
    return v;
}
}  // namespace

void write_monitor_control(uint8_t* out, const MonitorControlPayload& mc) {
    std::memset(out, 0, kMonitorControlSize);
    out[0] = uint8_t(mc.op);
    out[1] = mc.channel_index;
    out[2] = mc.flags;
    out[3] = mc.reserved;
    store_le32(out + 4, mc.client_id);
    store_le16(out + 8, mc.bitrate_kbps);
    store_le16(out + 10, mc.target_latency_ms);
    if (mc.op == MonitorControlOp::kHello) {
        std::memcpy(out + 12, mc.client_name, 16);
        store_le64(out + 28, mc.active_channel_mask);
    } else if (mc.op == MonitorControlOp::kSetChannelMix) {
        store_f32_le(out + 12, mc.gain_db);
        store_f32_le(out + 16, mc.pan);
    } else if (mc.op == MonitorControlOp::kSetDsp) {
        store_f32_le(out + 12, mc.hpf_hz);
        store_f32_le(out + 16, mc.eq_low_db);
        store_f32_le(out + 20, mc.eq_mid_hz);
        store_f32_le(out + 24, mc.eq_mid_db);
        store_f32_le(out + 28, mc.eq_high_db);
        store_f32_le(out + 32, mc.comp_thresh_db);
        store_f32_le(out + 36, mc.comp_ratio);
        store_f32_le(out + 40, mc.comp_attack_ms);
        store_f32_le(out + 44, mc.comp_release_ms);
        store_f32_le(out + 48, mc.comp_makeup_db);
        store_f32_le(out + 52, mc.master_volume_db);
    }
}

bool parse_monitor_control(const uint8_t* in, size_t len, MonitorControlPayload& mc) {
    if (len < kMonitorControlSize) return false;
    mc = MonitorControlPayload{};
    mc.op = MonitorControlOp(in[0]);
    mc.channel_index = in[1];
    mc.flags = in[2];
    mc.reserved = in[3];
    mc.client_id = load_le32(in + 4);
    mc.bitrate_kbps = load_le16(in + 8);
    mc.target_latency_ms = load_le16(in + 10);
    if (mc.op == MonitorControlOp::kHello) {
        std::memcpy(mc.client_name, in + 12, 16);
        mc.client_name[15] = '\0';
        // Backward compatibility: older clients leave bytes 28..35 as 0 without kMonitorCapRxSubMask.
        mc.active_channel_mask = (mc.reserved & kMonitorCapRxSubMask) ? load_le64(in + 28) : ~0ull;
    } else if (mc.op == MonitorControlOp::kSetChannelMix) {
        mc.gain_db = load_f32_le(in + 12);
        mc.pan = load_f32_le(in + 16);
    } else if (mc.op == MonitorControlOp::kSetDsp) {
        mc.hpf_hz = load_f32_le(in + 12);
        mc.eq_low_db = load_f32_le(in + 16);
        mc.eq_mid_hz = load_f32_le(in + 20);
        mc.eq_mid_db = load_f32_le(in + 24);
        mc.eq_high_db = load_f32_le(in + 28);
        mc.comp_thresh_db = load_f32_le(in + 32);
        mc.comp_ratio = load_f32_le(in + 36);
        mc.comp_attack_ms = load_f32_le(in + 40);
        mc.comp_release_ms = load_f32_le(in + 44);
        mc.comp_makeup_db = load_f32_le(in + 48);
        mc.master_volume_db = load_f32_le(in + 52);
    }
    return true;
}

size_t write_monitor_keepalive(uint8_t* out, size_t cap, const MonitorKeepAlivePayload& ka) {
    if (!out || cap < kMonitorKeepAliveSize) return 0;
    out[0] = 'L';
    out[1] = 'V';
    out[2] = 'K';
    out[3] = 'A';
    store_le32(out + 4, ka.client_id);
    store_le64(out + 8, ka.active_channel_mask);
    return kMonitorKeepAliveSize;
}

bool parse_monitor_keepalive(const uint8_t* in, size_t len, MonitorKeepAlivePayload& ka) {
    if (!in || len < 4 || in[0] != 'L' || in[1] != 'V' || in[2] != 'K' || in[3] != 'A') return false;
    if (len >= kMonitorKeepAliveSize) {
        ka.client_id = load_le32(in + 4);
        ka.active_channel_mask = load_le64(in + 8);
    } else {
        ka.client_id = 0;
        ka.active_channel_mask = ~0ull;
    }
    return true;
}

size_t write_monitor_audio_flow(uint8_t* out, size_t cap, const MonitorAudioFlowHeader& fh,
                                const uint8_t* const* ch_payloads, const uint16_t* ch_lengths) {
    if (!out || !ch_payloads || !ch_lengths) return 0;
    if (fh.channel_count == 0 || fh.channel_count > kMaxMonitorFlowChannels) return 0;
    if (fh.num_samples == 0 || fh.sample_rate == 0) return 0;

    const bool var_len = (fh.sample_format == AudioFormat2::kOpus) || ((fh.flags & kMonitorFlowFlagVarLen) != 0);
    const size_t table_bytes = var_len ? (size_t(fh.channel_count) * 2u) : 0u;
    size_t body_bytes = 0;
    for (uint8_t i = 0; i < fh.channel_count; ++i) {
        if (!ch_payloads[i] || ch_lengths[i] == 0) return 0;
        body_bytes += ch_lengths[i];
    }
    const size_t total = kMonitorAudioFlowHeaderSize + table_bytes + body_bytes;
    if (cap < total) return 0;

    const uint8_t wire_flags = uint8_t(fh.flags | (var_len ? kMonitorFlowFlagVarLen : 0u));
    store_le32(out + 0, fh.flow_seq);
    store_le64(out + 4, fh.media_timestamp_samples);
    store_le32(out + 12, fh.sample_rate);
    store_le16(out + 16, fh.num_samples);
    store_le16(out + 18, fh.channel_mask);
    out[20] = uint8_t(fh.sample_format);
    out[21] = wire_flags;
    out[22] = fh.channel_count;
    out[23] = fh.reserved;

    size_t off = kMonitorAudioFlowHeaderSize;
    if (var_len) {
        for (uint8_t i = 0; i < fh.channel_count; ++i) {
            store_le16(out + off, ch_lengths[i]);
            off += 2;
        }
    }
    for (uint8_t i = 0; i < fh.channel_count; ++i) {
        std::memcpy(out + off, ch_payloads[i], ch_lengths[i]);
        off += ch_lengths[i];
    }
    return total;
}

bool parse_monitor_audio_flow(const uint8_t* in, size_t len, MonitorAudioFlowHeader& fh,
                              const uint8_t* out_ch_payloads[kMaxMonitorFlowChannels],
                              uint16_t out_ch_lengths[kMaxMonitorFlowChannels]) {
    if (!in || len < kMonitorAudioFlowHeaderSize) return false;
    fh = MonitorAudioFlowHeader{};
    fh.flow_seq = load_le32(in + 0);
    fh.media_timestamp_samples = load_le64(in + 4);
    fh.sample_rate = load_le32(in + 12);
    fh.num_samples = load_le16(in + 16);
    fh.channel_mask = load_le16(in + 18);
    fh.sample_format = AudioFormat2(in[20]);
    fh.flags = in[21];
    fh.channel_count = in[22];
    fh.reserved = in[23];

    if (fh.channel_count == 0 || fh.channel_count > kMaxMonitorFlowChannels) return false;
    if (fh.num_samples == 0 || fh.sample_rate == 0) return false;
    if (uint8_t(fh.sample_format) < 1 || uint8_t(fh.sample_format) > 9) return false;

    // Verify popcount(channel_mask) == channel_count
    uint8_t pop = 0;
    for (uint8_t b = 0; b < kMaxMonitorFlowChannels; ++b) {
        if ((fh.channel_mask >> b) & 1u) ++pop;
    }
    if (pop != fh.channel_count) return false;

    const bool var_len = (fh.flags & kMonitorFlowFlagVarLen) != 0;
    size_t off = kMonitorAudioFlowHeaderSize;
    if (var_len) {
        if (len < off + size_t(fh.channel_count) * 2u) return false;
        for (uint8_t i = 0; i < fh.channel_count; ++i) {
            out_ch_lengths[i] = load_le16(in + off);
            if (out_ch_lengths[i] == 0) return false;
            off += 2;
        }
    } else {
        const size_t fixed = audio2_payload_bytes(fh.sample_format, fh.num_samples);
        if (fixed == 0 || fixed > 65535u) return false;
        for (uint8_t i = 0; i < fh.channel_count; ++i) out_ch_lengths[i] = uint16_t(fixed);
    }

    for (uint8_t i = 0; i < fh.channel_count; ++i) {
        if (off + out_ch_lengths[i] > len) return false;
        out_ch_payloads[i] = in + off;
        off += out_ch_lengths[i];
    }
    return true;
}

size_t write_monitor_meta(uint8_t* out, size_t cap, uint32_t client_id, uint8_t num_channels,
                          const MonitorMetaChannel* channels, const MonitorBusTelemetry* bus) {
    const uint8_t n = num_channels > kMaxAudioChannels ? uint8_t(kMaxAudioChannels) : num_channels;
    const size_t total = 8 + size_t(n) * 17;
    if (cap < total) return 0;
    out[0] = uint8_t(MonitorControlOp::kSyncMetaResp);
    out[1] = n;
    out[2] = 0;
    out[3] = uint8_t(int8_t(-120));
    if (bus) {
        const float gr = bus->gain_reduction_db < 0.0f ? -bus->gain_reduction_db : 0.0f;
        out[2] = uint8_t(gr * 4.0f >= 255.0f ? 255 : int(gr * 4.0f + 0.5f));
        out[3] = uint8_t(bus->mix_peak_dbfs);
    }
    store_le32(out + 4, client_id);
    for (uint8_t i = 0; i < n; ++i) {
        uint8_t* dst = out + 8 + size_t(i) * 17;
        std::memset(dst, 0, 17);
        if (channels) {
            std::memcpy(dst, channels[i].name, 16);
            dst[15] = '\0';
            dst[16] = uint8_t(channels[i].peak_dbfs);
        } else {
            dst[16] = uint8_t(int8_t(-120));
        }
    }
    return total;
}

bool parse_monitor_meta(const uint8_t* in, size_t len, uint32_t& client_id, uint8_t& num_channels,
                        MonitorMetaChannel* out_channels, uint8_t max_channels, MonitorBusTelemetry* bus) {
    if (len < 8 || in[0] != uint8_t(MonitorControlOp::kSyncMetaResp)) return false;
    const uint8_t n = in[1];
    if (n > kMaxAudioChannels || len < 8 + size_t(n) * 17) return false;
    client_id = load_le32(in + 4);
    if (bus) {
        bus->gain_reduction_db = -float(in[2]) / 4.0f;
        bus->mix_peak_dbfs = int8_t(in[3]);
    }
    num_channels = n;
    const uint8_t copy_n = (n < max_channels) ? n : max_channels;
    for (uint8_t i = 0; i < copy_n; ++i) {
        const uint8_t* src = in + 8 + size_t(i) * 17;
        std::memcpy(out_channels[i].name, src, 16);
        out_channels[i].name[15] = '\0';
        out_channels[i].peak_dbfs = int8_t(src[16]);
    }
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
