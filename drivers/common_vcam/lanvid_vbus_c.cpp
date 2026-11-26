#include "lanvid_vbus_c.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "video/video_bus.hpp"

namespace {

std::mutex g_open_mu;
lv::VideoBus g_bus;
uint64_t g_retry_ms = 0;

struct SlotState {
    std::mutex mu;
    std::vector<uint8_t> src;      // copy of the bus frame
    std::vector<uint8_t> out;      // converted output (w*h*4), reused while the frame is unchanged
    uint32_t out_w = 0, out_h = 0;
    uint64_t out_seq = 0;
    bool out_live = false;
    uint64_t placeholder_ms = 0;
};
SlotState g_slots[lv::kVideoBusSlots];

bool ensure_open() {
    std::lock_guard<std::mutex> lk(g_open_mu);
    if (g_bus.is_open()) return true;
    const uint64_t now = lv::video_bus_now_ms();
    if (now < g_retry_ms) return false;
    if (!g_bus.open()) {
        g_retry_ms = now + 1000;  // LanVid Video not running yet: retry once a second
        return false;
    }
    return true;
}

void fill_black(uint8_t* p, size_t px) {
    const uint8_t pat[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < px; ++i) std::memcpy(p + i * 4, pat, 4);
}

void copy_out(const SlotState& st, uint8_t* dst, uint32_t stride) {
    for (uint32_t y = 0; y < st.out_h; ++y)
        std::memcpy(dst + size_t(y) * stride, st.out.data() + size_t(y) * st.out_w * 4, size_t(st.out_w) * 4);
}

}  // namespace

extern "C" int32_t lvb_device_count(void) { return int32_t(lv::kVideoBusSlots); }

extern "C" const char* lvb_device_name(int32_t slot) { return lv::video_bus_device_name(uint32_t(slot)); }

extern "C" int32_t lvb_render_bgra(int32_t slot, uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride) {
    if (slot < 0 || slot >= int32_t(lv::kVideoBusSlots) || !dst || !w || !h || stride < w * 4) return 0;
    SlotState& st = g_slots[slot];
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.out_w != w || st.out_h != h) {
        st.out.assign(size_t(w) * h * 4, 0);
        st.out_w = w;
        st.out_h = h;
        st.out_seq = 0;
        st.out_live = false;
        st.placeholder_ms = 0;
    }
    const bool open = ensure_open();
    const lv::VideoBusHeader* hdr = open ? g_bus.header() : nullptr;
    const lv::VideoBusSlot* s = open ? g_bus.slot(uint32_t(slot)) : nullptr;
    const uint64_t now = lv::video_bus_now_ms();
    const uint64_t hb = hdr ? hdr->host_heartbeat_ms.load(std::memory_order_relaxed) : 0;
    const bool host_alive = hdr && hb && (hb >= now || now - hb < 3000);
    const bool enabled = !host_alive || uint32_t(slot) == lv::kVideoBusProgramSlot ||
                         uint32_t(slot) < hdr->active_vcams.load(std::memory_order_relaxed);

    if (s && enabled) {
        const uint64_t seq = s->frame_seq.load(std::memory_order_acquire);
        const uint64_t wrote = s->last_write_ms.load(std::memory_order_relaxed);
        const bool fresh = s->connected.load() && !s->standby.load() && (wrote >= now ? 0 : now - wrote) <= lv::kVideoBusStaleMs;
        if (fresh && st.out_live && seq == st.out_seq) {
            copy_out(st, dst, stride);  // unchanged frame
            return 1;
        }
        if (fresh) {
            if (st.src.size() < lv::kVideoBusFrameBytes) st.src.resize(lv::kVideoBusFrameBytes);
            lv::VideoBus::FrameInfo fi;
            if (g_bus.read_latest(uint32_t(slot), st.src.data(), st.src.size(), fi) && fi.width >= 2 && fi.height >= 2) {
                fill_black(st.out.data(), size_t(w) * h);
                uint32_t fw = w, fh = h;
                const uint64_t lhs = uint64_t(fi.width) * h, rhs = uint64_t(fi.height) * w;
                if (lhs > rhs + rhs / 100) fh = std::max<uint32_t>(2, uint32_t(uint64_t(w) * fi.height / fi.width));
                else if (rhs > lhs + lhs / 100) fw = std::max<uint32_t>(2, uint32_t(uint64_t(h) * fi.width / fi.height));
                uint8_t* origin = st.out.data() + size_t((h - fh) / 2) * w * 4 + size_t((w - fw) / 2) * 4;
                lv::video_bus_convert_to_bgra(st.src.data(), fi, origin, fw, fh, w * 4);
                st.out_seq = fi.frame_seq;
                st.out_live = true;
                copy_out(st, dst, stride);
                return 1;
            }
        }
    }
    // Placeholder (re-rendered twice a second for the pulse) or black when the host disabled it.
    if (st.out_live || now - st.placeholder_ms >= 500 || !st.placeholder_ms) {
        const bool placeholder = !host_alive || hdr->placeholder_enabled.load(std::memory_order_relaxed) != 0;
        if (placeholder) {
            const char* line2 = !host_alive                                         ? "LanVid Video is not running"
                                : !enabled                                          ? "Disabled in LanVid Video"
                                : (s && s->standby.load(std::memory_order_relaxed)) ? "Standby"
                                                                                    : "No Signal / Standby";
            lv::video_bus_render_placeholder_bgra(st.out.data(), w, h, lv::video_bus_device_name(uint32_t(slot)), line2, now);
        } else {
            fill_black(st.out.data(), size_t(w) * h);
        }
        st.out_live = false;
        st.placeholder_ms = now;
    }
    copy_out(st, dst, stride);
    return 0;
}
