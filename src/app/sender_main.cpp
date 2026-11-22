// LVSP Ultra-Low-Latency LAN Video + Audio Sender
// Sources: pattern | screen (DXGI/GDI) | camera (Media Foundation) | file (Y4M/PPM/raw RGB/RGBA)
// Audio:   none | loopback (WASAPI system audio) | mic (WASAPI microphone)

#include <algorithm>
#include <thread>
#include <cmath>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/file_source.hpp"
#include "app/test_pattern.hpp"
#include "codec/dct.hpp"
#include "codec/slice_format.hpp"
#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "common/cpu_features.hpp"
#include "common/image.hpp"
#include "net/sender.hpp"
#include "audio/shm_audio_host.hpp"
#include "platform/audio_capture.hpp"
#include "platform/camera_capture.hpp"
#include "platform/screen_capture.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

enum class SourceKind { kPattern, kScreen, kCamera, kFile };

void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  --host <ip>              Destination IPv4 address (default: 127.0.0.1)\n"
        "  --port <port>            Destination UDP port (default: 5000)\n"
        "  --source <kind>          pattern | screen | camera | file (default: pattern)\n"
        "  --monitor <idx>          Monitor index for --source screen (default: 0)\n"
        "  --capture-backend <name> auto | dxgi | gdi for screen capture (default: auto)\n"
        "  --device <idx>           Camera index for --source camera (default: 0)\n"
        "  --audio <mode>           none | loopback | mic | tone (default: none)\n"
        "                           tone = N-channel test bus (see --audio-channels)\n"
        "  --audio-channels <1..64> Channels of the 64-ch AoIP bus for --audio tone (default: 2)\n"
        "  --audio-latency <2..2000> Latency budget in ms: picks chunk size / FEC / NACK profile (default: 20)\n"
        "  --audio-rate <48000|96000> Sample rate of the AoIP bus (default: 48000)\n"
        "  --audio-device <dev>     default | test | virtual | <idx>  (default: default)\n"
        "                           test    = N-channel tone bus (same as --audio tone)\n"
        "                           virtual = stream the OS virtual device \"LanVid\" (ASIO / CoreAudio /\n"
        "                                     JACK) out-channels 1..N from any DAW via LanVidAudioBus_v1\n"
        "                           <idx>   = WASAPI endpoint index for --audio loopback|mic\n"
        "  --audio-ms <2.5|5>       Audio packet duration in ms (default: 5)\n"
        "  --list-devices           List monitors, cameras, and audio devices and exit\n"
        "  --input <file>           Input file: .y4m, .ppm (P6), or raw RGB24/RGBA32 (implies --source file)\n"
        "  --input-fmt <rgb|rgba>   Pixel format for raw files (default: rgb)\n"
        "  --width <px>             Frame width (default: 1920; native for screen/camera/y4m/ppm if omitted)\n"
        "  --height <px>            Frame height (default: 1080; native if omitted)\n"
        "  --fps <n>                Target frame rate (default: 60)\n"
        "  --codec <dct|h264|h265>  Video codec: dct (~2ms), h264, h265 (default: dct)\n"
        "  --bitrate <kbps>         Target bitrate in kbps for h264/h265 (default: 8000)\n"
        "  --keyint <frames>        IDR keyframe interval for h264/h265 (default: 120)\n"
        "  --quality <1-100>        Quantization quality factor for dct (default: 75)\n"
        "  --chroma <420|422|444>   Chroma subsampling (default: 420)\n"
        "  --slice-height <n>       Slice height in luma rows (default: 16)\n"
        "  --mtu <bytes>            Max UDP datagram size (default: 1400)\n"
        "  --threads <n>            Worker thread count, 0=auto (default: 0)\n"
        "  --no-simd                Force scalar codec kernels (disable AVX2/NEON)\n"
        "  --no-crc                 Disable per-packet CRC32C\n"
        "  --fec <0|1>              Enable slice-level XOR FEC (1 -> group size 6; default: 0)\n"
        "  --fec-group <n>          XOR FEC group size (0=off, e.g. 6 or 8)\n"
        "  --adaptive-quality       Enable AIMD adaptive quality from receiver feedback\n"
        "  --q-min <1-100>          Min quality for AIMD (default: 35)\n"
        "  --q-max <1-100>          Max quality for AIMD (default: 85)\n"
        "  --loss <pct>             Simulate random video packet loss %% (default: 0)\n"
        "  --loss-off-after <sec>   Turn simulated loss off after S seconds (for AIMD recovery tests)\n"
        "  --pace <mbps>            Optional token-bucket pacing in Mbps (default: 0=off)\n"
        "  --frames <n>             Stop after N frames (default: 0=infinite)\n"
        "  --duration <sec>         Stop after S seconds (default: 0=infinite)\n"
        "  -V, --version            Show LanVid version and exit\n"
        "  -h, --help               Show this help\n",
        prog);
}

void print_devices() {
    const auto mons = lv::list_monitors();
    std::printf("Monitors (%zu):\n", mons.size());
    for (const auto& m : mons) {
        std::printf("  [%d] %s  %dx%d at (%d,%d)%s\n", m.index, m.name.c_str(), m.width, m.height,
                    m.left, m.top, m.primary ? " [primary]" : "");
    }
    const auto cams = lv::list_cameras();
    std::printf("Cameras (%zu):\n", cams.size());
    for (const auto& c : cams) {
        std::printf("  [%d] %s\n", c.index, c.name.c_str());
    }
    const auto loop_devs = lv::list_audio_devices(true);
    std::printf("Audio Loopback / Render Devices (%zu):\n", loop_devs.size());
    for (const auto& d : loop_devs) {
        std::printf("  [%d] %s%s\n", d.index, d.name.c_str(), d.is_default ? " [default]" : "");
    }
    const auto mic_devs = lv::list_audio_devices(false);
    std::printf("Audio Microphone / Capture Devices (%zu):\n", mic_devs.size());
    for (const auto& d : mic_devs) {
        std::printf("  [%d] %s%s\n", d.index, d.name.c_str(), d.is_default ? " [default]" : "");
    }
    const auto codecs = lv::get_supported_codecs();
    std::printf("Video Codecs (%zu):\n", codecs.size());
    for (const auto& c : codecs) {
        std::printf("  [%d] %-10s enc=%s [%s] | dec=%s [%s]\n", int(c.codec), c.name.c_str(),
                    c.encoder_available ? (c.encoder_hardware ? "HW" : "SW") : "N/A",
                    c.encoder_backend.c_str(),
                    c.decoder_available ? (c.decoder_hardware ? "HW" : "SW") : "N/A",
                    c.decoder_backend.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    lv::SenderConfig scfg;
    lv::CodecParams cp;
    double fps = 60.0;
    bool fps_explicit = false;
    bool width_explicit = false;
    bool height_explicit = false;
    bool quality_explicit = false;
    uint64_t max_frames = 0;
    double max_duration_s = 0.0;
    double loss_off_after_s = 0.0;

    SourceKind src_kind = SourceKind::kPattern;
    int monitor_idx = 0;
    lv::CaptureBackend cap_backend = lv::CaptureBackend::kAuto;
    int camera_idx = 0;
    std::string input_path;
    bool input_rgba = false;

    lv::AudioCaptureMode audio_mode = lv::AudioCaptureMode::kNone;
    int audio_device_idx = -1;
    double audio_chunk_ms = 5.0;
    bool audio_tone = false;
    bool audio_virtual = false;
    int audio_channels = 2;
    int audio_latency_ms = 20;
    int audio_rate = 48000;

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
            std::printf("LanVid sender v%s (SIMD: %s)\n", LANVID_VERSION_STRING,
                        lv::dct_kernels_best().name);
            return 0;
        } else if (arg == "--list-devices") {
            print_devices();
            return 0;
        } else if (arg == "--host") scfg.dest_ip = need_val("--host");
        else if (arg == "--port") scfg.dest_port = uint16_t(std::atoi(need_val("--port")));
        else if (arg == "--width") { cp.width = std::atoi(need_val("--width")); width_explicit = true; }
        else if (arg == "--height") { cp.height = std::atoi(need_val("--height")); height_explicit = true; }
        else if (arg == "--fps") { fps = std::atof(need_val("--fps")); fps_explicit = true; }
        else if (arg == "--codec") {
            const std::string c = need_val("--codec");
            if (c == "dct" || c == "lanvid_dct") scfg.codec = lv::VideoCodecId::kLanVidDct;
            else if (c == "h264" || c == "avc") scfg.codec = lv::VideoCodecId::kH264;
            else if (c == "h265" || c == "hevc") scfg.codec = lv::VideoCodecId::kH265;
            else {
                std::fprintf(stderr, "Invalid --codec: %s (expected dct|h264|h265)\n", c.c_str());
                return 2;
            }
        } else if (arg == "--bitrate") {
            scfg.bitrate_kbps = uint32_t(std::clamp(std::atoi(need_val("--bitrate")), 250, 100000));
        } else if (arg == "--keyint") {
            scfg.keyframe_interval_frames = uint32_t(std::clamp(std::atoi(need_val("--keyint")), 1, 600));
        }
        else if (arg == "--quality") { cp.quality = std::atoi(need_val("--quality")); quality_explicit = true; }
        else if (arg == "--slice-height") cp.slice_height = std::atoi(need_val("--slice-height"));
        else if (arg == "--mtu") scfg.mtu = size_t(std::atoi(need_val("--mtu")));
        else if (arg == "--threads") scfg.threads = uint32_t(std::atoi(need_val("--threads")));
        else if (arg == "--no-simd") lv::set_simd_disabled(true);
        else if (arg == "--no-crc") scfg.enable_crc = false;
        else if (arg == "--fec") scfg.fec_group_size = std::atoi(need_val("--fec")) ? 6 : 0;
        else if (arg == "--fec-group") scfg.fec_group_size = uint16_t(std::max(0, std::atoi(need_val("--fec-group"))));
        else if (arg == "--adaptive-quality") scfg.adaptive.enabled = true;
        else if (arg == "--q-min") scfg.adaptive.q_min = std::atoi(need_val("--q-min"));
        else if (arg == "--q-max") scfg.adaptive.q_max = std::atoi(need_val("--q-max"));
        else if (arg == "--loss") scfg.sim_loss_pct = std::atof(need_val("--loss"));
        else if (arg == "--loss-off-after") loss_off_after_s = std::atof(need_val("--loss-off-after"));
        else if (arg == "--pace") scfg.pace_mbps = std::atof(need_val("--pace"));
        else if (arg == "--frames") max_frames = std::strtoull(need_val("--frames"), nullptr, 10);
        else if (arg == "--duration") max_duration_s = std::atof(need_val("--duration"));
        else if (arg == "--monitor") monitor_idx = std::atoi(need_val("--monitor"));
        else if (arg == "--device") camera_idx = std::atoi(need_val("--device"));
        else if (arg == "--audio-device") {
            const std::string d = need_val("--audio-device");
            if (d == "virtual") audio_virtual = true;
            else if (d == "test") audio_tone = true;
            else if (d == "default") audio_device_idx = -1;
            else audio_device_idx = std::atoi(d.c_str());
        }
        else if (arg == "--audio-ms") audio_chunk_ms = std::atof(need_val("--audio-ms"));
        else if (arg == "--input") { input_path = need_val("--input"); src_kind = SourceKind::kFile; }
        else if (arg == "--input-fmt") input_rgba = (std::string(need_val("--input-fmt")) == "rgba");
        else if (arg == "--audio-channels") audio_channels = std::atoi(need_val("--audio-channels"));
        else if (arg == "--audio-latency") audio_latency_ms = std::atoi(need_val("--audio-latency"));
        else if (arg == "--audio-rate") audio_rate = std::atoi(need_val("--audio-rate"));
        else if (arg == "--audio") {
            const std::string a = need_val("--audio");
            if (a == "none") audio_mode = lv::AudioCaptureMode::kNone;
            else if (a == "tone") audio_tone = true;
            else if (a == "loopback") audio_mode = lv::AudioCaptureMode::kLoopback;
            else if (a == "mic") audio_mode = lv::AudioCaptureMode::kMic;
            else {
                std::fprintf(stderr, "Invalid --audio: %s (expected none|loopback|mic|tone)\n", a.c_str());
                return 2;
            }
        } else if (arg == "--source") {
            const std::string s = need_val("--source");
            if (s == "pattern") src_kind = SourceKind::kPattern;
            else if (s == "screen") src_kind = SourceKind::kScreen;
            else if (s == "camera") src_kind = SourceKind::kCamera;
            else if (s == "file") src_kind = SourceKind::kFile;
            else {
                std::fprintf(stderr, "Invalid --source: %s (expected pattern|screen|camera|file)\n", s.c_str());
                return 2;
            }
        } else if (arg == "--capture-backend") {
            const std::string b = need_val("--capture-backend");
            if (b == "auto") cap_backend = lv::CaptureBackend::kAuto;
            else if (b == "dxgi") cap_backend = lv::CaptureBackend::kDxgi;
            else if (b == "gdi") cap_backend = lv::CaptureBackend::kGdi;
            else {
                std::fprintf(stderr, "Invalid --capture-backend: %s (expected auto|dxgi|gdi)\n", b.c_str());
                return 2;
            }
        } else if (arg == "--chroma") {
            const std::string c = need_val("--chroma");
            if (c == "420") cp.chroma = lv::ChromaFormat::k420;
            else if (c == "422") cp.chroma = lv::ChromaFormat::k422;
            else if (c == "444") cp.chroma = lv::ChromaFormat::k444;
            else {
                std::fprintf(stderr, "Invalid --chroma: %s (expected 420, 422, 444)\n", c.c_str());
                return 2;
            }
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return 2;
        }
    }

    // Initialize video source.
    lv::ScreenCapture screen;
    lv::CameraCapture camera;
    lv::FileVideoSource file_src;
    std::string source_desc = "pattern";
    lv::PixelOrder pixel_order = lv::PixelOrder::kRGBA;

    if (src_kind == SourceKind::kScreen) {
        if (!screen.open(monitor_idx, cap_backend)) {
            std::fprintf(stderr, "Failed to open screen capture on monitor %d. Run --list-devices.\n",
                         monitor_idx);
            return 1;
        }
        if (!width_explicit) cp.width = screen.native_width();
        if (!height_explicit) cp.height = screen.native_height();
        pixel_order = lv::PixelOrder::kBGRA;
        source_desc = std::string("screen[") + std::to_string(monitor_idx) + "," +
                      screen.active_backend_name() + "]";
    } else if (src_kind == SourceKind::kCamera) {
        if (!fps_explicit) fps = 30.0;
        const int req_w = width_explicit ? cp.width : 1280;
        const int req_h = height_explicit ? cp.height : 720;
        if (!camera.open(camera_idx, req_w, req_h, int(fps + 0.5))) {
            std::fprintf(stderr, "Failed to open camera device %d. Run --list-devices.\n", camera_idx);
            return 1;
        }
        if (!width_explicit) cp.width = camera.native_width();
        if (!height_explicit) cp.height = camera.native_height();
        pixel_order = lv::PixelOrder::kBGRA;
        source_desc = std::string("camera[") + std::to_string(camera_idx) + ":" + camera.device_name() + "]";
    } else if (src_kind == SourceKind::kFile) {
        if (input_path.empty()) {
            std::fprintf(stderr, "--source file requires --input <path>\n");
            return 2;
        }
        if (!file_src.open(input_path, cp.width, cp.height, input_rgba)) {
            std::fprintf(stderr, "Failed to open input file: %s\n", input_path.c_str());
            return 1;
        }
        if (!width_explicit) cp.width = file_src.width();
        if (!height_explicit) cp.height = file_src.height();
        if (!fps_explicit && file_src.fps() > 0.0) fps = file_src.fps();
        source_desc = std::string("file[") + file_src.format_name() + "]";
    }

    lv::Geometry geom;
    if (!lv::compute_geometry(cp, geom) || fps <= 0.0) {
        std::fprintf(stderr, "Invalid codec parameters (%dx%d) or FPS (%.1f).\n", cp.width, cp.height, fps);
        return 1;
    }

    if (audio_channels < 1 || audio_channels > 64 || audio_latency_ms < 2 || audio_latency_ms > 2000 ||
        (audio_rate != 48000 && audio_rate != 96000)) {
        std::fprintf(stderr, "Invalid audio bus options: --audio-channels 1..64, --audio-latency 2..2000, --audio-rate 48000|96000\n");
        return 2;
    }
    scfg.audio_sample_rate = uint32_t(audio_rate);
    scfg.audio_latency_ms = uint32_t(audio_latency_ms);
    scfg.fps = fps;
    scfg.adaptive.initial_bitrate_kbps = scfg.bitrate_kbps;
    if (scfg.adaptive.enabled) {
        scfg.adaptive.initial_q = quality_explicit ? cp.quality : scfg.adaptive.q_max;
        cp.quality = std::clamp(scfg.adaptive.initial_q, scfg.adaptive.q_min, scfg.adaptive.q_max);
    } else {
        scfg.adaptive.initial_q = cp.quality;
    }

    lv::UdpSender sender(scfg);
    if (!sender.init()) {
        std::fprintf(stderr, "Failed to initialize UDP sender to %s:%u\n", scfg.dest_ip.c_str(),
                     unsigned(scfg.dest_port));
        return 1;
    }
    if (scfg.codec != lv::VideoCodecId::kLanVidDct) {
        if (!sender.prepare_encoder(cp.width, cp.height)) {
            std::fprintf(stderr, "Failed to initialize %s video encoder for %dx%d\n",
                         lv::video_codec_name(scfg.codec), cp.width, cp.height);
            return 1;
        }
    }

    // Optional WASAPI audio stream on the same UDP socket.
    lv::AudioCapture audio_cap;
    std::string audio_desc = "off";
    if (audio_mode != lv::AudioCaptureMode::kNone) {
        const uint16_t chunk_frames = uint16_t(std::clamp(int(48000.0 * audio_chunk_ms / 1000.0 + 0.5), 120, 480));
        if (audio_cap.start(audio_mode, audio_device_idx, chunk_frames,
                            [&sender](uint32_t seq_id, uint32_t ts_us, const int16_t* pcm16, uint16_t nf) {
                                sender.send_audio(seq_id, ts_us, pcm16, nf, 48000, 2);
                            })) {
            audio_desc = std::string(audio_mode == lv::AudioCaptureMode::kLoopback ? "loopback" : "mic") +
                         "[" + audio_cap.device_name() + "]";
        } else {
            std::fprintf(stderr, "Warning: failed to start audio capture (mode=%d); continuing video-only.\n",
                         int(audio_mode));
        }
    }

    // 64-channel AoIP test bus: one distinct sine per channel, all channels share one timestamp.
    std::thread tone_thread;
    if (audio_tone) {
        const lv::AudioReliabilityProfile prof = sender.audio2_profile();
        audio_desc = "tone x" + std::to_string(audio_channels) + " @" + std::to_string(audio_rate) + " Hz, " +
                     std::to_string(audio_latency_ms) + " ms [" + lv::audio_mode_name(prof.mode) +
                     ", chunk=" + std::to_string(prof.chunk_samples) + ", FEC " + std::to_string(prof.fec_k) +
                     "+1 x" + std::to_string(prof.fec_interleave) + ", NACK x" +
                     std::to_string(prof.nack_attempts) + "]";
        tone_thread = std::thread([&]() {
            const uint32_t blk = std::max<uint32_t>(prof.chunk_samples, uint32_t(audio_rate / 200));  // >= 5 ms
            std::vector<std::vector<int16_t>> bufs(static_cast<size_t>(audio_channels));
            for (auto& b : bufs) b.assign(blk, 0);
            std::vector<double> phase(static_cast<size_t>(audio_channels), 0.0);
            const uint64_t period_us = uint64_t(blk) * 1000000ull / uint64_t(audio_rate);
            uint64_t next = lv::now_us() + period_us;
            while (!g_stop.load(std::memory_order_relaxed)) {
                lv::sleep_until_us(next, [] {});
                next += period_us;
                const uint64_t ts = sender.next_auto_media_ts(blk);
                for (int c = 0; c < audio_channels; ++c) {
                    const double w = 6.283185307179586 * (110.0 * (c + 1)) / double(audio_rate);
                    for (uint32_t i = 0; i < blk; ++i) {
                        bufs[size_t(c)][i] = int16_t(std::sin(phase[size_t(c)]) * 6000.0);
                        phase[size_t(c)] += w;
                    }
                    phase[size_t(c)] = std::fmod(phase[size_t(c)], 6.283185307179586);
                    sender.push_audio_channel(uint16_t(c), bufs[size_t(c)].data(), blk,
                                              lv::AudioFormat2::kPcm16, ts);
                }
                sender.set_next_auto_media_ts(ts + blk);
            }
        });
    }

    // DAW -> LanVidAudioBus_v1 (virtual OS device) -> network.
    lv::ShmAudioHost vhost;
    if (audio_virtual) {
        lv::ShmAudioHost::Config hc;
        hc.channels = uint32_t(audio_channels);
        const bool ok = vhost.start(
            hc,
            [&sender](const float* const* planes, uint32_t ch, uint32_t n, bool disc) {
                const uint64_t ts = disc ? sender.master_samples_now() - n : sender.next_auto_media_ts(n);
                for (uint32_t c = 0; c < ch; ++c)
                    sender.push_audio_channel(uint16_t(c), planes[c], n, lv::AudioFormat2::kFloat32, ts);
                sender.set_next_auto_media_ts(ts + n);
            },
            nullptr);
        if (!ok) {
            std::fprintf(stderr, "Failed to open shared audio bus %s\n", lv::kShmBusName);
            return 1;
        }
        audio_desc = "virtual device x" + std::to_string(audio_channels) + " [" + lv::kShmBusName + ", " +
                     std::to_string(audio_latency_ms) + " ms]";
    }

    std::signal(SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal(SIGTERM, on_signal);
#endif

    if (scfg.codec == lv::VideoCodecId::kLanVidDct) {
        std::printf(
            "LVSP Sender -> %s:%u | src=%s | codec=DCT | audio=%s | %dx%d @ %.1f fps | Q=%d%s | slices=%d mtu=%zu fec=%u | kernel=%s | loss=%.1f%%\n",
            scfg.dest_ip.c_str(), unsigned(scfg.dest_port), source_desc.c_str(), audio_desc.c_str(),
            cp.width, cp.height, fps, cp.quality, scfg.adaptive.enabled ? " (AIMD)" : "", geom.total_slices,
            scfg.mtu, unsigned(scfg.fec_group_size), lv::dct_kernels_best().name, scfg.sim_loss_pct);
    } else {
        std::printf(
            "LVSP Sender -> %s:%u | src=%s | codec=%s | target=%u kbps%s | keyint=%u | audio=%s | %dx%d @ %.1f fps | mtu=%zu fec=%u | loss=%.1f%%\n",
            scfg.dest_ip.c_str(), unsigned(scfg.dest_port), source_desc.c_str(),
            lv::video_codec_name(scfg.codec), unsigned(scfg.bitrate_kbps),
            scfg.adaptive.enabled ? " (AIMD)" : "", unsigned(scfg.keyframe_interval_frames),
            audio_desc.c_str(), cp.width, cp.height, fps, scfg.mtu, unsigned(scfg.fec_group_size),
            scfg.sim_loss_pct);
    }
    std::fflush(stdout);

    lv::Image frame;
    frame.allocate(cp.width, cp.height, pixel_order);

    const uint64_t t_start = lv::now_us();
    uint64_t t_last_report = t_start;
    uint64_t prev_frames = 0, prev_bytes = 0, prev_pkts = 0, prev_sim_drop = 0, prev_aud_bytes = 0;
    double win_proc_ms_sum = 0.0;
    uint64_t win_proc_cnt = 0;

    uint32_t frame_id = 0;
    int consec_cap_fail = 0;
    bool loss_cleared = false;
    const double frame_interval_us = 1e6 / fps;
    const bool live_capture = (src_kind == SourceKind::kScreen || src_kind == SourceKind::kCamera);

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (max_frames > 0 && frame_id >= max_frames) break;
        const uint64_t now = lv::now_us();
        const double elapsed_s = double(now - t_start) / 1e6;
        if (max_duration_s > 0.0 && elapsed_s >= max_duration_s) break;
        if (loss_off_after_s > 0.0 && !loss_cleared && elapsed_s >= loss_off_after_s) {
            sender.set_sim_loss_pct(0.0);
            loss_cleared = true;
            std::printf("[%5.1fs] Simulated packet loss -> 0.0%%\n", elapsed_s);
            std::fflush(stdout);
        }

        const uint64_t target_us = t_start + uint64_t(double(frame_id) * frame_interval_us);
        uint64_t t_cap = 0;
        if (live_capture) {
            lv::sleep_until_us(target_us, [&] { sender.service_control(); });
            t_cap = lv::now_us();
            bool ok = false;
            if (src_kind == SourceKind::kScreen) ok = screen.capture(frame, 2);
            else ok = camera.grab(frame, 2000);
            if (!ok) {
                ++consec_cap_fail;
                if (frame_id == 0 && consec_cap_fail >= 3) {
                    std::fprintf(stderr, "Capture failed on initial frame (desktop locked or inaccessible).\n");
                    break;
                }
                if (consec_cap_fail == 1) {
                    std::fprintf(stderr, "Transient capture failure on frame %u; retrying...\n", frame_id);
                }
                if (consec_cap_fail > 60) break;
                ++frame_id;
                continue;
            }
            consec_cap_fail = 0;
        } else {
            if (src_kind == SourceKind::kFile) {
                if (!file_src.read_frame(frame)) {
                    std::fprintf(stderr, "Failed to read frame from input file.\n");
                    break;
                }
            } else {
                lv::generate_test_frame(frame, frame_id);
            }
            lv::sleep_until_us(target_us, [&] { sender.service_control(); });
            t_cap = lv::now_us();
        }

        if (scfg.adaptive.enabled) cp.quality = sender.active_quality();
        sender.send_frame(frame, cp, frame_id, uint32_t(t_cap));
        const uint64_t t_done = lv::now_us();
        sender.service_control();

        win_proc_ms_sum += double(t_done - t_cap) / 1000.0;
        ++win_proc_cnt;
        ++frame_id;

        if (t_done - t_last_report >= 1000000) {
            const auto st = sender.stats();
            const double dt = double(t_done - t_last_report) / 1e6;
            const double tx_fps = double(st.frames_sent - prev_frames) / dt;
            const double mbps = double(st.bytes_sent - prev_bytes) * 8.0 / (dt * 1e6);
            const double pps = double(st.packets_sent - prev_pkts) / dt;
            const double avg_ms = win_proc_cnt ? (win_proc_ms_sum / double(win_proc_cnt)) : 0.0;
            const double aud_kbps = double(st.audio_bytes_sent - prev_aud_bytes) * 8.0 / (dt * 1000.0);
            if (scfg.codec == lv::VideoCodecId::kLanVidDct) {
                std::printf(
                    "[%5.1fs] TX FPS:%5.1f | DCT Q:%-3d | %6.1f Mbps | %s:%5.2f ms | %6.0f pkt/s | rx_loss:%4.1f%% | sim_drop:%llu err:%llu",
                    double(t_done - t_start) / 1e6, tx_fps, st.active_quality, mbps,
                    live_capture ? "cap+tx" : "enc+tx", avg_ms, pps, double(st.last_rx_loss_permille) / 10.0,
                    (unsigned long long)(st.packets_dropped_sim - prev_sim_drop),
                    (unsigned long long)st.send_errors);
            } else {
                std::printf(
                    "[%5.1fs] TX FPS:%5.1f | %s [%s] tgt:%ukbps | %6.2f Mbps | %s:%5.2f ms | %6.0f pkt/s | PLI:%llu | rx_loss:%4.1f%% | err:%llu",
                    double(t_done - t_start) / 1e6, tx_fps, lv::video_codec_name(st.active_codec),
                    sender.encoder_is_hardware() ? "HW" : "SW", unsigned(st.active_bitrate_kbps), mbps,
                    live_capture ? "cap+tx" : "enc+tx", avg_ms, pps,
                    (unsigned long long)st.pli_received, double(st.last_rx_loss_permille) / 10.0,
                    (unsigned long long)st.send_errors);
            }
            if (audio_cap.is_running() || st.audio_packets_sent > 0) {
                std::printf(" | Aud:%5.0f kbps", aud_kbps);
            }
            std::printf("\n");
            std::fflush(stdout);

            t_last_report = t_done;
            prev_frames = st.frames_sent;
            prev_bytes = st.bytes_sent;
            prev_pkts = st.packets_sent;
            prev_sim_drop = st.packets_dropped_sim;
            if (audio_virtual) {
                const auto vs = vhost.status();
                std::printf("[VDEV  ] DAW %s%s%s | %u Hz buf=%u | tx_fill=%u | sent=%llu frames | overruns=%llu reanchors=%llu\n",
                            vs.driver_connected ? "connected" : "not connected",
                            vs.driver_name.empty() ? "" : " via ", vs.driver_name.c_str(), vs.sample_rate,
                            vs.buffer_size, vs.tx_fill, (unsigned long long)vs.tx_frames,
                            (unsigned long long)vs.tx_overruns, (unsigned long long)vs.reanchors);
            }
            prev_aud_bytes = st.audio_bytes_sent;
            win_proc_ms_sum = 0.0;
            win_proc_cnt = 0;
        }
    }

    audio_cap.stop();
    vhost.stop();
    if (tone_thread.joinable()) {
        g_stop.store(true, std::memory_order_relaxed);
        tone_thread.join();
    }
    if (audio_tone || audio_virtual) {
        const auto a2 = sender.audio2_stats();
        std::printf("[AUDIO64] mode=%s pkts=%llu parity=%llu retransmits=%llu nacks=%llu dup=%llu streams=%u\n",
                    lv::audio_mode_name(a2.mode), (unsigned long long)a2.packets_sent,
                    (unsigned long long)a2.parity_sent, (unsigned long long)a2.retransmits,
                    (unsigned long long)a2.nacks_received, (unsigned long long)a2.duplicates, a2.active_streams);
    }
    const auto st = sender.stats();
    const double tot_s = std::max(1e-6, double(lv::now_us() - t_start) / 1e6);
    std::printf(
        "[TOTAL ] codec:%s [%s] | frames:%llu (%.1f fps) | Q:%d | %.2f Mbps | pkts:%llu | pli:%llu | aud_pkts:%llu | sim_drop:%llu | err:%llu | pongs:%llu fb:%llu\n",
        lv::video_codec_name(st.active_codec), sender.encoder_backend_name().c_str(),
        (unsigned long long)st.frames_sent, double(st.frames_sent) / tot_s, st.active_quality,
        double(st.bytes_sent) * 8.0 / (tot_s * 1e6), (unsigned long long)st.packets_sent,
        (unsigned long long)st.pli_received, (unsigned long long)st.audio_packets_sent,
        (unsigned long long)st.packets_dropped_sim, (unsigned long long)st.send_errors,
        (unsigned long long)st.pongs_sent, (unsigned long long)st.feedbacks_received);
    return 0;
}
