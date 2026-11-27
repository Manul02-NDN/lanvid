#if defined(__APPLE__)

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "codec/video_codec_common.hpp"

namespace lv {

namespace {

CMVideoCodecType apple_codec_type(VideoCodecId codec) {
    return (codec == VideoCodecId::kH265) ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
}

void append_annexb_nalu(std::vector<uint8_t>& out, const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    static constexpr uint8_t kSc[4] = {0, 0, 0, 1};
    out.insert(out.end(), kSc, kSc + 4);
    out.insert(out.end(), data, data + size);
}

// ---------------------------------------------------------------- Apple VideoToolbox Encoder

class VtVideoEncoder final : public IVideoEncoder {
public:
    explicit VtVideoEncoder(VideoCodecId codec) : codec_(codec) {}

    ~VtVideoEncoder() override { shutdown(); }

    bool init(const VideoEncoderConfig& cfg) override {
        shutdown();
        width_ = cfg.width & ~1;
        height_ = cfg.height & ~1;
        if (width_ < 64 || height_ < 64) return false;
        fps_ = std::clamp(cfg.fps, 1.0, 240.0);
        bitrate_kbps_ = std::clamp<uint32_t>(cfg.bitrate_kbps, 250u, 100000u);
        keyint_ = std::clamp<uint32_t>(cfg.keyframe_interval, 1u, 600u);

        CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (enc_spec) {
#if !TARGET_OS_IPHONE
            if (cfg.prefer_hardware) {
                CFDictionarySetValue(enc_spec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                                     kCFBooleanTrue);
            }
#endif
            if (@available(iOS 14.5, macOS 11.3, *)) {
                CFDictionarySetValue(enc_spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl,
                                     kCFBooleanTrue);
            }
        }
        CFMutableDictionaryRef src_attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (src_attrs) {
            const int32_t pf = int32_t(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
            const int32_t w32 = int32_t(width_);
            const int32_t h32 = int32_t(height_);
            CFNumberRef pf_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);
            CFNumberRef w_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w32);
            CFNumberRef h_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h32);
            CFMutableDictionaryRef io_props = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (pf_num) { CFDictionarySetValue(src_attrs, kCVPixelBufferPixelFormatTypeKey, pf_num); CFRelease(pf_num); }
            if (w_num) { CFDictionarySetValue(src_attrs, kCVPixelBufferWidthKey, w_num); CFRelease(w_num); }
            if (h_num) { CFDictionarySetValue(src_attrs, kCVPixelBufferHeightKey, h_num); CFRelease(h_num); }
            if (io_props) { CFDictionarySetValue(src_attrs, kCVPixelBufferIOSurfacePropertiesKey, io_props); CFRelease(io_props); }
        }
        OSStatus st = VTCompressionSessionCreate(
            kCFAllocatorDefault, width_, height_, apple_codec_type(codec_), enc_spec, src_attrs,
            kCFAllocatorDefault, &VtVideoEncoder::output_callback, this, &session_);
        if (src_attrs) CFRelease(src_attrs);
        if (enc_spec) CFRelease(enc_spec);
        if (st != noErr || !session_) return false;

        VTSessionSetProperty(session_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        if (@available(iOS 14.0, macOS 11.0, *)) {
            VTSessionSetProperty(session_, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                                 kCFBooleanTrue);
        }

        // Strict zero-frame encoder lookahead for ultra-low-latency live camera links
        int32_t max_delay = 0;
        CFNumberRef delay_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &max_delay);
        if (delay_num) {
            VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxFrameDelayCount, delay_num);
            CFRelease(delay_num);
        }

        int32_t kf_int = int32_t(keyint_);
        CFNumberRef kf_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &kf_int);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval, kf_num);
        CFRelease(kf_num);

        double kf_dur = std::max(0.5, double(keyint_) / fps_);
        CFNumberRef kf_dur_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &kf_dur);
        if (kf_dur_num) {
            VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, kf_dur_num);
            CFRelease(kf_dur_num);
        }

        double fps_val = fps_;
        CFNumberRef fps_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &fps_val);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_ExpectedFrameRate, fps_num);
        CFRelease(fps_num);

        if (codec_ == VideoCodecId::kH264) {
            VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel,
                                 kVTProfileLevel_H264_Main_AutoLevel);
        } else {
            VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel,
                                 kVTProfileLevel_HEVC_Main_AutoLevel);
        }

        apply_bitrate(bitrate_kbps_);
        VTCompressionSessionPrepareToEncodeFrames(session_);

        is_hardware_ = true;
#if !TARGET_OS_IPHONE
        CFBooleanRef using_hw = nullptr;
        if (VTSessionCopyProperty(session_, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                  kCFAllocatorDefault, &using_hw) == noErr && using_hw) {
            is_hardware_ = (using_hw == kCFBooleanTrue);
            CFRelease(using_hw);
        }
#endif
        backend_name_ = std::string("VideoToolbox ") + (codec_ == VideoCodecId::kH265 ? "HEVC" : "H264") +
                        (is_hardware_ ? " (HW)" : " (SW)");
        frame_index_ = 0;
        force_idr_.store(true, std::memory_order_relaxed);
        return true;
    }

    bool encode_image(const Image& img, EncodedVideoPacket& out) override {
        if (!session_ || (img.width & ~1) != width_ || (img.height & ~1) != height_) return false;

        CVPixelBufferRef pb = acquire_pool_pixel_buffer();
        if (!pb) return false;

        CVPixelBufferLockBaseAddress(pb, 0);
        auto* dst_y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
        auto* dst_uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
        const int ys = int(CVPixelBufferGetBytesPerRowOfPlane(pb, 0));
        const int uvs = int(CVPixelBufferGetBytesPerRowOfPlane(pb, 1));
        packed_rgb_to_nv12(img, dst_y, ys, dst_uv, uvs);
        CVPixelBufferUnlockBaseAddress(pb, 0);

        const bool ok = encode_pixel_buffer(pb, out);
        CVPixelBufferRelease(pb);
        return ok;
    }

    bool encode_yuv420(const Yuv420View& yuv, int width, int height,
                       EncodedVideoPacket& out) override {
        if (!session_ || (width & ~1) != width_ || (height & ~1) != height_) return false;

        CVPixelBufferRef pb = acquire_pool_pixel_buffer();
        if (!pb) return false;

        CVPixelBufferLockBaseAddress(pb, 0);
        auto* dst_y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
        auto* dst_uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
        const int ys = int(CVPixelBufferGetBytesPerRowOfPlane(pb, 0));
        const int uvs = int(CVPixelBufferGetBytesPerRowOfPlane(pb, 1));
        yuv420_to_nv12(yuv, width_, height_, dst_y, ys, dst_uv, uvs);
        CVPixelBufferUnlockBaseAddress(pb, 0);

        const bool ok = encode_pixel_buffer(pb, out);
        CVPixelBufferRelease(pb);
        return ok;
    }

    bool encode_native_buffer(const void* native_buf, int width, int height,
                              EncodedVideoPacket& out) override {
        if (!session_ || !native_buf || (width & ~1) != width_ || (height & ~1) != height_) return false;
        auto pb = reinterpret_cast<CVPixelBufferRef>(const_cast<void*>(native_buf));
        if ((int(CVPixelBufferGetWidth(pb)) & ~1) != width_ ||
            (int(CVPixelBufferGetHeight(pb)) & ~1) != height_) {
            return false;
        }
        return encode_pixel_buffer(pb, out);
    }

    bool set_bitrate_kbps(uint32_t bitrate_kbps) override {
        bitrate_kbps = std::clamp<uint32_t>(bitrate_kbps, 250u, 100000u);
        if (bitrate_kbps == bitrate_kbps_) return true;
        bitrate_kbps_ = bitrate_kbps;
        return apply_bitrate(bitrate_kbps_);
    }

    void force_keyframe() override { force_idr_.store(true, std::memory_order_release); }
    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return is_hardware_; }

private:
    CVPixelBufferRef acquire_pool_pixel_buffer() {
        CVPixelBufferRef pb = nullptr;
        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session_);
        if (pool && CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) == kCVReturnSuccess && pb) {
            return pb;
        }
        CFMutableDictionaryRef io_props = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (attrs && io_props) {
            CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, io_props);
        }
        CVReturn cr = CVPixelBufferCreate(kCFAllocatorDefault, size_t(width_), size_t(height_),
                                          kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pb);
        if (attrs) CFRelease(attrs);
        if (io_props) CFRelease(io_props);
        return (cr == kCVReturnSuccess) ? pb : nullptr;
    }

    bool apply_bitrate(uint32_t kbps) {
        if (!session_) return false;
        int64_t bps = int64_t(kbps) * 1000LL;
        CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bps);
        OSStatus st = VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate, num);
        CFRelease(num);
        return st == noErr;
    }

    bool encode_pixel_buffer(CVPixelBufferRef pb, EncodedVideoPacket& out) {
        out.data.clear();
        out.is_keyframe = false;
        out.has_codec_config = false;

        {
            std::lock_guard<std::mutex> lk(out_mu_);
            current_out_ = &out;
        }
        const bool want_idr = force_idr_.exchange(false, std::memory_order_acq_rel);
        CFMutableDictionaryRef frame_props = nullptr;
        if (want_idr) {
            frame_props = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(frame_props, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        }

        const CMTime pts = CMTimeMake(int64_t(frame_index_), int32_t(fps_ + 0.5));
        const CMTime dur = CMTimeMake(1, int32_t(fps_ + 0.5));
        ++frame_index_;

        VTEncodeInfoFlags flags = 0;
        OSStatus st = VTCompressionSessionEncodeFrame(session_, pb, pts, dur, frame_props, nullptr, &flags);
        if (frame_props) CFRelease(frame_props);
        if (st != noErr) {
            std::lock_guard<std::mutex> lk(out_mu_);
            current_out_ = nullptr;
            return false;
        }
        VTCompressionSessionCompleteFrames(session_, pts);
        std::lock_guard<std::mutex> lk(out_mu_);
        current_out_ = nullptr;
        return !out.data.empty();
    }

    static void output_callback(void* refcon, void*, OSStatus status, VTEncodeInfoFlags,
                                CMSampleBufferRef sbuf) {
        auto* self = static_cast<VtVideoEncoder*>(refcon);
        if (!self || status != noErr || !sbuf) return;
        std::lock_guard<std::mutex> lk(self->out_mu_);
        if (!self->current_out_) return;

        bool is_kf = true;
        CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sbuf, false);
        if (atts && CFArrayGetCount(atts) > 0) {
            auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(atts, 0));
            if (dict && CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync)) {
                is_kf = !CFBooleanGetValue(
                    static_cast<CFBooleanRef>(CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync)));
            }
        }

        EncodedVideoPacket& out = *self->current_out_;
        out.data.clear();
        out.is_keyframe = is_kf;
        out.has_codec_config = false;

        if (is_kf) {
            CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sbuf);
            if (fmt) {
                if (self->codec_ == VideoCodecId::kH264) {
                    size_t count = 0;
                    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, nullptr);
                    for (size_t i = 0; i < count; ++i) {
                        const uint8_t* ptr = nullptr;
                        size_t sz = 0;
                        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ptr, &sz, nullptr, nullptr) == noErr) {
                            append_annexb_nalu(out.data, ptr, sz);
                            out.has_codec_config = true;
                        }
                    }
                } else {
                    size_t count = 0;
                    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, nullptr);
                    for (size_t i = 0; i < count; ++i) {
                        const uint8_t* ptr = nullptr;
                        size_t sz = 0;
                        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i, &ptr, &sz, nullptr, nullptr) == noErr) {
                            append_annexb_nalu(out.data, ptr, sz);
                            out.has_codec_config = true;
                        }
                    }
                }
            }
        }

        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sbuf);
        if (!bb) return;
        size_t len_at_offset = 0;
        size_t total_len = 0;
        char* data_ptr = nullptr;
        std::vector<uint8_t> contig_buf;
        const uint8_t* bytes = nullptr;
        if (CMBlockBufferGetDataPointer(bb, 0, &len_at_offset, &total_len, &data_ptr) == kCMBlockBufferNoErr &&
            data_ptr && len_at_offset == total_len) {
            bytes = reinterpret_cast<const uint8_t*>(data_ptr);
        } else {
            total_len = CMBlockBufferGetDataLength(bb);
            if (total_len == 0) return;
            contig_buf.resize(total_len);
            if (CMBlockBufferCopyDataBytes(bb, 0, total_len, contig_buf.data()) != kCMBlockBufferNoErr) {
                return;
            }
            bytes = contig_buf.data();
        }

        // Convert 4-byte length-prefixed AVCC/HVCC NALUs to Annex-B start codes
        size_t off = 0;
        while (off + 4 <= total_len) {
            uint32_t nalu_len = (uint32_t(bytes[off + 0]) << 24) |
                                (uint32_t(bytes[off + 1]) << 16) |
                                (uint32_t(bytes[off + 2]) << 8) |
                                uint32_t(bytes[off + 3]);
            off += 4;
            if (nalu_len > total_len - off) break;
            append_annexb_nalu(out.data, bytes + off, nalu_len);
            off += nalu_len;
        }
    }

    void shutdown() {
        if (session_) {
            VTCompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        std::lock_guard<std::mutex> lk(out_mu_);
        current_out_ = nullptr;
    }

    VideoCodecId codec_;
    VTCompressionSessionRef session_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 60.0;
    uint32_t bitrate_kbps_ = 8000;
    uint32_t keyint_ = 120;
    uint64_t frame_index_ = 0;
    std::atomic<bool> force_idr_{true};
    bool is_hardware_ = true;
    std::string backend_name_;
    std::mutex out_mu_;
    EncodedVideoPacket* current_out_ = nullptr;
};

// ---------------------------------------------------------------- Apple VideoToolbox Decoder

class VtVideoDecoder final : public IVideoDecoder {
public:
    explicit VtVideoDecoder(VideoCodecId codec) : codec_(codec) {
        backend_name_ = std::string("VideoToolbox ") +
                        (codec_ == VideoCodecId::kH265 ? "HEVC" : "H264") + " (HW)";
    }

    ~VtVideoDecoder() override { reset(); }

    bool init(int, int, PixelOrder output_order) override {
        output_order_ = output_order;
        return true;
    }

    bool decode(const uint8_t* data, size_t size, PixelOrder output_order, Image& out_image) override {
        if (!data || size < 5) return false;
        output_order_ = output_order;

        const auto nalus = annexb_scan_nalus(data, size, codec_);
        if (nalus.empty()) return false;

        std::vector<const uint8_t*> ps_ptrs;
        std::vector<size_t> ps_sizes;
        std::vector<uint8_t> avcc_payload;
        avcc_payload.reserve(size + 32);

        for (const auto& n : nalus) {
            if (annexb_is_param_set(codec_, n.nal_type)) {
                ps_ptrs.push_back(n.payload);
                ps_sizes.push_back(n.payload_size);
            } else {
                const uint32_t len = uint32_t(n.payload_size);
                avcc_payload.push_back(uint8_t((len >> 24) & 0xFF));
                avcc_payload.push_back(uint8_t((len >> 16) & 0xFF));
                avcc_payload.push_back(uint8_t((len >> 8) & 0xFF));
                avcc_payload.push_back(uint8_t(len & 0xFF));
                avcc_payload.insert(avcc_payload.end(), n.payload, n.payload + n.payload_size);
            }
        }

        if (!ps_ptrs.empty()) {
            CMFormatDescriptionRef new_fmt = nullptr;
            OSStatus st = noErr;
            if (codec_ == VideoCodecId::kH264) {
                st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    kCFAllocatorDefault, ps_ptrs.size(), ps_ptrs.data(), ps_sizes.data(), 4, &new_fmt);
            } else {
                st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    kCFAllocatorDefault, ps_ptrs.size(), ps_ptrs.data(), ps_sizes.data(), 4, nullptr,
                    &new_fmt);
            }
            if (st == noErr && new_fmt) {
                if (!fmt_desc_ || !CMFormatDescriptionEqual(fmt_desc_, new_fmt)) {
                    reset();
                    fmt_desc_ = new_fmt;
                    create_session();
                } else {
                    CFRelease(new_fmt);
                }
            }
        }

        if (!session_ || !fmt_desc_ || avcc_payload.empty()) return false;

        CMBlockBufferRef bb = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, avcc_payload.size(),
                                               kCFAllocatorDefault, nullptr, 0, avcc_payload.size(),
                                               0, &bb) != kCMBlockBufferNoErr ||
            !bb) {
            return false;
        }
        if (CMBlockBufferReplaceDataBytes(avcc_payload.data(), bb, 0, avcc_payload.size()) !=
            kCMBlockBufferNoErr) {
            CFRelease(bb);
            return false;
        }

        CMSampleBufferRef sbuf = nullptr;
        const size_t sample_sz = avcc_payload.size();
        OSStatus st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, fmt_desc_, 1, 0, nullptr, 1,
                                                &sample_sz, &sbuf);
        CFRelease(bb);
        if (st != noErr || !sbuf) return false;

        {
            std::lock_guard<std::mutex> lk(out_mu_);
            current_out_ = &out_image;
            got_frame_ = false;
        }
        VTDecodeInfoFlags info = 0;
        // Synchronous zero-latency decode (flags = 0 waits until callback finishes on this call!)
        st = VTDecompressionSessionDecodeFrame(session_, sbuf, 0, nullptr, &info);
        CFRelease(sbuf);
        std::lock_guard<std::mutex> lk(out_mu_);
        current_out_ = nullptr;
        return st == noErr && got_frame_;
    }

    void reset() override {
        if (session_) {
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (fmt_desc_) {
            CFRelease(fmt_desc_);
            fmt_desc_ = nullptr;
        }
        std::lock_guard<std::mutex> lk(out_mu_);
        current_out_ = nullptr;
        got_frame_ = false;
    }

    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return true; }

private:
    bool create_session() {
        CFMutableDictionaryRef dst_attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const int32_t pf = int32_t(kCVPixelFormatType_32BGRA);
        CFNumberRef pf_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);
        CFMutableDictionaryRef io_props = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(dst_attrs, kCVPixelBufferPixelFormatTypeKey, pf_num);
        if (io_props) {
            CFDictionarySetValue(dst_attrs, kCVPixelBufferIOSurfacePropertiesKey, io_props);
            CFRelease(io_props);
        }
        CFRelease(pf_num);

        VTDecompressionOutputCallbackRecord cb{};
        cb.decompressionOutputCallback = &VtVideoDecoder::decode_callback;
        cb.decompressionOutputRefCon = this;

        OSStatus st = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt_desc_, nullptr, dst_attrs,
                                                   &cb, &session_);
        CFRelease(dst_attrs);
        return st == noErr && session_ != nullptr;
    }

    static void decode_callback(void* refcon, void*, OSStatus status, VTDecodeInfoFlags,
                                CVImageBufferRef img_buf, CMTime, CMTime) {
        auto* self = static_cast<VtVideoDecoder*>(refcon);
        if (!self || status != noErr || !img_buf) return;
        std::lock_guard<std::mutex> lk(self->out_mu_);
        if (!self->current_out_) return;

        CVPixelBufferLockBaseAddress(img_buf, kCVPixelBufferLock_ReadOnly);
        const int w = int(CVPixelBufferGetWidth(img_buf));
        const int h = int(CVPixelBufferGetHeight(img_buf));
        const size_t stride = CVPixelBufferGetBytesPerRow(img_buf);
        const auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(img_buf));
        if (src && w > 0 && h > 0) {
            Image& dst = *self->current_out_;
            if (dst.width != w || dst.height != h || dst.order != self->output_order_) {
                dst.allocate(w, h, self->output_order_);
            }
            const bool swap_rb = (self->output_order_ == PixelOrder::kRGBA);
            if (!swap_rb && stride == size_t(w) * 4) {
                std::memcpy(dst.data.data(), src, size_t(w) * size_t(h) * 4);
            } else {
                for (int y = 0; y < h; ++y) {
                    const uint8_t* srow = src + size_t(y) * stride;
                    uint8_t* drow = dst.row(y);
                    if (!swap_rb) {
                        std::memcpy(drow, srow, size_t(w) * 4);
                    } else {
                        for (int x = 0; x < w; ++x) {
                            drow[x * 4 + 0] = srow[x * 4 + 2];
                            drow[x * 4 + 1] = srow[x * 4 + 1];
                            drow[x * 4 + 2] = srow[x * 4 + 0];
                            drow[x * 4 + 3] = 255;
                        }
                    }
                }
            }
            self->got_frame_ = true;
        }
        CVPixelBufferUnlockBaseAddress(img_buf, kCVPixelBufferLock_ReadOnly);
    }

    VideoCodecId codec_;
    PixelOrder output_order_ = PixelOrder::kBGRA;
    CMFormatDescriptionRef fmt_desc_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    std::string backend_name_;
    std::mutex out_mu_;
    Image* current_out_ = nullptr;
    bool got_frame_ = false;
};

}  // namespace

std::unique_ptr<IVideoEncoder> create_platform_video_encoder(VideoCodecId codec,
                                                             const VideoEncoderConfig& cfg) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto enc = std::make_unique<VtVideoEncoder>(codec);
    if (!enc->init(cfg)) return nullptr;
    return enc;
}

std::unique_ptr<IVideoDecoder> create_platform_video_decoder(VideoCodecId codec,
                                                             PixelOrder output_order) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto dec = std::make_unique<VtVideoDecoder>(codec);
    if (!dec->init(0, 0, output_order)) return nullptr;
    return dec;
}

void query_platform_codecs(std::vector<CodecCapability>& out) {
    CodecCapability h264;
    h264.codec = VideoCodecId::kH264;
    h264.name = "H264";
    h264.encoder_available = true;
    h264.decoder_available = true;
    h264.encoder_hardware = true;
    h264.decoder_hardware = true;
    h264.encoder_backend = "VideoToolbox H264 (HW)";
    h264.decoder_backend = "VideoToolbox H264 (HW)";
    out.push_back(std::move(h264));

    CodecCapability h265;
    h265.codec = VideoCodecId::kH265;
    h265.name = "H265";
    h265.encoder_available = true;
    h265.decoder_available = true;
    h265.encoder_hardware = true;
    h265.decoder_hardware = true;
    h265.encoder_backend = "VideoToolbox HEVC (HW)";
    h265.decoder_backend = "VideoToolbox HEVC (HW)";
    out.push_back(std::move(h265));
}

}  // namespace lv

#endif  // __APPLE__
