#pragma once
// VirtualCamBridge — Lock-free LanVidVideoBus_v1 publisher ("LanVid Cam 1..8" + "LanVid Program Out")
// with Lavender standby placeholder support and isolated mutexes.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common/image.hpp"
#include "video/video_bus.hpp"
#include "hub/camera_slot.hpp"

namespace lv::hub {

class VirtualCamBridge {
public:
    VirtualCamBridge() = default;
    ~VirtualCamBridge();

    VirtualCamBridge(const VirtualCamBridge&) = delete;
    VirtualCamBridge& operator=(const VirtualCamBridge&) = delete;

    bool open(const std::string& bus_name = "", uint32_t vcam_count = 8, bool placeholder = true);
    void close();
    bool is_open() const;

    void configure(uint32_t vcam_count, bool placeholder);
    void set_lip_sync_ms(int ms) { lip_sync_ms_.store(ms, std::memory_order_relaxed); }
    int lip_sync_ms() const { return lip_sync_ms_.load(std::memory_order_relaxed); }

    uint32_t vcam_count() const { return vcam_count_.load(std::memory_order_relaxed); }
    bool placeholder_enabled() const { return placeholder_.load(std::memory_order_relaxed); }

    // Publishes a decoded/playout BGRA frame to slot (0..7) and/or PGM Out (slot 8).
    void publish_frame(uint32_t slot, const lv::Image& bgra, bool as_program, uint64_t now_us, double fps);

    // Refreshes header heartbeat + slot labels / standby states (never blocks callers).
    void sync_slots_metadata(const std::array<CameraSlotSnapshot, kMaxHubSlots>& snaps, int program_slot);

private:
    lv::VideoBus bus_;
    mutable std::mutex mu_;
    std::vector<uint8_t> nv12_scratch_;
    std::atomic<uint32_t> vcam_count_{8};
    std::atomic<bool> placeholder_{true};
    std::atomic<int> lip_sync_ms_{0};
};

}  // namespace lv::hub
