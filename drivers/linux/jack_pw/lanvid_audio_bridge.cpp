// lanvid-audio-bridge — JACK / PipeWire (pw-jack) 64-channel virtual device for LanVid.
//
// Registers the graph node "LanVid-64ch" with 64 input ports (send_1..send_64: audio from other
// apps -> network) and 64 output ports (recv_1..recv_64: network -> apps), exchanging audio with
// LanVid Studio / CLI through the lock-free shared-memory bus LanVidAudioBus_v1.
//
// libjack.so.0 is loaded with dlopen() at runtime, so the binary builds and starts on systems
// without JACK (it then explains what to install). The JACK process callback is RT-safe.
//
// Optional --pulse: creates a PulseAudio/PipeWire null sink "LanVid" (pactl) and links its
// monitor into send_1/send_2 with pw-link, so ordinary desktop apps can play into LanVid.

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>

#include "audio/shm_audio_bridge.hpp"

namespace {

// ---- minimal libjack ABI (stable since JACK 0.x; implemented by JACK2 and pipewire-jack) ----
using jack_client_t = struct _jack_client;
using jack_port_t = struct _jack_port;
using jack_nframes_t = uint32_t;
using JackProcessCallback = int (*)(jack_nframes_t, void*);
using JackShutdownCallback = void (*)(void*);
using JackSampleRateCallback = int (*)(jack_nframes_t, void*);
using JackBufferSizeCallback = int (*)(jack_nframes_t, void*);
constexpr int JackNoStartServer = 0x01;
constexpr unsigned long JackPortIsInput = 0x1;
constexpr unsigned long JackPortIsOutput = 0x2;
constexpr unsigned long JackPortIsTerminal = 0x10;
constexpr const char* JACK_DEFAULT_AUDIO_TYPE = "32 bit float mono audio";

struct JackApi {
    void* lib = nullptr;
    jack_client_t* (*client_open)(const char*, int, int*, ...) = nullptr;
    int (*client_close)(jack_client_t*) = nullptr;
    jack_port_t* (*port_register)(jack_client_t*, const char*, const char*, unsigned long, unsigned long) = nullptr;
    void* (*port_get_buffer)(jack_port_t*, jack_nframes_t) = nullptr;
    int (*set_process_callback)(jack_client_t*, JackProcessCallback, void*) = nullptr;
    void (*on_shutdown)(jack_client_t*, JackShutdownCallback, void*) = nullptr;
    int (*set_sample_rate_callback)(jack_client_t*, JackSampleRateCallback, void*) = nullptr;
    int (*set_buffer_size_callback)(jack_client_t*, JackBufferSizeCallback, void*) = nullptr;
    int (*activate)(jack_client_t*) = nullptr;
    int (*deactivate)(jack_client_t*) = nullptr;
    jack_nframes_t (*get_sample_rate)(jack_client_t*) = nullptr;
    jack_nframes_t (*get_buffer_size)(jack_client_t*) = nullptr;
    const char* (*get_client_name)(jack_client_t*) = nullptr;

    bool load() {
        const char* names[] = {"libjack.so.0", "libjack.so", "libjack.0.dylib", "libjack.dylib"};
        for (const char* n : names) {
            lib = dlopen(n, RTLD_NOW | RTLD_LOCAL);
            if (lib) break;
        }
        if (!lib) return false;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };
        return sym(client_open, "jack_client_open") && sym(client_close, "jack_client_close") &&
               sym(port_register, "jack_port_register") && sym(port_get_buffer, "jack_port_get_buffer") &&
               sym(set_process_callback, "jack_set_process_callback") && sym(on_shutdown, "jack_on_shutdown") &&
               sym(set_sample_rate_callback, "jack_set_sample_rate_callback") &&
               sym(set_buffer_size_callback, "jack_set_buffer_size_callback") && sym(activate, "jack_activate") &&
               sym(deactivate, "jack_deactivate") && sym(get_sample_rate, "jack_get_sample_rate") &&
               sym(get_buffer_size, "jack_get_buffer_size") && sym(get_client_name, "jack_get_client_name");
    }
};

constexpr uint32_t kCh = lv::kShmBusChannels;

JackApi g_jack;
jack_client_t* g_client = nullptr;
jack_port_t* g_in[kCh] = {};
jack_port_t* g_out[kCh] = {};
lv::ShmAudioBus g_bus;
std::atomic<bool> g_stop{false};
std::atomic<uint32_t> g_rate{48000};
std::atomic<uint32_t> g_bufsize{256};

// RT thread: only pointer lookups, memcpy and atomics.
int process(jack_nframes_t n, void*) {
    const float* in[kCh];
    float* out[kCh];
    for (uint32_t c = 0; c < kCh; ++c) {
        in[c] = static_cast<const float*>(g_jack.port_get_buffer(g_in[c], n));
        out[c] = static_cast<float*>(g_jack.port_get_buffer(g_out[c], n));
    }
    g_bus.driver_write_tx(in, kCh, n);   // send_* -> network
    g_bus.driver_read_rx(out, kCh, n);   // network -> recv_* (zeros on underrun)
    g_bus.driver_heartbeat();
    return 0;
}

int on_rate(jack_nframes_t r, void*) {
    g_rate.store(r);
    if (auto* h = g_bus.header()) h->sample_rate.store(r);
    return 0;
}
int on_bufsize(jack_nframes_t b, void*) {
    g_bufsize.store(b);
    if (auto* h = g_bus.header()) h->buffer_size.store(b);
    return 0;
}
void on_shutdown(void*) { g_stop.store(true); }
void on_signal(int) { g_stop.store(true); }

int run_cmd(const std::string& cmd, std::string* out = nullptr) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return -1;
    char buf[256];
    std::string acc;
    while (fgets(buf, sizeof(buf), p)) acc += buf;
    const int rc = pclose(p);
    if (out) *out = acc;
    return rc;
}

void usage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  --name <client>   JACK/PipeWire node name (default: LanVid-64ch)\n"
        "  --pulse           Also create a desktop sink \"LanVid\" (pactl null-sink + pw-link to send_1/2)\n"
        "  --bus <name>      Shared-memory bus name (default: %s)\n"
        "  -h, --help        Show this help\n"
        "Run with pw-jack on PipeWire systems if libjack is not the PipeWire one: pw-jack %s\n",
        prog, lv::kShmBusName, prog);
}

}  // namespace

int main(int argc, char** argv) {
    std::string name = "LanVid-64ch";
    std::string bus_name;
    bool pulse = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "--name" && i + 1 < argc) name = argv[++i];
        else if (a == "--bus" && i + 1 < argc) bus_name = argv[++i];
        else if (a == "--pulse") pulse = true;
        else { usage(argv[0]); return 2; }
    }

    if (!g_jack.load()) {
        std::fprintf(stderr,
                     "lanvid-audio-bridge: libjack.so.0 not found.\n"
                     "  PipeWire: sudo apt install pipewire-jack   (then run: pw-jack lanvid-audio-bridge)\n"
                     "  JACK2:    sudo apt install libjack-jackd2-0\n");
        return 3;
    }
    if (!g_bus.open(bus_name.empty() ? nullptr : bus_name.c_str())) {
        std::fprintf(stderr, "lanvid-audio-bridge: cannot open shared memory bus\n");
        return 4;
    }

    int status = 0;
    g_client = g_jack.client_open(name.c_str(), JackNoStartServer, &status);
    if (!g_client) {
        std::fprintf(stderr, "lanvid-audio-bridge: no JACK/PipeWire server (status=0x%x). Start PipeWire or jackd.\n", status);
        return 5;
    }
    char pname[32];
    for (uint32_t c = 0; c < kCh; ++c) {
        std::snprintf(pname, sizeof(pname), "send_%u", c + 1);
        g_in[c] = g_jack.port_register(g_client, pname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
        std::snprintf(pname, sizeof(pname), "recv_%u", c + 1);
        g_out[c] = g_jack.port_register(g_client, pname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
        if (!g_in[c] || !g_out[c]) {
            std::fprintf(stderr, "lanvid-audio-bridge: failed to register port %u\n", c + 1);
            g_jack.client_close(g_client);
            return 6;
        }
    }
    g_jack.set_process_callback(g_client, process, nullptr);
    g_jack.set_sample_rate_callback(g_client, on_rate, nullptr);
    g_jack.set_buffer_size_callback(g_client, on_bufsize, nullptr);
    g_jack.on_shutdown(g_client, on_shutdown, nullptr);

    g_rate.store(g_jack.get_sample_rate(g_client));
    g_bufsize.store(g_jack.get_buffer_size(g_client));
    g_bus.driver_set_running(true, g_rate.load(), g_bufsize.load(), "JACK/PipeWire");
    if (g_jack.activate(g_client) != 0) {
        std::fprintf(stderr, "lanvid-audio-bridge: jack_activate failed\n");
        g_bus.driver_set_running(false, 0, 0, nullptr);
        g_jack.client_close(g_client);
        return 7;
    }
    const std::string node = g_jack.get_client_name(g_client);

    std::string pulse_module;
    if (pulse) {
        std::string out;
        if (run_cmd("pactl load-module module-null-sink sink_name=lanvid_sink "
                    "sink_properties=device.description=LanVid channels=2 2>/dev/null", &out) == 0) {
            pulse_module = out.substr(0, out.find_first_not_of("0123456789"));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            run_cmd("pw-link lanvid_sink:monitor_FL '" + node + ":send_1' 2>/dev/null");
            run_cmd("pw-link lanvid_sink:monitor_FR '" + node + ":send_2' 2>/dev/null");
            std::printf("Desktop sink \"LanVid\" created (module %s) -> %s:send_1/2\n", pulse_module.c_str(), node.c_str());
        } else {
            std::fprintf(stderr, "Warning: pactl not available; --pulse sink not created\n");
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    std::printf("LanVid JACK/PipeWire bridge \"%s\": 64 in (send_*) / 64 out (recv_*) @ %u Hz, %u frames | bus %s\n",
                node.c_str(), g_rate.load(), g_bufsize.load(), bus_name.empty() ? lv::kShmBusName : bus_name.c_str());
    std::fflush(stdout);

    int tick = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (++tick % 25 == 0) {
            std::printf("[bridge] host %s | tx_overruns=%llu rx_underruns=%llu\n",
                        g_bus.host_connected() ? "connected" : "waiting for LanVid Studio / CLI",
                        (unsigned long long)g_bus.tx_overruns(), (unsigned long long)g_bus.rx_underruns());
            std::fflush(stdout);
        }
    }

    g_jack.deactivate(g_client);
    g_bus.driver_set_running(false, 0, 0, nullptr);
    g_jack.client_close(g_client);
    if (!pulse_module.empty()) run_cmd("pactl unload-module " + pulse_module + " 2>/dev/null");
    g_bus.close();
    return 0;
}
