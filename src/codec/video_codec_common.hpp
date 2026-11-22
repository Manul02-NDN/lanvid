#pragma once
// Shared utilities for H.264 / H.265 hardware/software codec backends:
//  - Fast BT.601 studio-range BGRA/RGBA/YUV420 <-> NV12 conversion
//  - Annex-B start-code NALU parser, IDR detection, and VPS/SPS/PPS caching/prepending
//  - Platform factory hooks implemented by hw_codec_{win32,apple,android,linux}.*

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codec/video_decoder.hpp"
#include "codec/video_encoder.hpp"

namespace lv {

// ---- Fast BT.601 Studio-Range NV12 Conversions ----
// NV12 layout: Y plane (height * y_stride), followed by interleaved UV plane ((height/2) * uv_stride).
void packed_rgb_to_nv12(const Image& src, uint8_t* dst_y, int y_stride, uint8_t* dst_uv,
                        int uv_stride);

void yuv420_to_nv12(const Yuv420View& src, int width, int height, uint8_t* dst_y, int y_stride,
                    uint8_t* dst_uv, int uv_stride);

void nv12_to_packed_rgb(const uint8_t* src_y, int y_stride, const uint8_t* src_uv, int uv_stride,
                        int width, int height, PixelOrder order, Image& dst);

// ---- Annex-B NALU Inspection & Parameter-Set Cache ----
struct AnnexBNalu {
    const uint8_t* start_code = nullptr;  // points to 00 00 01 or 00 00 00 01
    const uint8_t* payload = nullptr;     // points to NALU header byte
    size_t payload_size = 0;
    size_t total_size = 0;                // start_code + payload_size
    uint8_t nal_type = 0;                 // H.264 (b0 & 0x1F) or H.265 ((b0 >> 1) & 0x3F)
};

std::vector<AnnexBNalu> annexb_scan_nalus(const uint8_t* data, size_t size, VideoCodecId codec);

bool annexb_is_param_set(VideoCodecId codec, uint8_t nal_type);
bool annexb_is_keyframe_nalu(VideoCodecId codec, uint8_t nal_type);

// Caches VPS/SPS/PPS seen in `bitstream` into `cached_params`.
// Sets `is_keyframe` and `has_codec_config`; if `is_keyframe` is true and parameter sets are missing
// from `bitstream`, prepends `cached_params` so late-joining receivers always decode IDRs cleanly.
void annexb_normalize_access_unit(VideoCodecId codec, std::vector<uint8_t>& bitstream,
                                  std::vector<uint8_t>& cached_params, bool& is_keyframe,
                                  bool& has_codec_config);

// ---- Platform Backend Hooks (implemented per OS) ----
std::unique_ptr<IVideoEncoder> create_platform_video_encoder(VideoCodecId codec,
                                                             const VideoEncoderConfig& cfg);
std::unique_ptr<IVideoDecoder> create_platform_video_decoder(VideoCodecId codec,
                                                             PixelOrder output_order);
void query_platform_codecs(std::vector<CodecCapability>& out);

}  // namespace lv
