#include "audio/shm_audio_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lv {

static_assert(std::atomic<uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free for IPC");

uint64_t shm_bus_now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool ShmAudioBus::open(const char* name) {
    if (bus_) return true;
    // LANVID_AUDIO_BUS_NAME lets tests / multiple instances use a private bus (not read on RT threads).
    const char* env = std::getenv("LANVID_AUDIO_BUS_NAME");
    const char* nm = name ? name : (env && *env ? env : kShmBusName);
    const size_t size = sizeof(ShmBusLayout);
    void* mem = nullptr;
    bool created = false;
#if defined(_WIN32)
    char full[128];
    std::snprintf(full, sizeof(full), "Local\\%s", nm);
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(size), full);
    if (!h) return false;
    created = GetLastError() != ERROR_ALREADY_EXISTS;
    mem = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!mem) { CloseHandle(h); return false; }
    map_handle_ = h;
#else
    char full[128];
    std::snprintf(full, sizeof(full), "/%s", nm);
    int fd = shm_open(full, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd >= 0) {
        created = true;
        if (ftruncate(fd, off_t(size)) != 0) { ::close(fd); shm_unlink(full); return false; }
    } else {
        fd = shm_open(full, O_RDWR, 0666);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) != 0) { ::close(fd); return false; }
        if (size_t(st.st_size) < size && ftruncate(fd, off_t(size)) != 0) {
            // An older, smaller layout (e.g. v1 = 4096 frames). POSIX shm cannot grow on macOS, so
            // replace it — but only if no driver / host is actively using it.
            bool busy = false;
            if (size_t(st.st_size) >= sizeof(ShmBusHeader)) {
                void* hm = mmap(nullptr, sizeof(ShmBusHeader), PROT_READ, MAP_SHARED, fd, 0);
                if (hm != MAP_FAILED) {
                    const auto* oh = static_cast<const ShmBusHeader*>(hm);
                    const uint64_t now = shm_bus_now_ms();
                    const uint64_t dhb = oh->driver_heartbeat_ms.load(std::memory_order_acquire);
                    const uint64_t hhb = oh->host_heartbeat_ms.load(std::memory_order_acquire);
                    busy = (oh->driver_running.load() && now - dhb < 2000) || (hhb && now - hhb < 2000);
                    munmap(hm, sizeof(ShmBusHeader));
                }
            }
            ::close(fd);
            if (busy) {
                std::fprintf(stderr, "LanVidAudioBus: an older bus layout is in use (reinstall/restart the LanVid "
                                     "virtual audio driver and LanVid apps)\n");
                return false;
            }
            shm_unlink(full);
            fd = shm_open(full, O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd < 0) return false;
            created = true;
            if (ftruncate(fd, off_t(size)) != 0) { ::close(fd); shm_unlink(full); return false; }
        }
    }
    if (created) fchmod(fd, 0666);  // cross-user: coreaudiod's driver service <-> user apps
    mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) return false;
#endif
    bus_ = static_cast<ShmBusLayout*>(mem);
    ShmBusHeader& h0 = bus_->hdr;
    // New segments are zero-filled by the OS; the creator stamps the layout. A stale/mismatched
    // segment (other version) is re-initialised.
    if (created || h0.magic.load(std::memory_order_acquire) != kShmBusMagic ||
        h0.version != kShmBusVersion || h0.channels != kShmBusChannels || h0.frames != kShmBusFrames) {
        std::memset(static_cast<void*>(bus_), 0, sizeof(ShmBusLayout));
        h0.version = kShmBusVersion;
        h0.channels = kShmBusChannels;
        h0.frames = kShmBusFrames;
        h0.sample_rate.store(48000);
        h0.target_latency_ms.store(20);
        h0.magic.store(kShmBusMagic, std::memory_order_release);
    }
    return true;
}

void ShmAudioBus::close() {
    if (!bus_) return;
#if defined(_WIN32)
    UnmapViewOfFile(bus_);
    if (map_handle_) CloseHandle(static_cast<HANDLE>(map_handle_));
    map_handle_ = nullptr;
#else
    munmap(bus_, sizeof(ShmBusLayout));
#endif
    bus_ = nullptr;
}

uint32_t ShmAudioBus::ring_write(ShmRing& r, const float* const* planes, const float* inter,
                                 uint32_t channels, uint32_t frames) {
    channels = std::min(channels, kShmBusChannels);
    const uint64_t w = r.write_pos.load(std::memory_order_relaxed);
    const uint64_t rd = r.read_pos.load(std::memory_order_acquire);
    const uint64_t free_frames = kShmBusFrames - (w - rd);
    uint32_t n = frames;
    if (n > free_frames) {
        n = uint32_t(free_frames);
        r.overruns.fetch_add(1, std::memory_order_relaxed);
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t idx = uint32_t((w + i) & (kShmBusFrames - 1));
        for (uint32_t c = 0; c < kShmBusChannels; ++c) {
            float v = 0.0f;
            if (c < channels) v = planes ? (planes[c] ? planes[c][i] : 0.0f) : inter[size_t(i) * channels + c];
            r.data[c][idx] = v;
        }
    }
    r.write_pos.store(w + n, std::memory_order_release);
    return n;
}

uint32_t ShmAudioBus::ring_read(ShmRing& r, float* const* planes, float* inter, uint32_t channels,
                                uint32_t frames, bool zero_fill) {
    channels = std::min(channels, kShmBusChannels);
    const uint64_t rd = r.read_pos.load(std::memory_order_relaxed);
    const uint64_t w = r.write_pos.load(std::memory_order_acquire);
    const uint32_t avail = uint32_t(std::min<uint64_t>(w - rd, kShmBusFrames));
    const uint32_t n = std::min(avail, frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const bool have = i < n;
        const uint32_t idx = uint32_t((rd + i) & (kShmBusFrames - 1));
        if (!have && !zero_fill) break;
        for (uint32_t c = 0; c < channels; ++c) {
            const float v = have ? r.data[c][idx] : 0.0f;
            if (planes) { if (planes[c]) planes[c][i] = v; }
            else inter[size_t(i) * channels + c] = v;
        }
    }
    if (n < frames && zero_fill) r.underruns.fetch_add(1, std::memory_order_relaxed);
    r.read_pos.store(rd + n, std::memory_order_release);
    return n;
}

uint32_t ShmAudioBus::driver_write_tx(const float* const* planes, uint32_t channels, uint32_t frames) {
    return bus_ ? ring_write(bus_->tx, planes, nullptr, channels, frames) : 0;
}
uint32_t ShmAudioBus::driver_write_tx_interleaved(const float* in, uint32_t channels, uint32_t frames) {
    return bus_ ? ring_write(bus_->tx, nullptr, in, channels, frames) : 0;
}
uint32_t ShmAudioBus::driver_read_rx(float* const* planes, uint32_t channels, uint32_t frames) {
    return bus_ ? ring_read(bus_->rx, planes, nullptr, channels, frames, true) : 0;
}
uint32_t ShmAudioBus::driver_read_rx_interleaved(float* out, uint32_t channels, uint32_t frames) {
    return bus_ ? ring_read(bus_->rx, nullptr, out, channels, frames, true) : 0;
}

void ShmAudioBus::driver_set_running(bool running, uint32_t sample_rate, uint32_t buffer_size,
                                     const char* name) {
    if (!bus_) return;
    ShmBusHeader& h = bus_->hdr;
    if (running) {
        h.sample_rate.store(sample_rate);
        h.buffer_size.store(buffer_size);
        h.active_channels.store(kShmBusChannels);
        if (name) {
            std::strncpy(h.driver_name, name, sizeof(h.driver_name) - 1);
            h.driver_name[sizeof(h.driver_name) - 1] = 0;
        }
        // Consumer owns read_pos: start RX from the newest data the host has written.
        bus_->rx.read_pos.store(bus_->rx.write_pos.load(std::memory_order_acquire), std::memory_order_release);
        driver_heartbeat();
    }
    h.driver_running.store(running ? 1u : 0u, std::memory_order_release);
}

uint32_t ShmAudioBus::host_tx_available() const {
    if (!bus_) return 0;
    return uint32_t(bus_->tx.write_pos.load(std::memory_order_acquire) - bus_->tx.read_pos.load(std::memory_order_relaxed));
}
uint32_t ShmAudioBus::host_read_tx(float* const* planes, uint32_t channels, uint32_t frames) {
    return bus_ ? ring_read(bus_->tx, planes, nullptr, channels, frames, false) : 0;
}
uint32_t ShmAudioBus::host_rx_fill() const {
    if (!bus_) return 0;
    return uint32_t(bus_->rx.write_pos.load(std::memory_order_relaxed) - bus_->rx.read_pos.load(std::memory_order_acquire));
}
uint32_t ShmAudioBus::host_write_rx(const float* const* planes, uint32_t channels, uint32_t frames) {
    return bus_ ? ring_write(bus_->rx, planes, nullptr, channels, frames) : 0;
}
void ShmAudioBus::host_drop_stale_tx() {
    if (bus_) bus_->tx.read_pos.store(bus_->tx.write_pos.load(std::memory_order_acquire), std::memory_order_release);
}

bool ShmAudioBus::driver_connected(uint32_t timeout_ms) const {
    if (!bus_ || bus_->hdr.driver_running.load(std::memory_order_acquire) == 0) return false;
    const uint64_t hb = bus_->hdr.driver_heartbeat_ms.load(std::memory_order_acquire);
    return shm_bus_now_ms() - hb <= timeout_ms;
}
bool ShmAudioBus::host_connected(uint32_t timeout_ms) const {
    if (!bus_) return false;
    const uint64_t hb = bus_->hdr.host_heartbeat_ms.load(std::memory_order_acquire);
    return hb != 0 && shm_bus_now_ms() - hb <= timeout_ms;
}

}  // namespace lv
