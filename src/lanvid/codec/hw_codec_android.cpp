#if defined(__ANDROID__)

#include <android/log.h>
#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "codec/video_codec_common.hpp"

namespace lv {

namespace {

const char* android_mime(VideoCodecId codec) {
    return (codec == VideoCodecId::kH265) ? "video/hevc" : "video/avc";
}

// AMediaCodec_getInputFormat was added in Android API 28 (Android 9), while our APK targets minSdk 26.
// Resolve it dynamically at runtime via dlsym so API 28+ devices (Pixel / Exynos / Qualcomm / MediaTek)
// ALWAYS query the real hardware-aligned stride & slice-height (e.g. 1088 for 1080p) instead of falling
// back to unaligned height_ and causing a 16 px UV chroma shift + green bottom bar!
using GetInputFormatFn = AMediaFormat* (*)(AMediaCodec*);
GetInputFormatFn resolve_get_input_format() {
    static GetInputFormatFn fn = []() -> GetInputFormatFn {
        if (void* sym = dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat")) {
            return reinterpret_cast<GetInputFormatFn>(sym);
        }
        if (void* lib = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL)) {
            if (void* sym = dlsym(lib, "AMediaCodec_getInputFormat")) {
                return reinterpret_cast<GetInputFormatFn>(sym);
            }
        }
        return nullptr;
    }();
    return fn;
}

void pad_nv12_edges(uint8_t* y_plane, uint8_t* uv_plane, int width, int height,
                    int stride, int slice_h, int uv_alloc_rows) {
    // Replicate right-edge Y pixels into [width .. stride - 1]
    if (stride > width && width > 0) {
        const size_t pad_w = size_t(stride - width);
        for (int y = 0; y < height; ++y) {
            uint8_t* row = y_plane + size_t(y) * size_t(stride);
            std::memset(row + width, row[width - 1], pad_w);
        }
    }
    // Replicate bottom Y row into [height .. slice_h - 1]
    if (slice_h > height && height > 0) {
        const uint8_t* last_y = y_plane + size_t(height - 1) * size_t(stride);
        for (int y = height; y < slice_h; ++y) {
            std::memcpy(y_plane + size_t(y) * size_t(stride), last_y, size_t(stride));
        }
    }
    const int uv_h = (height + 1) / 2;
    const int uv_w_bytes = ((width + 1) & ~1);
    // Replicate right-edge UV pair into [uv_w_bytes .. stride - 1]
    if (stride > uv_w_bytes && uv_w_bytes >= 2) {
        for (int y = 0; y < uv_h; ++y) {
            uint8_t* row = uv_plane + size_t(y) * size_t(stride);
            const uint8_t u_last = row[uv_w_bytes - 2];
            const uint8_t v_last = row[uv_w_bytes - 1];
            for (int x = uv_w_bytes; x + 1 < stride; x += 2) {
                row[x + 0] = u_last;
                row[x + 1] = v_last;
            }
        }
    }
    // Replicate bottom UV row into [uv_h .. uv_alloc_rows - 1] (never leave 0x00 -> bright green!)
    if (uv_alloc_rows > uv_h && uv_h > 0) {
        const uint8_t* last_uv = uv_plane + size_t(uv_h - 1) * size_t(stride);
        for (int y = uv_h; y < uv_alloc_rows; ++y) {
            std::memcpy(uv_plane + size_t(y) * size_t(stride), last_uv, size_t(stride));
        }
    }
}

void pad_i420_plane(uint8_t* plane, int w, int h, int stride, int slice_h) {
    if (stride > w && w > 0) {
        const size_t pad_w = size_t(stride - w);
        for (int y = 0; y < h; ++y) {
            uint8_t* row = plane + size_t(y) * size_t(stride);
            std::memset(row + w, row[w - 1], pad_w);
        }
    }
    if (slice_h > h && h > 0) {
        const uint8_t* last = plane + size_t(h - 1) * size_t(stride);
        for (int y = h; y < slice_h; ++y) {
            std::memcpy(plane + size_t(y) * size_t(stride), last, size_t(stride));
        }
    }
}

class AndroidMediaCodecEncoder final : public IVideoEncoder {
public:
    explicit AndroidMediaCodecEncoder(VideoCodecId codec) : codec_(codec) {}
    ~AndroidMediaCodecEncoder() override { shutdown(); }

    bool init(const VideoEncoderConfig& cfg) override {
        shutdown();
        width_ = cfg.width & ~1;
        height_ = cfg.height & ~1;
        if (width_ < 64 || height_ < 64) return false;
        fps_ = std::clamp(int(cfg.fps + 0.5), 1, 120);
        bitrate_kbps_ = std::clamp<uint32_t>(cfg.bitrate_kbps, 250u, 100000u);

        codec_obj_ = AMediaCodec_createEncoderByType(android_mime(codec_));
        if (!codec_obj_) return false;

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, android_mime(codec_));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width_);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height_);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);  // COLOR_FormatYUV420SemiPlanar (NV12)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, int32_t(bitrate_kbps_ * 1000u));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps_);
        const int i_sec = std::max(1, int(cfg.keyframe_interval / uint32_t(fps_)));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, i_sec);
        AMediaFormat_setInt32(fmt, "low-latency", 1);
        AMediaFormat_setInt32(fmt, "max-bframes", 0);
        AMediaFormat_setInt32(fmt, "priority", 0);  // real-time

        media_status_t st = AMediaCodec_configure(codec_obj_, fmt, nullptr, nullptr,
                                                  AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) {
            shutdown();
            return false;
        }
        if (AMediaCodec_start(codec_obj_) != AMEDIA_OK) {
            shutdown();
            return false;
        }
        started_ = true;

        // Query hardware encoder's aligned input stride, slice-height, and color format at runtime
        // (critical on Pixel/Exynos/Qualcomm where 1920x1080 uses slice-height=1088 and 1080x1920
        // uses stride=1088/1152). Works even when compiled with minSdk=26 via dlsym!
        in_stride_ = width_;
        in_slice_h_ = height_;
        in_color_format_ = 21;
        if (auto get_in_fmt = resolve_get_input_format()) {
            if (AMediaFormat* in_fmt = get_in_fmt(codec_obj_)) {
                int32_t s = 0, sh = 0, cf = 0;
                if (AMediaFormat_getInt32(in_fmt, "stride", &s) && s >= width_) {
                    in_stride_ = s;
                }
                if (AMediaFormat_getInt32(in_fmt, "slice-height", &sh) && sh >= height_) {
                    in_slice_h_ = sh;
                }
                if (AMediaFormat_getInt32(in_fmt, "color-format", &cf) && cf > 0) {
                    in_color_format_ = cf;
                }
                AMediaFormat_delete(in_fmt);
            }
        }
        __android_log_print(ANDROID_LOG_INFO, "LanVidCodec",
                            "Encoder %s %dx%d configured: stride=%d slice_h=%d color_fmt=%d",
                            android_mime(codec_), width_, height_, in_stride_, in_slice_h_,
                            in_color_format_);
        backend_name_ = std::string("Android MediaCodec ") +
                        (codec_ == VideoCodecId::kH265 ? "HEVC" : "H264") + " (HW)";
        frame_index_ = 0;
        force_idr_.store(true, std::memory_order_relaxed);
        return true;
    }

    bool encode_image(const Image& img, EncodedVideoPacket& out) override {
        if (!codec_obj_ || (img.width & ~1) != width_ || (img.height & ~1) != height_) return false;
        const int uv_rows_min = (height_ + 1) / 2;
        const int uv_rows_full = (in_slice_h_ + 1) / 2;
        const size_t y_plane_sz = size_t(in_stride_) * size_t(in_slice_h_);
        const size_t nv12_min_sz = y_plane_sz + size_t(in_stride_) * size_t(uv_rows_min);
        const size_t nv12_full_sz = y_plane_sz + size_t(in_stride_) * size_t(uv_rows_full);
        if (nv12_.size() != nv12_full_sz) nv12_.assign(nv12_full_sz, 128);
        packed_rgb_to_nv12(img, nv12_.data(), in_stride_, nv12_.data() + y_plane_sz, in_stride_);
        pad_nv12_edges(nv12_.data(), nv12_.data() + y_plane_sz, width_, height_, in_stride_,
                       in_slice_h_, uv_rows_full);
        if (in_color_format_ == 19) {
            return encode_i420_from_nv12(nv12_.data(), nv12_.data() + y_plane_sz, out);
        }
        return encode_nv12(nv12_.data(), nv12_full_sz, nv12_min_sz, out);
    }

    bool encode_yuv420(const Yuv420View& yuv, int width, int height,
                       EncodedVideoPacket& out) override {
        if (!codec_obj_ || (width & ~1) != width_ || (height & ~1) != height_) return false;
        const int uv_rows_min = (height_ + 1) / 2;
        const int uv_rows_full = (in_slice_h_ + 1) / 2;
        const size_t y_plane_sz = size_t(in_stride_) * size_t(in_slice_h_);
        const size_t nv12_min_sz = y_plane_sz + size_t(in_stride_) * size_t(uv_rows_min);
        const size_t nv12_full_sz = y_plane_sz + size_t(in_stride_) * size_t(uv_rows_full);
        if (nv12_.size() != nv12_full_sz) nv12_.assign(nv12_full_sz, 128);
        yuv420_to_nv12(yuv, width_, height_, nv12_.data(), in_stride_, nv12_.data() + y_plane_sz,
                       in_stride_);
        pad_nv12_edges(nv12_.data(), nv12_.data() + y_plane_sz, width_, height_, in_stride_,
                       in_slice_h_, uv_rows_full);
        if (in_color_format_ == 19) {
            return encode_i420_from_nv12(nv12_.data(), nv12_.data() + y_plane_sz, out);
        }
        return encode_nv12(nv12_.data(), nv12_full_sz, nv12_min_sz, out);
    }

    bool set_bitrate_kbps(uint32_t bitrate_kbps) override {
        bitrate_kbps = std::clamp<uint32_t>(bitrate_kbps, 250u, 100000u);
        bitrate_kbps_ = bitrate_kbps;
#if __ANDROID_API__ >= 26
        if (codec_obj_) {
            AMediaFormat* p = AMediaFormat_new();
            AMediaFormat_setInt32(p, "video-bitrate", int32_t(bitrate_kbps_ * 1000u));
            AMediaCodec_setParameters(codec_obj_, p);
            AMediaFormat_delete(p);
            return true;
        }
#endif
        return false;
    }

    void force_keyframe() override { force_idr_.store(true, std::memory_order_release); }
    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return true; }

private:
    bool encode_i420_from_nv12(const uint8_t* y_src, const uint8_t* uv_src, EncodedVideoPacket& out) {
        const int c_stride = in_stride_ / 2;
        const int c_slice_h = (in_slice_h_ + 1) / 2;
        const int c_w = width_ / 2;
        const int c_h = (height_ + 1) / 2;
        const size_t y_sz = size_t(in_stride_) * size_t(in_slice_h_);
        const size_t c_plane_sz = size_t(c_stride) * size_t(c_slice_h);
        const size_t full_sz = y_sz + c_plane_sz * 2;
        const size_t min_sz = y_sz + c_plane_sz + size_t(c_stride) * size_t(c_h);
        if (i420_.size() != full_sz) i420_.assign(full_sz, 128);
        std::memcpy(i420_.data(), y_src, y_sz);
        uint8_t* u_dst = i420_.data() + y_sz;
        uint8_t* v_dst = u_dst + c_plane_sz;
        for (int y = 0; y < c_h; ++y) {
            const uint8_t* suv = uv_src + size_t(y) * size_t(in_stride_);
            uint8_t* du = u_dst + size_t(y) * size_t(c_stride);
            uint8_t* dv = v_dst + size_t(y) * size_t(c_stride);
            for (int x = 0; x < c_w; ++x) {
                du[x] = suv[x * 2 + 0];
                dv[x] = suv[x * 2 + 1];
            }
        }
        pad_i420_plane(u_dst, c_w, c_h, c_stride, c_slice_h);
        pad_i420_plane(v_dst, c_w, c_h, c_stride, c_slice_h);
        return encode_nv12(i420_.data(), full_sz, min_sz, out);
    }

    bool encode_nv12(const uint8_t* nv12, size_t full_sz, size_t min_sz, EncodedVideoPacket& out) {
        out.data.clear();
        out.is_keyframe = false;
        out.has_codec_config = false;

#if __ANDROID_API__ >= 26
        if (force_idr_.exchange(false, std::memory_order_acq_rel)) {
            AMediaFormat* p = AMediaFormat_new();
            AMediaFormat_setInt32(p, "request-sync", 0);
            AMediaCodec_setParameters(codec_obj_, p);
            AMediaFormat_delete(p);
        }
#endif
        const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_obj_, 10000);
        if (in_idx < 0) return false;
        size_t in_cap = 0;
        uint8_t* in_buf = AMediaCodec_getInputBuffer(codec_obj_, size_t(in_idx), &in_cap);
        if (!in_buf || in_cap < min_sz) {
            // Return buffer index to MediaCodec so we never leak input buffers and freeze the pipeline.
            AMediaCodec_queueInputBuffer(codec_obj_, size_t(in_idx), 0, 0, 0, 0);
            return false;
        }
        const size_t copy_sz = (in_cap >= full_sz) ? full_sz : min_sz;
        std::memcpy(in_buf, nv12, copy_sz);
        const int64_t pts_us = int64_t(frame_index_++) * (1000000LL / int64_t(fps_));
        AMediaCodec_queueInputBuffer(codec_obj_, size_t(in_idx), 0, copy_sz, uint64_t(pts_us), 0);

        for (int tries = 0; tries < 8; ++tries) {
            AMediaCodecBufferInfo info{};
            const ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_obj_, &info, 10000);
            if (out_idx >= 0) {
                size_t out_cap = 0;
                uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_obj_, size_t(out_idx), &out_cap);
                if (out_buf && info.size > 0) {
                    const uint8_t* p = out_buf + info.offset;
                    if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                        cached_params_.assign(p, p + info.size);
                        AMediaCodec_releaseOutputBuffer(codec_obj_, size_t(out_idx), false);
                        continue;
                    }
                    out.data.assign(p, p + info.size);
                    bool is_kf = (info.flags & 1u) != 0;  // BUFFER_FLAG_KEY_FRAME
                    bool has_cfg = false;
                    annexb_normalize_access_unit(codec_, out.data, cached_params_, is_kf, has_cfg);
                    out.is_keyframe = is_kf;
                    out.has_codec_config = has_cfg;
                }
                AMediaCodec_releaseOutputBuffer(codec_obj_, size_t(out_idx), false);
                return !out.data.empty();
            } else if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                continue;
            } else {
                break;
            }
        }
        return false;
    }

    void shutdown() {
        if (codec_obj_) {
            if (started_) AMediaCodec_stop(codec_obj_);
            AMediaCodec_delete(codec_obj_);
            codec_obj_ = nullptr;
        }
        started_ = false;
        cached_params_.clear();
    }

    VideoCodecId codec_;
    AMediaCodec* codec_obj_ = nullptr;
    bool started_ = false;
    int width_ = 0;
    int height_ = 0;
    int in_stride_ = 0;
    int in_slice_h_ = 0;
    int in_color_format_ = 21;
    int fps_ = 60;
    uint32_t bitrate_kbps_ = 8000;
    uint64_t frame_index_ = 0;
    std::atomic<bool> force_idr_{true};
    std::string backend_name_;
    std::vector<uint8_t> nv12_;
    std::vector<uint8_t> i420_;
    std::vector<uint8_t> cached_params_;
};

class AndroidMediaCodecDecoder final : public IVideoDecoder {
public:
    explicit AndroidMediaCodecDecoder(VideoCodecId codec) : codec_(codec) {}
    ~AndroidMediaCodecDecoder() override { shutdown(); }

    bool init(int width_hint, int height_hint, PixelOrder output_order) override {
        shutdown();
        width_ = width_hint > 0 ? width_hint : 1920;
        height_ = height_hint > 0 ? height_hint : 1080;
        stride_ = width_;
        slice_h_ = height_;
        output_order_ = output_order;

        codec_obj_ = AMediaCodec_createDecoderByType(android_mime(codec_));
        if (!codec_obj_) return false;
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, android_mime(codec_));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width_);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height_);
        AMediaFormat_setInt32(fmt, "low-latency", 1);
        media_status_t st = AMediaCodec_configure(codec_obj_, fmt, nullptr, nullptr, 0);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK || AMediaCodec_start(codec_obj_) != AMEDIA_OK) {
            shutdown();
            return false;
        }
        started_ = true;
        color_format_ = 21;  // assume semi-planar NV12 until output format reports otherwise
        backend_name_ = std::string("Android MediaCodec ") +
                        (codec_ == VideoCodecId::kH265 ? "HEVC" : "H264") + " (HW)";
        return true;
    }

    bool decode(const uint8_t* data, size_t size, PixelOrder output_order, Image& out_image) override {
        if (!codec_obj_ || !data || size == 0) return false;
        const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_obj_, 10000);
        if (in_idx < 0) return false;
        size_t in_cap = 0;
        uint8_t* in_buf = AMediaCodec_getInputBuffer(codec_obj_, size_t(in_idx), &in_cap);
        if (!in_buf || in_cap < size) {
            AMediaCodec_queueInputBuffer(codec_obj_, size_t(in_idx), 0, 0, 0, 0);
            return false;
        }
        std::memcpy(in_buf, data, size);
        AMediaCodec_queueInputBuffer(codec_obj_, size_t(in_idx), 0, size, uint64_t(frame_index_++ * 16667), 0);

        for (int tries = 0; tries < 8; ++tries) {
            AMediaCodecBufferInfo info{};
            const ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_obj_, &info, 10000);
            if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* ofmt = AMediaCodec_getOutputFormat(codec_obj_);
                if (ofmt) {
                    int32_t fmt_w = width_, fmt_h = height_;
                    AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &fmt_w);
                    AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &fmt_h);
                    int32_t s = 0, sh = 0, cf = color_format_;
                    AMediaFormat_getInt32(ofmt, "stride", &s);
                    AMediaFormat_getInt32(ofmt, "slice-height", &sh);
                    AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &cf);
                    stride_ = std::max(s, fmt_w);
                    slice_h_ = std::max(sh, fmt_h);
                    color_format_ = cf;
                    int32_t cl = 0, ct = 0, cr = 0, cb = 0;
                    if (AMediaFormat_getInt32(ofmt, "crop-left", &cl) &&
                        AMediaFormat_getInt32(ofmt, "crop-right", &cr) &&
                        AMediaFormat_getInt32(ofmt, "crop-top", &ct) &&
                        AMediaFormat_getInt32(ofmt, "crop-bottom", &cb) &&
                        cr >= cl && cb >= ct) {
                        crop_left_ = std::max(0, cl) & ~1;
                        crop_top_ = std::max(0, ct) & ~1;
                        width_ = std::max(2, (cr - cl + 1) & ~1);
                        height_ = std::max(2, (cb - ct + 1) & ~1);
                    } else {
                        crop_left_ = 0;
                        crop_top_ = 0;
                        width_ = fmt_w;
                        height_ = fmt_h;
                    }
                    AMediaFormat_delete(ofmt);
                }
                continue;
            }
            if (out_idx >= 0) {
                size_t out_cap = 0;
                uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_obj_, size_t(out_idx), &out_cap);
                bool ok = false;
                const int eff_stride = std::max(stride_, crop_left_ + width_);
                const int eff_slice_h = std::max(slice_h_, crop_top_ + height_);
                const size_t y_bytes = size_t(eff_stride) * size_t(eff_slice_h);
                const size_t uv_bytes = size_t(eff_stride) * size_t((crop_top_ + height_ + 1) / 2);
                const size_t off = info.offset >= 0 ? size_t(info.offset) : 0;
                if (out_buf && info.size > 0 && width_ > 0 && height_ > 0 &&
                    off <= out_cap && y_bytes + uv_bytes <= out_cap - off) {
                    const uint8_t* y_base = out_buf + off;
                    const uint8_t* y = y_base + size_t(crop_top_) * size_t(eff_stride) + size_t(crop_left_);
                    if (color_format_ == 19) {  // COLOR_FormatYUV420Planar (I420)
                        const int u_stride = eff_stride / 2;
                        const size_t u_plane = size_t(u_stride) * size_t((eff_slice_h + 1) / 2);
                        if (y_bytes + u_plane * 2 <= out_cap - off) {
                            const uint8_t* u = y_base + y_bytes +
                                               size_t(crop_top_ >> 1) * size_t(u_stride) +
                                               size_t(crop_left_ >> 1);
                            const uint8_t* v = y_base + y_bytes + u_plane +
                                               size_t(crop_top_ >> 1) * size_t(u_stride) +
                                               size_t(crop_left_ >> 1);
                            const size_t dst_uv_sz = size_t(width_) * size_t((height_ + 1) / 2);
                            if (uv_tmp_.size() < dst_uv_sz) uv_tmp_.resize(dst_uv_sz);
                            const int cw = width_ >> 1, ch = height_ >> 1;
                            for (int row = 0; row < ch; ++row) {
                                const uint8_t* su = u + size_t(row) * size_t(u_stride);
                                const uint8_t* sv = v + size_t(row) * size_t(u_stride);
                                uint8_t* duv = uv_tmp_.data() + size_t(row) * size_t(width_);
                                for (int x = 0; x < cw; ++x) {
                                    duv[x * 2 + 0] = su[x];
                                    duv[x * 2 + 1] = sv[x];
                                }
                            }
                            nv12_to_packed_rgb(y, eff_stride, uv_tmp_.data(), width_, width_, height_,
                                               output_order, out_image);
                            ok = true;
                        }
                    } else {
                        const uint8_t* uv = y_base + y_bytes +
                                            size_t(crop_top_ >> 1) * size_t(eff_stride) +
                                            size_t(crop_left_);
                        nv12_to_packed_rgb(y, eff_stride, uv, eff_stride, width_, height_, output_order, out_image);
                        ok = true;
                    }
                }
                AMediaCodec_releaseOutputBuffer(codec_obj_, size_t(out_idx), false);
                return ok;
            }
            break;
        }
        return false;
    }

    void reset() override {
        if (codec_obj_ && started_) AMediaCodec_flush(codec_obj_);
    }

    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return true; }

private:
    void shutdown() {
        if (codec_obj_) {
            if (started_) AMediaCodec_stop(codec_obj_);
            AMediaCodec_delete(codec_obj_);
            codec_obj_ = nullptr;
        }
        started_ = false;
    }

    VideoCodecId codec_;
    AMediaCodec* codec_obj_ = nullptr;
    bool started_ = false;
    int width_ = 0;
    int height_ = 0;
    int crop_left_ = 0;
    int crop_top_ = 0;
    int stride_ = 0;
    int slice_h_ = 0;
    int color_format_ = 21;
    std::vector<uint8_t> uv_tmp_;
    PixelOrder output_order_ = PixelOrder::kRGBA;
    uint64_t frame_index_ = 0;
    std::string backend_name_;
};

}  // namespace

std::unique_ptr<IVideoEncoder> create_platform_video_encoder(VideoCodecId codec,
                                                             const VideoEncoderConfig& cfg) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto enc = std::make_unique<AndroidMediaCodecEncoder>(codec);
    if (!enc->init(cfg)) return nullptr;
    return enc;
}

std::unique_ptr<IVideoDecoder> create_platform_video_decoder(VideoCodecId codec,
                                                             PixelOrder output_order) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto dec = std::make_unique<AndroidMediaCodecDecoder>(codec);
    if (!dec->init(1920, 1080, output_order)) return nullptr;
    return dec;
}

void query_platform_codecs(std::vector<CodecCapability>& out) {
    for (VideoCodecId cid : {VideoCodecId::kH264, VideoCodecId::kH265}) {
        CodecCapability cap;
        cap.codec = cid;
        cap.name = (cid == VideoCodecId::kH265) ? "H265" : "H264";
        AMediaCodec* e = AMediaCodec_createEncoderByType(android_mime(cid));
        if (e) {
            cap.encoder_available = true;
            cap.encoder_hardware = true;
            cap.encoder_backend = std::string("Android MediaCodec ") + cap.name + " (HW)";
            AMediaCodec_delete(e);
        }
        AMediaCodec* d = AMediaCodec_createDecoderByType(android_mime(cid));
        if (d) {
            cap.decoder_available = true;
            cap.decoder_hardware = true;
            cap.decoder_backend = std::string("Android MediaCodec ") + cap.name + " (HW)";
            AMediaCodec_delete(d);
        }
        out.push_back(std::move(cap));
    }
}

}  // namespace lv

#endif  // __ANDROID__
