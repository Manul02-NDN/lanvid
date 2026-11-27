// External C++17 Audio Client consuming CrossLinkSDK exclusively via find_package(CrossLinkSDK REQUIRED)
// and #include <crosslink/crosslink.hpp> (zero LanVid video headers or dependencies).

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include <crosslink/crosslink.hpp>

int main() {
    crosslink::Runtime rt;
    if (!rt.check_abi()) {
        std::fprintf(stderr, "CrossLink ABI mismatch!\n");
        return 1;
    }
    std::printf("CrossLink Audio Client | version=%s | simd=%s\n",
                rt.version().c_str(), rt.simd_info().c_str());

    std::atomic<int> midi_rx{0};
    crosslink::Receiver rx;
    rx.on_midi([&](const CL_MidiEvent& ev) {
        if (ev.status == 0x90 && ev.data1 == 60) midi_rx.fetch_add(1, std::memory_order_relaxed);
    });

    auto rcfg = crosslink::Receiver::default_config(0, 10, 8, 48000);
    rcfg.discovery_port = 15210;
    std::snprintf(rcfg.device_name, sizeof(rcfg.device_name), "ExternalAudioNode");
    if (!rx.start(rcfg)) {
        std::fprintf(stderr, "Failed to start CrossLink receiver\n");
        return 2;
    }

    const uint16_t port = rx.local_port();
    const auto nodes = crosslink::Discovery::scan(250, 15210);
    std::printf("Discovered CrossLink nodes: %zu (local port=%u)\n", nodes.size(), unsigned(port));

    crosslink::Sender tx;
    auto scfg = crosslink::Sender::default_config("127.0.0.1", port, 10, 48000);
    if (!tx.start(scfg)) {
        std::fprintf(stderr, "Failed to start CrossLink sender\n");
        return 3;
    }

    constexpr int kCh = 8;
    constexpr int kFrames = 240;
    std::vector<float> audio(kCh * kFrames, 0.0f);
    for (int f = 0; f < kFrames; ++f) {
        for (int c = 0; c < kCh; ++c) {
            audio[f * kCh + c] = 0.25f * std::sin(float(f) * 0.1f + float(c));
        }
    }

    tx.push_midi(0x90, 60, 100, 0);
    for (int i = 0; i < 10; ++i) {
        tx.push_multichannel(audio.data(), kCh, kFrames);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::vector<float> out(kCh * kFrames, 0.0f);
    int rendered = 0;
    for (int i = 0; i < 40; ++i) {
        rendered += rx.pop_multichannel(out.data(), kCh, kFrames);
        if (rendered > 0 && midi_rx.load() > 0 && rx.channel_stats(0).packets_rx > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto ch0 = rx.channel_stats(0);
    rx.stop();
    tx.stop();

    std::printf("CrossLink 8ch AoIP + MIDI loopback verified: ch0_pkts=%llu midi=%d nodes=%zu\n",
                static_cast<unsigned long long>(ch0.packets_rx), midi_rx.load(), nodes.size());
    return (ch0.packets_rx > 0 && midi_rx.load() > 0 && !nodes.empty()) ? 0 : 4;
}
