#if defined(__ANDROID__)

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
        backend_name_ = std::string("Android MediaCodec ") +
                        (codec_ == VideoCodecId::kH265 ? "HEVC" : "H264") + " (HW)";
        frame_index_ = 0;
        force_idr_.store(true, std::memory_order_relaxed);
        return true;
    }

    bool encode_image(const Image& img, EncodedVideoPacket& out) override {
        if (!codec_obj_ || img.width != width_ || img.height != height_) return false;
        const size_t nv12_sz = size_t(width_) * size_t(height_) * 3 / 2;
        if (nv12_.size() != nv12_sz) nv12_.resize(nv12_sz);
        packed_rgb_to_nv12(img, nv12_.data(), width_, nv12_.data() + size_t(width_) * size_t(height_),
                           width_);
        return encode_nv12(nv12_.data(), nv12_sz, out);
    }

    bool encode_yuv420(const Yuv420View& yuv, int width, int height,
                       EncodedVideoPacket& out) override {
        if (!codec_obj_ || width != width_ || height != height_) return false;
        const size_t nv12_sz = size_t(width_) * size_t(height_) * 3 / 2;
        if (nv12_.size() != nv12_sz) nv12_.resize(nv12_sz);
        yuv420_to_nv12(yuv, width_, height_, nv12_.data(), width_,
                       nv12_.data() + size_t(width_) * size_t(height_), width_);
        return encode_nv12(nv12_.data(), nv12_sz, out);
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
    bool encode_nv12(const uint8_t* nv12, size_t nv12_sz, EncodedVideoPacket& out) {
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
        if (!in_buf || in_cap < nv12_sz) return false;
        std::memcpy(in_buf, nv12, nv12_sz);
        const int64_t pts_us = int64_t(frame_index_++) * (1000000LL / int64_t(fps_));
        AMediaCodec_queueInputBuffer(codec_obj_, size_t(in_idx), 0, nv12_sz, uint64_t(pts_us), 0);

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
            AMediaCodec_stop(codec_obj_);
            AMediaCodec_delete(codec_obj_);
            codec_obj_ = nullptr;
        }
        cached_params_.clear();
    }

    VideoCodecId codec_;
    AMediaCodec* codec_obj_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 60;
    uint32_t bitrate_kbps_ = 8000;
    uint64_t frame_index_ = 0;
    std::atomic<bool> force_idr_{true};
    std::string backend_name_;
    std::vector<uint8_t> nv12_;
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
        if (!in_buf || in_cap < size) return false;
        std::memcpy(in_buf, data, size);
        AMediaCodec_queueInputBuffer(codec_obj_, size_t(in_idx), 0, size, uint64_t(frame_index_++ * 16667), 0);

        for (int tries = 0; tries < 8; ++tries) {
            AMediaCodecBufferInfo info{};
            const ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_obj_, &info, 10000);
            if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* ofmt = AMediaCodec_getOutputFormat(codec_obj_);
                if (ofmt) {
                    AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &width_);
                    AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &height_);
                    if (!AMediaFormat_getInt32(ofmt, "stride", &stride_)) stride_ = width_;
                    if (!AMediaFormat_getInt32(ofmt, "slice-height", &slice_h_)) slice_h_ = height_;
                    AMediaFormat_delete(ofmt);
                }
                continue;
            }
            if (out_idx >= 0) {
                size_t out_cap = 0;
                uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_obj_, size_t(out_idx), &out_cap);
                bool ok = false;
                if (out_buf && info.size > 0 && width_ > 0 && height_ > 0) {
                    const uint8_t* y = out_buf + info.offset;
                    const uint8_t* uv = y + size_t(stride_) * size_t(slice_h_);
                    nv12_to_packed_rgb(y, stride_, uv, stride_, width_, height_, output_order, out_image);
                    ok = true;
                }
                AMediaCodec_releaseOutputBuffer(codec_obj_, size_t(out_idx), false);
                return ok;
            }
            break;
        }
        return false;
    }

    void reset() override {
        if (codec_obj_) AMediaCodec_flush(codec_obj_);
    }

    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return true; }

private:
    void shutdown() {
        if (codec_obj_) {
            AMediaCodec_stop(codec_obj_);
            AMediaCodec_delete(codec_obj_);
            codec_obj_ = nullptr;
        }
    }

    VideoCodecId codec_;
    AMediaCodec* codec_obj_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    int slice_h_ = 0;
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
