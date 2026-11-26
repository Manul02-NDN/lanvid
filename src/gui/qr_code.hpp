#pragma once
// Minimal QR Code encoder (ISO/IEC 18004): byte mode, error correction level M, versions 1..6
// (up to 106 bytes - enough for "lanvid://connect?host=...&port=...&tally=...&name=..."), mask 0.
// Used by LanVid Video's "Add camera" page: the phone scans it to connect in one tap.

#include <cstdint>
#include <string>
#include <vector>

namespace lv::gui {

struct QrCode {
    int size = 0;                 // modules per side (21 + 4*(version-1)); 0 = encoding failed
    int version = 0;
    std::vector<uint8_t> modules; // size*size, 1 = dark
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[size_t(y) * size + x]; }
};

QrCode qr_encode(const std::string& text);

}  // namespace lv::gui
