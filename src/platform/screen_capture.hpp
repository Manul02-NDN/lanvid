#pragma once
// Windows low-latency desktop screen capture:
//  - Primary: DXGI Desktop Duplication (D3D11) (< 1 ms/frame)
//  - Automatic fallback: Win32 GDI BitBlt (works in RDP / headless / access-lost situations)

#include <memory>
#include <string>
#include <vector>

#include "common/image.hpp"

namespace lv {

struct MonitorInfo {
    int index = 0;
    std::string name;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    bool primary = false;
};

enum class CaptureBackend { kAuto, kDxgi, kGdi };

std::vector<MonitorInfo> list_monitors();

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool open(int monitor_index = 0, CaptureBackend backend = CaptureBackend::kAuto);
    void close();

    int native_width() const;
    int native_height() const;
    const char* active_backend_name() const;

    // Captures current desktop frame into `out` (sets out.order = PixelOrder::kBGRA).
    // If `out` is empty, allocates it to (native_width, native_height).
    // If `out` has different dimensions, rescales cleanly via fast bilinear filter.
    bool capture(Image& out, int timeout_ms = 16);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lv
