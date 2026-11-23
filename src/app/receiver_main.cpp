// LVSP Ultra-Low-Latency LAN Video + Audio Receiver
// Console telemetry + Direct3D 11 / GDI / SDL2 preview window + WASAPI low-latency audio playback.

#include <algorithm>
#include <vector>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "app/stats.hpp"
#include "codec/dct.hpp"
#include "common/clock.hpp"
#include "common/cpu_features.hpp"
#include "common/image.hpp"
#include "net/receiver.hpp"
#include "audio/shm_audio_host.hpp"
#include "platform/audio_playback.hpp"
#include "platform/preview_window.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  --bind <ip>              Local IPv4 bind address (default: 0.0.0.0)\n"
        "  --port <port>            Local UDP listen port (default: 5000)\n"
        "  --timeout <ms>           Frame assembly timeout in ms (default: 25)\n"
        "  --conceal <prev|gray>    Missing/corrupt slice concealment (default: prev)\n"
        "  --threads <n>            Decode worker threads, 0=auto (default: 0)\n"
        "  --no-simd                Force scalar codec kernels (disable AVX2/NEON)\n"
        "  --no-sync                Disable PING/PONG clock offset estimation\n"
        "  --no-audio               Disable WASAPI audio playback\n"
        "  --audio-target-ms <ms>   Audio jitter buffer target in ms (default: 12)\n"
        "  --audio-channels <1..64> Enable the 64-ch AoIP bus: per-channel telemetry + ch1/ch2 monitor (default: off)\n"
        "  --audio-latency <2..2000> AoIP playout latency in ms, phase-aligned across channels (default: 20)\n"
        "  --audio-rate <48000|96000> AoIP bus sample rate (default: 48000)\n"
        "  --fixed-latency          Fixed AoIP jitter buffer held at --audio-latency (wired Ethernet / live\n"
        "                           monitoring). Default: Adaptive Wi-Fi (grows up to 200 ms on jitter bursts)\n"
        "  --audio-device <dev>     default | virtual (default: default)\n"
        "                           virtual = deliver the phase-aligned bus to the OS virtual device \"LanVid\"\n"
        "                                     (DAW in-channels 1..N) via LanVidAudioBus_v1; implies --audio-channels 64\n"
        "  --preview, --window      Open live video preview window (D3D11/GDI on Win, SDL2 on UNIX)\n"
        "                           Hotkeys: Esc/Q=quit, F/dblclick=fullscreen, C=conceal, S=snapshot\n"
        "  --preview-backend <name> auto | d3d11 | gdi (Windows only; default: auto)\n"
        "  --win-width <px>         Initial preview window width (default: 1280)\n"
        "  --win-height <px>        Initial preview window height (default: 720)\n"
        "  --dump-ppm <prefix>      Save periodic decoded frames as <prefix>_<id>.ppm\n"
        "  --dump-every <n>         Dump every N-th received frame (default: 60)\n"
        "  --max-frames <n>         Exit after receiving N frames (default: 0=infinite)\n"
        "  --duration <sec>         Exit after S seconds (default: 0=infinite)\n"
        "  --audio-only, --no-video Headless AoIP bridge: no preview window, video packets are dropped\n"
        "                           before reassembly/decoding (implies --audio-channels 64)\n"
        "  -V, --version            Show LanVid version and exit\n"
        "  -h, --help               Show this help\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    lv::ReceiverConfig rcfg;
    bool enable_preview = false;
    bool force_gdi = false;
    bool enable_audio = true;
    int aoip_channels = 0;
    bool audio_virtual = false;
    bool audio_only = false;
    int win_w = 1280, win_h = 720;
    std::string dump_prefix;
    uint64_t dump_every = 60;
    uint64_t max_frames = 0;
    double max_duration_s = 0.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_val = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-V" || arg == "--version") {
#ifndef LANVID_VERSION_STRING
#define LANVID_VERSION_STRING "2.4.0"
#endif
            std::printf("LanVid receiver v%s (SIMD: %s)\n", LANVID_VERSION_STRING,
                        lv::dct_kernels_best().name);
            return 0;
        } else if (arg == "--bind") rcfg.bind_ip = need_val("--bind");
        else if (arg == "--port") rcfg.bind_port = uint16_t(std::atoi(need_val("--port")));
        else if (arg == "--timeout") rcfg.assembler.frame_timeout_us = uint64_t(std::atof(need_val("--timeout")) * 1000.0);
        else if (arg == "--threads") rcfg.decode_threads = uint32_t(std::atoi(need_val("--threads")));
        else if (arg == "--no-simd") lv::set_simd_disabled(true);
        else if (arg == "--no-sync") rcfg.enable_clock_sync = false;
        else if (arg == "--no-audio") enable_audio = false;
        else if (arg == "--audio-only" || arg == "--no-video") audio_only = true;
        else if (arg == "--audio-device") {
            const std::string d = need_val("--audio-device");
            if (d == "virtual") audio_virtual = true;
            else if (d != "default") {
                std::fprintf(stderr, "Invalid --audio-device: %s (expected default|virtual)\n", d.c_str());
                return 2;
            }
        } else if (arg == "--audio-channels") aoip_channels = std::atoi(need_val("--audio-channels"));
        else if (arg == "--audio-latency") rcfg.audio_latency_ms = uint32_t(std::clamp(std::atoi(need_val("--audio-latency")), 2, 2000));
        else if (arg == "--fixed-latency") rcfg.audio_adaptive_jitter = false;
        else if (arg == "--audio-rate") rcfg.audio_sample_rate = uint32_t(std::atoi(need_val("--audio-rate")) == 96000 ? 96000 : 48000);
        else if (arg == "--audio-target-ms") rcfg.audio_target_ms = std::atof(need_val("--audio-target-ms"));
        else if (arg == "--preview" || arg == "--window") enable_preview = true;
        else if (arg == "--preview-backend") {
            const std::string b = need_val("--preview-backend");
            if (b == "gdi") force_gdi = true;
            else if (b == "auto" || b == "d3d11") force_gdi = false;
            else {
                std::fprintf(stderr, "Invalid --preview-backend: %s (expected auto|d3d11|gdi)\n", b.c_str());
                return 2;
            }
        } else if (arg == "--win-width") win_w = std::atoi(need_val("--win-width"));
        else if (arg == "--win-height") win_h = std::atoi(need_val("--win-height"));
        else if (arg == "--dump-ppm") dump_prefix = need_val("--dump-ppm");
        else if (arg == "--dump-every") dump_every = std::max<uint64_t>(1, std::strtoull(need_val("--dump-every"), nullptr, 10));
        else if (arg == "--max-frames") max_frames = std::strtoull(need_val("--max-frames"), nullptr, 10);
        else if (arg == "--duration") max_duration_s = std::atof(need_val("--duration"));
        else if (arg == "--conceal") {
            const std::string m = need_val("--conceal");
            if (m == "prev") rcfg.assembler.conceal = lv::ConcealMode::kPrev;
            else if (m == "gray") rcfg.assembler.conceal = lv::ConcealMode::kGray;
            else {
                std::fprintf(stderr, "Invalid --conceal: %s (expected prev or gray)\n", m.c_str());
                return 2;
            }
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return 2;
        }
    }

    if (audio_only) {
        if (enable_preview) std::fprintf(stderr, "Note: --preview ignored in --audio-only mode.\n");
        enable_preview = false;
        dump_prefix.clear();
        rcfg.audio_only = true;
        rcfg.decode_threads = 1;  // the decoder pool stays idle
        if (aoip_channels <= 0) aoip_channels = 64;
    }

    // Decode directly to BGRA when previewing -> zero swizzle before D3D11 / GDI / SDL2 upload!
    if (enable_preview) {
        rcfg.assembler.output_order = lv::PixelOrder::kBGRA;
    }

    std::unique_ptr<lv::IPreviewWindow> preview;
    lv::AudioPlayback audio_out;
    bool audio_started = false;
    std::atomic<uint32_t> snap_seq{0};
    int stream_w = 0, stream_h = 0;

    const uint64_t t_start = lv::now_us();
    uint64_t t_first_frame = 0;
    uint64_t t_last_report = t_start;
    uint64_t rx_frames = 0;
    std::unique_ptr<lv::ReceiverStatsTracker> tracker;
    lv::UdpReceiver* rx_ptr = nullptr;

    lv::UdpReceiver receiver(rcfg, [&](const lv::CompletedFrame& cf) {
        if (!tracker) {
            t_first_frame = cf.first_arrival_us;
            t_last_report = lv::now_us();
            tracker = std::make_unique<lv::ReceiverStatsTracker>(t_first_frame);
            stream_w = cf.image->width;
            stream_h = cf.image->height;
            std::printf("Stream detected: %dx%d | codec=%s [%s] (%u slices/frame)\n", stream_w, stream_h,
                        lv::video_codec_name(cf.codec), rx_ptr->decoder_backend_name().c_str(),
                        unsigned(cf.total_slices));
            std::fflush(stdout);
        }
        const double lat_ms = rx_ptr->estimate_latency_ms(cf.complete_time_us, cf.sender_timestamp_us);
        tracker->record_frame(cf, lat_ms);
        ++rx_frames;

        if (enable_preview && preview && preview->is_open()) {
            preview->present(*cf.image);
        }

        if (!dump_prefix.empty() && (rx_frames % dump_every) == 1) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s_%06u.ppm", dump_prefix.c_str(), cf.frame_id);
            lv::write_ppm(path, *cf.image);
        }
        if (max_frames > 0 && rx_frames >= max_frames) {
            g_stop.store(true, std::memory_order_relaxed);
        }
    });
    rx_ptr = &receiver;

    if (!receiver.init()) {
        std::fprintf(stderr, "Failed to bind UDP receiver on %s:%u\n", rcfg.bind_ip.c_str(),
                     unsigned(rcfg.bind_port));
        return 1;
    }

    if (enable_preview) {
        preview = lv::create_preview_window(force_gdi);
        lv::PreviewCallbacks cbs;
        cbs.on_close = [] { g_stop.store(true, std::memory_order_relaxed); };
        cbs.on_toggle_conceal = [&receiver] {
            const auto next = (receiver.conceal_mode() == lv::ConcealMode::kPrev)
                                  ? lv::ConcealMode::kGray
                                  : lv::ConcealMode::kPrev;
            receiver.set_conceal_mode(next);
            std::printf("[Hotkey C] Concealment mode -> %s\n",
                        next == lv::ConcealMode::kPrev ? "prev" : "gray");
            std::fflush(stdout);
        };
        cbs.on_snapshot = [&snap_seq](const lv::Image& img) {
            const uint32_t id = ++snap_seq;
            char path[128];
            std::snprintf(path, sizeof(path), "snapshot_%04u.ppm", id);
            if (lv::write_ppm(path, img)) {
                std::printf("[Hotkey S] Saved %s (%dx%d)\n", path, img.width, img.height);
                std::fflush(stdout);
            }
        };
        char init_title[128];
        std::snprintf(init_title, sizeof(init_title), "LVSP Receiver — waiting on :%u",
                      unsigned(receiver.local_port()));
        if (!preview || !preview->open(win_w, win_h, init_title, std::move(cbs))) {
            std::fprintf(stderr, "Warning: failed to open preview window; continuing headless.\n");
            enable_preview = false;
            preview.reset();
        }
    }

    std::signal(SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal(SIGTERM, on_signal);
#endif

    if (audio_only) {
        std::printf("LVSP Receiver (audio-only AoIP bridge) on %s:%u | video=off (packets dropped) | bus=%d ch, %u ms (%s) @ %u Hz%s\n",
                    rcfg.bind_ip.c_str(), unsigned(receiver.local_port()), aoip_channels, rcfg.audio_latency_ms,
                    rcfg.audio_adaptive_jitter ? "AUTO" : "FIXED", rcfg.audio_sample_rate,
                    audio_virtual ? " -> virtual device" : "");
    } else std::printf(
        "LVSP Receiver listening on %s:%u | timeout=%.1f ms | conceal=%s | preview=%s | audio=%s | kernel=%s\n",
        rcfg.bind_ip.c_str(), unsigned(receiver.local_port()),
        double(rcfg.assembler.frame_timeout_us) / 1000.0,
        rcfg.assembler.conceal == lv::ConcealMode::kPrev ? "prev" : "gray",
        (enable_preview && preview) ? preview->renderer_name() : "off",
        enable_audio ? "auto" : "off", lv::dct_kernels_best().name);
    std::fflush(stdout);

    // Network -> AudioSyncMatrix -> LanVidAudioBus_v1 -> DAW inputs (the host pump is the only
    // consumer of the matrix in this mode, so the stereo monitor below is disabled).
    lv::ShmAudioHost vhost;
    if (audio_virtual) {
        if (aoip_channels <= 0) aoip_channels = 64;
        lv::ShmAudioHost::Config hc;
        hc.channels = uint32_t(std::min(aoip_channels, 64));
        if (!vhost.start(hc, nullptr, [&receiver](float* const* planes, uint32_t ch, uint32_t n) {
                receiver.pop_audio_multichannel_planar(planes, ch, n);
            })) {
            std::fprintf(stderr, "Failed to open shared audio bus %s\n", lv::kShmBusName);
            return 1;
        }
        std::printf("Virtual audio device bridge active: %d channels -> %s\n", aoip_channels, lv::kShmBusName);
    }

    uint64_t next_mon_us = lv::now_us();
    uint64_t next_aoip_report_us = next_mon_us + 1000000;
    std::vector<float> mon_f(480 * 2);
    std::vector<int16_t> mon_i(480 * 2);
    while (!g_stop.load(std::memory_order_relaxed)) {
        receiver.step_once();

        if (aoip_channels > 0) {
            const uint64_t tnow = lv::now_us();
            const uint32_t blk = rcfg.audio_sample_rate / 200;  // 5 ms
            if (tnow > next_mon_us + 200000) next_mon_us = tnow;  // stalled > 200 ms: re-anchor
            while (!audio_virtual && tnow >= next_mon_us) {  // render every due block so the cursor keeps real time
                next_mon_us += uint64_t(blk) * 1000000ull / rcfg.audio_sample_rate;
                // Stereo monitor of channels 1/2 (the matrix is rendered once; meters track all channels).
                receiver.pop_audio_multichannel_interleaved(mon_f.data(), 2, std::min<uint32_t>(blk, 480));
                if (enable_audio && rcfg.audio_sample_rate == 48000) {
                    for (uint32_t i = 0; i < blk * 2 && i < mon_i.size(); ++i)
                        mon_i[i] = int16_t(std::clamp(mon_f[i], -1.0f, 1.0f) * 32767.0f);
                    receiver.audio_buffer().push_pcm16(mon_i.data(), std::min<uint32_t>(blk, 480));
                }
            }
            if (tnow >= next_aoip_report_us) {
                next_aoip_report_us = tnow + 1000000;
                int active = 0;
                uint64_t under = 0, lost = 0, fec = 0, nack = 0;
                const int n = std::min(aoip_channels, 64);
                for (int c = 0; c < n; ++c) {
                    const auto m = receiver.audio_matrix().meter(uint16_t(c));
                    const auto rs = receiver.audio_channel_rx_stats(uint16_t(c));
                    active += m.active ? 1 : 0;
                    under += m.underruns;
                    lost += rs.packets_lost;
                    fec += rs.fec_recovered;
                    nack += rs.nack_recovered;
                }
                const auto& mc = receiver.master_clock();
                std::printf("[AUDIO64] ch active %d/%d | latency=%u ms (%s) actual buffer=%.1f ms | clk %s skew=%+.1f ppm rtt=%u us | "
                            "lost=%llu fec_rec=%llu nack_rec=%llu underruns=%llu | resample=%+.1f ppm | L1=%.1f dBFS\n",
                            active, n, receiver.audio_latency_ms(), receiver.audio_adaptive_jitter() ? "AUTO" : "FIXED",
                            receiver.audio_effective_latency_ms(), mc.synced() ? "synced" : "free-run",
                            mc.skew_ppm(), mc.min_rtt_us(), (unsigned long long)lost, (unsigned long long)fec,
                            (unsigned long long)nack, (unsigned long long)under,
                            receiver.audio_matrix().resample_ppm(),
                            double(receiver.audio_matrix().meter(0).level_dbfs));
                if (audio_virtual) {
                    vhost.publish_network_state(receiver.audio_latency_ms(), mc.synced(), mc.skew_ppm());
                    const auto vs = vhost.status();
                    std::printf("[VDEV  ] DAW %s%s%s | %u Hz buf=%u | rx_fill=%u | delivered=%llu frames | underruns=%llu\n",
                                vs.driver_connected ? "connected" : "not connected",
                                vs.driver_name.empty() ? "" : " via ", vs.driver_name.c_str(), vs.sample_rate,
                                vs.buffer_size, vs.rx_fill, (unsigned long long)vs.rx_frames,
                                (unsigned long long)vs.rx_underruns);
                }
                std::fflush(stdout);
            }
        }

        // Start WASAPI playback lazily on first received audio packet (avoids opening audio device on video-only streams).
        if (enable_audio && !audio_started && receiver.audio_stats().packets_rx > 0) {
            audio_started = true;
            if (audio_out.start(&receiver.audio_buffer())) {
                std::printf("Audio stream active -> playback on [%s] (target=%.1f ms)\n",
                            audio_out.device_name().c_str(), rcfg.audio_target_ms);
                std::fflush(stdout);
            } else {
                std::fprintf(stderr, "Warning: failed to open audio playback device.\n");
            }
        }

        const uint64_t now = lv::now_us();
        if (max_duration_s > 0.0 && double(now - t_start) / 1e6 >= max_duration_s) break;

        if (tracker && now - t_last_report >= 1000000) {
            const auto cs = receiver.clock_sync();
            const auto ast = receiver.audio_stats();
            const auto rep =
                tracker->snapshot_interval(now, receiver.assembler_stats(), receiver.seq_tracker(),
                                           cs.synced, cs.min_rtt_us, &ast);
            std::printf("%s\n", lv::ReceiverStatsTracker::format_line(rep).c_str());
            std::fflush(stdout);

            if (enable_preview && preview && preview->is_open()) {
                char hud[320];
                int n = std::snprintf(
                    hud, sizeof(hud),
                    "LVSP [%s | %s] %dx%d | %.1f FPS | %.2f Mbps | Lat p50:%.2fms | Loss:%.2f%% | FEC:%llu | PLI:%llu",
                    preview->renderer_name(), receiver.decoder_backend_name().c_str(), stream_w, stream_h,
                    rep.fps, rep.bitrate_mbps, rep.lat_p50_ms, rep.pkt_loss_pct,
                    (unsigned long long)rep.fec_recovered, (unsigned long long)rep.pli_requests);
                if (rep.has_audio && n > 0 && size_t(n) < sizeof(hud)) {
                    std::snprintf(hud + n, sizeof(hud) - size_t(n), " | Aud:%.0fkbps(%.1fms)",
                                  rep.audio_kbps, rep.audio_buf_ms);
                }
                preview->set_title(hud);
            }
            t_last_report = now;
        }
    }

    vhost.stop();
    audio_out.stop();
    if (preview) preview->close();
    receiver.flush();
    if (tracker) {
        const auto cs = receiver.clock_sync();
        const auto ast = receiver.audio_stats();
        const auto tot = tracker->snapshot_total(lv::now_us(), receiver.assembler_stats(),
                                                 receiver.seq_tracker(), cs.synced, cs.min_rtt_us, &ast);
        std::printf("%s\n", lv::ReceiverStatsTracker::format_line(tot, "TOTAL ").c_str());
    } else {
        if (audio_only) {
            uint64_t pk = 0, lost = 0;
            for (int c = 0; c < std::min(aoip_channels, 64); ++c) {
                const auto st = receiver.audio_channel_rx_stats(uint16_t(c));
                pk += st.packets_rx;
                lost += st.packets_lost;
            }
            std::printf("[TOTAL ] audio-only: AUDIO2 packets=%llu lost=%llu | video packets ignored=%llu\n",
                        (unsigned long long)pk, (unsigned long long)lost,
                        (unsigned long long)receiver.video_packets_ignored());
        } else {
            std::printf("No frames received.\n");
        }
    }
    return 0;
}
