#pragma once
// Compact ISO/IEC 18004 QR Code encoder & ImGui draw helper (versions 1..6, ECC level M).

#include <cstdint>
#include <string>
#include <vector>

namespace lv::ui {

struct QrCode {
    int size = 0;
    int version = 0;
    std::vector<uint8_t> modules;
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[size_t(y) * size + x]; }
};

QrCode qr_encode(const std::string& text);
void draw_qr_box(const std::string& text, float box_px = 200.0f);

}  // namespace lv::ui
