// External C++17 Video Client consuming LanVidSDK exclusively via find_package(LanVidSDK REQUIRED)
// and #include <lanvid/lanvid.hpp> (zero CrossLink headers or dependencies).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <lanvid/lanvid.hpp>

int main() {
    lanvid::Runtime rt;
    if (!rt.check_abi()) {
        std::fprintf(stderr, "LanVid ABI mismatch!\n");
        return 1;
    }
    std::printf("LanVid Video Client | version=%s | simd=%s\n",
                rt.version().c_str(), rt.simd_info().c_str());

    std::atomic<int> got_frames{0};
    std::atomic<int> got_stereo{0};

    lanvid::Receiver rx;
    rx.on_video([&](const LV_VideoFrameView& f) {
        if (f.width == 160 && f.height == 96) got_frames.fetch_add(1, std::memory_order_relaxed);
    });
    rx.on_audio([&](const LV_AudioChunkView& a) {
        if (a.num_frames == 240 && a.channels == 2) got_stereo.fetch_add(1, std::memory_order_relaxed);
    });

    auto rcfg = lanvid::Receiver::default_config(0, LV_PIXEL_FORMAT_BGRA8888);
    rcfg.discovery_port = 15209;
    std::snprintf(rcfg.device_name, sizeof(rcfg.device_name), "ExternalVideoClient");
    if (!rx.start(rcfg)) {
        std::fprintf(stderr, "Failed to start LanVid receiver\n");
        return 2;
    }

    const uint16_t port = rx.local_port();
    const auto peers = lanvid::Discovery::scan(250, 15209);
    std::printf("Discovered LanVid peers: %zu (local port=%u)\n", peers.size(), unsigned(port));

    lanvid::Sender tx;
    auto scfg = lanvid::Sender::default_config("127.0.0.1", port);
    scfg.width = 160;
    scfg.height = 96;
    scfg.quality = 82;
    scfg.fec_group_size = 6;
    if (!tx.start(scfg)) {
        std::fprintf(stderr, "Failed to start LanVid sender\n");
        return 3;
    }

    std::vector<uint8_t> bgra(160 * 96 * 4, 210);
    std::vector<int16_t> stereo_pcm(240 * 2, 1234);
    for (int i = 0; i < 4; ++i) {
        tx.push_frame(bgra.data(), 160, 96);
        tx.push_audio(stereo_pcm.data(), 240, 48000, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (int i = 0; i < 60 && (got_frames.load() < 4 || got_stereo.load() < 4); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    rx.stop();
    tx.stop();

    std::printf("LanVid Video+Stereo loopback verified: frames=%d/4 stereo_chunks=%d/4\n",
                got_frames.load(), got_stereo.load());
    return (got_frames.load() == 4 && got_stereo.load() == 4 && !peers.empty()) ? 0 : 4;
}
