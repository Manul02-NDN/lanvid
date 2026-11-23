#include "audio/shm_audio_host.hpp"
#include "common/rt_thread.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace lv {

bool ShmAudioHost::start(const Config& cfg, PushFn tx, PopFn rx) {
    stop();
    cfg_ = cfg;
    cfg_.channels = std::clamp<uint32_t>(cfg_.channels, 1, kShmBusChannels);
    cfg_.block_frames = std::clamp<uint32_t>(cfg_.block_frames, 16, kShmBusFrames / 4);
    tx_ = std::move(tx);
    rx_ = std::move(rx);
    if (!bus_.open(cfg_.bus_name.empty() ? nullptr : cfg_.bus_name.c_str())) return false;
    bus_.host_heartbeat();
    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void ShmAudioHost::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    bus_.close();
}

void ShmAudioHost::publish_network_state(uint32_t latency_ms, bool clock_synced, double skew_ppm) {
    ShmBusHeader* h = bus_.header();
    if (!h) return;
    h->target_latency_ms.store(latency_ms, std::memory_order_relaxed);
    h->clock_synced.store(clock_synced ? 1u : 0u, std::memory_order_relaxed);
    h->clock_skew_ppb.store(int32_t(std::lround(skew_ppm * 1000.0)), std::memory_order_relaxed);
}

void ShmAudioHost::loop() {
    using clock = std::chrono::steady_clock;
    RealtimeThreadScope rt(1000, 200, 700);  // ~1 ms pump cadence feeding the DAW / network rings
    const uint32_t ch = cfg_.channels;
    const uint32_t blk = cfg_.block_frames;
    std::vector<std::vector<float>> bufs(ch, std::vector<float>(blk));
    std::vector<float*> planes(ch);
    for (uint32_t c = 0; c < ch; ++c) planes[c] = bufs[c].data();

    bool was_connected = false;
    bool need_anchor = true;
    clock::time_point anchor_t{};
    uint64_t frames_since_anchor = 0;
    clock::time_point last_tx = clock::now();
    bool drifting = false;
    clock::time_point drift_since{};

#if defined(_WIN32)
    // Default Windows timer resolution is ~15.6 ms, far longer than the RX headroom: without this
    // the pump would wake up too rarely and the DAW inputs would underrun.
    timeBeginPeriod(1);
#endif
    while (!stop_.load(std::memory_order_acquire)) {
        bus_.host_heartbeat();
        const bool connected = bus_.driver_connected();
        const ShmBusHeader* h = bus_.header();
        const uint32_t rate = std::max<uint32_t>(h->sample_rate.load(std::memory_order_relaxed), 8000);

        if (connected && !was_connected) {
            bus_.host_drop_stale_tx();  // never forward audio queued before this session
            need_anchor = true;
        }
        was_connected = connected;

        // ---- TX: DAW -> network ----
        if (tx_ && connected) {
            for (int guard = 0; guard < 64; ++guard) {
                const uint32_t avail = bus_.host_tx_available();
                if (avail == 0) break;
                const uint32_t n = bus_.host_read_tx(planes.data(), ch, std::min(avail, blk));
                if (n == 0) break;
                const auto now = clock::now();
                if (now - last_tx > std::chrono::milliseconds(kStallMs)) need_anchor = true;  // DAW stopped
                if (!need_anchor) {
                    const double elapsed_ms =
                        std::chrono::duration<double, std::milli>(now - anchor_t).count();
                    const double media_ms = 1000.0 * double(frames_since_anchor + n) / rate;
                    if (std::fabs(media_ms - elapsed_ms) > double(kDriftMs)) {
                        if (!drifting) { drifting = true; drift_since = now; }
                        else if (now - drift_since > std::chrono::milliseconds(kDriftHoldMs)) need_anchor = true;
                    } else {
                        drifting = false;
                    }
                }
                const bool disc = need_anchor;
                if (need_anchor) {
                    need_anchor = false;
                    drifting = false;
                    anchor_t = now;
                    frames_since_anchor = 0;
                    reanchors_.fetch_add(1, std::memory_order_relaxed);
                }
                tx_(planes.data(), ch, n, disc);
                frames_since_anchor += n;
                last_tx = now;
                tx_frames_.fetch_add(n, std::memory_order_relaxed);
            }
        }

        // ---- RX: network -> DAW (fill to target so pops follow the device clock) ----
        if (rx_ && connected) {
            const uint32_t bs = h->buffer_size.load(std::memory_order_relaxed);
            uint32_t target = cfg_.rx_target_frames ? cfg_.rx_target_frames : std::max<uint32_t>(3 * bs, 512);
            target = std::min<uint32_t>(target, kShmBusFrames - blk);
            for (int guard = 0; guard < 64; ++guard) {
                const uint32_t fill = bus_.host_rx_fill();
                if (fill >= target) break;
                const uint32_t n = std::min(blk, target - fill);
                for (uint32_t c = 0; c < ch; ++c) std::fill(bufs[c].begin(), bufs[c].begin() + n, 0.0f);
                rx_(planes.data(), ch, n);
                std::vector<const float*> cp(planes.begin(), planes.end());
                bus_.host_write_rx(cp.data(), ch, n);
                rx_frames_.fetch_add(n, std::memory_order_relaxed);
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

ShmHostStatus ShmAudioHost::status() const {
    ShmHostStatus s;
    auto& bus = const_cast<ShmAudioBus&>(bus_);
    const ShmBusHeader* h = bus.header();
    if (!h) return s;
    s.bus_open = true;
    s.driver_connected = bus.driver_connected();
    s.sample_rate = h->sample_rate.load(std::memory_order_relaxed);
    s.buffer_size = h->buffer_size.load(std::memory_order_relaxed);
    s.tx_fill = bus.host_tx_available();
    s.rx_fill = bus.host_rx_fill();
    s.tx_frames = tx_frames_.load(std::memory_order_relaxed);
    s.rx_frames = rx_frames_.load(std::memory_order_relaxed);
    s.reanchors = reanchors_.load(std::memory_order_relaxed);
    s.tx_overruns = bus.tx_overruns();
    s.rx_underruns = bus.rx_underruns();
    s.driver_name.assign(h->driver_name, strnlen(h->driver_name, sizeof(h->driver_name)));
    return s;
}

}  // namespace lv
