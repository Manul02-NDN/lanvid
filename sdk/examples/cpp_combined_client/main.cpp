// Combined C++17 Client linking BOTH LanVidSDK (Video + Stereo) AND CrossLinkSDK (64ch AoIP + MIDI)
// simultaneously in one binary to prove complete ABI/symbol isolation and concurrent operation.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <crosslink/crosslink.hpp>
#include <lanvid/lanvid.hpp>

int main() {
    lanvid::Runtime lv_rt;
    crosslink::Runtime cl_rt;
    if (!lv_rt.check_abi() || !cl_rt.check_abi()) {
        std::fprintf(stderr, "ABI mismatch!\n");
        return 1;
    }
    std::printf("Combined Client | LanVid v%s + CrossLink v%s\n",
                lv_rt.version().c_str(), cl_rt.version().c_str());

    // 1. LanVid Video + Stereo loopback
    std::atomic<int> video_rx{0};
    lanvid::Receiver vrx;
    vrx.on_video([&](const LV_VideoFrameView& f) {
        if (f.width == 128 && f.height == 64) video_rx.fetch_add(1, std::memory_order_relaxed);
    });
    auto vrcfg = lanvid::Receiver::default_config(0, LV_PIXEL_FORMAT_BGRA8888);
    vrcfg.enable_discovery = 0;
    if (!vrx.start(vrcfg)) return 2;

    lanvid::Sender vtx;
    auto vscfg = lanvid::Sender::default_config("127.0.0.1", vrx.local_port());
    vscfg.width = 128;
    vscfg.height = 64;
    if (!vtx.start(vscfg)) return 3;

    // 2. CrossLink AoIP + MIDI loopback running concurrently in the same process
    std::atomic<int> midi_rx{0};
    crosslink::Receiver arx;
    arx.on_midi([&](const CL_MidiEvent&) { midi_rx.fetch_add(1, std::memory_order_relaxed); });
    auto arcfg = crosslink::Receiver::default_config(0, 10, 4, 48000);
    arcfg.enable_discovery = 0;
    if (!arx.start(arcfg)) return 4;

    crosslink::Sender atx;
    auto ascfg = crosslink::Sender::default_config("127.0.0.1", arx.local_port(), 10, 48000);
    if (!atx.start(ascfg)) return 5;

    std::vector<uint8_t> bgra(128 * 64 * 4, 180);
    std::vector<float> pcm(4 * 240, 0.2f);
    atx.push_midi(0x90, 64, 110, 0);
    for (int i = 0; i < 4; ++i) {
        vtx.push_frame(bgra.data(), 128, 64);
        atx.push_multichannel(pcm.data(), 4, 240);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (int i = 0; i < 60 && (video_rx.load() < 4 || midi_rx.load() < 1 || arx.channel_stats(0).packets_rx == 0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const uint64_t ch0_pkts = arx.channel_stats(0).packets_rx;
    vtx.stop(); vrx.stop();
    atx.stop(); arx.stop();

    std::printf("Combined verification: video_frames=%d/4 aoip_ch0_pkts=%llu midi=%d\n",
                video_rx.load(), static_cast<unsigned long long>(ch0_pkts), midi_rx.load());
    return (video_rx.load() == 4 && ch0_pkts > 0 && midi_rx.load() >= 1) ? 0 : 6;
}
