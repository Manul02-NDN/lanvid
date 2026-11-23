#pragma once
// LanVidAudioBus_v1: cross-process lock-free shared-memory audio bus between OS virtual audio
// drivers (ASIO DLL / CoreAudio HAL plug-in / JACK bridge, "driver" side) and the network host
// (LanVid Studio / CLI, "host" side).
//
//   TX ring: driver (DAW output) -> host -> network   (SPSC: producer = driver, consumer = host)
//   RX ring: network -> host -> driver (DAW input)    (SPSC: producer = host,   consumer = driver)
//
// Each ring: 64 planar float32 channels x 16384 frames (~341 ms @ 48 kHz, 8 MB for both rings),
// monotonic 64-bit frame cursors. Layout version 2; a v1 (4096-frame) segment is replaced on open
// when nobody is using it (macOS cannot grow an existing POSIX shm object).
// RT-safe: the data path uses only atomics and memcpy (no locks, allocation or syscalls).
// Self-contained (no lanvid_core dependency) so drivers can compile it directly.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lv {

constexpr uint32_t kShmBusMagic = 0x4241564Cu;  // "LVAB"
constexpr uint32_t kShmBusVersion = 2;  // v2: 16384-frame rings (v1: 4096)
constexpr uint32_t kShmBusChannels = 64;
constexpr uint32_t kShmBusFrames = 16384;  // per channel, power of two (~341 ms @ 48 kHz)
constexpr const char* kShmBusName = "LanVidAudioBus_v1";

struct alignas(64) ShmRing {
    alignas(64) std::atomic<uint64_t> write_pos;
    alignas(64) std::atomic<uint64_t> read_pos;
    alignas(64) std::atomic<uint64_t> overruns;   // producer had no room
    std::atomic<uint64_t> underruns;              // consumer found too little data
    float data[kShmBusChannels][kShmBusFrames];
};

struct ShmBusHeader {
    std::atomic<uint32_t> magic;
    uint32_t version;
    uint32_t channels;
    uint32_t frames;
    std::atomic<uint32_t> sample_rate;        // set by the driver (DAW device rate)
    std::atomic<uint32_t> buffer_size;        // driver IO buffer size in frames
    std::atomic<uint32_t> active_channels;
    std::atomic<uint32_t> driver_running;     // 1 while the DAW has IO started
    std::atomic<uint64_t> driver_heartbeat_ms;
    std::atomic<uint64_t> host_heartbeat_ms;
    std::atomic<uint32_t> target_latency_ms;  // published by the host (network latency)
    std::atomic<uint32_t> clock_synced;       // host MasterClock status
    std::atomic<int32_t> clock_skew_ppb;
    uint32_t reserved;
    char driver_name[64];                     // e.g. "ASIO", "CoreAudio HAL", "JACK"
};

struct ShmBusLayout {
    ShmBusHeader hdr;
    ShmRing tx;
    ShmRing rx;
};

// Shared steady clock in ms (system-wide monotonic on Windows/macOS/Linux).
uint64_t shm_bus_now_ms();

class ShmAudioBus {
public:
    ShmAudioBus() = default;
    ~ShmAudioBus() { close(); }
    ShmAudioBus(const ShmAudioBus&) = delete;
    ShmAudioBus& operator=(const ShmAudioBus&) = delete;

    // Creates the segment if missing, otherwise attaches. `name` overrides kShmBusName (tests).
    bool open(const char* name = nullptr);
    void close();
    bool is_open() const { return bus_ != nullptr; }
    ShmBusHeader* header() { return bus_ ? &bus_->hdr : nullptr; }

    // ---- driver side (RT) ----
    uint32_t driver_write_tx(const float* const* planes, uint32_t channels, uint32_t frames);
    uint32_t driver_write_tx_interleaved(const float* in, uint32_t channels, uint32_t frames);
    // Always fills `frames` (zeros on underrun). Returns frames of real data.
    uint32_t driver_read_rx(float* const* planes, uint32_t channels, uint32_t frames);
    uint32_t driver_read_rx_interleaved(float* out, uint32_t channels, uint32_t frames);
    void driver_set_running(bool running, uint32_t sample_rate, uint32_t buffer_size, const char* name);
    void driver_heartbeat() { if (bus_) bus_->hdr.driver_heartbeat_ms.store(shm_bus_now_ms(), std::memory_order_release); }

    // ---- host side ----
    uint32_t host_tx_available() const;
    uint32_t host_read_tx(float* const* planes, uint32_t channels, uint32_t frames);
    uint32_t host_rx_fill() const;
    uint32_t host_write_rx(const float* const* planes, uint32_t channels, uint32_t frames);
    void host_heartbeat() { if (bus_) bus_->hdr.host_heartbeat_ms.store(shm_bus_now_ms(), std::memory_order_release); }
    void host_drop_stale_tx();  // skip to the newest TX data (on attach)
    // DAW connected: IO running and driver heartbeat younger than `timeout_ms`.
    bool driver_connected(uint32_t timeout_ms = 500) const;
    bool host_connected(uint32_t timeout_ms = 500) const;
    uint64_t tx_overruns() const { return bus_ ? bus_->tx.overruns.load(std::memory_order_relaxed) : 0; }
    uint64_t rx_underruns() const { return bus_ ? bus_->rx.underruns.load(std::memory_order_relaxed) : 0; }

private:
    static uint32_t ring_write(ShmRing& r, const float* const* planes, const float* inter,
                               uint32_t channels, uint32_t frames);
    static uint32_t ring_read(ShmRing& r, float* const* planes, float* inter, uint32_t channels,
                              uint32_t frames, bool zero_fill);

    ShmBusLayout* bus_ = nullptr;
    [[maybe_unused]] void* map_handle_ = nullptr;  // Windows file mapping handle
};

}  // namespace lv
