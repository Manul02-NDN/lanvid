#pragma once
// Deterministic dynamic test-scene generator (moving gradients, color bars, checkerboard,
// moving circle, and crisp bitmap frame counter) for codec tests and streaming demos.

#include <cstdint>

#include "common/image.hpp"

namespace lv {

// Renders test frame `frame_id` into `img` (must already be allocated).
void generate_test_frame(Image& img, uint32_t frame_id);

}  // namespace lv
