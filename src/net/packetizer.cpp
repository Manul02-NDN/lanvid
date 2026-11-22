#include "net/packetizer.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace lv {

uint16_t packetize_slice(uint32_t frame_id, uint32_t timestamp_us, uint16_t slice_id, uint16_t total_slices,
                         const uint8_t* slice_data, size_t slice_size, const PacketizerConfig& cfg,
                         std::atomic<uint16_t>& seq_counter,
                         const std::function<void(const uint8_t* dgram, size_t len)>& on_packet) {
    const size_t fec_overhead = cfg.fec_group_size > 0 ? kFecHeaderSize : 0;
    if (!slice_data || slice_size == 0 || cfg.mtu <= kPacketHeaderSize + fec_overhead) return 0;

    const size_t max_payload = std::min(cfg.mtu, kMaxDatagram) - kPacketHeaderSize - fec_overhead;
    const size_t frag_count_sz = (slice_size + max_payload - 1) / max_payload;
    if (frag_count_sz > 65000) return 0;
    const uint16_t frag_count = uint16_t(frag_count_sz);

    uint8_t stack_buf[2048];
    std::vector<uint8_t> heap_buf;
    uint8_t* dgram = stack_buf;
    const size_t buf_cap = cfg.mtu <= sizeof(stack_buf) ? sizeof(stack_buf) : cfg.mtu;
    if (cfg.mtu > sizeof(stack_buf)) {
        heap_buf.resize(cfg.mtu);
        dgram = heap_buf.data();
    }

    uint8_t stack_parity[2048];
    std::vector<uint8_t> heap_parity;
    uint8_t* parity_payload = stack_parity;
    if (cfg.fec_group_size > 0 && kFecHeaderSize + max_payload > sizeof(stack_parity)) {
        heap_parity.resize(kFecHeaderSize + max_payload);
        parity_payload = heap_parity.data();
    }

    const uint8_t extra = uint8_t(cfg.extra_flags & (kFlagKeyFrame | kFlagCodecConfig | kFlagCodecMask));
    const uint8_t base_flags =
        uint8_t((cfg.enable_crc ? kFlagHasCrc : 0) |
                (slice_id + 1 == total_slices ? kFlagLastSliceOfFrame : 0) | extra);

    uint16_t emitted = 0;
    uint16_t group_idx = 0;
    uint16_t group_start = 0;
    uint16_t group_count = 0;
    uint16_t group_xor_len = 0;
    size_t group_max_len = 0;

    auto reset_group = [&](uint16_t start_fi) {
        group_start = start_fi;
        group_count = 0;
        group_xor_len = 0;
        group_max_len = 0;
    };

    auto emit_parity = [&]() {
        FecHeader fh;
        fh.group_start_frag = group_start;
        fh.group_data_count = group_count;
        fh.xor_payload_len = group_xor_len;
        fh.reserved = 0;
        write_fec_header(parity_payload, fh);

        PacketHeader ph;
        ph.version = kProtocolVersion;
        ph.type = PacketType::kVideoSlice;
        ph.flags = uint8_t(base_flags | kFlagFecParity);
        ph.frame_id = frame_id;
        ph.timestamp_us = timestamp_us;
        ph.slice_id = slice_id;
        ph.total_slices = total_slices;
        ph.frag_index = uint16_t(frag_count + group_idx);
        ph.frag_count = frag_count;
        ph.payload_size = uint16_t(kFecHeaderSize + group_max_len);
        ph.seq = seq_counter.fetch_add(1, std::memory_order_relaxed);

        const size_t n = write_packet(dgram, buf_cap, ph, parity_payload, kFecHeaderSize + group_max_len);
        if (n) {
            on_packet(dgram, n);
            ++emitted;
        }
        ++group_idx;
    };

    for (uint16_t fi = 0; fi < frag_count; ++fi) {
        const size_t off = size_t(fi) * max_payload;
        const size_t chunk = std::min(max_payload, slice_size - off);
        const uint8_t* ptr = slice_data + off;

        PacketHeader h;
        h.version = kProtocolVersion;
        h.type = PacketType::kVideoSlice;
        h.flags = uint8_t(base_flags | (fi + 1 == frag_count ? kFlagLastFrag : 0));
        h.frame_id = frame_id;
        h.timestamp_us = timestamp_us;
        h.slice_id = slice_id;
        h.total_slices = total_slices;
        h.frag_index = fi;
        h.frag_count = frag_count;
        h.payload_size = uint16_t(chunk);
        h.seq = seq_counter.fetch_add(1, std::memory_order_relaxed);

        const size_t n = write_packet(dgram, buf_cap, h, ptr, chunk);
        if (n) {
            on_packet(dgram, n);
            ++emitted;
        }

        if (cfg.fec_group_size > 0) {
            uint8_t* pbuf = parity_payload + kFecHeaderSize;
            if (group_count == 0) {
                reset_group(fi);
                std::memcpy(pbuf, ptr, chunk);
                group_max_len = chunk;
            } else {
                if (chunk > group_max_len) {
                    std::memset(pbuf + group_max_len, 0, chunk - group_max_len);
                    group_max_len = chunk;
                }
                for (size_t i = 0; i < chunk; ++i) pbuf[i] ^= ptr[i];
            }
            group_xor_len ^= uint16_t(chunk);
            ++group_count;

            if (group_count == cfg.fec_group_size || fi + 1 == frag_count) {
                emit_parity();
                group_count = 0;
            }
        }
    }
    return emitted;
}

}  // namespace lv
