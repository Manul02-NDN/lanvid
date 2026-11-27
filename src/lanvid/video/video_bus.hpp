#pragma once
// LanVidVideoBus_v1: cross-process shared-memory video bus between LanVid Video (the "host", which
// decodes every camera) and the OS virtual camera drivers ("LanVid Cam 1..8" + "LanVid Program Out":
// DirectShow filter on Windows, CoreMediaIO Camera Extension on macOS, v4l2loopback on Linux).
//
// One slot per camera + one Program slot. Each slot holds the LATEST frame only (a virtual camera
// shows "now", it never queues): two frame buffers per slot, the host writes the back buffer and then
// publishes it; readers copy the published buffer and re-check a 64-bit sequence (seqlock), so a reader
// can never return a torn frame and never blocks the host.
//
// Frame formats: BGRA (up to 1920x1080, what the decoders produce) or NV12 (up to 3840x2160).
// When a slot has no fresh frame (camera disconnected / standby) a reader renders the lavender
// "LanVid Cam N - No Signal / Standby" card itself (render_placeholder), so client apps never stall.
//
// Self-contained (no lanvid_core dependency): drivers compile video_bus.cpp directly.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace lv {

constexpr uint32_t kVideoBusMagic = 0x4256564Cu;  // "LVVB"
constexpr uint32_t kVideoBusVersion = 1;
constexpr uint32_t kVideoBusCameraSlots = 8;
constexpr uint32_t kVideoBusProgramSlot = kVideoBusCameraSlots;  // index 8 = "LanVid Program Out"
constexpr uint32_t kVideoBusSlots = kVideoBusCameraSlots + 1;
constexpr uint32_t kVideoBusMaxWidth = 3840;
constexpr uint32_t kVideoBusMaxHeight = 2160;
// One buffer fits 4K NV12 (12.4 MB) or 1080p BGRA (8.3 MB).
constexpr size_t kVideoBusFrameBytes = size_t(kVideoBusMaxWidth) * kVideoBusMaxHeight * 3 / 2;
constexpr uint64_t kVideoBusStaleMs = 750;  // no new frame for this long -> placeholder
#if defined(__APPLE__)
// POSIX shm names on macOS are limited to 31 characters; a sandboxed Camera Extension may only open
// names that start with its App Group, so the name can be overridden at build time.
#ifndef LANVID_VIDEO_BUS_NAME
#define LANVID_VIDEO_BUS_NAME "LanVidVideoBus_v1"
#endif
constexpr const char* kVideoBusName = LANVID_VIDEO_BUS_NAME;
#else
constexpr const char* kVideoBusName = "LanVidVideoBus_v1";
#endif

enum class VideoBusFormat : uint32_t { kNone = 0, kBGRA = 1, kNV12 = 2 };

struct alignas(64) VideoBusSlot {
    std::atomic<uint64_t> seq;          // even = stable, odd = host is publishing
    std::atomic<uint64_t> frame_seq;    // published frames (monotonic)
    std::atomic<uint64_t> last_write_ms;
    std::atomic<uint32_t> front;        // 0/1: buffer that holds the published frame
    std::atomic<uint32_t> connected;    // camera bound to this slot and streaming
    std::atomic<uint32_t> standby;      // camera deliberately in standby (host decision)
    std::atomic<uint32_t> enabled;      // virtual device exposed (host setting "number of virtual cams")
    // Published frame description (valid together with `seq`)
    uint32_t width;
    uint32_t height;
    uint32_t stride;                    // bytes per row (BGRA) / luma row stride (NV12)
    uint32_t format;                    // VideoBusFormat
    uint32_t fps_x100;
    uint32_t reserved0;
    uint64_t timestamp_us;
    char label[64];                     // e.g. "CAM 1 - Wide"
    char source_name[64];               // e.g. "iPhone 15 Pro"
};

struct VideoBusHeader {
    std::atomic<uint32_t> magic;
    uint32_t version;
    uint32_t slots;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t frame_bytes;
    std::atomic<uint64_t> host_heartbeat_ms;
    std::atomic<uint32_t> active_vcams;      // how many "LanVid Cam N" devices the host wants exposed
    std::atomic<uint32_t> placeholder_enabled;
    char host_name[64];
};

uint64_t video_bus_now_ms();

class VideoBus {
public:
    VideoBus() = default;
    ~VideoBus() { close(); }
    VideoBus(const VideoBus&) = delete;
    VideoBus& operator=(const VideoBus&) = delete;

    // Creates the segment if missing, otherwise attaches. `name` overrides kVideoBusName (tests).
    bool open(const char* name = nullptr);
    void close();
    bool is_open() const { return base_ != nullptr; }
    VideoBusHeader* header() const { return hdr_; }
    VideoBusSlot* slot(uint32_t i) const { return (hdr_ && i < kVideoBusSlots) ? &slots_[i] : nullptr; }

    // ---- host side ----
    // Publishes one frame into `slot_index` (BGRA up to 1920x1080 or NV12 up to 4K). `src_stride` 0 =
    // tightly packed. Returns false if the frame does not fit or the bus is closed.
    bool publish(uint32_t slot_index, VideoBusFormat fmt, uint32_t width, uint32_t height, const uint8_t* data,
                 uint32_t src_stride, uint64_t timestamp_us, uint32_t fps_x100 = 0);
    void set_slot_info(uint32_t slot_index, const char* label, const char* source_name, bool connected,
                       bool standby);
    void set_active_vcams(uint32_t count, bool placeholder_enabled);
    void heartbeat();

    // ---- reader side (virtual camera drivers) ----
    struct FrameInfo {
        uint32_t width = 0, height = 0, stride = 0;
        VideoBusFormat format = VideoBusFormat::kNone;
        uint64_t frame_seq = 0;
        uint64_t timestamp_us = 0;
        bool fresh = false;   // false -> caller should show the placeholder
        char label[64] = {};
    };
    // Copies the latest frame of `slot_index` into `dst` (capacity `dst_cap`). Returns false when no
    // fresh frame is available (camera gone / standby / stale) - `info.label` is still filled.
    bool read_latest(uint32_t slot_index, uint8_t* dst, size_t dst_cap, FrameInfo& info) const;

    uint8_t* frame_buffer(uint32_t slot_index, uint32_t buf) const;

private:
    mutable std::mutex host_mu_;
    void* base_ = nullptr;
    size_t size_ = 0;
    VideoBusHeader* hdr_ = nullptr;
    VideoBusSlot* slots_ = nullptr;
    uint8_t* frames_ = nullptr;
#if defined(_WIN32)
    void* mapping_ = nullptr;
#else
    char shm_name_[64] = {};
#endif
};

// ---- pixel helpers shared by host and drivers (plain C++, no SIMD dependency) ----

// Renders the lavender "LanVid Cam N - No Signal / Standby" card as BGRA (stride = width*4).
void video_bus_render_placeholder_bgra(uint8_t* dst, uint32_t width, uint32_t height, const char* line1,
                                       const char* line2, uint64_t anim_ms);
// Nearest-neighbour scale + convert any bus frame to BGRA / YUY2 / NV12 at the requested size.
void video_bus_convert_to_bgra(const uint8_t* src, const VideoBus::FrameInfo& in, uint8_t* dst, uint32_t dw,
                               uint32_t dh, uint32_t dst_stride);
void video_bus_bgra_to_yuy2(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t bgra_stride, uint8_t* yuy2,
                            uint32_t yuy2_stride);
void video_bus_bgra_to_nv12(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t bgra_stride, uint8_t* nv12,
                            uint32_t y_stride);
// Device names: "LanVid Cam 1".."LanVid Cam 8", slot 8 = "LanVid Program Out".
const char* video_bus_device_name(uint32_t slot_index);

}  // namespace lv
