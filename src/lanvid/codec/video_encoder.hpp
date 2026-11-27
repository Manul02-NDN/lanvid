#pragma once
// Unified Video Encoder abstraction supporting LanVid DCT, H.264 (AVC), and H.265 (HEVC)
// with zero-latency hardware acceleration (Media Foundation / VideoToolbox / MediaCodec / FFmpeg).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codec/slice_codec.hpp"
#include "common/image.hpp"
#include "net/packet.hpp"

namespace lv {

struct VideoEncoderConfig {
    int width = 1920;
    int height = 1080;
    double fps = 60.0;
    uint32_t bitrate_kbps = 8000;          // Target CBR/VBR bitrate for H.264/H.265
    uint32_t keyframe_interval = 120;      // IDR interval in frames (e.g. 120 = 2s @ 60fps)
    int quality = 80;                      // 1..100 for LanVid DCT
    ChromaFormat chroma = ChromaFormat::k420;
    int slice_height = 16;
    bool prefer_hardware = true;
};

struct EncodedVideoPacket {
    std::vector<uint8_t> data;  // Annex-B NALU bitstream (H.264/H.265) or slice blob (DCT)
    bool is_keyframe = false;   // True for IDR / intra frames
    bool has_codec_config = false;  // True when VPS/SPS/PPS precede the slice
    uint64_t timestamp_us = 0;
};

struct CodecCapability {
    VideoCodecId codec = VideoCodecId::kLanVidDct;
    std::string name;         // "LANVID_DCT", "H264", "H265"
    bool encoder_available = false;
    bool decoder_available = false;
    bool encoder_hardware = false;
    bool decoder_hardware = false;
    std::string encoder_backend;
    std::string decoder_backend;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual bool init(const VideoEncoderConfig& cfg) = 0;

    // Encodes a packed RGBA/BGRA image into an Annex-B Access Unit (or single-slice DCT blob).
    virtual bool encode_image(const Image& img, EncodedVideoPacket& out) = 0;

    // Zero-RGB-conversion encode from planar/semi-planar YUV 4:2:0 (I420 / NV12 / NV21).
    virtual bool encode_yuv420(const Yuv420View& yuv, int width, int height,
                               EncodedVideoPacket& out) = 0;

    // True zero-copy encode from a native platform hardware surface (e.g. Apple CVPixelBufferRef).
    virtual bool encode_native_buffer(const void* /*native_buf*/, int /*width*/, int /*height*/,
                                      EncodedVideoPacket& /*out*/) {
        return false;
    }

    // Dynamically updates target bitrate without recreating the encoder (for AIMD / GUI slider).
    virtual bool set_bitrate_kbps(uint32_t bitrate_kbps) = 0;

    // Requests an immediate IDR keyframe with prepended VPS/SPS/PPS on the very next frame (PLI).
    virtual void force_keyframe() = 0;

    virtual VideoCodecId codec_id() const = 0;
    virtual std::string backend_name() const = 0;
    virtual bool is_hardware() const = 0;
};

// Factory & capability discovery
std::unique_ptr<IVideoEncoder> create_video_encoder(VideoCodecId codec, const VideoEncoderConfig& cfg);
std::vector<CodecCapability> get_supported_codecs();
std::string supported_codecs_json();

}  // namespace lv
