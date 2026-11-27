#include "mini_test.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "sources/file_source.hpp"
#include "sources/test_pattern.hpp"
#include "audio/stereo_audio_buffer.hpp"
#include "common/clock.hpp"
#include "common/image.hpp"
#include "net/packet.hpp"
#include "net/video_receiver.hpp"
#include "net/video_sender.hpp"

TEST(audio_packet_header_roundtrip) {
    CHECK(uint8_t(lv::PacketType::kAudio) == 3);

    // 5 ms chunk at 48 kHz Stereo PCM16 = 240 frames = 960 bytes.
    constexpr uint16_t kFrames = 240;
    int16_t pcm[kFrames * 2];
    for (int i = 0; i < kFrames; ++i) {
        pcm[i * 2 + 0] = int16_t(i * 100 - 12000);
        pcm[i * 2 + 1] = int16_t(-i * 100 + 12000);
    }

    uint8_t payload[lv::kAudioHeaderSize + sizeof(pcm)];
    lv::AudioHeader ah{};
    ah.sample_rate = 48000;
    ah.channels = 2;
    ah.sample_format = lv::AudioSampleFormat::kPcm16;
    ah.num_frames = kFrames;
    lv::write_audio_header(payload, ah);
    std::memcpy(payload + lv::kAudioHeaderSize, pcm, sizeof(pcm));

    lv::PacketHeader h{};
    h.type = lv::PacketType::kAudio;
    h.flags = lv::kFlagHasCrc;
    h.frame_id = 777;
    h.timestamp_us = 1234567;
    h.seq = 777;

    uint8_t dgram[1400];
    const size_t n = lv::write_packet(dgram, sizeof(dgram), h, payload, sizeof(payload));
    REQUIRE(n == lv::kPacketHeaderSize + lv::kAudioHeaderSize + sizeof(pcm));
    CHECK(n <= lv::kDefaultMtu);  // fits in a single UDP packet without fragmentation!

    lv::PacketHeader oh{};
    const uint8_t* opl = nullptr;
    REQUIRE(lv::parse_packet(dgram, n, oh, &opl) == lv::PacketParseStatus::kOk);
    CHECK(oh.type == lv::PacketType::kAudio);
    CHECK(oh.frame_id == 777);
    CHECK(oh.timestamp_us == 1234567);

    lv::AudioHeader oah{};
    REQUIRE(lv::parse_audio_header(opl, oh.payload_size, oah));
    CHECK(oah.sample_rate == 48000);
    CHECK(oah.channels == 2);
    CHECK(oah.sample_format == lv::AudioSampleFormat::kPcm16);
    CHECK(oah.num_frames == kFrames);
    CHECK(std::memcmp(opl + lv::kAudioHeaderSize, pcm, sizeof(pcm)) == 0);
}

TEST(audio_jitter_buffer_lockfree_and_drift) {
    lv::AudioJitterBuffer jb(48000, 12.0);  // target = 576 frames (12 ms)

    // 1. Pre-buffering: below 12 ms outputs silence without advancing read pointer.
    int16_t chunk[256 * 2];
    for (int i = 0; i < 256; ++i) {
        chunk[i * 2 + 0] = int16_t(1000 + i);
        chunk[i * 2 + 1] = int16_t(-1000 - i);
    }
    lv::AudioHeader ah{48000, 2, lv::AudioSampleFormat::kPcm16, 240};
    jb.push_packet(0, ah, reinterpret_cast<const uint8_t*>(chunk), 996);  // 5 ms
    jb.push_packet(1, ah, reinterpret_cast<const uint8_t*>(chunk), 996);  // 10 ms (< 12 ms target)

    int16_t out[240 * 2];
    jb.pop_pcm16(out, 240);
    bool all_zero = true;
    for (int16_t v : out) if (v != 0) all_zero = false;
    CHECK(all_zero);

    // 3rd packet crosses 12 ms -> starts playing exact samples!
    jb.push_packet(2, ah, reinterpret_cast<const uint8_t*>(chunk), 996);  // 15 ms
    jb.pop_pcm16(out, 240);
    CHECK(std::memcmp(out, chunk, sizeof(out)) == 0);

    // 2. Packet gap detection: seq 2 -> seq 5 counts 2 lost packets.
    jb.push_packet(5, ah, reinterpret_cast<const uint8_t*>(chunk), 996);
    CHECK(jb.stats().packets_lost == 2);

    // 3. Positive clock drift (producer sends slightly faster than consumer): triggers drift_drops!
    for (int iter = 0; iter < 80; ++iter) {
        jb.push_pcm16(chunk, 245);  // +2% fast
        jb.pop_pcm16(out, 240);
    }
    CHECK(jb.stats().drift_drops > 0);
    CHECK(jb.buffered_ms() <= 35.0);

    // 4. Negative clock drift (producer sends slightly slower than consumer): triggers drift_dups!
    jb.reset();
    jb.push_pcm16(chunk, 240);
    jb.push_pcm16(chunk, 240);
    jb.push_pcm16(chunk, 120);  // prime 12.5 ms (600 frames)
    for (int iter = 0; iter < 120; ++iter) {
        jb.pop_pcm16(out, 240);
        jb.push_pcm16(chunk, 238);  // -0.83% slow -> drift_dups compensates without underrun
    }
    CHECK(jb.stats().drift_dups > 0);

    // 5. Float32 packet input conversion.
    jb.reset();
    float f32[240 * 2];
    for (int i = 0; i < 240; ++i) {
        f32[i * 2 + 0] = 0.5f;
        f32[i * 2 + 1] = -0.5f;
    }
    lv::AudioHeader ah_f{48000, 2, lv::AudioSampleFormat::kFloat32, 240};
    for (uint32_t s = 0; s < 3; ++s) jb.push_packet(s, ah_f, reinterpret_cast<const uint8_t*>(f32), 1956);
    jb.pop_pcm16(out, 240);
    CHECK(std::abs(int(out[0]) - 16384) <= 1);
    CHECK(std::abs(int(out[1]) + 16384) <= 1);
}

TEST(file_video_source_ppm_and_y4m) {
    // 1. PPM roundtrip
    lv::Image orig;
    orig.allocate(64, 48, lv::PixelOrder::kRGBA);
    lv::generate_test_frame(orig, 5);
    const char* ppm_path = "lv_test_src.ppm";
    REQUIRE(lv::write_ppm(ppm_path, orig));

    lv::FileVideoSource fsrc;
    REQUIRE(fsrc.open(ppm_path, 0, 0, false));
    CHECK(fsrc.width() == 64 && fsrc.height() == 48);
    lv::Image loaded;
    loaded.allocate(64, 48, lv::PixelOrder::kRGBA);
    REQUIRE(fsrc.read_frame(loaded));
    CHECK(loaded.data == orig.data);
    fsrc.close();
    std::remove(ppm_path);

    // 2. Y4M (YUV4MPEG2 4:2:0) roundtrip
    const char* y4m_path = "lv_test_src.y4m";
    {
        std::ofstream out(y4m_path, std::ios::binary);
        out << "YUV4MPEG2 W32 H16 F60:1 Ip A1:1 C420jpeg\n";
        out << "FRAME\n";
        std::vector<uint8_t> y(32 * 16, 200);
        std::vector<uint8_t> cb(16 * 8, 128);
        std::vector<uint8_t> cr(16 * 8, 128);
        out.write(reinterpret_cast<const char*>(y.data()), std::streamsize(y.size()));
        out.write(reinterpret_cast<const char*>(cb.data()), std::streamsize(cb.size()));
        out.write(reinterpret_cast<const char*>(cr.data()), std::streamsize(cr.size()));
    }
    REQUIRE(fsrc.open(y4m_path, 0, 0, false));
    CHECK(fsrc.width() == 32 && fsrc.height() == 16);
    CHECK(std::abs(fsrc.fps() - 60.0) < 0.01);
    lv::Image y4m_img;
    y4m_img.allocate(32, 16, lv::PixelOrder::kBGRA);
    REQUIRE(fsrc.read_frame(y4m_img));
    CHECK(std::abs(int(y4m_img.row(0)[0]) - 200) <= 1);
    CHECK(std::abs(int(y4m_img.row(0)[1]) - 200) <= 1);
    CHECK(std::abs(int(y4m_img.row(0)[2]) - 200) <= 1);
    // Looping on EOF must succeed cleanly!
    REQUIRE(fsrc.read_frame(y4m_img));
    fsrc.close();
    std::remove(y4m_path);
}

TEST(udp_loopback_av_simultaneous) {
    // Streams video slices AND 5ms audio packets over the exact same UDP socket;
    // verifies 0% video loss (audio seq never pollutes video seq) AND 100% audio delivery.
    lv::ReceiverConfig rcfg;
    rcfg.bind_ip = "127.0.0.1";
    rcfg.bind_port = 0;
    rcfg.recv_timeout_ms = 5;
    rcfg.decode_threads = 4;

    std::atomic<int> got_video{0};
    lv::UdpReceiver rx(rcfg, [&](const lv::CompletedFrame&) {
        got_video.fetch_add(1, std::memory_order_release);
    });
    REQUIRE(rx.init());

    std::atomic<bool> stop{false};
    std::thread rx_thread([&] { rx.run(stop); });

    lv::SenderConfig scfg;
    scfg.dest_ip = "127.0.0.1";
    scfg.dest_port = rx.local_port();
    scfg.threads = 4;
    lv::UdpSender tx(scfg);
    REQUIRE(tx.init());

    lv::CodecParams cp;
    cp.width = 320;
    cp.height = 180;
    cp.quality = 80;
    lv::Image img;
    img.allocate(cp.width, cp.height);

    int16_t pcm[240 * 2];
    for (int i = 0; i < 240; ++i) pcm[i * 2] = pcm[i * 2 + 1] = int16_t(i * 50);

    uint32_t aseq = 0;
    for (int f = 0; f < 6; ++f) {
        lv::generate_test_frame(img, uint32_t(f));
        REQUIRE(tx.send_frame(img, cp, uint32_t(f), lv::now_us32()));
        for (int a = 0; a < 3; ++a) {
            REQUIRE(tx.send_audio(aseq++, lv::now_us32(), pcm, 240, 48000, 2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (int i = 0; i < 100 && (got_video.load() < 6 || rx.audio_stats().packets_rx < 18); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true, std::memory_order_release);
    rx_thread.join();

    CHECK(got_video.load() == 6);
    CHECK(rx.assembler_stats().frames_complete == 6);
    CHECK(rx.seq_tracker().lost(0) == 0);
    CHECK(rx.audio_stats().packets_rx == 18);
    CHECK(rx.audio_stats().packets_lost == 0);
}
