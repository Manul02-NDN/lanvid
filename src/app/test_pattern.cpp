#include "app/test_pattern.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "common/platform.hpp"

namespace lv {

namespace {

// Compact 5x7 bitmap font for digits 0-9, 'F', '#', ':', ' '.
constexpr uint8_t kDigitFont[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
};

constexpr uint8_t kGlyphF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
constexpr uint8_t kGlyphColon[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};

const uint8_t* lookup_glyph(char ch) {
    if (ch >= '0' && ch <= '9') return kDigitFont[ch - '0'];
    if (ch == 'F') return kGlyphF;
    if (ch == ':') return kGlyphColon;
    return nullptr;
}

void draw_rect(Image& img, int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    const int x1 = std::min(img.width, x0 + w);
    const int y1 = std::min(img.height, y0 + h);
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    for (int y = y0; y < y1; ++y) {
        uint8_t* p = img.row(y) + size_t(x0) * 4;
        for (int x = x0; x < x1; ++x) {
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
            p += 4;
        }
    }
}

void draw_text(Image& img, int x0, int y0, int scale, const char* s, uint8_t r, uint8_t g, uint8_t b) {
    int x = x0;
    for (; *s; ++s) {
        if (const uint8_t* gl = lookup_glyph(*s)) {
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (gl[row] & (0x10 >> col)) {
                        draw_rect(img, x + col * scale, y0 + row * scale, scale, scale, r, g, b);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

// 7 SMPTE-like saturated bars (R, G, B).
constexpr uint8_t kBars[7][3] = {
    {235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16},
    {235, 16, 235},  {235, 16, 16},  {16, 16, 235},
};

}  // namespace

void generate_test_frame(Image& img, uint32_t frame_id) {
    const int W = img.width;
    const int H = img.height;
    if (W <= 0 || H <= 0) return;

    const int shift_x = int((frame_id * 5u) % uint32_t(W));
    const int shift_y = int((frame_id * 3u) % uint32_t(H));
    const int bar_h = H / 6;           // top band: color bars
    const int chk_y0 = H - H / 4;      // bottom band: scrolling checkerboard + luma ramp
    const int chk_x_mid = W / 2;

    // Moving disc parameters (smooth motion across frame).
    const double t = double(frame_id) * 0.06;
    const int cx = W / 2 + int(double(W) * 0.30 * std::cos(t));
    const int cy = H / 2 + int(double(H) * 0.22 * std::sin(t * 1.3));
    const int rad = std::max(12, std::min(W, H) / 10);
    const int rad2 = rad * rad;

    for (int y = 0; y < H; ++y) {
        uint8_t* p = img.row(y);
        const int dy = y - cy;
        const int dy2 = dy * dy;

        if (y < bar_h) {
            // Saturated color bars (test chroma subsampling & ringing).
            for (int x = 0; x < W; ++x) {
                const int bar = ((x + shift_x) % W) * 7 / W;
                p[x * 4 + 0] = kBars[bar][0];
                p[x * 4 + 1] = kBars[bar][1];
                p[x * 4 + 2] = kBars[bar][2];
                p[x * 4 + 3] = 255;
            }
        } else if (y >= chk_y0) {
            // Left half: medium-frequency checkerboard (16px tiles); right half: smooth grey ramp.
            const int ty = (y + shift_y) >> 4;
            for (int x = 0; x < W; ++x) {
                if (x < chk_x_mid) {
                    const int tx = (x + shift_x) >> 4;
                    const uint8_t v = ((tx ^ ty) & 1) ? 220 : 36;
                    p[x * 4 + 0] = v;
                    p[x * 4 + 1] = v;
                    p[x * 4 + 2] = v;
                } else {
                    const uint8_t v = uint8_t(((x - chk_x_mid) * 255) / std::max(1, W - chk_x_mid - 1));
                    p[x * 4 + 0] = v;
                    p[x * 4 + 1] = v;
                    p[x * 4 + 2] = v;
                }
                p[x * 4 + 3] = 255;
            }
        } else {
            // Main area: smooth 2D color gradient with subtle diagonal wave.
            const int gy = ((y - bar_h) * 255) / std::max(1, chk_y0 - bar_h);
            for (int x = 0; x < W; ++x) {
                const int gx = ((x + shift_x) * 255) / std::max(1, W);
                const int diag = ((x + y + int(frame_id * 4)) & 255);
                const int wave = diag < 128 ? diag * 2 : (255 - diag) * 2;
                p[x * 4 + 0] = uint8_t((gx * 3 + wave) >> 2);
                p[x * 4 + 1] = uint8_t((gy * 3 + (255 - wave)) >> 2);
                p[x * 4 + 2] = uint8_t(((255 - gx) + gy) >> 1);
                p[x * 4 + 3] = 255;
            }
        }

        // Overlay moving anti-aliased-ish disc (sharp boundary + radial shading).
        if (dy2 < rad2) {
            for (int x = std::max(0, cx - rad); x < std::min(W, cx + rad); ++x) {
                const int dx = x - cx;
                const int d2 = dx * dx + dy2;
                if (d2 < rad2) {
                    const int shade = 255 - (d2 * 180) / rad2;
                    p[x * 4 + 0] = clamp_u8(shade);
                    p[x * 4 + 1] = clamp_u8(shade / 2 + 40);
                    p[x * 4 + 2] = clamp_u8(255 - shade / 2);
                    p[x * 4 + 3] = 255;
                }
            }
        }
    }

    // Frame counter badge (top-left below color bars) for visual latency/continuity inspection.
    const int scale = std::max(1, std::min(W, H) / 180);
    const int bx = 16, by = bar_h + 16;
    char label[32];
    std::snprintf(label, sizeof(label), "F:%06u", frame_id % 1000000u);
    draw_rect(img, bx - 6 * scale / 2, by - 4 * scale / 2, 8 * 6 * scale + 6 * scale, 7 * scale + 4 * scale,
              16, 16, 24);
    draw_text(img, bx, by, scale, label, 245, 245, 64);
}

}  // namespace lv
