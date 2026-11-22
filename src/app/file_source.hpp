#pragma once
// Dependency-free cross-platform file video source supporting:
//  - Y4M (YUV4MPEG2: 4:2:0, 4:2:2, 4:4:4; auto-detects width, height, FPS)
//  - PPM (binary P6 RGB; auto-detects width, height; loops cleanly)
//  - Raw RGB24 / RGBA32 frame sequences (uses caller-specified width/height; loops cleanly)

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "codec/color.hpp"
#include "common/image.hpp"

namespace lv {

class FileVideoSource {
public:
    enum class Kind { kRawRgb, kRawRgba, kPpm, kY4m };

    // Opens `path`; for raw files, `hint_w`/`hint_h` specify dimensions and `raw_rgba` picks RGBA vs RGB.
    // For PPM and Y4M, file header dimensions override hints automatically.
    bool open(const std::string& path, int hint_w, int hint_h, bool raw_rgba = false);
    void close();

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }  // 0 if not encoded in file header
    const char* format_name() const;

    // Reads next frame (looping to start at EOF) into `dst` (allocates/resizes as needed).
    bool read_frame(Image& dst);

private:
    bool read_y4m_frame(Image& dst);
    bool read_raw_frame(Image& dst);

    std::ifstream file_;
    Kind kind_ = Kind::kRawRgb;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    ChromaFormat y4m_chroma_ = ChromaFormat::k420;
    std::streamoff data_start_ = 0;

    Image cached_ppm_;
    std::vector<uint8_t> tmp_buf_;
    std::vector<uint8_t> y_buf_, cb_buf_, cr_buf_;
};

}  // namespace lv
