#ifndef LANVID_VBUS_C_H
#define LANVID_VBUS_C_H
// Platform-neutral virtual camera frame source over LanVidVideoBus_v1 (src/video/video_bus.*), with a C ABI:
// used by the macOS CoreMediaIO Camera Extension (Swift) and the Linux v4l2loopback bridge.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of devices: "LanVid Cam 1".."LanVid Cam 8" + "LanVid Program Out".
int32_t lvb_device_count(void);
const char* lvb_device_name(int32_t slot);
// Renders the latest frame of `slot` as BGRA (w x h, `stride` bytes per row), aspect-fit with black bars;
// without a fresh frame the lavender "LanVid Cam N - No Signal / Standby" card. Returns 1 for a live frame.
int32_t lvb_render_bgra(int32_t slot, uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride);

#ifdef __cplusplus
}
#endif

#endif  // LANVID_VBUS_C_H
