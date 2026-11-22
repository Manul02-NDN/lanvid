#include "codec/video_codec_common.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "codec/dct.hpp"
#include "codec/frame_codec.hpp"
#include "common/cpu_features.hpp"

namespace lv {

namespace {

inline uint8_t clamp_u8(int v) {
    return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Wrap existing LanVid slice DCT codec as an IVideoEncoder / IVideoDecoder so callers can use
// the unified interface uniformly if desired (UdpSender/FrameAssembler still use the parallel
// streaming slice fast path directly for VideoCodecId::kLanVidDct!).
class LanVidDctEncoder final : public IVideoEncoder {
public:
    bool init(const VideoEncoderConfig& cfg) override {
        cfg_ = cfg;
        cp_.width = cfg.width;
        cp_.height = cfg.height;
        cp_.quality = std::clamp(cfg.quality, 1, 100);
        cp_.chroma = cfg.chroma;
        cp_.slice_height = cfg.slice_height >= 8 ? cfg.slice_height : 16;
        return true;
    }

    bool encode_image(const Image& img, EncodedVideoPacket& out) override {
        cp_.width = img.width;
        cp_.height = img.height;
        EncodedFrame ef;
        if (!fc_.encode(img, cp_, ef, nullptr)) return false;
        out.data.clear();
        if (!ef.slices.empty()) {
            // Simple length-prefixed pack when invoked through single-blob IVideoEncoder
            size_t total = 4;
            for (const auto& s : ef.slices) total += 4 + s.size();
            out.data.resize(total);
            uint32_t ns = uint32_t(ef.slices.size());
            std::memcpy(out.data.data(), &ns, 4);
            size_t off = 4;
            for (const auto& s : ef.slices) {
                uint32_t sz = uint32_t(s.size());
                std::memcpy(out.data.data() + off, &sz, 4);
                off += 4;
                std::memcpy(out.data.data() + off, s.data(), sz);
                off += sz;
            }
        }
        out.is_keyframe = true;
        out.has_codec_config = false;
        return true;
    }

    bool encode_yuv420(const Yuv420View& yuv, int width, int height,
                       EncodedVideoPacket& out) override {
        cp_.width = width;
        cp_.height = height;
        cp_.chroma = ChromaFormat::k420;
        std::vector<std::vector<uint8_t>> slices;
        bool ok = fc_.encode_streaming_yuv420(
            yuv, cp_,
            [&](int sid, int total, const uint8_t* data, size_t size) {
                if (int(slices.size()) < total) slices.resize(size_t(total));
                slices[size_t(sid)].assign(data, data + size);
            },
            nullptr);
        if (!ok) return false;
        size_t total = 4;
        for (const auto& s : slices) total += 4 + s.size();
        out.data.resize(total);
        uint32_t ns = uint32_t(slices.size());
        std::memcpy(out.data.data(), &ns, 4);
        size_t off = 4;
        for (const auto& s : slices) {
            uint32_t sz = uint32_t(s.size());
            std::memcpy(out.data.data() + off, &sz, 4);
            off += 4;
            std::memcpy(out.data.data() + off, s.data(), sz);
            off += sz;
        }
        out.is_keyframe = true;
        out.has_codec_config = false;
        return true;
    }

    bool set_bitrate_kbps(uint32_t) override { return true; }
    void force_keyframe() override {}
    VideoCodecId codec_id() const override { return VideoCodecId::kLanVidDct; }
    std::string backend_name() const override {
        return std::string("LanVid DCT (") + dct_kernels_best().name + ")";
    }
    bool is_hardware() const override { return false; }

private:
    VideoEncoderConfig cfg_{};
    CodecParams cp_{};
    FrameCodec fc_;
};

class LanVidDctDecoder final : public IVideoDecoder {
public:
    bool init(int, int, PixelOrder) override { return true; }

    bool decode(const uint8_t* data, size_t size, PixelOrder output_order, Image& out_image) override {
        if (!data || size < 4) return false;
        uint32_t ns = 0;
        std::memcpy(&ns, data, 4);
        if (ns == 0 || ns > 4096) return false;
        EncodedFrame ef;
        ef.slices.resize(ns);
        size_t off = 4;
        for (uint32_t i = 0; i < ns; ++i) {
            if (off + 4 > size) return false;
            uint32_t sz = 0;
            std::memcpy(&sz, data + off, 4);
            off += 4;
            if (off + sz > size) return false;
            ef.slices[i].assign(data + off, data + off + sz);
            off += sz;
        }
        SliceHeader sh{};
        if (!parse_slice_header(ef.slices[0].data(), ef.slices[0].size(), sh)) return false;
        ef.params.width = sh.width;
        ef.params.height = sh.height;
        ef.params.chroma = ChromaFormat(sh.chroma);
        ef.params.quality = sh.quality;
        ef.params.slice_height = sh.slice_h;
        ef.geom.total_slices = int(ns);
        if (out_image.width != sh.width || out_image.height != sh.height ||
            out_image.order != output_order) {
            out_image.allocate(sh.width, sh.height, output_order);
        }
        const auto res = fc_.decode(ef, out_image, nullptr);
        return res.slices_ok > 0;
    }

    void reset() override {}
    VideoCodecId codec_id() const override { return VideoCodecId::kLanVidDct; }
    std::string backend_name() const override {
        return std::string("LanVid DCT (") + dct_kernels_best().name + ")";
    }
    bool is_hardware() const override { return false; }

private:
    FrameCodec fc_;
};

std::string json_esc(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 4);
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- Color Conversions (BT.601 Studio)

#if LV_HAVE_AVX2_TU
void nv12_to_packed_rgb_avx2(const uint8_t* src_y, int y_stride, const uint8_t* src_uv,
                             int uv_stride, int width, int height, PixelOrder order, uint8_t* dst,
                             size_t dst_stride);
void packed_rgb_to_nv12_avx2(const uint8_t* src, size_t src_stride, int width, int height,
                             PixelOrder order, uint8_t* dst_y, int y_stride, uint8_t* dst_uv,
                             int uv_stride);
#endif

void packed_rgb_to_nv12(const Image& src, uint8_t* dst_y, int y_stride, uint8_t* dst_uv,
                        int uv_stride) {
#if LV_HAVE_AVX2_TU
    if (cpu_has_avx2() && simd_enabled() && src.width >= 16) {
        packed_rgb_to_nv12_avx2(src.data.data(), src.stride, src.width, src.height, src.order,
                                dst_y, y_stride, dst_uv, uv_stride);
        return;
    }
#endif
    const int w = src.width;
    const int h = src.height;
    const bool is_bgra = (src.order == PixelOrder::kBGRA);
    const int r_idx = is_bgra ? 2 : 0;
    const int g_idx = 1;
    const int b_idx = is_bgra ? 0 : 2;

    const int h2 = h & ~1;
    const int w2 = w & ~1;

    for (int y = 0; y < h2; y += 2) {
        const uint8_t* row0 = src.row(y);
        const uint8_t* row1 = src.row(y + 1);
        uint8_t* y0 = dst_y + size_t(y) * size_t(y_stride);
        uint8_t* y1 = dst_y + size_t(y + 1) * size_t(y_stride);
        uint8_t* uv = dst_uv + size_t(y >> 1) * size_t(uv_stride);

        for (int x = 0; x < w2; x += 2) {
            const uint8_t* p00 = row0 + size_t(x) * 4;
            const uint8_t* p01 = p00 + 4;
            const uint8_t* p10 = row1 + size_t(x) * 4;
            const uint8_t* p11 = p10 + 4;

            const int r00 = p00[r_idx], g00 = p00[g_idx], b00 = p00[b_idx];
            const int r01 = p01[r_idx], g01 = p01[g_idx], b01 = p01[b_idx];
            const int r10 = p10[r_idx], g10 = p10[g_idx], b10 = p10[b_idx];
            const int r11 = p11[r_idx], g11 = p11[g_idx], b11 = p11[b_idx];

            y0[x]     = uint8_t(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
            y0[x + 1] = uint8_t(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
            y1[x]     = uint8_t(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
            y1[x + 1] = uint8_t(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);

            const int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;
            const int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            const int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;

            uv[x]     = uint8_t(((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128);
            uv[x + 1] = uint8_t(((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128);
        }
    }
}

void yuv420_to_nv12(const Yuv420View& src, int width, int height, uint8_t* dst_y, int y_stride,
                    uint8_t* dst_uv, int uv_stride) {
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + size_t(y) * size_t(y_stride), src.y + size_t(y) * size_t(src.y_stride),
                    size_t(width));
    }
    const int cw = width >> 1;
    const int ch = height >> 1;
    for (int y = 0; y < ch; ++y) {
        const uint8_t* su = src.u + size_t(y) * size_t(src.u_stride);
        const uint8_t* sv = src.v + size_t(y) * size_t(src.v_stride);
        uint8_t* duv = dst_uv + size_t(y) * size_t(uv_stride);
        if (src.uv_pixel_stride == 2 && sv == su + 1) {
            std::memcpy(duv, su, size_t(cw) * 2);
        } else {
            for (int x = 0; x < cw; ++x) {
                duv[x * 2 + 0] = su[x * src.uv_pixel_stride];
                duv[x * 2 + 1] = sv[x * src.uv_pixel_stride];
            }
        }
    }
}

void nv12_to_packed_rgb(const uint8_t* src_y, int y_stride, const uint8_t* src_uv, int uv_stride,
                        int width, int height, PixelOrder order, Image& dst) {
    if (dst.width != width || dst.height != height || dst.order != order) {
        dst.allocate(width, height, order);
    }
#if LV_HAVE_AVX2_TU
    if (cpu_has_avx2() && simd_enabled() && width >= 16) {
        nv12_to_packed_rgb_avx2(src_y, y_stride, src_uv, uv_stride, width, height, order,
                                dst.data.data(), dst.stride);
        return;
    }
#endif
    const bool is_bgra = (order == PixelOrder::kBGRA);
    const int r_idx = is_bgra ? 2 : 0;
    const int g_idx = 1;
    const int b_idx = is_bgra ? 0 : 2;

    for (int y = 0; y < height; ++y) {
        const uint8_t* yp = src_y + size_t(y) * size_t(y_stride);
        const uint8_t* uvp = src_uv + size_t(y >> 1) * size_t(uv_stride);
        uint8_t* dp = dst.row(y);

        const int w2 = width & ~1;
        for (int x = 0; x < w2; x += 2) {
            const int d = int(uvp[x]) - 128;
            const int e = int(uvp[x + 1]) - 128;
            const int r_add = 409 * e + 128;
            const int g_add = -100 * d - 208 * e + 128;
            const int b_add = 516 * d + 128;

            const int c0 = 298 * (int(yp[x]) - 16);
            dp[x * 4 + r_idx] = clamp_u8((c0 + r_add) >> 8);
            dp[x * 4 + g_idx] = clamp_u8((c0 + g_add) >> 8);
            dp[x * 4 + b_idx] = clamp_u8((c0 + b_add) >> 8);
            dp[x * 4 + 3] = 255;

            const int c1 = 298 * (int(yp[x + 1]) - 16);
            dp[(x + 1) * 4 + r_idx] = clamp_u8((c1 + r_add) >> 8);
            dp[(x + 1) * 4 + g_idx] = clamp_u8((c1 + g_add) >> 8);
            dp[(x + 1) * 4 + b_idx] = clamp_u8((c1 + b_add) >> 8);
            dp[(x + 1) * 4 + 3] = 255;
        }
        if (width & 1) {
            const int x = width - 1;
            const int d = int(uvp[x & ~1]) - 128;
            const int e = int(uvp[(x & ~1) + 1]) - 128;
            const int c = 298 * (int(yp[x]) - 16);
            dp[x * 4 + r_idx] = clamp_u8((c + 409 * e + 128) >> 8);
            dp[x * 4 + g_idx] = clamp_u8((c - 100 * d - 208 * e + 128) >> 8);
            dp[x * 4 + b_idx] = clamp_u8((c + 516 * d + 128) >> 8);
            dp[x * 4 + 3] = 255;
        }
    }
}

// ---------------------------------------------------------------- Annex-B Helpers

std::vector<AnnexBNalu> annexb_scan_nalus(const uint8_t* data, size_t size, VideoCodecId codec) {
    std::vector<AnnexBNalu> out;
    if (!data || size < 4) return out;

    auto find_sc = [&](size_t from, size_t& sc_len) -> size_t {
        for (size_t i = from; i + 3 <= size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0) {
                if (data[i + 2] == 1) {
                    sc_len = 3;
                    return i;
                }
                if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                    sc_len = 4;
                    return i;
                }
            }
        }
        return size;
    };

    size_t sc_len = 0;
    size_t pos = find_sc(0, sc_len);
    while (pos < size) {
        const size_t pay_start = pos + sc_len;
        size_t next_sc_len = 0;
        const size_t next_pos = find_sc(pay_start, next_sc_len);
        if (pay_start < next_pos) {
            AnnexBNalu n;
            n.start_code = data + pos;
            n.payload = data + pay_start;
            n.payload_size = next_pos - pay_start;
            n.total_size = next_pos - pos;
            n.nal_type = (codec == VideoCodecId::kH265)
                             ? uint8_t((n.payload[0] >> 1) & 0x3Fu)
                             : uint8_t(n.payload[0] & 0x1Fu);
            out.push_back(n);
        }
        pos = next_pos;
        sc_len = next_sc_len;
    }
    return out;
}

bool annexb_is_param_set(VideoCodecId codec, uint8_t nal_type) {
    if (codec == VideoCodecId::kH265) {
        return nal_type == 32 || nal_type == 33 || nal_type == 34;  // VPS, SPS, PPS
    }
    return nal_type == 7 || nal_type == 8;  // SPS, PPS
}

bool annexb_is_keyframe_nalu(VideoCodecId codec, uint8_t nal_type) {
    if (codec == VideoCodecId::kH265) {
        // IDR_W_RADL (19), IDR_N_LP (20), CRA_NUT (21)
        return nal_type == 19 || nal_type == 20 || nal_type == 21;
    }
    return nal_type == 5;  // IDR slice
}

void annexb_normalize_access_unit(VideoCodecId codec, std::vector<uint8_t>& bitstream,
                                  std::vector<uint8_t>& cached_params, bool& is_keyframe,
                                  bool& has_codec_config) {
    const auto nalus = annexb_scan_nalus(bitstream.data(), bitstream.size(), codec);
    bool found_params = false;
    bool found_idr = false;
    bool has_filler = false;
    std::vector<uint8_t> new_params;

    const uint8_t filler_type = (codec == VideoCodecId::kH265) ? 38u : 12u;

    for (const auto& n : nalus) {
        if (n.nal_type == filler_type) {
            has_filler = true;
            continue;
        }
        if (annexb_is_param_set(codec, n.nal_type)) {
            found_params = true;
            static constexpr uint8_t kSc[4] = {0, 0, 0, 1};
            new_params.insert(new_params.end(), kSc, kSc + 4);
            new_params.insert(new_params.end(), n.payload, n.payload + n.payload_size);
        } else if (annexb_is_keyframe_nalu(codec, n.nal_type)) {
            found_idr = true;
        }
    }

    if (found_params && !new_params.empty()) {
        cached_params = std::move(new_params);
    }

    is_keyframe = is_keyframe || found_idr;
    has_codec_config = found_params;

    // Strip CBR filler data NALUs (saves bandwidth AND prevents Microsoft H.264 MFT 1-frame stall!)
    // and ensure every IDR starts with VPS/SPS/PPS inline for late-joining / PLI recovery.
    const bool need_prepend = (is_keyframe && !found_params && !cached_params.empty());
    if (has_filler || need_prepend) {
        std::vector<uint8_t> rebuilt;
        rebuilt.reserve(bitstream.size() + (need_prepend ? cached_params.size() : 0));
        if (need_prepend) {
            rebuilt.insert(rebuilt.end(), cached_params.begin(), cached_params.end());
            has_codec_config = true;
        }
        for (const auto& n : nalus) {
            if (n.nal_type == filler_type) continue;
            rebuilt.insert(rebuilt.end(), n.start_code, n.start_code + n.total_size);
        }
        bitstream = std::move(rebuilt);
    }
}

// ---------------------------------------------------------------- Public Factories

std::unique_ptr<IVideoEncoder> create_video_encoder(VideoCodecId codec,
                                                    const VideoEncoderConfig& cfg) {
    if (codec == VideoCodecId::kLanVidDct) {
        auto enc = std::make_unique<LanVidDctEncoder>();
        if (!enc->init(cfg)) return nullptr;
        return enc;
    }
    return create_platform_video_encoder(codec, cfg);
}

std::unique_ptr<IVideoDecoder> create_video_decoder(VideoCodecId codec, PixelOrder output_order) {
    if (codec == VideoCodecId::kLanVidDct) {
        auto dec = std::make_unique<LanVidDctDecoder>();
        dec->init(0, 0, output_order);
        return dec;
    }
    return create_platform_video_decoder(codec, output_order);
}

std::vector<CodecCapability> get_supported_codecs() {
    std::vector<CodecCapability> out;

    CodecCapability dct;
    dct.codec = VideoCodecId::kLanVidDct;
    dct.name = "LANVID_DCT";
    dct.encoder_available = true;
    dct.decoder_available = true;
    dct.encoder_hardware = false;
    dct.decoder_hardware = false;
    dct.encoder_backend = std::string("LanVid DCT (") + dct_kernels_best().name + ")";
    dct.decoder_backend = dct.encoder_backend;
    out.push_back(std::move(dct));

    query_platform_codecs(out);
    return out;
}

std::string supported_codecs_json() {
    const auto caps = get_supported_codecs();
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < caps.size(); ++i) {
        if (i) os << ",";
        os << "{\"id\":" << int(caps[i].codec)
           << ",\"name\":\"" << json_esc(caps[i].name)
           << "\",\"encoder\":" << (caps[i].encoder_available ? "true" : "false")
           << ",\"decoder\":" << (caps[i].decoder_available ? "true" : "false")
           << ",\"encoder_hw\":" << (caps[i].encoder_hardware ? "true" : "false")
           << ",\"decoder_hw\":" << (caps[i].decoder_hardware ? "true" : "false")
           << ",\"encoder_backend\":\"" << json_esc(caps[i].encoder_backend)
           << "\",\"decoder_backend\":\"" << json_esc(caps[i].decoder_backend) << "\"}";
    }
    os << "]";
    return os.str();
}

}  // namespace lv
