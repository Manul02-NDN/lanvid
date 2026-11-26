#include "hub/vcam_bus.hpp"

#include <algorithm>
#include <cstdio>

namespace lv::hub {

VirtualCamBridge::~VirtualCamBridge() {
    close();
}

bool VirtualCamBridge::open(const std::string& bus_name, uint32_t vcam_count, bool placeholder) {
    close();
    vcam_count_.store(std::clamp<uint32_t>(vcam_count, 0, lv::kVideoBusCameraSlots), std::memory_order_relaxed);
    placeholder_.store(placeholder, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu_);
    if (!bus_.open(bus_name.empty() ? nullptr : bus_name.c_str())) {
        return false;
    }
    if (auto* hdr = bus_.header()) {
        std::snprintf(hdr->host_name, sizeof(hdr->host_name), "LanVid Video Hub");
    }
    bus_.set_active_vcams(vcam_count_.load(std::memory_order_relaxed), placeholder);
    bus_.heartbeat();
    return true;
}

void VirtualCamBridge::close() {
    std::lock_guard<std::mutex> lk(mu_);
    bus_.close();
}

bool VirtualCamBridge::is_open() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bus_.is_open();
}

void VirtualCamBridge::configure(uint32_t vcam_count, bool placeholder) {
    const uint32_t vc = std::clamp<uint32_t>(vcam_count, 0, lv::kVideoBusCameraSlots);
    vcam_count_.store(vc, std::memory_order_relaxed);
    placeholder_.store(placeholder, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu_);
    if (bus_.is_open()) {
        bus_.set_active_vcams(vc, placeholder);
        bus_.heartbeat();
    }
}

void VirtualCamBridge::publish_frame(uint32_t slot, const lv::Image& bgra, bool as_program, uint64_t now_us,
                                     double fps) {
    if (bgra.width <= 0 || bgra.height <= 0 || bgra.data.empty()) return;
    const uint32_t vc = vcam_count_.load(std::memory_order_relaxed);
    const bool to_slot = slot < lv::kVideoBusCameraSlots && slot < vc;
    const bool to_pgm = as_program;
    if (!to_slot && !to_pgm) return;

    std::lock_guard<std::mutex> lk(mu_);
    if (!bus_.is_open()) return;
    const uint32_t w = uint32_t(bgra.width);
    const uint32_t h = uint32_t(bgra.height);
    const uint32_t fps100 = uint32_t(std::max(0.0, fps) * 100.0);
    if (w * h <= 1920u * 1080u) {
        if (to_slot && bus_.publish(slot, lv::VideoBusFormat::kBGRA, w, h, bgra.data.data(), uint32_t(bgra.stride),
                                    now_us, fps100)) {
            if (auto* s = bus_.slot(slot)) {
                s->standby.store(0, std::memory_order_release);
                s->connected.store(1, std::memory_order_release);
            }
        }
        if (to_pgm && bus_.publish(lv::kVideoBusProgramSlot, lv::VideoBusFormat::kBGRA, w, h, bgra.data.data(),
                                   uint32_t(bgra.stride), now_us, fps100)) {
            if (auto* s = bus_.slot(lv::kVideoBusProgramSlot)) {
                s->standby.store(0, std::memory_order_release);
                s->connected.store(1, std::memory_order_release);
            }
        }
    } else {
        const uint32_t ew = w & ~1u;
        const uint32_t eh = h & ~1u;
        nv12_scratch_.resize(size_t(ew) * eh * 3u / 2u);
        lv::video_bus_bgra_to_nv12(bgra.data.data(), ew, eh, uint32_t(bgra.stride), nv12_scratch_.data(), ew);
        if (to_slot && bus_.publish(slot, lv::VideoBusFormat::kNV12, ew, eh, nv12_scratch_.data(), ew, now_us,
                                    fps100)) {
            if (auto* s = bus_.slot(slot)) {
                s->standby.store(0, std::memory_order_release);
                s->connected.store(1, std::memory_order_release);
            }
        }
        if (to_pgm && bus_.publish(lv::kVideoBusProgramSlot, lv::VideoBusFormat::kNV12, ew, eh, nv12_scratch_.data(),
                                   ew, now_us, fps100)) {
            if (auto* s = bus_.slot(lv::kVideoBusProgramSlot)) {
                s->standby.store(0, std::memory_order_release);
                s->connected.store(1, std::memory_order_release);
            }
        }
    }
}

void VirtualCamBridge::sync_slots_metadata(const std::array<CameraSlotSnapshot, kMaxHubSlots>& snaps,
                                           int program_slot) {
    const uint32_t vc = vcam_count_.load(std::memory_order_relaxed);
    const bool ph = placeholder_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu_);
    if (!bus_.is_open()) return;
    bus_.set_active_vcams(vc, ph);
    bus_.heartbeat();
    for (uint32_t i = 0; i < lv::kVideoBusCameraSlots; ++i) {
        const auto& s = snaps[i];
        const std::string tag_lbl = !s.settings.assigned_cam_tag.empty() ? s.settings.assigned_cam_tag : s.label;
        const std::string lbl = (tag_lbl != s.label && !s.label.empty()) ? (tag_lbl + " • " + s.label) : tag_lbl;
        bus_.set_slot_info(i, lbl.c_str(), s.source_name.c_str(), s.streaming, !s.streaming && ph);
    }
    const bool pgm_live = (program_slot >= 0 && program_slot < int(kMaxHubSlots)) && snaps[program_slot].streaming;
    const char* pgm_src = (program_slot >= 0 && program_slot < int(kMaxHubSlots))
                              ? snaps[program_slot].label.c_str()
                              : "";
    bus_.set_slot_info(lv::kVideoBusProgramSlot, "LanVid Program Out", pgm_src, pgm_live, !pgm_live && ph);
}

}  // namespace lv::hub
