#include "video/video_bus.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lv {

namespace {
constexpr size_t kHeaderBytes = 4096;  // header page; slots follow
size_t layout_size() {
    return kHeaderBytes + sizeof(VideoBusSlot) * kVideoBusSlots + size_t(kVideoBusSlots) * 2 * kVideoBusFrameBytes;
}
}  // namespace

uint64_t video_bus_now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool VideoBus::open(const char* name) {
    close();
#if defined(__ANDROID__)
    (void)name;
    return false;  // no OS virtual cameras on Android (bionic has no POSIX shm_open)
#else
    const char* nm = name ? name : kVideoBusName;
    const size_t size = layout_size();
    bool created = false;
#if defined(_WIN32)
    char full[128];
    std::snprintf(full, sizeof(full), "Local\\%s", nm);
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(uint64_t(size) >> 32),
                                  DWORD(size & 0xFFFFFFFFu), full);
    if (!h) return false;
    created = GetLastError() != ERROR_ALREADY_EXISTS;
    void* mem = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!mem) {
        CloseHandle(h);
        return false;
    }
    mapping_ = h;
#else
    std::snprintf(shm_name_, sizeof(shm_name_), "/%s", nm);
    int fd = shm_open(shm_name_, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd >= 0) {
        created = true;
        fchmod(fd, 0666);  // readable by the Camera Extension / other users regardless of umask
        if (ftruncate(fd, off_t(size)) != 0) {
            ::close(fd);
            shm_unlink(shm_name_);
            return false;
        }
    } else {
        fd = shm_open(shm_name_, O_RDWR, 0666);
        if (fd < 0) return false;
        struct stat st {};
        bool size_ok = false;
        for (int wait_i = 0; wait_i < 20; ++wait_i) {
            if (fstat(fd, &st) == 0 && size_t(st.st_size) >= size) {
                size_ok = true;
                break;
            }
            usleep(1000);
        }
        if (!size_ok) {
            ::close(fd);
            return false;  // older / foreign layout: never write into it
        }
    }
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) return false;
#endif
    std::lock_guard<std::mutex> lk(host_mu_);
    base_ = mem;
    size_ = size;
    hdr_ = static_cast<VideoBusHeader*>(mem);
    slots_ = reinterpret_cast<VideoBusSlot*>(static_cast<uint8_t*>(mem) + kHeaderBytes);
    frames_ = reinterpret_cast<uint8_t*>(slots_ + kVideoBusSlots);
    if (created) {
        std::memset(mem, 0, kHeaderBytes + sizeof(VideoBusSlot) * kVideoBusSlots);
        hdr_->version = kVideoBusVersion;
        hdr_->slots = kVideoBusSlots;
        hdr_->max_width = kVideoBusMaxWidth;
        hdr_->max_height = kVideoBusMaxHeight;
        hdr_->frame_bytes = uint32_t(kVideoBusFrameBytes);
        hdr_->active_vcams.store(kVideoBusCameraSlots, std::memory_order_relaxed);
        hdr_->placeholder_enabled.store(1, std::memory_order_relaxed);
        for (uint32_t i = 0; i < kVideoBusSlots; ++i) {
            std::snprintf(slots_[i].label, sizeof(slots_[i].label), "%s", video_bus_device_name(i));
            slots_[i].enabled.store(1, std::memory_order_relaxed);
        }
        hdr_->magic.store(kVideoBusMagic, std::memory_order_release);
    } else {
        for (int wait_i = 0; wait_i < 50 && hdr_->magic.load(std::memory_order_acquire) != kVideoBusMagic; ++wait_i) {
#if defined(_WIN32)
            Sleep(1);
#else
            usleep(1000);
#endif
        }
        if (hdr_->magic.load(std::memory_order_acquire) != kVideoBusMagic) {
            // Segment existed but was never initialized (stale crash): initialize cleanly now.
            std::memset(mem, 0, kHeaderBytes + sizeof(VideoBusSlot) * kVideoBusSlots);
            hdr_->version = kVideoBusVersion;
            hdr_->slots = kVideoBusSlots;
            hdr_->max_width = kVideoBusMaxWidth;
            hdr_->max_height = kVideoBusMaxHeight;
            hdr_->frame_bytes = uint32_t(kVideoBusFrameBytes);
            hdr_->active_vcams.store(kVideoBusCameraSlots, std::memory_order_relaxed);
            hdr_->placeholder_enabled.store(1, std::memory_order_relaxed);
            for (uint32_t i = 0; i < kVideoBusSlots; ++i) {
                std::snprintf(slots_[i].label, sizeof(slots_[i].label), "%s", video_bus_device_name(i));
                slots_[i].enabled.store(1, std::memory_order_relaxed);
            }
            hdr_->magic.store(kVideoBusMagic, std::memory_order_release);
        }
    }
    return hdr_->version == kVideoBusVersion;
#endif
}

void VideoBus::close() {
    std::lock_guard<std::mutex> lk(host_mu_);
    if (!base_) return;
#if defined(_WIN32)
    UnmapViewOfFile(base_);
    if (mapping_) CloseHandle(HANDLE(mapping_));
    mapping_ = nullptr;
#else
    munmap(base_, size_);
#endif
    base_ = nullptr;
    hdr_ = nullptr;
    slots_ = nullptr;
    frames_ = nullptr;
    size_ = 0;
}

uint8_t* VideoBus::frame_buffer(uint32_t slot_index, uint32_t buf) const {
    if (!frames_ || slot_index >= kVideoBusSlots || buf > 1) return nullptr;
    return frames_ + (size_t(slot_index) * 2 + buf) * kVideoBusFrameBytes;
}

bool VideoBus::publish(uint32_t slot_index, VideoBusFormat fmt, uint32_t width, uint32_t height, const uint8_t* data,
                       uint32_t src_stride, uint64_t timestamp_us, uint32_t fps_x100) {
    std::lock_guard<std::mutex> lk(host_mu_);
    VideoBusSlot* s = slot(slot_index);
    if (!s || !data || width == 0 || height == 0) return false;
    size_t bytes = 0;
    uint32_t row = 0;
    if (fmt == VideoBusFormat::kBGRA) {
        row = width * 4;
        bytes = size_t(row) * height;
    } else if (fmt == VideoBusFormat::kNV12) {
        if ((width & 1) || (height & 1)) return false;
        row = width;
        bytes = size_t(width) * height * 3 / 2;
    } else {
        return false;
    }
    if (bytes > kVideoBusFrameBytes || width > kVideoBusMaxWidth || height > kVideoBusMaxHeight) return false;
    if (src_stride == 0) src_stride = row;

    const uint32_t back = (s->front.load(std::memory_order_relaxed) & 1u) ^ 1u;
    uint8_t* dst = frame_buffer(slot_index, back);
    if (!dst) return false;
    if (fmt == VideoBusFormat::kBGRA) {
        for (uint32_t y = 0; y < height; ++y) std::memcpy(dst + size_t(y) * row, data + size_t(y) * src_stride, row);
    } else {
        for (uint32_t y = 0; y < height; ++y) std::memcpy(dst + size_t(y) * row, data + size_t(y) * src_stride, row);
        const uint8_t* uv = data + size_t(src_stride) * height;
        uint8_t* duv = dst + size_t(row) * height;
        for (uint32_t y = 0; y < height / 2; ++y) std::memcpy(duv + size_t(y) * row, uv + size_t(y) * src_stride, row);
    }
    // seqlock publish: odd while the description changes, even afterwards (serialized by host_mu_)
    const uint64_t q = s->seq.load(std::memory_order_relaxed) & ~uint64_t(1);
    s->seq.store(q + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    s->width = width;
    s->height = height;
    s->stride = row;
    s->format = uint32_t(fmt);
    s->fps_x100 = fps_x100;
    s->timestamp_us = timestamp_us;
    s->front.store(back, std::memory_order_relaxed);
    s->frame_seq.fetch_add(1, std::memory_order_relaxed);
    s->last_write_ms.store(video_bus_now_ms(), std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->seq.store(q + 2, std::memory_order_release);
    return true;
}

void VideoBus::set_slot_info(uint32_t slot_index, const char* label, const char* source_name, bool connected,
                             bool standby) {
    std::lock_guard<std::mutex> lk(host_mu_);
    VideoBusSlot* s = slot(slot_index);
    if (!s) return;
    const uint64_t q = s->seq.load(std::memory_order_relaxed) & ~uint64_t(1);
    s->seq.store(q + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    if (label) std::snprintf(s->label, sizeof(s->label), "%s", label);
    if (source_name) std::snprintf(s->source_name, sizeof(s->source_name), "%s", source_name);
    std::atomic_thread_fence(std::memory_order_release);
    s->seq.store(q + 2, std::memory_order_release);
    s->connected.store(connected ? 1u : 0u, std::memory_order_release);
    s->standby.store(standby ? 1u : 0u, std::memory_order_release);
}

void VideoBus::set_active_vcams(uint32_t count, bool placeholder_enabled) {
    std::lock_guard<std::mutex> lk(host_mu_);
    if (!hdr_) return;
    count = std::min(count, kVideoBusCameraSlots);
    hdr_->active_vcams.store(count, std::memory_order_release);
    hdr_->placeholder_enabled.store(placeholder_enabled ? 1u : 0u, std::memory_order_release);
    for (uint32_t i = 0; i < kVideoBusCameraSlots; ++i) slots_[i].enabled.store(i < count ? 1u : 0u);
    slots_[kVideoBusProgramSlot].enabled.store(1u);
}

void VideoBus::heartbeat() {
    std::lock_guard<std::mutex> lk(host_mu_);
    if (hdr_) hdr_->host_heartbeat_ms.store(video_bus_now_ms(), std::memory_order_release);
}

bool VideoBus::read_latest(uint32_t slot_index, uint8_t* dst, size_t dst_cap, FrameInfo& info) const {
    info = FrameInfo{};
    VideoBusSlot* s = slot(slot_index);
    if (!s) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t q0 = s->seq.load(std::memory_order_acquire);
        if (q0 & 1u) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        FrameInfo fi;
        fi.width = s->width;
        fi.height = s->height;
        fi.stride = s->stride;
        fi.format = VideoBusFormat(s->format);
        fi.timestamp_us = s->timestamp_us;
        fi.frame_seq = s->frame_seq.load(std::memory_order_relaxed);
        std::memcpy(fi.label, s->label, sizeof(fi.label));
        fi.label[sizeof(fi.label) - 1] = 0;
        const uint32_t front = s->front.load(std::memory_order_relaxed);
        const bool dims_ok = fi.width > 0 && fi.height > 0 && fi.width <= kVideoBusMaxWidth &&
                             fi.height <= kVideoBusMaxHeight && front <= 1u;
        const size_t bytes = !dims_ok ? 0
                             : fi.format == VideoBusFormat::kBGRA ? size_t(fi.stride) * size_t(fi.height)
                             : fi.format == VideoBusFormat::kNV12 ? size_t(fi.stride) * size_t(fi.height) * 3 / 2
                                                                  : 0;
        const bool connected = s->connected.load(std::memory_order_acquire) != 0;
        const bool standby = s->standby.load(std::memory_order_acquire) != 0;
        const uint64_t now_ms = video_bus_now_ms(), wrote_ms = s->last_write_ms.load(std::memory_order_relaxed);
        const uint64_t age = wrote_ms >= now_ms ? 0 : now_ms - wrote_ms;  // host may have written after `now_ms`
        fi.fresh = connected && !standby && bytes > 0 && bytes <= kVideoBusFrameBytes &&
                   age <= kVideoBusStaleMs && fi.frame_seq > 0;
        if (fi.fresh && dst) {
            const uint8_t* src_buf = frame_buffer(slot_index, front);
            if (!src_buf || bytes > dst_cap) return false;
            std::memcpy(dst, src_buf, bytes);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s->seq.load(std::memory_order_acquire) != q0) continue;  // host published meanwhile: retry
        info = fi;
        return fi.fresh;
    }
    std::snprintf(info.label, sizeof(info.label), "%s", s->label);
    return false;
}

const char* video_bus_device_name(uint32_t slot_index) {
    static const char* names[kVideoBusSlots] = {"LanVid Cam 1", "LanVid Cam 2", "LanVid Cam 3",
                                                "LanVid Cam 4", "LanVid Cam 5", "LanVid Cam 6",
                                                "LanVid Cam 7", "LanVid Cam 8", "LanVid Program Out"};
    return slot_index < kVideoBusSlots ? names[slot_index] : "LanVid Cam";
}

// ---------------------------------------------------------------------------------------------------
// Placeholder card: soft lavender gradient + glass panel + 5x7 bitmap text (no font dependency).
// ---------------------------------------------------------------------------------------------------
namespace {
const uint8_t* glyph(char c) {
    static const uint8_t kSpace[7] = {0, 0, 0, 0, 0, 0, 0};
    static const struct {
        char c;
        uint8_t rows[7];
    } kFont[] = {
        {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
        {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
        {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
        {'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
        {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
        {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
        {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
        {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
        {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'0', {14, 17, 19, 21, 25, 17, 14}},
        {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
        {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},   {'6', {6, 8, 16, 30, 17, 17, 14}},
        {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
        {'-', {0, 0, 0, 31, 0, 0, 0}},       {'/', {1, 1, 2, 4, 8, 16, 16}},      {'.', {0, 0, 0, 0, 0, 12, 12}},
        {':', {0, 12, 12, 0, 12, 12, 0}},    {'(', {2, 4, 8, 8, 8, 4, 2}},        {')', {8, 4, 2, 2, 2, 4, 8}},
    };
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (const auto& g : kFont)
        if (g.c == c) return g.rows;
    return kSpace;
}

void blend(uint8_t* px, uint8_t b, uint8_t g, uint8_t r, float a) {
    px[0] = uint8_t(px[0] + (b - px[0]) * a);
    px[1] = uint8_t(px[1] + (g - px[1]) * a);
    px[2] = uint8_t(px[2] + (r - px[2]) * a);
    px[3] = 255;
}

void draw_text(uint8_t* dst, uint32_t w, uint32_t h, const char* text, int cx, int cy, int scale, uint8_t b,
               uint8_t g, uint8_t r) {
    if (!text) return;
    const int len = int(std::strlen(text));
    const int adv = 6 * scale;
    int x0 = cx - (len * adv) / 2;
    const int y0 = cy - (7 * scale) / 2;
    for (int i = 0; i < len; ++i, x0 += adv) {
        const uint8_t* rows = glyph(text[i]);
        for (int ry = 0; ry < 7; ++ry)
            for (int rx = 0; rx < 5; ++rx) {
                if (!(rows[ry] & (16 >> rx))) continue;
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx) {
                        const int px = x0 + rx * scale + sx, py = y0 + ry * scale + sy;
                        if (px < 0 || py < 0 || px >= int(w) || py >= int(h)) continue;
                        blend(dst + (size_t(py) * w + size_t(px)) * 4, b, g, r, 1.0f);
                    }
            }
    }
}
}  // namespace

void video_bus_render_placeholder_bgra(uint8_t* dst, uint32_t w, uint32_t h, const char* line1, const char* line2,
                                       uint64_t anim_ms) {
    if (!dst || !w || !h) return;
    // Soft Pastel Lavender gradient (top #E9E3FF -> bottom #C9D6FF)
    for (uint32_t y = 0; y < h; ++y) {
        const float t = float(y) / float(h);
        const uint8_t r = uint8_t(233 - 32 * t), g = uint8_t(227 - 13 * t), b = 255;
        uint8_t* row = dst + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    // Glass panel
    const uint32_t pw = w * 3 / 5, ph = h / 3, px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    for (uint32_t y = py0; y < py0 + ph; ++y)
        for (uint32_t x = px0; x < px0 + pw; ++x) blend(dst + (size_t(y) * w + x) * 4, 255, 250, 250, 0.55f);
    // Pulsing accent dot (lavender-violet)
    const float pulse = 0.5f + 0.5f * float(((anim_ms / 16) % 64) < 32 ? ((anim_ms / 16) % 32) : 31 - ((anim_ms / 16) % 32)) / 31.0f;
    const int dot_r = int(std::max<uint32_t>(4, h / 60));
    const int dcx = int(px0 + pw / 2), dcy = int(py0 + ph / 5);
    for (int y = -dot_r; y <= dot_r; ++y)
        for (int x = -dot_r; x <= dot_r; ++x)
            if (x * x + y * y <= dot_r * dot_r && dcx + x >= 0 && dcy + y >= 0 && dcx + x < int(w) && dcy + y < int(h))
                blend(dst + (size_t(dcy + y) * w + size_t(dcx + x)) * 4, 230, 110, 150, 0.4f + 0.6f * pulse);
    const int scale1 = std::max(2, int(h / 150)), scale2 = std::max(1, int(h / 260));
    draw_text(dst, w, h, line1, int(w / 2), int(py0 + ph / 2), scale1, 120, 60, 80);
    draw_text(dst, w, h, line2, int(w / 2), int(py0 + ph * 3 / 4), scale2, 170, 120, 130);
}

void video_bus_convert_to_bgra(const uint8_t* src, const VideoBus::FrameInfo& in, uint8_t* dst, uint32_t dw,
                               uint32_t dh, uint32_t dst_stride) {
    if (!src || !dst || !in.width || !in.height || !dw || !dh) return;
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t sy = uint32_t(uint64_t(y) * in.height / dh);
        uint8_t* drow = dst + size_t(y) * dst_stride;
        if (in.format == VideoBusFormat::kBGRA) {
            const uint8_t* srow = src + size_t(sy) * in.stride;
            for (uint32_t x = 0; x < dw; ++x) {
                const uint32_t sx = uint32_t(uint64_t(x) * in.width / dw);
                std::memcpy(drow + x * 4, srow + sx * 4, 4);
                drow[x * 4 + 3] = 255;
            }
        } else {
            const uint8_t* yrow = src + size_t(sy) * in.stride;
            const uint8_t* uvrow = src + size_t(in.stride) * in.height + size_t(sy / 2) * in.stride;
            for (uint32_t x = 0; x < dw; ++x) {
                const uint32_t sx = uint32_t(uint64_t(x) * in.width / dw);
                const int Y = yrow[sx] - 16, U = uvrow[sx & ~1u] - 128, V = uvrow[(sx & ~1u) + 1] - 128;
                const int c = 298 * Y;
                drow[x * 4 + 2] = uint8_t(std::clamp((c + 409 * V + 128) >> 8, 0, 255));
                drow[x * 4 + 1] = uint8_t(std::clamp((c - 100 * U - 208 * V + 128) >> 8, 0, 255));
                drow[x * 4 + 0] = uint8_t(std::clamp((c + 516 * U + 128) >> 8, 0, 255));
                drow[x * 4 + 3] = 255;
            }
        }
    }
}

namespace {
inline void rgb_to_yuv(int r, int g, int b, int& y, int& u, int& v) {
    y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}
}  // namespace

void video_bus_bgra_to_yuy2(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t bgra_stride, uint8_t* yuy2,
                            uint32_t yuy2_stride) {
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = bgra + size_t(y) * bgra_stride;
        uint8_t* d = yuy2 + size_t(y) * yuy2_stride;
        for (uint32_t x = 0; x + 1 < w; x += 2) {
            int y0, u0, v0, y1, u1, v1;
            rgb_to_yuv(s[x * 4 + 2], s[x * 4 + 1], s[x * 4 + 0], y0, u0, v0);
            rgb_to_yuv(s[x * 4 + 6], s[x * 4 + 5], s[x * 4 + 4], y1, u1, v1);
            d[x * 2 + 0] = uint8_t(std::clamp(y0, 0, 255));
            d[x * 2 + 1] = uint8_t(std::clamp((u0 + u1) / 2, 0, 255));
            d[x * 2 + 2] = uint8_t(std::clamp(y1, 0, 255));
            d[x * 2 + 3] = uint8_t(std::clamp((v0 + v1) / 2, 0, 255));
        }
    }
}

void video_bus_bgra_to_nv12(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t bgra_stride, uint8_t* nv12,
                            uint32_t y_stride) {
    uint8_t* uvp = nv12 + size_t(y_stride) * h;
    const uint32_t w2 = w & ~1u;
    const uint32_t h2 = h & ~1u;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = bgra + size_t(y) * bgra_stride;
        uint8_t* d = nv12 + size_t(y) * y_stride;
        for (uint32_t x = 0; x < w; ++x) {
            int Y, U, V;
            rgb_to_yuv(s[x * 4 + 2], s[x * 4 + 1], s[x * 4 + 0], Y, U, V);
            d[x] = uint8_t(std::clamp(Y, 0, 255));
            if (y < h2 && x < w2 && !(y & 1) && !(x & 1)) {
                uint8_t* uv = uvp + size_t(y / 2) * y_stride + x;
                uv[0] = uint8_t(std::clamp(U, 0, 255));
                uv[1] = uint8_t(std::clamp(V, 0, 255));
            }
        }
    }
}

}  // namespace lv
