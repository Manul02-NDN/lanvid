#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "codec/video_codec_common.hpp"

#if defined(LV_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}
#endif

namespace lv {

#if defined(LV_HAVE_FFMPEG)

namespace {

class FfmpegVideoEncoder final : public IVideoEncoder {
public:
    explicit FfmpegVideoEncoder(VideoCodecId codec) : codec_(codec) {}
    ~FfmpegVideoEncoder() override { shutdown(); }

    bool init(const VideoEncoderConfig& cfg) override {
        shutdown();
        width_ = cfg.width & ~1;
        height_ = cfg.height & ~1;
        if (width_ < 64 || height_ < 64) return false;
        fps_ = std::clamp(int(cfg.fps + 0.5), 1, 240);
        bitrate_kbps_ = std::clamp<uint32_t>(cfg.bitrate_kbps, 250u, 100000u);
        keyint_ = std::clamp<uint32_t>(cfg.keyframe_interval, 1u, 600u);

        const std::vector<std::pair<const char*, bool>> candidates =
            (codec_ == VideoCodecId::kH265)
                ? std::vector<std::pair<const char*, bool>>{
                      {"hevc_nvenc", true}, {"hevc_vaapi", true}, {"libx265", false}}
                : std::vector<std::pair<const char*, bool>>{
                      {"h264_nvenc", true}, {"h264_vaapi", true}, {"libx264", false}};

        for (const auto& [name, is_hw] : candidates) {
            if (!cfg.prefer_hardware && is_hw) continue;
            const AVCodec* avc = avcodec_find_encoder_by_name(name);
            if (!avc) continue;
            if (try_open(avc, name, is_hw)) return true;
            shutdown();
        }
        return false;
    }

    bool encode_image(const Image& img, EncodedVideoPacket& out) override {
        if (!ctx_ || !frame_ || img.width != width_ || img.height != height_) return false;
        if (av_frame_make_writable(frame_) < 0) return false;
        if (ctx_->pix_fmt == AV_PIX_FMT_NV12) {
            packed_rgb_to_nv12(img, frame_->data[0], frame_->linesize[0], frame_->data[1],
                               frame_->linesize[1]);
        } else {
            // Convert via NV12 scratch -> I420 planes
            const size_t sz = size_t(width_) * size_t(height_) * 3 / 2;
            if (nv12_tmp_.size() != sz) nv12_tmp_.resize(sz);
            packed_rgb_to_nv12(img, nv12_tmp_.data(), width_,
                               nv12_tmp_.data() + size_t(width_) * size_t(height_), width_);
            nv12_to_avframe_i420(nv12_tmp_.data());
        }
        return encode_current_frame(out);
    }

    bool encode_yuv420(const Yuv420View& yuv, int width, int height,
                       EncodedVideoPacket& out) override {
        if (!ctx_ || !frame_ || width != width_ || height != height_) return false;
        if (av_frame_make_writable(frame_) < 0) return false;
        if (ctx_->pix_fmt == AV_PIX_FMT_NV12) {
            yuv420_to_nv12(yuv, width_, height_, frame_->data[0], frame_->linesize[0],
                           frame_->data[1], frame_->linesize[1]);
        } else {
            for (int y = 0; y < height_; ++y) {
                std::memcpy(frame_->data[0] + size_t(y) * size_t(frame_->linesize[0]),
                            yuv.y + size_t(y) * size_t(yuv.y_stride), size_t(width_));
            }
            const int cw = width_ >> 1, ch = height_ >> 1;
            for (int y = 0; y < ch; ++y) {
                const uint8_t* su = yuv.u + size_t(y) * size_t(yuv.u_stride);
                const uint8_t* sv = yuv.v + size_t(y) * size_t(yuv.v_stride);
                uint8_t* du = frame_->data[1] + size_t(y) * size_t(frame_->linesize[1]);
                uint8_t* dv = frame_->data[2] + size_t(y) * size_t(frame_->linesize[2]);
                for (int x = 0; x < cw; ++x) {
                    du[x] = su[x * yuv.uv_pixel_stride];
                    dv[x] = sv[x * yuv.uv_pixel_stride];
                }
            }
        }
        return encode_current_frame(out);
    }

    bool set_bitrate_kbps(uint32_t bitrate_kbps) override {
        bitrate_kbps_ = std::clamp<uint32_t>(bitrate_kbps, 250u, 100000u);
        if (ctx_) {
            ctx_->bit_rate = int64_t(bitrate_kbps_) * 1000LL;
            ctx_->rc_max_rate = ctx_->bit_rate;
            return true;
        }
        return false;
    }

    void force_keyframe() override { force_idr_.store(true, std::memory_order_release); }
    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return is_hardware_; }

private:
    bool try_open(const AVCodec* avc, const char* name, bool is_hw) {
        ctx_ = avcodec_alloc_context3(avc);
        if (!ctx_) return false;
        ctx_->width = width_;
        ctx_->height = height_;
        ctx_->time_base = AVRational{1, fps_};
        ctx_->framerate = AVRational{fps_, 1};
        ctx_->bit_rate = int64_t(bitrate_kbps_) * 1000LL;
        ctx_->rc_max_rate = ctx_->bit_rate;
        ctx_->rc_buffer_size = int(ctx_->bit_rate / 4);
        ctx_->gop_size = int(keyint_);
        ctx_->max_b_frames = 0;
        ctx_->pix_fmt = is_hw ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;

        if (std::strstr(name, "nvenc")) {
            av_opt_set(ctx_->priv_data, "preset", "p1", 0);
            av_opt_set(ctx_->priv_data, "tune", "ull", 0);
            av_opt_set(ctx_->priv_data, "zerolatency", "1", 0);
            av_opt_set(ctx_->priv_data, "rc", "cbr", 0);
        } else if (std::strcmp(name, "libx264") == 0) {
            av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
            av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
            av_opt_set(ctx_->priv_data, "forced-idr", "1", 0);
            av_opt_set(ctx_->priv_data, "x264-params", "repeat-headers=1:bframes=0:open-gop=0", 0);
        } else if (std::strcmp(name, "libx265") == 0) {
            av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
            av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
            av_opt_set(ctx_->priv_data, "forced-idr", "1", 0);
            av_opt_set(ctx_->priv_data, "x265-params",
                       "repeat-headers=1:bframes=0:open-gop=0:scenecut=0:log-level=error", 0);
        }

        if (avcodec_open2(ctx_, avc, nullptr) < 0) return false;

        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        if (!frame_ || !pkt_) return false;
        frame_->format = ctx_->pix_fmt;
        frame_->width = width_;
        frame_->height = height_;
        if (av_frame_get_buffer(frame_, 32) < 0) return false;

        is_hardware_ = is_hw;
        backend_name_ = std::string("FFmpeg ") + name + (is_hw ? " (HW)" : " (SW)");
        frame_index_ = 0;
        force_idr_.store(true, std::memory_order_relaxed);
        return true;
    }

    void nv12_to_avframe_i420(const uint8_t* nv12) {
        for (int y = 0; y < height_; ++y) {
            std::memcpy(frame_->data[0] + size_t(y) * size_t(frame_->linesize[0]),
                        nv12 + size_t(y) * size_t(width_), size_t(width_));
        }
        const uint8_t* uv = nv12 + size_t(width_) * size_t(height_);
        const int cw = width_ >> 1, ch = height_ >> 1;
        for (int y = 0; y < ch; ++y) {
            const uint8_t* suv = uv + size_t(y) * size_t(width_);
            uint8_t* du = frame_->data[1] + size_t(y) * size_t(frame_->linesize[1]);
            uint8_t* dv = frame_->data[2] + size_t(y) * size_t(frame_->linesize[2]);
            for (int x = 0; x < cw; ++x) {
                du[x] = suv[x * 2 + 0];
                dv[x] = suv[x * 2 + 1];
            }
        }
    }

    bool encode_current_frame(EncodedVideoPacket& out) {
        out.data.clear();
        frame_->pts = frame_index_++;
        const bool want_idr = force_idr_.exchange(false, std::memory_order_acq_rel);
        frame_->pict_type = want_idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
#ifdef AV_FRAME_FLAG_KEY
        if (want_idr) frame_->flags |= AV_FRAME_FLAG_KEY;
        else frame_->flags &= ~AV_FRAME_FLAG_KEY;
#endif

        if (avcodec_send_frame(ctx_, frame_) < 0) return false;
        if (avcodec_receive_packet(ctx_, pkt_) < 0) return false;

        out.data.assign(pkt_->data, pkt_->data + pkt_->size);
        bool is_kf = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        bool has_cfg = false;
        av_packet_unref(pkt_);

        annexb_normalize_access_unit(codec_, out.data, cached_params_, is_kf, has_cfg);
        out.is_keyframe = is_kf;
        out.has_codec_config = has_cfg;
        return !out.data.empty();
    }

    void shutdown() {
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
        cached_params_.clear();
    }

    VideoCodecId codec_;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 60;
    uint32_t bitrate_kbps_ = 8000;
    uint32_t keyint_ = 120;
    int64_t frame_index_ = 0;
    std::atomic<bool> force_idr_{true};
    bool is_hardware_ = false;
    std::string backend_name_;
    std::vector<uint8_t> nv12_tmp_;
    std::vector<uint8_t> cached_params_;
};

class FfmpegVideoDecoder final : public IVideoDecoder {
public:
    explicit FfmpegVideoDecoder(VideoCodecId codec) : codec_(codec) {}
    ~FfmpegVideoDecoder() override { shutdown(); }

    bool init(int, int, PixelOrder) override {
        shutdown();
        const AVCodecID id = (codec_ == VideoCodecId::kH265) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
        const AVCodec* avc = avcodec_find_decoder(id);
        if (!avc) return false;
        ctx_ = avcodec_alloc_context3(avc);
        if (!ctx_) return false;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx_->thread_count = 1;  // single-frame zero-latency decode
        if (avcodec_open2(ctx_, avc, nullptr) < 0) {
            shutdown();
            return false;
        }
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        backend_name_ = std::string("FFmpeg ") + avc->name + " (SW)";
        return frame_ && pkt_;
    }

    bool decode(const uint8_t* data, size_t size, PixelOrder output_order, Image& out_image) override {
        if (!ctx_ || !data || size == 0) return false;
        pkt_->data = const_cast<uint8_t*>(data);
        pkt_->size = int(size);
        if (avcodec_send_packet(ctx_, pkt_) < 0) return false;
        if (avcodec_receive_frame(ctx_, frame_) < 0) return false;

        const int w = frame_->width;
        const int h = frame_->height;
        if (frame_->format == AV_PIX_FMT_NV12) {
            nv12_to_packed_rgb(frame_->data[0], frame_->linesize[0], frame_->data[1],
                               frame_->linesize[1], w, h, output_order, out_image);
        } else {
            // Interleave I420 U/V into scratch NV12 UV plane and reuse fast studio conversion
            const size_t uv_sz = size_t(w) * size_t((h + 1) / 2);
            if (uv_tmp_.size() < uv_sz) uv_tmp_.resize(uv_sz);
            const int cw = w >> 1, ch = h >> 1;
            for (int y = 0; y < ch; ++y) {
                const uint8_t* su = frame_->data[1] + size_t(y) * size_t(frame_->linesize[1]);
                const uint8_t* sv = frame_->data[2] + size_t(y) * size_t(frame_->linesize[2]);
                uint8_t* duv = uv_tmp_.data() + size_t(y) * size_t(w);
                for (int x = 0; x < cw; ++x) {
                    duv[x * 2 + 0] = su[x];
                    duv[x * 2 + 1] = sv[x];
                }
            }
            nv12_to_packed_rgb(frame_->data[0], frame_->linesize[0], uv_tmp_.data(), w, w, h,
                               output_order, out_image);
        }
        av_frame_unref(frame_);
        return true;
    }

    void reset() override {
        if (ctx_) avcodec_flush_buffers(ctx_);
    }

    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return false; }

private:
    void shutdown() {
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
    }

    VideoCodecId codec_;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    std::string backend_name_;
    std::vector<uint8_t> uv_tmp_;
};

}  // namespace

std::unique_ptr<IVideoEncoder> create_platform_video_encoder(VideoCodecId codec,
                                                             const VideoEncoderConfig& cfg) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto enc = std::make_unique<FfmpegVideoEncoder>(codec);
    if (!enc->init(cfg)) return nullptr;
    return enc;
}

std::unique_ptr<IVideoDecoder> create_platform_video_decoder(VideoCodecId codec,
                                                             PixelOrder output_order) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto dec = std::make_unique<FfmpegVideoDecoder>(codec);
    if (!dec->init(0, 0, output_order)) return nullptr;
    return dec;
}

void query_platform_codecs(std::vector<CodecCapability>& out) {
    auto probe = [&](VideoCodecId cid, const char* label, const char* const* enc_names,
                     const bool* enc_hw, int enc_n, AVCodecID dec_id) {
        CodecCapability cap;
        cap.codec = cid;
        cap.name = label;
        for (int i = 0; i < enc_n; ++i) {
            if (avcodec_find_encoder_by_name(enc_names[i])) {
                cap.encoder_available = true;
                cap.encoder_hardware = enc_hw[i];
                cap.encoder_backend =
                    std::string("FFmpeg ") + enc_names[i] + (enc_hw[i] ? " (HW)" : " (SW)");
                break;
            }
        }
        if (const AVCodec* d = avcodec_find_decoder(dec_id)) {
            cap.decoder_available = true;
            cap.decoder_hardware = false;
            cap.decoder_backend = std::string("FFmpeg ") + d->name + " (SW)";
        }
        out.push_back(std::move(cap));
    };

    const char* h264_e[] = {"h264_nvenc", "h264_vaapi", "libx264"};
    const bool h264_hw[] = {true, true, false};
    probe(VideoCodecId::kH264, "H264", h264_e, h264_hw, 3, AV_CODEC_ID_H264);

    const char* h265_e[] = {"hevc_nvenc", "hevc_vaapi", "libx265"};
    const bool h265_hw[] = {true, true, false};
    probe(VideoCodecId::kH265, "H265", h265_e, h265_hw, 3, AV_CODEC_ID_HEVC);
}

#else  // !LV_HAVE_FFMPEG

std::unique_ptr<IVideoEncoder> create_platform_video_encoder(VideoCodecId, const VideoEncoderConfig&) {
    return nullptr;
}

std::unique_ptr<IVideoDecoder> create_platform_video_decoder(VideoCodecId, PixelOrder) {
    return nullptr;
}

void query_platform_codecs(std::vector<CodecCapability>& out) {
    CodecCapability h264;
    h264.codec = VideoCodecId::kH264;
    h264.name = "H264";
    h264.encoder_backend = "Unavailable (install libavcodec-dev)";
    h264.decoder_backend = h264.encoder_backend;
    out.push_back(std::move(h264));

    CodecCapability h265;
    h265.codec = VideoCodecId::kH265;
    h265.name = "H265";
    h265.encoder_backend = "Unavailable (install libavcodec-dev)";
    h265.decoder_backend = h265.encoder_backend;
    out.push_back(std::move(h265));
}

#endif  // LV_HAVE_FFMPEG

}  // namespace lv

#endif
