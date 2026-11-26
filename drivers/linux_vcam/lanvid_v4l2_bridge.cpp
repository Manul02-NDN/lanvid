// lanvid-vcam-bridge - Linux virtual cameras "LanVid Cam 1..8" + "LanVid Program Out".
//
// Reads LanVidVideoBus_v1 (published by LanVid Video) and writes one YUYV stream per camera into
// v4l2loopback output devices, so OBS, Zoom, Teams, Chrome/Firefox (WebRTC), ffmpeg, GStreamer see each
// phone as /dev/videoN. Devices are matched by their v4l2loopback card label, not by number:
//
//   sudo modprobe v4l2loopback devices=9 video_nr=10,11,12,13,14,15,16,17,18 exclusive_caps=1 \
//        card_label="LanVid Cam 1,LanVid Cam 2,LanVid Cam 3,LanVid Cam 4,LanVid Cam 5,LanVid Cam 6,LanVid Cam 7,LanVid Cam 8,LanVid Program Out"
//   lanvid-vcam-bridge [--width 1920] [--height 1080] [--fps 30] [--list]
//
// (the .deb installs /etc/modprobe.d/lanvid-v4l2loopback.conf and a systemd --user service for this).

#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "lanvid_vbus_c.h"
#include "video/video_bus.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Device {
    int fd = -1;
    int32_t slot = -1;
    std::string path;
    std::string label;
};

// Finds v4l2loopback devices whose card label is one of the LanVid device names.
std::vector<Device> scan_devices(bool verbose) {
    std::vector<Device> out;
    for (int n = 0; n < 64; ++n) {
        const std::string path = "/dev/video" + std::to_string(n);
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        v4l2_capability cap;
        std::memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            ::close(fd);
            continue;
        }
        const std::string card(reinterpret_cast<const char*>(cap.card));
        int32_t slot = -1;
        for (int32_t i = 0; i < lvb_device_count(); ++i)
            if (card == lvb_device_name(i)) slot = i;
        if (verbose) std::printf("  %s: %s%s\n", path.c_str(), card.c_str(), slot >= 0 ? "  <- LanVid" : "");
        bool taken = false;
        for (const auto& d : out) taken = taken || d.slot == slot;
        if (slot < 0 || taken) {
            ::close(fd);
            continue;
        }
        Device d;
        d.fd = fd;
        d.slot = slot;
        d.path = path;
        d.label = card;
        out.push_back(d);
    }
    return out;
}

bool configure(const Device& d, uint32_t w, uint32_t h, uint32_t fps) {
    v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = w * 2;
    fmt.fmt.pix.sizeimage = w * h * 2;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;  // BT.601 limited range (video_bus_bgra_to_yuy2)
    if (ioctl(d.fd, VIDIOC_S_FMT, &fmt) != 0) {
        std::fprintf(stderr, "%s: VIDIOC_S_FMT %ux%u YUYV failed: %s\n", d.path.c_str(), w, h, std::strerror(errno));
        return false;
    }
    v4l2_streamparm parm;
    std::memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = fps;
    (void)ioctl(d.fd, VIDIOC_S_PARM, &parm);  // optional on older v4l2loopback versions
    return true;
}

uint32_t arg_u32(int argc, char** argv, const char* name, uint32_t def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return uint32_t(std::strtoul(argv[i + 1], nullptr, 10));
    return def;
}

bool has_arg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t w = arg_u32(argc, argv, "--width", 1920) & ~1u;
    uint32_t h = arg_u32(argc, argv, "--height", 1080) & ~1u;
    const uint32_t fps = std::max<uint32_t>(1, std::min<uint32_t>(60, arg_u32(argc, argv, "--fps", 30)));
    if (w < 160 || h < 120 || w > lv::kVideoBusMaxWidth || h > lv::kVideoBusMaxWidth) {
        std::fprintf(stderr, "invalid size %ux%u\n", w, h);
        return 2;
    }
    const bool list = has_arg(argc, argv, "--list");
    std::printf("lanvid-vcam-bridge: scanning /dev/video* for LanVid v4l2loopback devices\n");
    std::vector<Device> devs = scan_devices(true);
    if (list) return devs.empty() ? 1 : 0;
    if (devs.empty()) {
        std::fprintf(stderr,
                     "No v4l2loopback device labelled 'LanVid Cam N' found. Load the module:\n"
                     "  sudo modprobe v4l2loopback devices=9 video_nr=10,11,12,13,14,15,16,17,18 exclusive_caps=1 "
                     "card_label=\"LanVid Cam 1,LanVid Cam 2,LanVid Cam 3,LanVid Cam 4,LanVid Cam 5,LanVid Cam 6,"
                     "LanVid Cam 7,LanVid Cam 8,LanVid Program Out\"\n");
        return 1;
    }
    for (auto it = devs.begin(); it != devs.end();) {
        if (!configure(*it, w, h, fps)) {
            ::close(it->fd);
            it = devs.erase(it);
        } else {
            std::printf("  %s -> %s (%ux%u YUYV @ %u fps)\n", it->path.c_str(), it->label.c_str(), w, h, fps);
            ++it;
        }
    }
    if (devs.empty()) return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    std::vector<uint8_t> bgra(size_t(w) * h * 4), yuyv(size_t(w) * h * 2);
    const auto period = std::chrono::nanoseconds(1000000000ll / fps);
    auto next = std::chrono::steady_clock::now();
    while (!g_stop) {
        for (const auto& d : devs) {
            lvb_render_bgra(d.slot, bgra.data(), w, h, w * 4);
            lv::video_bus_bgra_to_yuy2(bgra.data(), w, h, w * 4, yuyv.data(), w * 2);
            const ssize_t n = ::write(d.fd, yuyv.data(), yuyv.size());
            if (n < 0 && errno != EAGAIN && errno != EINTR)
                std::fprintf(stderr, "%s: write failed: %s\n", d.path.c_str(), std::strerror(errno));
        }
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (now - next > period * 3) next = now;  // fell behind: resync instead of bursting
        std::this_thread::sleep_until(next);
    }
    for (auto& d : devs) ::close(d.fd);
    return 0;
}
