#include "mini_test.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/crc32c.hpp"
#include "net/packet.hpp"

TEST(crosslink_packet_header_roundtrip_and_isolation) {
    uint8_t payload[256];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = uint8_t((i * 37) ^ 0xA5);

    cl::PacketHeader h{};
    h.type = cl::PacketType::kAudioFrame;
    h.flags = cl::kFlagHasCrc;
    h.seq_id = 0x11223344u;
    h.timestamp_us = 0x55667788u;
    h.stream_id = 7;
    h.stream_epoch = 42;
    h.seq16 = 65530;

    uint8_t dgram[512];
    const size_t n = cl::write_packet(dgram, sizeof(dgram), h, payload, sizeof(payload));
    REQUIRE(n == cl::kPacketHeaderSize + sizeof(payload));
    CHECK(cl::kPacketHeaderSize == 24);
    CHECK(dgram[0] == 'C' && dgram[1] == 'L');

    cl::PacketHeader out{};
    const uint8_t* pl = nullptr;
    REQUIRE(cl::parse_packet(dgram, n, out, &pl) == cl::PacketParseStatus::kOk);
    CHECK(out.version == cl::kProtocolVersion);
    CHECK(out.type == cl::PacketType::kAudioFrame);
    CHECK(out.seq_id == h.seq_id);
    CHECK(out.timestamp_us == h.timestamp_us);
    CHECK(out.stream_id == 7);
    CHECK(out.stream_epoch == 42);
    CHECK(out.seq16 == 65530);
    CHECK(out.payload_size == sizeof(payload));
    CHECK(std::memcmp(pl, payload, sizeof(payload)) == 0);

    dgram[cl::kPacketHeaderSize + 10] ^= 0x40;
    CHECK(cl::parse_packet(dgram, n, out) == cl::PacketParseStatus::kBadCrc);
    dgram[cl::kPacketHeaderSize + 10] ^= 0x40;

    // Cross-protocol rejection: LanVid magic "LV" MUST be rejected as kBadMagic!
    dgram[0] = 'L';
    dgram[1] = 'V';
    CHECK(cl::parse_packet(dgram, n, out) == cl::PacketParseStatus::kBadMagic);
}

TEST(crosslink_audio2_midi2_controlsync_and_keepalive_wire) {
    cl::AudioHeader2 ah{};
    ah.stream_id = 12;
    ah.channel_index = 12;
    ah.sample_rate = 96000;
    ah.sample_format = cl::AudioFormat2::kPcm24Packed;
    ah.num_samples = 192;
    ah.flags = cl::kAudioFlagFecParity;
    ah.media_timestamp_samples = 0x123456789ABCDEF0ull;
    std::vector<uint8_t> abuf(cl::kAudioHeader2Size + cl::kAudioFecInfoSize + 192 * 3, 0);
    cl::write_audio_header2(abuf.data(), ah);
    cl::AudioHeader2 ah_out{};
    REQUIRE(cl::parse_audio_header2(abuf.data(), abuf.size(), ah_out));
    CHECK(ah_out.stream_id == 12 && ah_out.sample_rate == 96000 && ah_out.num_samples == 192);
    CHECK(ah_out.sample_format == cl::AudioFormat2::kPcm24Packed &&
          ah_out.media_timestamp_samples == ah.media_timestamp_samples);

    cl::ControlSyncPayload cs{};
    cs.revision = 9;
    cs.origin_id = 0xCAFE0001u;
    cs.audio_channels = 64;
    cs.audio_bitrate_kbps = 256;
    cs.target_buffer_ms = 20;
    cs.latency_mode = 1;
    cs.midi_only = 0;
    cs.active_preset_id = 2;
    cs.master_gain_cb = -30;
    cs.audio_codec = 4;
    uint8_t csbuf[cl::kControlSyncSize];
    cl::write_control_sync(csbuf, cs);
    cl::ControlSyncPayload cs_out{};
    REQUIRE(cl::parse_control_sync(csbuf, sizeof(csbuf), cs_out));
    CHECK(cs_out.revision == 9 && cs_out.audio_channels == 64 && cs_out.audio_bitrate_kbps == 256);
    CHECK(cs_out.master_gain_cb == -30 && cs_out.audio_codec == 4);

    uint8_t ka[cl::kMonitorKeepAliveSize];
    cl::MonitorKeepAlivePayload ka_in{};
    ka_in.client_id = 0x12345678u;
    ka_in.active_channel_mask = 0x00FF00FF00FF00FFull;
    const size_t kan = cl::write_monitor_keepalive(ka, sizeof(ka), ka_in);
    REQUIRE(kan == cl::kMonitorKeepAliveSize);
    CHECK(ka[0] == 'C' && ka[1] == 'L' && ka[2] == 'K' && ka[3] == 'A');
    cl::MonitorKeepAlivePayload ka_out{};
    REQUIRE(cl::parse_monitor_keepalive(ka, kan, ka_out));
    CHECK(ka_out.client_id == 0x12345678u && ka_out.active_channel_mask == ka_in.active_channel_mask);

    ka[0] = 'L';
    ka[1] = 'V';
    CHECK(!cl::parse_monitor_keepalive(ka, kan, ka_out));
}
