#pragma once
// CLSIDs of the LanVid DirectShow virtual cameras (one COM class per device):
//   {6C4A8B10-2E5D-4F7A-9B3C-1D2E3F405161}  "LanVid Cam 1"
//   ...                                ...68  "LanVid Cam 8"
//   {6C4A8B10-2E5D-4F7A-9B3C-1D2E3F405169}  "LanVid Program Out"
// The last byte is 0x61 + bus slot index (lv::kVideoBusSlots = 9).

#include <guiddef.h>
#include <cstdint>

namespace lv::vcam {

constexpr uint32_t kDeviceCount = 9;

inline GUID device_clsid(uint32_t slot) {
    GUID g = {0x6C4A8B10, 0x2E5D, 0x4F7A, {0x9B, 0x3C, 0x1D, 0x2E, 0x3F, 0x40, 0x51, uint8_t(0x61 + slot)}};
    return g;
}

// Returns the bus slot for a LanVid virtual camera CLSID, or -1.
inline int device_slot(const GUID& g) {
    for (uint32_t i = 0; i < kDeviceCount; ++i)
        if (IsEqualGUID(g, device_clsid(i))) return int(i);
    return -1;
}

}  // namespace lv::vcam
