#include "grid/l2_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lv::grid {
namespace {

inline void put_u16_be(uint8_t* p, uint16_t v) {
    p[0] = uint8_t((v >> 8) & 0xFFu);
    p[1] = uint8_t(v & 0xFFu);
}
inline uint16_t get_u16_be(const uint8_t* p) {
    return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

inline void put_u16_le(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v & 0xFFu);
    p[1] = uint8_t((v >> 8) & 0xFFu);
}
inline uint16_t get_u16_le(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

inline void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v & 0xFFu);
    p[1] = uint8_t((v >> 8) & 0xFFu);
    p[2] = uint8_t((v >> 16) & 0xFFu);
    p[3] = uint8_t((v >> 24) & 0xFFu);
}
inline uint32_t get_u32_le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void put_u64_le(uint8_t* p, uint64_t v) {
    put_u32_le(p, uint32_t(v & 0xFFFFFFFFull));
    put_u32_le(p + 4, uint32_t((v >> 32) & 0xFFFFFFFFull));
}
inline uint64_t get_u64_le(const uint8_t* p) {
    return uint64_t(get_u32_le(p)) | (uint64_t(get_u32_le(p + 4)) << 32);
}

inline void put_f32_le(uint8_t* p, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    put_u32_le(p, u);
}
inline float get_f32_le(const uint8_t* p) {
    const uint32_t u = get_u32_le(p);
    float f = 0.0f;
    std::memcpy(&f, &u, 4);
    return f;
}

}  // namespace

bool MacAddress::parse(const std::string& str, MacAddress& out) {
    unsigned b[6]{};
    if (std::sscanf(str.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6 &&
        std::sscanf(str.c_str(), "%2x-%2x-%2x-%2x-%2x-%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) out.bytes[i] = uint8_t(b[i] & 0xFFu);
    return true;
}

std::string MacAddress::to_string() const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return buf;
}

size_t write_l2_frame(uint8_t* out, size_t cap, const MacAddress& dst, const MacAddress& src,
                      const VlanQosTag& vlan, const LvL2Header& hdr,
                      const uint8_t* payload, size_t payload_len) {
    const size_t eth_len = kEthHeaderSize + (vlan.enabled ? kVlanTagSize : 0);
    const size_t total = eth_len + kL2HeaderSize + payload_len;
    if (!out || total > cap) return 0;

    std::memcpy(out + 0, dst.bytes.data(), 6);
    std::memcpy(out + 6, src.bytes.data(), 6);
    size_t off = 12;
    if (vlan.enabled) {
        put_u16_be(out + off, kVlanEtherType);
        const uint16_t tci = uint16_t(((uint16_t(vlan.pcp & 7u) << 13) | (vlan.vlan_id & 0x0FFFu)));
        put_u16_be(out + off + 2, tci);
        off += 4;
    }
    put_u16_be(out + off, kGridEtherType);
    off += 2;

    uint8_t* h = out + off;
    h[0] = hdr.version;
    h[1] = uint8_t(hdr.type);
    put_u16_le(h + 2, hdr.stream_id);
    put_u16_le(h + 4, hdr.seq);
    put_u16_le(h + 6, hdr.channel_start);
    h[8] = hdr.channel_count;
    h[9] = hdr.samples_per_ch;
    h[10] = uint8_t(hdr.audio_fmt);
    h[11] = hdr.flags;
    put_u32_le(h + 12, hdr.media_ts_lo);

    if (payload && payload_len > 0) {
        std::memcpy(out + eth_len + kL2HeaderSize, payload, payload_len);
    }
    return total;
}

bool parse_l2_frame(const uint8_t* frame, size_t len, ParsedL2Frame& out) {
    if (!frame || len < kEthHeaderSize + kL2HeaderSize) return false;
    std::memcpy(out.dst.bytes.data(), frame + 0, 6);
    std::memcpy(out.src.bytes.data(), frame + 6, 6);
    size_t off = 12;
    uint16_t etype = get_u16_be(frame + off);
    off += 2;
    out.vlan = {};
    if (etype == kVlanEtherType) {
        if (len < kEthHeaderSize + kVlanTagSize + kL2HeaderSize) return false;
        const uint16_t tci = get_u16_be(frame + off);
        out.vlan.enabled = true;
        out.vlan.pcp = uint8_t((tci >> 13) & 7u);
        out.vlan.vlan_id = uint16_t(tci & 0x0FFFu);
        off += 2;
        etype = get_u16_be(frame + off);
        off += 2;
    }
    if (etype != kGridEtherType) return false;

    const uint8_t* h = frame + off;
    out.hdr.version = h[0];
    if (out.hdr.version != kGridProtoVersion) return false;
    out.hdr.type = static_cast<L2PacketType>(h[1]);
    out.hdr.stream_id = get_u16_le(h + 2);
    out.hdr.seq = get_u16_le(h + 4);
    out.hdr.channel_start = get_u16_le(h + 6);
    out.hdr.channel_count = h[8];
    out.hdr.samples_per_ch = h[9];
    out.hdr.audio_fmt = static_cast<L2AudioFormat>(h[10]);
    out.hdr.flags = h[11];
    out.hdr.media_ts_lo = get_u32_le(h + 12);

    out.payload = frame + off + kL2HeaderSize;
    out.payload_len = len - (off + kL2HeaderSize);
    return true;
}

size_t l2_audio_payload_bytes(L2AudioFormat fmt, uint16_t channels, uint16_t samples_per_ch) {
    const size_t bps = (fmt == L2AudioFormat::kFloat32) ? 4u : 3u;
    return size_t(channels) * size_t(samples_per_ch) * bps;
}

size_t pack_l2_audio_planar(L2AudioFormat fmt, const float* const* planes, uint16_t ch_start,
                            uint16_t ch_count, uint16_t samples_per_ch, uint8_t* out, size_t cap) {
    const size_t need = l2_audio_payload_bytes(fmt, ch_count, samples_per_ch);
    if (!out || !planes || need > cap) return 0;
    size_t pos = 0;
    if (fmt == L2AudioFormat::kFloat32) {
        for (uint16_t c = 0; c < ch_count; ++c) {
            const float* src = planes[ch_start + c];
            if (src) {
                std::memcpy(out + pos, src, size_t(samples_per_ch) * 4u);
            } else {
                std::memset(out + pos, 0, size_t(samples_per_ch) * 4u);
            }
            pos += size_t(samples_per_ch) * 4u;
        }
    } else {
        for (uint16_t c = 0; c < ch_count; ++c) {
            const float* src = planes[ch_start + c];
            for (uint16_t i = 0; i < samples_per_ch; ++i) {
                const float f = src ? std::clamp(src[i], -1.0f, 1.0f) : 0.0f;
                const int32_t q = std::clamp<int32_t>(int32_t(std::lrint(double(f) * 8388607.0)),
                                                      -8388608, 8388607);
                const uint32_t u = uint32_t(q) & 0xFFFFFFu;
                out[pos + 0] = uint8_t(u & 0xFFu);
                out[pos + 1] = uint8_t((u >> 8) & 0xFFu);
                out[pos + 2] = uint8_t((u >> 16) & 0xFFu);
                pos += 3;
            }
        }
    }
    return pos;
}

bool unpack_l2_audio_planar(L2AudioFormat fmt, const uint8_t* in, size_t in_len,
                            uint16_t ch_count, uint16_t samples_per_ch, float* const* out_planes,
                            uint16_t dst_ch_start) {
    const size_t need = l2_audio_payload_bytes(fmt, ch_count, samples_per_ch);
    if (!in || !out_planes || in_len < need) return false;
    size_t pos = 0;
    if (fmt == L2AudioFormat::kFloat32) {
        for (uint16_t c = 0; c < ch_count; ++c) {
            float* dst = out_planes[dst_ch_start + c];
            if (dst) std::memcpy(dst, in + pos, size_t(samples_per_ch) * 4u);
            pos += size_t(samples_per_ch) * 4u;
        }
    } else {
        constexpr float kInv24 = 1.0f / 8388608.0f;
        for (uint16_t c = 0; c < ch_count; ++c) {
            float* dst = out_planes[dst_ch_start + c];
            for (uint16_t i = 0; i < samples_per_ch; ++i) {
                int32_t v = int32_t(uint32_t(in[pos]) | (uint32_t(in[pos + 1]) << 8) |
                                    (uint32_t(in[pos + 2]) << 16));
                if (v & 0x800000) v |= ~0xFFFFFF;
                if (dst) dst[i] = float(v) * kInv24;
                pos += 3;
            }
        }
    }
    return true;
}

size_t write_l2_sync_clock(uint8_t* out, size_t cap, const L2SyncClockPayload& p) {
    if (!out || cap < kL2SyncClockSize) return 0;
    put_u64_le(out + 0, p.master_sample_pos);
    put_u64_le(out + 8, p.master_wallclock_us);
    put_u32_le(out + 16, p.sample_rate);
    put_u32_le(out + 20, p.clock_epoch);
    out[24] = p.clock_priority;
    out[25] = out[26] = out[27] = 0;
    return kL2SyncClockSize;
}

bool parse_l2_sync_clock(const uint8_t* in, size_t len, L2SyncClockPayload& p) {
    if (!in || len < kL2SyncClockSize) return false;
    p.master_sample_pos = get_u64_le(in + 0);
    p.master_wallclock_us = get_u64_le(in + 8);
    p.sample_rate = get_u32_le(in + 16);
    p.clock_epoch = get_u32_le(in + 20);
    p.clock_priority = in[24];
    return true;
}

size_t write_l2_discovery(uint8_t* out, size_t cap, const L2DiscoveryPayload& p) {
    if (!out || cap < kL2DiscoverySize) return 0;
    out[0] = uint8_t(p.role);
    out[1] = p.flags;
    put_u16_le(out + 2, p.max_channels);
    put_u32_le(out + 4, p.sample_rate);
    put_u16_le(out + 8, p.cpu_load_pct_x10);
    put_u16_le(out + 10, p.buffer_fill_pct_x10);
    put_u32_le(out + 12, p.proto_caps);
    std::memset(out + 16, 0, 32);
    std::memcpy(out + 16, p.node_name, strnlen(p.node_name, 31));
    return kL2DiscoverySize;
}

bool parse_l2_discovery(const uint8_t* in, size_t len, L2DiscoveryPayload& p) {
    if (!in || len < kL2DiscoverySize) return false;
    p.role = static_cast<GridNodeRole>(in[0]);
    p.flags = in[1];
    p.max_channels = get_u16_le(in + 2);
    p.sample_rate = get_u32_le(in + 4);
    p.cpu_load_pct_x10 = get_u16_le(in + 8);
    p.buffer_fill_pct_x10 = get_u16_le(in + 10);
    p.proto_caps = get_u32_le(in + 12);
    std::memcpy(p.node_name, in + 16, 31);
    p.node_name[31] = '\0';
    return true;
}

size_t write_l2_route_rule(uint8_t* out, size_t cap, const L2RouteRulePayload& p) {
    if (!out || cap < kL2RouteRuleSize) return 0;
    out[0] = p.op;
    out[1] = p.flags;
    put_u16_le(out + 2, p.route_id);
    std::memcpy(out + 4, p.dst_mac.bytes.data(), 6);
    put_u16_le(out + 10, p.src_ch_start);
    put_u16_le(out + 12, p.dst_ch_start);
    put_u16_le(out + 14, p.channel_count);
    put_u16_le(out + 16, p.stream_id);
    put_u16_le(out + 18, uint16_t(p.gain_cb));
    put_u16_le(out + 20, 0);
    put_u16_le(out + 22, 0);
    return kL2RouteRuleSize;
}

bool parse_l2_route_rule(const uint8_t* in, size_t len, L2RouteRulePayload& p) {
    if (!in || len < kL2RouteRuleSize) return false;
    p.op = in[0];
    p.flags = in[1];
    p.route_id = get_u16_le(in + 2);
    std::memcpy(p.dst_mac.bytes.data(), in + 4, 6);
    p.src_ch_start = get_u16_le(in + 10);
    p.dst_ch_start = get_u16_le(in + 12);
    p.channel_count = get_u16_le(in + 14);
    p.stream_id = get_u16_le(in + 16);
    p.gain_cb = int16_t(get_u16_le(in + 18));
    return true;
}

size_t write_l2_dsp_param(uint8_t* out, size_t cap, const L2DspParamPayload& p) {
    if (!out || cap < kL2DspParamSize) return 0;
    put_u16_le(out + 0, p.channel);
    out[2] = p.slot_index;
    out[3] = p.flags;
    put_f32_le(out + 4, p.hpf_hz);
    put_f32_le(out + 8, p.eq_low_db);
    put_f32_le(out + 12, p.eq_mid_hz);
    put_f32_le(out + 16, p.eq_mid_db);
    put_f32_le(out + 20, p.eq_high_db);
    put_f32_le(out + 24, p.comp_thresh_db);
    put_f32_le(out + 28, p.comp_ratio);
    put_f32_le(out + 32, p.comp_makeup_db);
    put_f32_le(out + 36, p.gate_thresh_db);
    put_f32_le(out + 40, p.output_gain_db);
    put_u32_le(out + 44, p.revision);
    return kL2DspParamSize;
}

bool parse_l2_dsp_param(const uint8_t* in, size_t len, L2DspParamPayload& p) {
    if (!in || len < kL2DspParamSize) return false;
    p.channel = get_u16_le(in + 0);
    p.slot_index = in[2];
    p.flags = in[3];
    p.hpf_hz = get_f32_le(in + 4);
    p.eq_low_db = get_f32_le(in + 8);
    p.eq_mid_hz = get_f32_le(in + 12);
    p.eq_mid_db = get_f32_le(in + 16);
    p.eq_high_db = get_f32_le(in + 20);
    p.comp_thresh_db = get_f32_le(in + 24);
    p.comp_ratio = get_f32_le(in + 28);
    p.comp_makeup_db = get_f32_le(in + 32);
    p.gate_thresh_db = get_f32_le(in + 36);
    p.output_gain_db = get_f32_le(in + 40);
    p.revision = get_u32_le(in + 44);
    return true;
}

size_t write_l2_heartbeat(uint8_t* out, size_t cap, const L2HeartbeatPayload& p) {
    if (!out || cap < kL2HeartbeatSize) return 0;
    put_u32_le(out + 0, p.uptime_ms);
    put_u16_le(out + 4, p.cpu_load_pct_x10);
    put_u16_le(out + 6, p.buffer_fill_pct_x10);
    put_u16_le(out + 8, p.active_channels);
    out[10] = p.is_backup;
    out[11] = p.health_ok;
    put_u32_le(out + 12, p.frames_processed);
    return kL2HeartbeatSize;
}

bool parse_l2_heartbeat(const uint8_t* in, size_t len, L2HeartbeatPayload& p) {
    if (!in || len < kL2HeartbeatSize) return false;
    p.uptime_ms = get_u32_le(in + 0);
    p.cpu_load_pct_x10 = get_u16_le(in + 4);
    p.buffer_fill_pct_x10 = get_u16_le(in + 6);
    p.active_channels = get_u16_le(in + 8);
    p.is_backup = in[10];
    p.health_ok = in[11];
    p.frames_processed = get_u32_le(in + 12);
    return true;
}

}  // namespace lv::grid
