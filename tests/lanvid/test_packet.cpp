#include "mini_test.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/crc32c.hpp"
#include "net/packet.hpp"
#include "net/packetizer.hpp"

TEST(crc32c_rfc3720_and_hw_sw_parity) {
    // Standard RFC 3720 / iSCSI CRC32C check value for "123456789" is 0xE3069283.
    const uint8_t msg[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(lv::crc32c_update_sw(0, msg, 9) == 0xE3069283u);
    CHECK(lv::crc32c(msg, 9) == 0xE3069283u);

    // Incremental update matches single-shot across varied buffer sizes.
    std::vector<uint8_t> data(1503);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t((i * 73 + 19) & 0xFF);

    const uint32_t full_hw = lv::crc32c(data.data(), data.size());
    const uint32_t full_sw = lv::crc32c_update_sw(0, data.data(), data.size());
    CHECK(full_hw == full_sw);

    uint32_t inc = lv::crc32c_update(0, data.data(), 24);
    inc = lv::crc32c_update(inc, data.data() + 24, data.size() - 24);
    CHECK(inc == full_hw);
}

TEST(packet_header_roundtrip_and_corruption) {
    uint8_t payload[317];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = uint8_t(i ^ 0x5A);

    lv::PacketHeader h;
    h.type = lv::PacketType::kVideoSlice;
    h.flags = lv::kFlagHasCrc | lv::kFlagLastFrag;
    h.frame_id = 0x12345678u;
    h.timestamp_us = 0x9ABCDEF0u;
    h.slice_id = 42;
    h.total_slices = 68;
    h.frag_index = 2;
    h.frag_count = 3;
    h.seq = 65534;

    uint8_t dgram[512];
    const size_t n = lv::write_packet(dgram, sizeof(dgram), h, payload, sizeof(payload));
    REQUIRE(n == lv::kPacketHeaderSize + sizeof(payload));

    lv::PacketHeader out;
    const uint8_t* p_out = nullptr;
    REQUIRE(lv::parse_packet(dgram, n, out, &p_out) == lv::PacketParseStatus::kOk);
    CHECK(out.version == lv::kProtocolVersion);
    CHECK(out.type == h.type);
    CHECK(out.flags == h.flags);
    CHECK(out.frame_id == h.frame_id);
    CHECK(out.timestamp_us == h.timestamp_us);
    CHECK(out.slice_id == h.slice_id);
    CHECK(out.total_slices == h.total_slices);
    CHECK(out.frag_index == h.frag_index);
    CHECK(out.frag_count == h.frag_count);
    CHECK(out.payload_size == sizeof(payload));
    CHECK(out.seq == h.seq);
    CHECK(std::memcmp(p_out, payload, sizeof(payload)) == 0);

    // Flip 1 bit in header -> CRC mismatch.
    dgram[5] ^= 0x01;
    CHECK(lv::parse_packet(dgram, n, out) == lv::PacketParseStatus::kBadCrc);
    dgram[5] ^= 0x01;

    // Flip 1 bit in payload -> CRC mismatch.
    dgram[lv::kPacketHeaderSize + 100] ^= 0x80;
    CHECK(lv::parse_packet(dgram, n, out) == lv::PacketParseStatus::kBadCrc);
    dgram[lv::kPacketHeaderSize + 100] ^= 0x80;

    // Bad magic / truncated.
    dgram[0] = 'X';
    CHECK(lv::parse_packet(dgram, n, out) == lv::PacketParseStatus::kBadMagic);
    dgram[0] = lv::kMagic0;
    CHECK(lv::parse_packet(dgram, lv::kPacketHeaderSize - 1, out) == lv::PacketParseStatus::kTooShort);
    CHECK(lv::parse_packet(dgram, n - 1, out) == lv::PacketParseStatus::kBadLength);
}

TEST(packetizer_mtu_fragmentation) {
    // 5000-byte slice with MTU=1400 -> max_payload=1372 -> ceil(5000/1372) = 4 packets.
    std::vector<uint8_t> slice(5000);
    for (size_t i = 0; i < slice.size(); ++i) slice[i] = uint8_t((i * 17 + 3) & 0xFF);

    lv::PacketizerConfig cfg;
    cfg.mtu = 1400;
    cfg.enable_crc = true;
    std::atomic<uint16_t> seq{65534};  // also exercises uint16 wraparound

    std::vector<uint8_t> reassembled;
    uint16_t count = 0;
    const uint16_t emitted = lv::packetize_slice(
        7, 123456, 67, 68, slice.data(), slice.size(), cfg, seq, [&](const uint8_t* d, size_t len) {
            CHECK(len <= cfg.mtu);
            lv::PacketHeader h;
            const uint8_t* pl = nullptr;
            REQUIRE(lv::parse_packet(d, len, h, &pl) == lv::PacketParseStatus::kOk);
            CHECK(h.frag_index == count);
            CHECK(h.frag_count == 4);
            CHECK(h.seq == uint16_t(65534 + count));
            CHECK((h.flags & lv::kFlagLastSliceOfFrame) != 0);
            CHECK(((h.flags & lv::kFlagLastFrag) != 0) == (count == 3));
            reassembled.insert(reassembled.end(), pl, pl + h.payload_size);
            ++count;
        });

    CHECK(emitted == 4);
    CHECK(count == 4);
    CHECK(reassembled == slice);
}
