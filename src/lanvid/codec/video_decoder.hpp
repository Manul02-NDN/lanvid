#pragma once
// Unified Video Decoder abstraction supporting LanVid DCT, H.264 (AVC), and H.265 (HEVC).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/image.hpp"
#include "net/packet.hpp"

namespace lv {

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual bool init(int width_hint, int height_hint, PixelOrder output_order) = 0;

    // Decodes a complete Annex-B Access Unit (H.264/H.265) into `out_image`.
    // Returns true if a decoded frame was written to `out_image`.
    virtual bool decode(const uint8_t* data, size_t size, PixelOrder output_order,
                        Image& out_image) = 0;

    // Resets decoder state cleanly (e.g. after resolution change or unrecoverable stream drop).
    virtual void reset() = 0;

    virtual VideoCodecId codec_id() const = 0;
    virtual std::string backend_name() const = 0;
    virtual bool is_hardware() const = 0;
};

std::unique_ptr<IVideoDecoder> create_video_decoder(VideoCodecId codec,
                                                    PixelOrder output_order = PixelOrder::kBGRA);

}  // namespace lv
