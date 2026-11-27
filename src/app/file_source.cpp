#include "app/file_source.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace lv {

namespace {

bool skip_ppm_ws_comments(std::istream& is) {
    for (;;) {
        int c = is.peek();
        if (c == EOF) return false;
        if (c == '#') {
            std::string dummy;
            std::getline(is, dummy);
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            is.get();
        } else {
            return true;
        }
    }
}

bool read_ppm_token(std::istream& is, std::string& tok) {
    if (!skip_ppm_ws_comments(is)) return false;
    tok.clear();
    while (true) {
        int c = is.peek();
        if (c == EOF || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '#') break;
        tok.push_back(char(is.get()));
    }
    return !tok.empty();
}

}  // namespace

const char* FileVideoSource::format_name() const {
    switch (kind_) {
        case Kind::kRawRgb: return "raw-rgb";
        case Kind::kRawRgba: return "raw-rgba";
        case Kind::kPpm: return "ppm";
        case Kind::kY4m: return "y4m";
    }
    return "?";
}

bool FileVideoSource::open(const std::string& path, int hint_w, int hint_h, bool raw_rgba) {
    close();
    file_.open(path, std::ios::binary);
    if (!file_) return false;

    // Peek magic bytes to auto-detect Y4M ("YUV4MPEG2 ") or PPM ("P6").
    char magic[10]{};
    file_.read(magic, sizeof(magic));
    const std::streamsize got = file_.gcount();
    file_.clear();
    file_.seekg(0, std::ios::beg);

    if (got >= 10 && std::memcmp(magic, "YUV4MPEG2 ", 10) == 0) {
        std::string hdr_line;
        if (!std::getline(file_, hdr_line)) return false;
        std::istringstream iss(hdr_line);
        std::string tok;
        iss >> tok;  // "YUV4MPEG2"
        width_ = 0;
        height_ = 0;
        fps_ = 0.0;
        y4m_chroma_ = ChromaFormat::k420;
        while (iss >> tok) {
            if (tok.size() > 1 && tok[0] == 'W') width_ = std::atoi(tok.c_str() + 1);
            else if (tok.size() > 1 && tok[0] == 'H') height_ = std::atoi(tok.c_str() + 1);
            else if (tok.size() > 1 && tok[0] == 'F') {
                const size_t colon = tok.find(':');
                if (colon != std::string::npos) {
                    const double num = std::atof(tok.c_str() + 1);
                    const double den = std::atof(tok.c_str() + colon + 1);
                    if (num > 0 && den > 0) fps_ = num / den;
                }
            } else if (tok.size() > 1 && tok[0] == 'C') {
                if (tok.rfind("C444", 0) == 0) y4m_chroma_ = ChromaFormat::k444;
                else if (tok.rfind("C422", 0) == 0) y4m_chroma_ = ChromaFormat::k422;
                else y4m_chroma_ = ChromaFormat::k420;
            }
        }
        if (width_ <= 0 || height_ <= 0 || width_ > 16384 || height_ > 16384 ||
            uint64_t(width_) * uint64_t(height_) > 33'554'432ull) return false;
        data_start_ = file_.tellg();
        kind_ = Kind::kY4m;
        const int sxs = chroma_shift_x(y4m_chroma_);
        const int sys = chroma_shift_y(y4m_chroma_);
        y_buf_.resize(size_t(width_) * size_t(height_));
        const size_t csz = size_t((width_ + (1 << sxs) - 1) >> sxs) * size_t((height_ + (1 << sys) - 1) >> sys);
        cb_buf_.resize(csz);
        cr_buf_.resize(csz);
        return true;
    }

    if (got >= 2 && magic[0] == 'P' && magic[1] == '6') {
        std::string t_magic, t_w, t_h, t_max;
        if (!read_ppm_token(file_, t_magic) || !read_ppm_token(file_, t_w) ||
            !read_ppm_token(file_, t_h) || !read_ppm_token(file_, t_max)) {
            return false;
        }
        file_.get();  // single whitespace byte after maxval
        const int w = std::atoi(t_w.c_str());
        const int h = std::atoi(t_h.c_str());
        const int maxv = std::atoi(t_max.c_str());
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
            uint64_t(w) * uint64_t(h) > 33'554'432ull || maxv != 255) return false;

        std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
        file_.read(reinterpret_cast<char*>(rgb.data()), std::streamsize(rgb.size()));
        if (size_t(file_.gcount()) < rgb.size()) return false;

        cached_ppm_.allocate(w, h, PixelOrder::kRGBA);
        uint8_t* out = cached_ppm_.data.data();
        const size_t px = size_t(w) * size_t(h);
        for (size_t i = 0; i < px; ++i) {
            out[i * 4 + 0] = rgb[i * 3 + 0];
            out[i * 4 + 1] = rgb[i * 3 + 1];
            out[i * 4 + 2] = rgb[i * 3 + 2];
            out[i * 4 + 3] = 255;
        }
        width_ = w;
        height_ = h;
        kind_ = Kind::kPpm;
        file_.close();
        return true;
    }

    // Raw RGB24 or RGBA32 sequence.
    if (hint_w <= 0 || hint_h <= 0) return false;
    width_ = hint_w;
    height_ = hint_h;
    kind_ = raw_rgba ? Kind::kRawRgba : Kind::kRawRgb;
    data_start_ = 0;
    if (!raw_rgba) tmp_buf_.resize(size_t(width_) * size_t(height_) * 3);
    return true;
}

void FileVideoSource::close() {
    if (file_.is_open()) file_.close();
    width_ = height_ = 0;
    fps_ = 0.0;
}

bool FileVideoSource::read_y4m_frame(Image& dst) {
    auto read_one = [&]() -> bool {
        std::string line;
        if (!std::getline(file_, line)) return false;
        if (line.rfind("FRAME", 0) != 0) return false;
        file_.read(reinterpret_cast<char*>(y_buf_.data()), std::streamsize(y_buf_.size()));
        if (size_t(file_.gcount()) < y_buf_.size()) return false;
        file_.read(reinterpret_cast<char*>(cb_buf_.data()), std::streamsize(cb_buf_.size()));
        if (size_t(file_.gcount()) < cb_buf_.size()) return false;
        file_.read(reinterpret_cast<char*>(cr_buf_.data()), std::streamsize(cr_buf_.size()));
        return size_t(file_.gcount()) == cr_buf_.size();
    };

    if (!read_one()) {
        file_.clear();
        file_.seekg(data_start_, std::ios::beg);
        if (!read_one()) return false;
    }

    const int sxs = chroma_shift_x(y4m_chroma_);
    const Planes pl{y_buf_.data(), cb_buf_.data(), cr_buf_.data(), width_, (width_ + (1 << sxs) - 1) >> sxs};
    if (dst.width == width_ && dst.height == height_) {
        ycbcr_to_rgba(pl, height_, width_, y4m_chroma_, ColorMatrix::kBT601Full, dst.data.data(),
                      dst.stride, dst.order);
    } else {
        Image tmp;
        tmp.allocate(width_, height_, dst.order);
        ycbcr_to_rgba(pl, height_, width_, y4m_chroma_, ColorMatrix::kBT601Full, tmp.data.data(),
                      tmp.stride, tmp.order);
        resize_bilinear_32bpp(tmp.data.data(), width_, height_, ptrdiff_t(tmp.stride), dst);
    }
    return true;
}

bool FileVideoSource::read_raw_frame(Image& dst) {
    const size_t pixels = size_t(width_) * size_t(height_);
    const bool is_rgba = (kind_ == Kind::kRawRgba);
    const size_t need = is_rgba ? pixels * 4 : pixels * 3;
    const bool direct = (is_rgba && dst.width == width_ && dst.height == height_ &&
                         dst.order == PixelOrder::kRGBA);
    if (!direct && tmp_buf_.size() < need) tmp_buf_.resize(need);
    uint8_t* raw = direct ? dst.data.data() : tmp_buf_.data();

    file_.read(reinterpret_cast<char*>(raw), std::streamsize(need));
    if (size_t(file_.gcount()) < need) {
        file_.clear();
        file_.seekg(data_start_, std::ios::beg);
        file_.read(reinterpret_cast<char*>(raw), std::streamsize(need));
        if (size_t(file_.gcount()) < need) return false;
    }

    if (direct) return true;

    Image tmp;
    tmp.allocate(width_, height_, dst.order);
    const int cR = (dst.order == PixelOrder::kBGRA) ? 2 : 0;
    const int cB = 2 - cR;
    uint8_t* out = tmp.data.data();
    if (is_rgba) {
        for (size_t i = 0; i < pixels; ++i) {
            out[i * 4 + cR] = raw[i * 4 + 0];
            out[i * 4 + 1] = raw[i * 4 + 1];
            out[i * 4 + cB] = raw[i * 4 + 2];
            out[i * 4 + 3] = 255;
        }
    } else {
        for (size_t i = 0; i < pixels; ++i) {
            out[i * 4 + cR] = raw[i * 3 + 0];
            out[i * 4 + 1] = raw[i * 3 + 1];
            out[i * 4 + cB] = raw[i * 3 + 2];
            out[i * 4 + 3] = 255;
        }
    }
    if (dst.width == width_ && dst.height == height_) {
        std::memcpy(dst.data.data(), tmp.data.data(), dst.data.size());
    } else {
        resize_bilinear_32bpp(tmp.data.data(), width_, height_, ptrdiff_t(tmp.stride), dst);
    }
    return true;
}

bool FileVideoSource::read_frame(Image& dst) {
    if (width_ <= 0 || height_ <= 0) return false;
    if (dst.width <= 0 || dst.height <= 0) dst.allocate(width_, height_, PixelOrder::kRGBA);

    if (kind_ == Kind::kPpm) {
        if (dst.order == cached_ppm_.order && dst.width == width_ && dst.height == height_) {
            std::memcpy(dst.data.data(), cached_ppm_.data.data(), dst.data.size());
        } else {
            Image src = cached_ppm_;
            if (dst.order != src.order) swap_rb_inplace(src);
            resize_bilinear_32bpp(src.data.data(), src.width, src.height, ptrdiff_t(src.stride), dst);
        }
        return true;
    }
    if (kind_ == Kind::kY4m) return read_y4m_frame(dst);
    return read_raw_frame(dst);
}

}  // namespace lv
