#pragma once
// Windows Media Foundation webcam capture (zero external dependencies).
// Runs a dedicated background reader thread so grab() always delivers the freshest BGRA frame.

#include <memory>
#include <string>
#include <vector>

#include "common/image.hpp"

namespace lv {

struct CameraInfo {
    int index = 0;
    std::string name;
};

std::vector<CameraInfo> list_cameras();

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    // Opens webcam `device_index`, selecting the native format closest to (req_w, req_h, req_fps).
    bool open(int device_index = 0, int req_w = 1280, int req_h = 720, int req_fps = 30);
    void close();

    int native_width() const;
    int native_height() const;
    std::string device_name() const;

    // Copies (and optionally rescales) the latest captured frame into `out` (PixelOrder::kBGRA).
    // Blocks up to `wait_ms` only if the very first frame has not arrived yet.
    bool grab(Image& out, int wait_ms = 2000);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lv
