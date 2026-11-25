#include "gui/lanvid_studio.hpp"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreGraphics/CoreGraphics.h>  // CGPreflightScreenCaptureAccess (Screen Recording / TCC)
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "common/clock.hpp"
#include "imgui.h"

namespace lv::gui {

namespace {

constexpr int kBusChoices[] = {2, 8, 16, 32, 64};

const char* latency_mode_label(int ms) {
    if (ms <= 10) return "Ultra Low Latency: 0.5-1 мс пакети, XOR FEC 1+1/2+1, без ARQ";
    if (ms <= 100) return "Balanced / Live: 2.5-5 мс пакети, XOR FEC 4+1 + 1 NACK";
    return "High-Reliability / Bulletproof: 10-20 мс пакети, інтерлівінг FEC + до 12 NACK + дублювання";
}

constexpr const char* kAudioCodecItems[4] = {
    "Uncompressed (Float32 · ~98 Mbps)",
    "Lossless 24-bit (PCM24 · -25%)",
    "Compact 16-bit (PCM16 · -50%)",
    "Ultra-Light (Opus CELT · -85%)"
};

double estimate_aoip_mbps(int channels, int codec, int bitrate_kbps = 0) {
    const int ch = std::clamp(channels, 1, 64);
    if (bitrate_kbps > 0) {
        return double(ch) * double(bitrate_kbps) / 1000.0;
    }
    const double bits_per_sample = (codec == 0) ? 32.0
                                                : (codec == 1 ? 24.0
                                                              : (codec == 2 ? 16.0
                                                                            : (codec == 4 ? 5.33
                                                                                          : (codec == 6 ? 2.67
                                                                                                        : (codec == 7 ? 2.0
                                                                                                                      : (codec == 8 ? 1.33 : 4.0))))));
    return double(ch) * 48000.0 * bits_per_sample / 1e6;
}

void noop_bus_callback(const LV_MultichannelAudioBlock*, void*) {}  // drives the playout clock + meters

void push_hist(float* arr, int len, float val) {
    for (int i = 0; i < len - 1; ++i) arr[i] = arr[i + 1];
    arr[len - 1] = val;
}

// Dual-Theme Palette: Soft Pastel Lavender (default) + Dark Studio (toggleable in Settings)
constexpr float kPi = 3.14159265358979323846f;

bool g_dark_theme = false;

ImVec4 kColBgTopLeft   = ImVec4(0.867f, 0.839f, 0.996f, 1.00f);  // #DDD6FE
ImVec4 kColBgTopRight  = ImVec4(0.878f, 0.906f, 1.000f, 1.00f);  // #E0E7FF
ImVec4 kColBgBotRight  = ImVec4(0.929f, 0.914f, 0.996f, 1.00f);  // #EDE9FE
ImVec4 kColBgBotLeft   = ImVec4(0.894f, 0.878f, 1.000f, 1.00f);  // #E4E0FF
ImVec4 kColSidebar     = ImVec4(0.961f, 0.953f, 1.000f, 0.92f);  // #F5F3FF @ 0.92
ImVec4 kColCard        = ImVec4(1.000f, 1.000f, 1.000f, 0.97f);  // #FFFFFF
ImVec4 kColCardSoft    = ImVec4(0.953f, 0.937f, 0.996f, 0.96f);  // #F3EFFE
ImVec4 kColBorder      = ImVec4(0.867f, 0.839f, 0.996f, 1.00f);  // #DDD6FE
ImVec4 kColAccentBlue  = ImVec4(0.376f, 0.647f, 0.980f, 1.00f);  // #60A5FA
ImVec4 kColAccentSky   = ImVec4(0.231f, 0.510f, 0.965f, 1.00f);  // #3B82F6
ImVec4 kColAccentInd   = ImVec4(0.388f, 0.400f, 0.945f, 1.00f);  // #6366F1
ImVec4 kColAccentNode  = ImVec4(0.545f, 0.361f, 0.965f, 1.00f);  // #8B5CF6
ImVec4 kColAccentPurp  = ImVec4(0.576f, 0.200f, 0.918f, 1.00f);  // #9333EA
ImVec4 kColAccentViol  = ImVec4(0.659f, 0.333f, 0.969f, 1.00f);  // #A855F7
ImVec4 kColGreen       = ImVec4(0.063f, 0.725f, 0.506f, 1.00f);  // #10B981
ImVec4 kColAmber       = ImVec4(0.961f, 0.620f, 0.043f, 1.00f);  // #F59E0B
ImVec4 kColPink        = ImVec4(0.925f, 0.282f, 0.600f, 1.00f);  // #EC4899
ImVec4 kColRed         = ImVec4(0.937f, 0.267f, 0.267f, 1.00f);  // #EF4444
ImVec4 kColTextMain    = ImVec4(0.118f, 0.106f, 0.294f, 1.00f);  // #1E1B4B
ImVec4 kColTextMuted   = ImVec4(0.486f, 0.451f, 0.600f, 1.00f);  // #7C7399
constexpr ImVec4 kColWhite = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);  // #FFFFFF

ImVec4 kColCyan = kColAccentInd;
ImVec4 kColTeal = kColAccentPurp;

ImVec4 lerp_col(const ImVec4& a, const ImVec4& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void draw_soft_shadow(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float rounding, float elevation = 1.0f) {
    // Lightweight single-pass soft lavender shadow (minimal vertex overhead, zero judder)
    const float s = std::clamp(elevation, 0.5f, 1.4f);
    const ImU32 col = g_dark_theme ? IM_COL32(0, 0, 0, 48) : IM_COL32(124, 58, 237, 20);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 2.0f * s), ImVec2(p1.x, p1.y + 3.5f * s), col, rounding + 1.0f);
}

void draw_rounded_gradient_rect(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float rounding,
                                const ImVec4& col_a, const ImVec4& col_b, bool diagonal = true) {
    const float w = std::max(1.0f, p1.x - p0.x);
    const float h = std::max(1.0f, p1.y - p0.y);
    const float r = std::min(rounding, std::min(w, h) * 0.5f);
    dl->PathClear();
    dl->PathRect(p0, p1, r, ImDrawFlags_RoundCornersAll);
    const int vtx_begin = dl->VtxBuffer.Size;
    dl->PathFillConvex(IM_COL32_WHITE);
    const int vtx_end = dl->VtxBuffer.Size;
    for (int i = vtx_begin; i < vtx_end; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float tx = std::clamp((v.pos.x - p0.x) / w, 0.0f, 1.0f);
        const float ty = std::clamp((v.pos.y - p0.y) / h, 0.0f, 1.0f);
        const float t = diagonal ? (tx * 0.65f + ty * 0.35f) : tx;
        v.col = ImGui::ColorConvertFloat4ToU32(lerp_col(col_a, col_b, t));
    }
}

bool begin_glass_card(const char* id, const ImVec2& size, bool soft_lavender = false, ImGuiWindowFlags flags = 0) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = (size.x > 0.0f) ? size.x : std::max(1.0f, avail.x + size.x);
    const float h = (size.y > 0.0f) ? size.y : std::max(1.0f, avail.y + size.y);
    draw_soft_shadow(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + w, p0.y + h), 16.0f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, soft_lavender ? kColCardSoft : kColCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kColBorder);
    return ImGui::BeginChild(id, size, true, flags);
}

void end_glass_card() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

bool gradient_pill_button(const char* label, const ImVec2& req_size = ImVec2(0, 0),
                          const ImVec4& col_a = kColAccentBlue, const ImVec4& col_b = kColAccentPurp) {
    const char* disp_end = std::strstr(label, "##");
    const ImVec2 tsz = disp_end ? ImGui::CalcTextSize(label, disp_end) : ImGui::CalcTextSize(label);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = (req_size.x > 0.0f) ? req_size.x : (req_size.x < 0.0f ? std::max(40.0f, avail.x) : (tsz.x + 28.0f));
    const float h = (req_size.y > 0.0f) ? req_size.y : (tsz.y + 12.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = h * 0.5f;
    ImVec4 ca = col_a;
    ImVec4 cb = col_b;
    if (held) {
        ca = lerp_col(ca, ImVec4(0.08f, 0.08f, 0.16f, 1.0f), 0.18f);
        cb = lerp_col(cb, ImVec4(0.08f, 0.08f, 0.16f, 1.0f), 0.18f);
    } else if (hovered) {
        ca = lerp_col(ca, kColWhite, 0.14f);
        cb = lerp_col(cb, kColWhite, 0.10f);
    }
    draw_soft_shadow(dl, p0, p1, r, hovered ? 1.15f : 0.85f);
    draw_rounded_gradient_rect(dl, p0, p1, r, ca, cb, false);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 65), r, 0, 1.0f);
    const ImVec2 tpos(p0.x + (w - tsz.x) * 0.5f, p0.y + (h - tsz.y) * 0.5f);
    if (disp_end) dl->AddText(tpos, IM_COL32(255, 255, 255, 255), label, disp_end);
    else dl->AddText(tpos, IM_COL32(255, 255, 255, 255), label);
    return clicked;
}

bool status_pill_button(const char* label, bool active, const ImVec2& req_size = ImVec2(0, 0)) {
    if (active) {
        return gradient_pill_button(label, req_size, kColAccentInd, kColAccentViol);
    }
    const char* disp_end = std::strstr(label, "##");
    const ImVec2 tsz = disp_end ? ImGui::CalcTextSize(label, disp_end) : ImGui::CalcTextSize(label);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = (req_size.x > 0.0f) ? req_size.x : (req_size.x < 0.0f ? std::max(40.0f, avail.x) : (tsz.x + 22.0f));
    const float h = (req_size.y > 0.0f) ? req_size.y : (tsz.y + 10.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = h * 0.5f;
    const ImU32 bg = g_dark_theme
                         ? (hovered ? IM_COL32(38, 46, 66, 255) : IM_COL32(26, 32, 46, 255))
                         : (hovered ? IM_COL32(237, 233, 254, 255) : IM_COL32(255, 255, 255, 245));
    const ImU32 brd = hovered ? ImGui::ColorConvertFloat4ToU32(kColAccentNode)
                              : ImGui::ColorConvertFloat4ToU32(kColBorder);
    dl->AddRectFilled(p0, p1, bg, r);
    dl->AddRect(p0, p1, brd, r, 0, 1.0f);
    const ImVec2 tpos(p0.x + (w - tsz.x) * 0.5f, p0.y + (h - tsz.y) * 0.5f);
    const ImU32 tc = ImGui::ColorConvertFloat4ToU32(kColTextMain);
    if (disp_end) dl->AddText(tpos, tc, label, disp_end);
    else dl->AddText(tpos, tc, label);
    return clicked;
}

bool selectable_choice_card(const char* id, const char* title, const char* subtitle, bool selected,
                            const ImVec2& req_size) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = (req_size.x > 0.0f) ? req_size.x : std::max(120.0f, avail.x);
    const float h = (req_size.y > 0.0f) ? req_size.y : 64.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 14.0f;

    if (selected) {
        draw_soft_shadow(dl, p0, p1, r, 1.0f);
        draw_rounded_gradient_rect(dl, p0, p1, r, kColAccentInd, kColAccentViol, false);
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 75), r, 0, 1.0f);
    } else {
        draw_soft_shadow(dl, p0, p1, r, hovered ? 0.9f : 0.55f);
        const ImU32 bg = g_dark_theme
                             ? (hovered ? IM_COL32(32, 39, 56, 255) : IM_COL32(22, 27, 38, 255))
                             : (hovered ? IM_COL32(245, 242, 255, 255) : IM_COL32(255, 255, 255, 248));
        dl->AddRectFilled(p0, p1, bg, r);
        dl->AddRect(p0, p1, hovered ? ImGui::ColorConvertFloat4ToU32(kColAccentNode)
                                    : ImGui::ColorConvertFloat4ToU32(kColBorder),
                    r, 0, 1.2f);
    }

    const ImU32 title_col = selected ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(kColTextMain);
    const ImU32 sub_col = selected ? IM_COL32(238, 235, 255, 235) : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
    dl->AddText(ImVec2(p0.x + 14.0f, p0.y + 12.0f), title_col, title);
    if (subtitle && subtitle[0]) {
        dl->AddText(ImVec2(p0.x + 14.0f, p0.y + 34.0f), sub_col, subtitle);
    }
    if (selected) {
        dl->AddCircleFilled(ImVec2(p1.x - 18.0f, p0.y + 18.0f), 6.0f, IM_COL32(255, 255, 255, 245));
        dl->AddCircleFilled(ImVec2(p1.x - 18.0f, p0.y + 18.0f), 2.8f, ImGui::ColorConvertFloat4ToU32(kColAccentPurp));
    }
    return clicked;
}

bool studio_toggle(const char* str_id, bool* v, const ImVec4& active_col = kColAccentInd) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 22.0f;
    const float w = 42.0f;
    const float r = h * 0.5f;
    const bool clicked = ImGui::InvisibleButton(str_id, ImVec2(w, h));
    if (clicked) *v = !*v;
    const ImVec2 p1(p.x + w, p.y + h);
    if (*v) {
        draw_rounded_gradient_rect(dl, p, p1, r, active_col, kColAccentViol, false);
    } else {
        dl->AddRectFilled(p, p1, g_dark_theme ? IM_COL32(42, 50, 68, 255) : IM_COL32(221, 214, 254, 255), r);
    }
    dl->AddRect(p, p1, *v ? IM_COL32(139, 92, 246, 120) : ImGui::ColorConvertFloat4ToU32(kColBorder), r, 0, 1.0f);
    const float knob_x = *v ? (p.x + w - r) : (p.x + r);
    dl->AddCircleFilled(ImVec2(knob_x, p.y + r + 1.0f), r - 2.5f, IM_COL32(124, 58, 237, 45));
    dl->AddCircleFilled(ImVec2(knob_x, p.y + r), r - 2.5f, IM_COL32(255, 255, 255, 255));
    return clicked;
}

}  // namespace

bool LanVidStudioApp::draw_gradient_icon_tile(const char* id, IconKind kind, float size, int badge_count,
                                              bool clickable) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size, p0.y + size);
    bool clicked = false;
    bool hovered = false;
    bool held = false;
    if (clickable) {
        clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
        hovered = ImGui::IsItemHovered();
        held = ImGui::IsItemActive();
    } else {
        ImGui::Dummy(ImVec2(size, size));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = std::min(10.0f, size * 0.30f);
    ImVec4 col_a = kColAccentViol;  // #A855F7
    ImVec4 col_b = kColAccentInd;   // #6366F1
    if (kind == IconKind::kPanic || kind == IconKind::kStop) {
        col_a = kColPink;
        col_b = kColRed;
    } else if (kind == IconKind::kPlay) {
        col_a = kColAccentBlue;
        col_b = kColGreen;
    }
    if (held) {
        col_a = lerp_col(col_a, kColTextMain, 0.12f);
        col_b = lerp_col(col_b, kColTextMain, 0.12f);
    } else if (hovered) {
        col_a = lerp_col(col_a, kColWhite, 0.14f);
        col_b = lerp_col(col_b, kColWhite, 0.10f);
    }

    draw_soft_shadow(dl, p0, p1, rounding, hovered ? 1.25f : 0.9f);
    draw_rounded_gradient_rect(dl, p0, p1, rounding, col_a, col_b, true);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 70), rounding, 0, 1.0f);

    const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
    const float u = size * 0.28f;
    const ImU32 wcol = IM_COL32(255, 255, 255, 245);
    const float stroke = std::max(1.6f, size * 0.06f);

    switch (kind) {
        case IconKind::kGear: {
            dl->AddCircle(c, u * 0.56f, wcol, 16, stroke);
            for (int i = 0; i < 8; ++i) {
                const float ang = float(i) * (2.0f * kPi / 8.0f);
                const float ca = std::cos(ang), sa = std::sin(ang);
                dl->AddLine(ImVec2(c.x + ca * u * 0.65f, c.y + sa * u * 0.65f),
                            ImVec2(c.x + ca * u * 1.05f, c.y + sa * u * 1.05f), wcol, stroke * 1.1f);
            }
            break;
        }
        case IconKind::kPlay: {
            dl->AddTriangleFilled(ImVec2(c.x - u * 0.45f, c.y - u * 0.72f),
                                  ImVec2(c.x - u * 0.45f, c.y + u * 0.72f),
                                  ImVec2(c.x + u * 0.78f, c.y), wcol);
            break;
        }
        case IconKind::kPause: {
            dl->AddRectFilled(ImVec2(c.x - u * 0.62f, c.y - u * 0.68f),
                              ImVec2(c.x - u * 0.16f, c.y + u * 0.68f), wcol, 2.0f);
            dl->AddRectFilled(ImVec2(c.x + u * 0.16f, c.y - u * 0.68f),
                              ImVec2(c.x + u * 0.62f, c.y + u * 0.68f), wcol, 2.0f);
            break;
        }
        case IconKind::kStop: {
            dl->AddRectFilled(ImVec2(c.x - u * 0.60f, c.y - u * 0.60f),
                              ImVec2(c.x + u * 0.60f, c.y + u * 0.60f), wcol, 2.5f);
            break;
        }
        case IconKind::kPanic: {
            dl->AddLine(ImVec2(c.x, c.y - u * 0.78f), ImVec2(c.x, c.y + u * 0.18f), wcol, stroke * 1.3f);
            dl->AddCircleFilled(ImVec2(c.x, c.y + u * 0.66f), stroke * 0.75f, wcol);
            break;
        }
        case IconKind::kWaveform: {
            const float hs[5] = {0.35f, 0.85f, 1.05f, 0.65f, 0.40f};
            for (int i = 0; i < 5; ++i) {
                const float x = c.x + (float(i) - 2.0f) * (u * 0.48f);
                const float hh = u * hs[i];
                dl->AddLine(ImVec2(x, c.y - hh), ImVec2(x, c.y + hh), wcol, stroke * 1.15f);
            }
            break;
        }
        case IconKind::kHeadphones: {
            dl->PathClear();
            dl->PathArcTo(ImVec2(c.x, c.y + u * 0.05f), u * 0.80f, kPi, 2.0f * kPi, 16);
            dl->PathStroke(wcol, 0, stroke);
            dl->AddRectFilled(ImVec2(c.x - u * 0.96f, c.y + u * 0.02f),
                              ImVec2(c.x - u * 0.50f, c.y + u * 0.78f), wcol, 3.0f);
            dl->AddRectFilled(ImVec2(c.x + u * 0.50f, c.y + u * 0.02f),
                              ImVec2(c.x + u * 0.96f, c.y + u * 0.78f), wcol, 3.0f);
            break;
        }
        case IconKind::kMidiPlug: {
            dl->AddCircle(c, u * 0.92f, wcol, 20, stroke);
            // 5-pin DIN arc at bottom half
            for (int i = 0; i < 5; ++i) {
                const float ang = kPi * 0.15f + float(i) * (kPi * 0.70f / 4.0f);
                dl->AddCircleFilled(ImVec2(c.x + std::cos(ang) * u * 0.50f, c.y + std::sin(ang) * u * 0.50f),
                                    std::max(1.3f, size * 0.042f), wcol);
            }
            break;
        }
        case IconKind::kSend: {
            dl->AddLine(ImVec2(c.x - u * 0.70f, c.y + u * 0.45f), ImVec2(c.x + u * 0.65f, c.y - u * 0.55f), wcol, stroke);
            dl->AddTriangleFilled(ImVec2(c.x + u * 0.78f, c.y - u * 0.65f),
                                  ImVec2(c.x + u * 0.12f, c.y - u * 0.62f),
                                  ImVec2(c.x + u * 0.52f, c.y - u * 0.05f), wcol);
            break;
        }
        case IconKind::kReceive: {
            dl->AddLine(ImVec2(c.x + u * 0.65f, c.y - u * 0.55f), ImVec2(c.x - u * 0.55f, c.y + u * 0.45f), wcol, stroke);
            dl->AddTriangleFilled(ImVec2(c.x - u * 0.75f, c.y + u * 0.60f),
                                  ImVec2(c.x - u * 0.10f, c.y + u * 0.58f),
                                  ImVec2(c.x - u * 0.50f, c.y + u * 0.02f), wcol);
            break;
        }
        case IconKind::kDuplex: {
            dl->AddLine(ImVec2(c.x - u * 0.75f, c.y - u * 0.30f), ImVec2(c.x + u * 0.65f, c.y - u * 0.30f), wcol, stroke);
            dl->AddTriangleFilled(ImVec2(c.x + u * 0.80f, c.y - u * 0.30f),
                                  ImVec2(c.x + u * 0.35f, c.y - u * 0.64f),
                                  ImVec2(c.x + u * 0.35f, c.y + u * 0.04f), wcol);
            dl->AddLine(ImVec2(c.x + u * 0.75f, c.y + u * 0.35f), ImVec2(c.x - u * 0.65f, c.y + u * 0.35f), wcol, stroke);
            dl->AddTriangleFilled(ImVec2(c.x - u * 0.80f, c.y + u * 0.35f),
                                  ImVec2(c.x - u * 0.35f, c.y + u * 0.01f),
                                  ImVec2(c.x - u * 0.35f, c.y + u * 0.69f), wcol);
            break;
        }
        case IconKind::kMatrix: {
            const float g = u * 0.55f;
            for (int r = -1; r <= 1; ++r) {
                for (int k = -1; k <= 1; ++k) {
                    const bool hi = (r == k);
                    dl->AddCircleFilled(ImVec2(c.x + float(k) * g, c.y + float(r) * g),
                                        hi ? std::max(2.1f, size * 0.075f) : std::max(1.4f, size * 0.045f),
                                        hi ? wcol : IM_COL32(255, 255, 255, 130));
                }
            }
            break;
        }
    }

    // Circular white counter badge in top-right corner
    if (badge_count >= 0) {
        char btxt[12];
        if (badge_count > 99) std::snprintf(btxt, sizeof(btxt), "99+");
        else std::snprintf(btxt, sizeof(btxt), "%d", badge_count);
        const ImVec2 bsz = ImGui::CalcTextSize(btxt);
        const float br = std::max(8.5f, bsz.x * 0.5f + 3.5f);
        const ImVec2 bc(p1.x - 1.5f, p0.y + 1.5f);
        dl->AddCircleFilled(ImVec2(bc.x, bc.y + 1.0f), br, IM_COL32(124, 58, 237, 50));
        dl->AddCircleFilled(bc, br, IM_COL32(255, 255, 255, 255));
        dl->AddCircle(bc, br, IM_COL32(221, 214, 254, 255), 16, 1.2f);
        dl->AddText(ImVec2(bc.x - bsz.x * 0.5f, bc.y - bsz.y * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(kColTextMain), btxt);
    }

    return clicked;
}

void LanVidStudioApp::draw_ring_gauge(const char* title, const char* value_str, const char* sub_str,
                                      float fraction, float card_w, float card_h, bool warn, bool health_mode) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + card_w, p0.y + card_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    draw_soft_shadow(dl, p0, p1, 14.0f, 0.85f);
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(kColCard), 14.0f);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(kColBorder), 14.0f, 0, 1.0f);

    const float frac = std::clamp(fraction, 0.0f, 1.0f);
    const float ring_r = std::min(card_w * 0.31f, (card_h - 30.0f) * 0.44f);
    const float thick = std::max(5.5f, ring_r * 0.22f);
    const ImVec2 center(p0.x + card_w * 0.5f, p0.y + 12.0f + ring_r + 2.0f);

    // Background ring track (#EDE9FE in lavender, subtle slate in dark mode)
    dl->PathClear();
    dl->PathArcTo(center, ring_r, 0.0f, 2.0f * kPi, 40);
    dl->PathStroke(g_dark_theme ? IM_COL32(37, 45, 61, 255) : IM_COL32(237, 233, 254, 255), ImDrawFlags_Closed, thick);

    // Foreground gradient arc
    const ImVec4 col_start = warn ? kColAmber : (health_mode ? kColGreen : kColAccentPurp);
    const ImVec4 col_end = warn ? kColPink : (health_mode ? kColAccentBlue : kColAccentBlue);
    if (frac > 0.005f) {
        constexpr int kSteps = 32;
        const int segs = std::max(2, int(std::round(float(kSteps) * frac)));
        const float start_ang = -kPi * 0.5f;
        const float sweep = 2.0f * kPi * frac;
        for (int i = 0; i < segs; ++i) {
            const float t0 = float(i) / float(segs);
            const float t1 = float(i + 1) / float(segs);
            const float a0 = start_ang + sweep * t0;
            const float a1 = start_ang + sweep * t1;
            const ImVec2 q0(center.x + std::cos(a0) * ring_r, center.y + std::sin(a0) * ring_r);
            const ImVec2 q1(center.x + std::cos(a1) * ring_r, center.y + std::sin(a1) * ring_r);
            dl->AddLine(q0, q1, ImGui::ColorConvertFloat4ToU32(lerp_col(col_start, col_end, (t0 + t1) * 0.5f)), thick);
        }
        // Rounded end-caps
        const ImVec2 cap0(center.x + std::cos(start_ang) * ring_r, center.y + std::sin(start_ang) * ring_r);
        const ImVec2 cap1(center.x + std::cos(start_ang + sweep) * ring_r,
                          center.y + std::sin(start_ang + sweep) * ring_r);
        dl->AddCircleFilled(cap0, thick * 0.5f, ImGui::ColorConvertFloat4ToU32(col_start));
        dl->AddCircleFilled(cap1, thick * 0.5f, ImGui::ColorConvertFloat4ToU32(col_end));
    }

    // Center value & sub-label inside the ring
    const ImVec2 vsz = ImGui::CalcTextSize(value_str);
    dl->AddText(ImVec2(center.x - vsz.x * 0.5f, center.y - vsz.y * 0.5f - (sub_str && sub_str[0] ? 5.0f : 0.0f)),
                ImGui::ColorConvertFloat4ToU32(kColTextMain), value_str);
    if (sub_str && sub_str[0]) {
        const ImVec2 ssz = ImGui::CalcTextSize(sub_str);
        dl->AddText(ImVec2(center.x - ssz.x * 0.5f, center.y + 4.0f),
                    ImGui::ColorConvertFloat4ToU32(kColTextMuted), sub_str);
    }

    // Card title at bottom
    const ImVec2 tsz = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(center.x - tsz.x * 0.5f, p1.y - tsz.y - 7.0f),
                ImGui::ColorConvertFloat4ToU32(kColTextMuted), title);

    ImGui::Dummy(ImVec2(card_w, card_h));
}

void LanVidStudioApp::apply_theme(bool dark) {
    g_dark_theme = dark;
    if (!dark) {
        // Soft Pastel Lavender / Blue-Purple (Soft Glass / Claymorphic UI)
        kColBgTopLeft   = ImVec4(0.867f, 0.839f, 0.996f, 1.00f);  // #DDD6FE
        kColBgTopRight  = ImVec4(0.878f, 0.906f, 1.000f, 1.00f);  // #E0E7FF
        kColBgBotRight  = ImVec4(0.929f, 0.914f, 0.996f, 1.00f);  // #EDE9FE
        kColBgBotLeft   = ImVec4(0.894f, 0.878f, 1.000f, 1.00f);  // #E4E0FF
        kColSidebar     = ImVec4(0.961f, 0.953f, 1.000f, 0.92f);  // #F5F3FF @ 0.92
        kColCard        = ImVec4(1.000f, 1.000f, 1.000f, 0.97f);  // #FFFFFF
        kColCardSoft    = ImVec4(0.953f, 0.937f, 0.996f, 0.96f);  // #F3EFFE
        kColBorder      = ImVec4(0.867f, 0.839f, 0.996f, 1.00f);  // #DDD6FE
        kColAccentInd   = ImVec4(0.388f, 0.400f, 0.945f, 1.00f);  // #6366F1
        kColAccentPurp  = ImVec4(0.576f, 0.200f, 0.918f, 1.00f);  // #9333EA
        kColTextMain    = ImVec4(0.118f, 0.106f, 0.294f, 1.00f);  // #1E1B4B
        kColTextMuted   = ImVec4(0.486f, 0.451f, 0.600f, 1.00f);  // #7C7399
    } else {
        // Dark Studio Theme
        kColBgTopLeft   = ImVec4(0.051f, 0.067f, 0.090f, 1.00f);  // #0D1117
        kColBgTopRight  = ImVec4(0.063f, 0.075f, 0.114f, 1.00f);
        kColBgBotRight  = ImVec4(0.051f, 0.067f, 0.090f, 1.00f);
        kColBgBotLeft   = ImVec4(0.071f, 0.067f, 0.114f, 1.00f);
        kColSidebar     = ImVec4(0.067f, 0.086f, 0.122f, 0.96f);  // #11161F
        kColCard        = ImVec4(0.086f, 0.106f, 0.149f, 0.98f);  // #161B26
        kColCardSoft    = ImVec4(0.110f, 0.133f, 0.188f, 0.96f);
        kColBorder      = ImVec4(0.145f, 0.176f, 0.239f, 1.00f);  // #252D3D
        kColAccentInd   = ImVec4(0.494f, 0.525f, 0.988f, 1.00f);
        kColAccentPurp  = ImVec4(0.753f, 0.518f, 0.988f, 1.00f);
        kColTextMain    = ImVec4(0.953f, 0.957f, 0.965f, 1.00f);  // #F3F4F6
        kColTextMuted   = ImVec4(0.545f, 0.580f, 0.650f, 1.00f);  // #8B949E
    }
    kColCyan = kColAccentInd;
    kColTeal = kColAccentPurp;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 16.0f;
    st.ChildRounding = 16.0f;
    st.FrameRounding = 999.0f;
    st.PopupRounding = 14.0f;
    st.GrabRounding = 999.0f;
    st.TabRounding = 999.0f;
    st.ScrollbarRounding = 999.0f;
    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.PopupBorderSize = 1.0f;
    st.WindowPadding = ImVec2(14.0f, 12.0f);
    st.FramePadding = ImVec2(12.0f, 6.0f);
    st.ItemSpacing = ImVec2(8.0f, 8.0f);
    st.ScrollbarSize = 9.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]             = kColBgBotRight;
    c[ImGuiCol_ChildBg]              = kColCard;
    c[ImGuiCol_PopupBg]              = dark ? ImVec4(0.086f, 0.106f, 0.149f, 0.98f)
                                            : ImVec4(1.000f, 1.000f, 1.000f, 0.98f);
    c[ImGuiCol_Border]               = kColBorder;
    c[ImGuiCol_BorderShadow]         = dark ? ImVec4(0, 0, 0, 0) : ImVec4(0.486f, 0.227f, 0.929f, 0.06f);
    c[ImGuiCol_FrameBg]              = dark ? kColSidebar : kColWhite;
    c[ImGuiCol_FrameBgHovered]       = dark ? ImVec4(0.12f, 0.15f, 0.22f, 1.0f) : ImVec4(0.961f, 0.953f, 1.000f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = dark ? ImVec4(0.15f, 0.19f, 0.27f, 1.0f) : ImVec4(0.929f, 0.914f, 0.996f, 1.00f);
    c[ImGuiCol_TitleBg]              = kColSidebar;
    c[ImGuiCol_TitleBgActive]        = kColCardSoft;
    c[ImGuiCol_CheckMark]            = kColAccentPurp;
    c[ImGuiCol_SliderGrab]           = kColAccentInd;
    c[ImGuiCol_SliderGrabActive]     = kColAccentPurp;
    c[ImGuiCol_Button]               = dark ? ImVec4(0.13f, 0.16f, 0.23f, 1.0f) : ImVec4(0.929f, 0.914f, 0.996f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = dark ? ImVec4(0.18f, 0.22f, 0.32f, 1.0f) : ImVec4(0.867f, 0.839f, 0.996f, 1.00f);
    c[ImGuiCol_ButtonActive]         = dark ? ImVec4(0.24f, 0.28f, 0.42f, 1.0f) : ImVec4(0.769f, 0.710f, 0.992f, 1.00f);
    c[ImGuiCol_Header]               = dark ? ImVec4(0.39f, 0.40f, 0.95f, 0.28f) : ImVec4(0.867f, 0.839f, 0.996f, 0.70f);
    c[ImGuiCol_HeaderHovered]        = dark ? ImVec4(0.39f, 0.40f, 0.95f, 0.42f) : ImVec4(0.769f, 0.710f, 0.992f, 0.55f);
    c[ImGuiCol_HeaderActive]         = dark ? ImVec4(0.58f, 0.20f, 0.92f, 0.50f) : ImVec4(0.659f, 0.333f, 0.969f, 0.35f);
    c[ImGuiCol_Tab]                  = kColCardSoft;
    c[ImGuiCol_TabHovered]           = dark ? ImVec4(0.18f, 0.22f, 0.32f, 1.0f) : ImVec4(0.867f, 0.839f, 0.996f, 1.00f);
    c[ImGuiCol_TabSelected]          = dark ? kColCard : kColWhite;
    c[ImGuiCol_PlotLines]            = kColAccentInd;
    c[ImGuiCol_PlotLinesHovered]     = kColAccentPurp;
    c[ImGuiCol_PlotHistogram]        = kColAccentViol;
    c[ImGuiCol_PlotHistogramHovered] = kColPink;
    c[ImGuiCol_Separator]            = kColBorder;
    c[ImGuiCol_Text]                 = kColTextMain;
    c[ImGuiCol_TextDisabled]         = kColTextMuted;
    c[ImGuiCol_ScrollbarBg]          = dark ? ImVec4(0.06f, 0.08f, 0.11f, 0.60f) : ImVec4(0.953f, 0.937f, 0.996f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = dark ? ImVec4(0.24f, 0.29f, 0.40f, 0.85f) : ImVec4(0.769f, 0.710f, 0.992f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = kColAccentViol;
    c[ImGuiCol_ScrollbarGrabActive]  = kColAccentPurp;
    c[ImGuiCol_TableHeaderBg]        = kColCardSoft;
    c[ImGuiCol_TableBorderStrong]    = kColBorder;
    c[ImGuiCol_TableBorderLight]     = dark ? ImVec4(0.14f, 0.18f, 0.24f, 1.0f) : ImVec4(0.929f, 0.914f, 0.996f, 1.00f);
    c[ImGuiCol_TableRowBg]           = dark ? ImVec4(0.086f, 0.106f, 0.149f, 0.90f) : ImVec4(1.000f, 1.000f, 1.000f, 0.90f);
    c[ImGuiCol_TableRowBgAlt]        = dark ? ImVec4(0.102f, 0.125f, 0.176f, 0.90f) : ImVec4(0.973f, 0.965f, 1.000f, 0.90f);
}

LanVidStudioApp::LanVidStudioApp(PlatformHost host) : host_(host) {}

LanVidStudioApp::~LanVidStudioApp() { shutdown(); }

void LanVidStudioApp::init() {
    lv_init();
    load_app_config(cfg_);
    apply_theme(cfg_.dark_theme);
    std::snprintf(ip_buf_, sizeof(ip_buf_), "%s", cfg_.dest_ip.c_str());
    std::snprintf(file_buf_, sizeof(file_buf_), "%s", cfg_.file_path.c_str());
    std::snprintf(dpx_peer_buf_, sizeof(dpx_peer_buf_), "%s", cfg_.dpx_peer_host.c_str());
    duplex_ = lv_duplex_create();
    for (int d = 0; d < 2; ++d)
        for (int c = 0; c < 64; ++c) route_[d][c] = int8_t(c);

    sender_ = lv_sender_create();
    receiver_ = lv_receiver_create();
    wire_network_midi();
    if (host_.create_texture) {
        sender_tex_ = host_.create_texture(host_.ctx);
        receiver_tex_ = host_.create_texture(host_.ctx);
    }

    refresh_devices();
    refresh_network_interfaces();
    init_audio_matrix();
    if (monitors_.empty() && cfg_.source_type == LV_SOURCE_SCREEN) {
        cfg_.source_type = LV_SOURCE_PATTERN;
    }

    // Auto-start Personal Monitor Server (UDP :5020 + LAN Broadcast Discovery :5021 + mDNS)
    // so smartphones running LanVid Monitor discover and connect immediately on launch.
    const std::string studio_label = "LanVid Studio (" + local_host_name() + ")";
    monitor_server_.set_studio_name(studio_label);
    monitor_server_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
    monitor_server_.set_network_interfaces(
        cfg_.primary_nic_ip,
        cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : std::string());
    tally_intercom_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
    bool mon_ok = monitor_server_.start(kDefaultMonitorPort, "0.0.0.0", kDefaultMonitorDiscoveryPort);
    if (!mon_ok) {
        for (uint16_t p = 5023; p <= 5030 && !mon_ok; ++p) {
            mon_ok = monitor_server_.start(p, "0.0.0.0", kDefaultMonitorDiscoveryPort);
        }
        if (!mon_ok) mon_ok = monitor_server_.start(0, "0.0.0.0", kDefaultMonitorDiscoveryPort);
    }
    if (mon_ok) {
        monitor_mdns_.start(studio_label, monitor_server_.bound_port(),
                            {{"role", "monitor"}, {"ver", LANVID_VERSION_STRING},
                             {"ch", std::to_string(cfg_.aoip_channels)}});
    }

    // Enumerate Layer 2 Ethernet adapters for LanVid Grid (256ch) — never disturbs Classic UDP/L3!
    grid_adapters_ = lv::grid::list_l2_adapters();
    lv::grid::l2_native_backend_available(&grid_backend_status_);
    if (cfg_.network_engine_mode != 0) {
        start_grid_node();
    }

    start_lan_scan();
}

void LanVidStudioApp::shutdown() {
    stop_grid_node();
    monitor_mdns_.stop();
    monitor_server_.stop();
    tally_intercom_.stop();
    audio_matrix_.stop();
    stop_sender();
    stop_receiver();
    if (disc_thread_.joinable()) disc_thread_.join();
    if (nodes_thread_.joinable()) nodes_thread_.join();
    if (duplex_) {
        lv_duplex_stop(duplex_);
        lv_duplex_destroy(duplex_);
        duplex_ = nullptr;
        duplex_running_ = false;
    }

    if (sender_) {
        stop_tone();
        stop_vdev_tx();
        vdev_rx_.stop();
        lv_sender_destroy(sender_);
        sender_ = nullptr;
    }
    if (receiver_) {
        lv_receiver_destroy(receiver_);
        receiver_ = nullptr;
    }
    if (host_.destroy_texture) {
        if (sender_tex_) {
            host_.destroy_texture(host_.ctx, sender_tex_);
            sender_tex_ = nullptr;
        }
        if (receiver_tex_) {
            host_.destroy_texture(host_.ctx, receiver_tex_);
            receiver_tex_ = nullptr;
        }
    }
    cfg_.dest_ip = ip_buf_;
    cfg_.file_path = file_buf_;
    cfg_.dpx_peer_host = dpx_peer_buf_;
    sync_matrix_to_config();
    save_app_config(cfg_);
    lv_shutdown();
}

// MIDI2 over IP <-> Audio & MIDI Matrix:
//   matrix "Network MIDI TX" slot -> duplex node (if running) or sender -> peer
//   peer -> receiver / duplex -> matrix "Network MIDI RX" slot -> crosspoints -> CoreMIDI / WinMM outputs
//   (e.g. "LanVid Virtual MIDI Out" into the DAW)
void LanVidStudioApp::wire_network_midi() {
    if (sync_origin_id_ == 0) {
        sync_origin_id_ = uint32_t((now_us() ^ 0x5A3C9E17ull) & 0xFFFFFFFFull);
        if (sync_origin_id_ == 0) sync_origin_id_ = 1;
    }
    audio_matrix_.set_midi_sink([this](int dst_slot, uint8_t st, uint8_t d1, uint8_t d2, uint8_t port, uint64_t) {
        bool to_net = false;
        for (const auto& s : audio_matrix_.midi_slots())
            if (s.id == dst_slot && s.type == MidiSlotType::kNetworkMidiTx) to_net = true;
        if (!to_net) return;  // OS MIDI outputs are driven by the matrix itself
        if (duplex_running_ && duplex_) lv_duplex_send_midi(duplex_, st, d1, d2, port, 0);
        else if (sender_running_ && sender_) lv_sender_push_midi(sender_, st, d1, d2, port, 0);
    });
    if (sender_) lv_sender_set_control_sync_callback(sender_, &LanVidStudioApp::on_remote_control_sync, this);
    if (receiver_) {
        lv_receiver_set_midi_callback(receiver_, &LanVidStudioApp::on_network_midi, this);
        lv_receiver_set_control_sync_callback(receiver_, &LanVidStudioApp::on_remote_control_sync, this);
    }
    if (duplex_) {
        lv_duplex_set_midi_callback(duplex_, &LanVidStudioApp::on_network_midi, this);
        lv_duplex_set_control_sync_callback(duplex_, &LanVidStudioApp::on_remote_control_sync, this);
    }
}

void LanVidStudioApp::on_remote_control_sync(const LV_ControlSyncState* state, void* user_data) {
    auto* self = static_cast<LanVidStudioApp*>(user_data);
    if (!self || !state) return;
    if (state->origin_id == self->sync_origin_id_) return;  // echo guard
    std::lock_guard<std::mutex> lk(self->remote_sync_mu_);
    if (state->revision <= self->last_applied_remote_revision_) return;  // anti-loop / dedup
    self->pending_remote_sync_ = *state;
    self->has_pending_remote_sync_ = true;
}

void LanVidStudioApp::broadcast_control_sync(bool mute_all_panic) {
    if (!cfg_.sync_settings || applying_remote_sync_) return;
    if (!sender_running_ && !receiver_running_ && !duplex_running_) return;
    local_sync_revision_ = std::max(local_sync_revision_, last_applied_remote_revision_) + 1;
    LV_ControlSyncState st{};
    st.revision = local_sync_revision_;
    st.origin_id = sync_origin_id_;
    st.audio_channels = uint16_t(std::clamp(cfg_.aoip_channels, 1, 64));
    st.audio_bitrate_kbps = uint16_t(std::max(0, cfg_.audio_bitrate_kbps));
    st.target_buffer_ms = uint16_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    st.latency_mode = uint8_t(cfg_.adaptive_jitter ? 1 : 0);
    st.audio_only = uint8_t(cfg_.midi_only ? 2 : (cfg_.audio_only ? 1 : 0));
    st.active_preset_id = int8_t(cfg_.sync_matrix_routing ? matrix_active_preset_ : -1);
    st.flags = uint8_t((mute_all_panic ? 0x01u : 0u) | (cfg_.sync_matrix_routing ? 0x02u : 0u));
    st.master_gain_cb = int16_t(std::clamp(int(std::lrint(matrix_new_gain_db_ * 100.0f)), -6000, 1200));
    st.audio_codec = uint8_t(std::clamp(cfg_.audio_codec, 0, 8));

    bool sent = false;
    if (duplex_running_ && duplex_) sent |= (lv_duplex_send_control_sync(duplex_, &st) == LV_OK);
    if (sender_running_ && sender_) sent |= (lv_sender_send_control_sync(sender_, &st) == LV_OK);
    if (receiver_running_ && receiver_) sent |= (lv_receiver_send_control_sync(receiver_, &st) == LV_OK);
    if (sent) last_synced_us_ = now_us();
}

void LanVidStudioApp::apply_pending_remote_sync() {
    LV_ControlSyncState st{};
    {
        std::lock_guard<std::mutex> lk(remote_sync_mu_);
        if (!has_pending_remote_sync_) return;
        has_pending_remote_sync_ = false;
        st = pending_remote_sync_;
    }
    if (!cfg_.sync_settings) return;
    if (st.revision <= last_applied_remote_revision_) return;

    applying_remote_sync_ = true;
    last_applied_remote_revision_ = st.revision;
    local_sync_revision_ = std::max(local_sync_revision_, st.revision);

    cfg_.aoip_channels = std::clamp<int>(st.audio_channels, 1, 64);
    tone_channels_.store(cfg_.aoip_channels);
    cfg_.audio_bitrate_kbps = int(st.audio_bitrate_kbps);
    cfg_.audio_codec = std::clamp<int>(st.audio_codec, 0, 8);
    cfg_.aoip_latency_ms = std::clamp<int>(st.target_buffer_ms, 2, 2000);
    cfg_.adaptive_jitter = st.latency_mode ? 1 : 0;
    cfg_.midi_only = (st.audio_only == 2);
    cfg_.audio_only = (st.audio_only >= 1);
    apply_stream_mode();
    matrix_new_gain_db_ = std::clamp(float(st.master_gain_cb) / 100.0f, -24.0f, 6.0f);

    // Apply live to audio engines without triggering a bounce-back sync packet
    if (sender_) {
        if (cfg_.audio_bitrate_kbps > 0) lv_sender_set_audio_bitrate(sender_, cfg_.audio_bitrate_kbps);
        else lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
    }
    if (receiver_) {
        apply_receiver_bus();
        lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
        lv_receiver_set_adaptive_jitter(receiver_, cfg_.adaptive_jitter);
    }
    if (duplex_running_ && duplex_) {
        if (cfg_.audio_bitrate_kbps > 0) lv_duplex_set_audio_bitrate(duplex_, cfg_.audio_bitrate_kbps);
        else lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
        lv_duplex_set_adaptive_jitter(duplex_, cfg_.adaptive_jitter);
    }

    if ((st.flags & 0x01u) != 0) {
        audio_matrix_.send_midi_panic(-1);
    }
    if (cfg_.sync_matrix_routing && (st.flags & 0x02u) != 0 && st.active_preset_id >= 0 && st.active_preset_id <= 2) {
        int net_id = -1, daw_id = -1, mon_id = -1;
        for (const auto& s : audio_matrix_.slots()) {
            if (s.type == MatrixSlotType::kNetworkAoip) net_id = s.id;
            else if (s.type == MatrixSlotType::kVirtualBus) daw_id = s.id;
            else if (s.type == MatrixSlotType::kSystemAudio && s.role == MatrixSlotRole::kOutput && mon_id < 0) mon_id = s.id;
        }
        matrix_active_preset_ = st.active_preset_id;
        audio_matrix_.apply_preset(st.active_preset_id, net_id, daw_id, mon_id);
    }

    last_synced_us_ = now_us();
    config_dirty_ = true;
    applying_remote_sync_ = false;
}

void LanVidStudioApp::on_network_midi(const LV_MidiEvent* ev, void* user_data) {
    auto* self = static_cast<LanVidStudioApp*>(user_data);
    if (!self || !ev) return;
    for (const auto& s : self->audio_matrix_.midi_slots()) {
        if (s.type == MidiSlotType::kNetworkMidiRx && s.active) {
            self->audio_matrix_.inject_midi_event(s.id, ev->status, ev->data1, ev->data2, ev->media_ts_us);
            return;
        }
    }
}

void LanVidStudioApp::refresh_devices() {
    last_dev_refresh_us_ = now_us();
    monitors_ = list_monitors();
    cameras_ = list_cameras();
    loop_devs_ = list_audio_devices(true);
    mic_devs_ = list_audio_devices(false);
    asio_devs_ = list_external_asio_drivers(true);
    midi_in_devs_ = list_midi_inputs();
    midi_out_devs_ = list_midi_outputs();
    sys_out_devs_ = list_system_audio_devices(true);   // macOS CoreAudio (empty on Windows/Linux)
    sys_in_devs_ = list_system_audio_devices(false);
    ifaces_ = list_network_interfaces();
    local_lan_ip_ = "127.0.0.1";
    for (const auto& iface : ifaces_) {
        if (!iface.is_loopback && !iface.ipv4.empty()) {
            local_lan_ip_ = iface.ipv4;
            break;
        }
    }
    if (!monitors_.empty() && cfg_.monitor_index >= int(monitors_.size())) cfg_.monitor_index = 0;
    if (!cameras_.empty() && cfg_.camera_index >= int(cameras_.size())) cfg_.camera_index = 0;
}

void LanVidStudioApp::start_lan_scan() {
    if (scanning_.exchange(true)) return;
    if (disc_thread_.joinable()) disc_thread_.join();
    disc_thread_ = std::thread([this]() {
        auto res = discovery_scan(450, kDiscoveryPort);
        {
            std::lock_guard<std::mutex> lk(disc_mu_);
            discovered_ = std::move(res);
        }
        scanning_.store(false, std::memory_order_release);
    });
}

void LanVidStudioApp::apply_preset(int preset_idx) {
    cfg_.preset = preset_idx;
    if (preset_idx == 0) {  // Low Latency 1080p60
        cfg_.pattern_width = 1920;
        cfg_.pattern_height = 1080;
        cfg_.fps = 60;
        cfg_.quality = 80;
        cfg_.fec_group = 0;
        cfg_.adaptive_quality = false;
    } else if (preset_idx == 1) {  // Ultra 4K60
        cfg_.pattern_width = 3840;
        cfg_.pattern_height = 2160;
        cfg_.fps = 60;
        cfg_.quality = 80;
        cfg_.fec_group = 6;
        cfg_.adaptive_quality = false;
    } else if (preset_idx == 2) {  // Wi-Fi Adaptive
        cfg_.fps = 60;
        cfg_.quality = 75;
        cfg_.fec_group = 6;
        cfg_.adaptive_quality = true;
        cfg_.q_min = 35;
        cfg_.q_max = 85;
    }
    config_dirty_ = true;
}

void LanVidStudioApp::start_sender() {
    if (!sender_) return;
    cfg_.dest_ip = ip_buf_;
    cfg_.file_path = file_buf_;
    save_app_config(cfg_);

    LvSenderConfig sc;
    lv_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "%s", cfg_.dest_ip.c_str());
    sc.dest_port = uint16_t(std::clamp(cfg_.dest_port, 1024, 65535));
    sc.source_type = cfg_.source_type;
    sc.monitor_index = cfg_.monitor_index;
    sc.capture_backend = cfg_.capture_backend;
    sc.camera_index = cfg_.camera_index;
    std::snprintf(sc.file_path, sizeof(sc.file_path), "%s", cfg_.file_path.c_str());
    sc.width = (cfg_.source_type == LV_SOURCE_PATTERN) ? cfg_.pattern_width : 0;
    sc.height = (cfg_.source_type == LV_SOURCE_PATTERN) ? cfg_.pattern_height : 0;
    sc.fps = double(cfg_.fps);
    sc.quality = cfg_.quality;
    sc.codec = std::clamp(cfg_.codec, 0, 2);
    sc.bitrate_kbps = uint32_t(std::clamp(cfg_.bitrate_mbps, 1, 50) * 1000);
    sc.keyframe_interval = uint32_t(std::clamp(cfg_.keyframe_interval, 15, 300));
    sc.fec_group_size = cfg_.fec_group;
    sc.adaptive_quality = cfg_.adaptive_quality ? 1 : 0;
    sc.q_min = cfg_.q_min;
    sc.q_max = cfg_.q_max;
    sc.audio_mode = cfg_.audio_mode;
    sc.audio_device_index = cfg_.audio_device_index;
    sc.audio_chunk_ms = cfg_.audio_chunk_ms;
    sc.audio_only = cfg_.audio_only ? 1 : 0;
    sc.audio_codec = std::clamp(cfg_.audio_codec, 0, 8);
    sc.audio_bitrate_kbps = cfg_.audio_bitrate_kbps;

    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    bus.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
    lv_sender_configure_audio_bus(sender_, &bus);

    lv_sender_set_network_interfaces(
        sender_, cfg_.primary_nic_ip.c_str(),
        cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip.c_str() : "");
    last_tx_preview_fid_ = 0;
    if (lv_sender_start(sender_, &sc)) {
        sender_running_ = true;
        lv_sender_set_midi_only(sender_, cfg_.midi_only ? 1 : 0);
        lv_sender_set_wifi_redundancy(sender_, cfg_.wifi_redundancy ? 1 : 0, 1500);
        if (!cfg_.midi_only && cfg_.aoip_tone && cfg_.aoip_source == 0) start_tone();
        if (!cfg_.midi_only && cfg_.aoip_source == 1) start_vdev_tx();
        broadcast_control_sync();  // Automatic handshake on start
        toast_msg_ = std::string(cfg_.audio_only ? "Передача [Audio Only] -> " : "Передача запущена -> ") +
                     cfg_.dest_ip + ":" + std::to_string(cfg_.dest_port);
        toast_until_us_ = now_us() + 3000000ull;
    } else {
        sender_running_ = false;
        toast_msg_ = "Помилка запуску передачі (перевірте IP / джерело)";
        toast_until_us_ = now_us() + 4000000ull;
    }
}

void LanVidStudioApp::start_tone() {
    stop_tone();
    tone_channels_.store(std::clamp(cfg_.aoip_channels, 1, 64));
    tone_stop_.store(false);
    tone_thread_ = std::thread([this]() {
        constexpr uint32_t kRate = 48000, kBlk = 240;  // 5 ms blocks
        std::vector<float> planes(size_t(LV_AUDIO_MAX_CHANNELS) * kBlk);
        const void* ptrs[LV_AUDIO_MAX_CHANNELS];
        for (int c = 0; c < LV_AUDIO_MAX_CHANNELS; ++c) ptrs[c] = planes.data() + size_t(c) * kBlk;
        uint64_t pos = 0;
        uint64_t next = now_us();
        while (!tone_stop_.load()) {
            next += uint64_t(kBlk) * 1000000ull / kRate;
            sleep_until_us(next, [] {});
            const int n = tone_channels_.load();
            for (int c = 0; c < n; ++c) {
                const double w = 6.283185307179586 * 110.0 * double(c + 1) / kRate;
                float* d = planes.data() + size_t(c) * kBlk;
                for (uint32_t i = 0; i < kBlk; ++i) d[i] = float(0.2 * std::sin(w * double(pos + i)));
            }
            pos += kBlk;
            lv_sender_push_audio_multichannel(sender_, ptrs, uint16_t(n), kBlk, LV_AUDIO_FORMAT_F32,
                                              LV_AUDIO_LAYOUT_PLANAR, LV_MEDIA_TS_AUTO);
        }
    });
}

void LanVidStudioApp::start_vdev_tx() {
    stop_vdev_tx();
    ShmAudioHost::Config hc;
    hc.channels = uint32_t(std::clamp(cfg_.aoip_channels, 1, 64));
    vdev_tx_.start(hc, [this](const float* const* p, uint32_t ch, uint32_t n, bool disc) {
        // Re-anchor to the sender Master Clock after DAW start/stall; otherwise stay contiguous.
        const uint64_t ts = disc ? lv_sender_get_master_clock_samples(sender_) - n : LV_MEDIA_TS_AUTO;
        lv_sender_push_audio_multichannel(sender_, p, uint16_t(ch), n, LV_AUDIO_FORMAT_F32,
                                          LV_AUDIO_LAYOUT_PLANAR, ts);
    }, nullptr);
}

void LanVidStudioApp::stop_vdev_tx() { vdev_tx_.stop(); }

void LanVidStudioApp::apply_receiver_output() {
    if (!receiver_) return;
    vdev_rx_.stop();
    rx_consumer_ = RxConsumer::kNone;
    if (!receiver_running_) return;
    if (matrix_net_rx_) {
        // The Audio Matrix network slot pulls the playout matrix (npop): it must be the ONLY consumer.
        // A second puller (meter pump / virtual-device bridge) would advance the shared playout cursor
        // twice per block - audio at double speed, constantly re-anchored: crackling AND slowed down.
        lv_receiver_set_multichannel_audio_callback(receiver_, nullptr, nullptr);
        rx_consumer_ = RxConsumer::kAudioMatrix;
        return;
    }
    if (cfg_.aoip_output == 1 && !matrix_vbus_) {
        rx_consumer_ = RxConsumer::kVirtualDevice;
        // The bridge becomes the single consumer of the playout matrix (it also drives the meters).
        lv_receiver_set_multichannel_audio_callback(receiver_, nullptr, nullptr);
        ShmAudioHost::Config hc;
        hc.channels = uint32_t(std::clamp(cfg_.aoip_channels, 1, 64));
        vdev_rx_.start(hc, nullptr, [this](float* const* p, uint32_t ch, uint32_t n) {
            static thread_local std::vector<float> tmp;
            tmp.resize(size_t(ch) * n);
            lv_receiver_pop_audio_multichannel(receiver_, tmp.data(), uint16_t(ch), n, LV_AUDIO_LAYOUT_PLANAR);
            for (uint32_t c = 0; c < ch; ++c) std::copy(tmp.begin() + c * n, tmp.begin() + (c + 1) * n, p[c]);
        });
    } else {
        lv_receiver_set_multichannel_audio_callback(receiver_, &noop_bus_callback, nullptr);
        rx_consumer_ = RxConsumer::kMeterPump;
    }
}

void LanVidStudioApp::enforce_audio_ownership(uint64_t now) {
    if (now - ownership_us_ < 100000ull) return;  // 10 Hz
    ownership_us_ = now;
    bool net_rx = false, net_tx = false, vbus = false;
    int net_slot_id = -1;
    if (audio_matrix_.is_running()) {
        for (const auto& sl : audio_matrix_.slots()) {
            if (!sl.active) continue;
            if (sl.type == MatrixSlotType::kNetworkAoip) {
                net_slot_id = sl.id;
                if (sl.role != MatrixSlotRole::kOutput) net_rx = true;  // matrix pulls the receiver
            } else if (sl.type == MatrixSlotType::kVirtualBus) {
                vbus = true;  // matrix owns both LanVidAudioBus_v1 rings
            }
        }
        // Only claim TX producer ownership when at least one unmuted crosspoint actually feeds Network AoIP!
        // Otherwise let the direct Sender source (DAW bridge / Test Tone / Loopback) feed UdpSender cleanly.
        if (net_slot_id >= 0) {
            for (const auto& cp : audio_matrix_.crosspoints()) {
                if (!cp.mute && cp.dst_slot == net_slot_id) {
                    net_tx = true;
                    break;
                }
            }
        }
    }
    matrix_has_net_tx_routes_.store(net_tx, std::memory_order_relaxed);
    const bool changed = net_rx != matrix_net_rx_ || net_tx != matrix_net_tx_ || vbus != matrix_vbus_;
    matrix_net_rx_ = net_rx;
    matrix_net_tx_ = net_tx;
    matrix_vbus_ = vbus;
    // Sender stream: one producer. The matrix network slot (npush), the test tone and the DAW TX bridge
    // all use LV_MEDIA_TS_AUTO - two of them would interleave blocks and run the media clock 2x.
    if ((net_tx || vbus) && vdev_tx_.running()) stop_vdev_tx();
    if (net_tx && tone_thread_.joinable()) stop_tone();
    if (!net_tx && sender_running_ && changed) {  // matrix released TX: restore direct Sender source
        if (cfg_.aoip_source == 1 && !vbus && !vdev_tx_.running()) start_vdev_tx();
        if (cfg_.aoip_source == 0 && cfg_.aoip_tone && !tone_thread_.joinable()) start_tone();
    }
    // Receiver playout matrix: one consumer.
    RxConsumer want = RxConsumer::kNone;
    if (receiver_running_) {
        want = net_rx ? RxConsumer::kAudioMatrix
                      : (cfg_.aoip_output == 1 && !vbus ? RxConsumer::kVirtualDevice : RxConsumer::kMeterPump);
    }
    if (want != rx_consumer_ || (vbus && vdev_rx_.running())) apply_receiver_output();
}

void LanVidStudioApp::draw_vdev_status(const ShmHostStatus& st, bool tx_side) {
    if (!st.bus_open) {
        ImGui::TextColored(kColRed, "Шина %s недоступна", kShmBusName);
        return;
    }
    if (st.driver_connected) {
        const uint32_t fill = tx_side ? st.tx_fill : st.rx_fill;
        ImGui::TextColored(kColGreen, "DAW підключена через %s",
                           st.driver_name.empty() ? "драйвер" : st.driver_name.c_str());
        ImGui::Text("%u Гц | буфер %u | %s %u / %u кадрів", st.sample_rate, st.buffer_size,
                    tx_side ? "TX" : "RX", fill, unsigned(kShmBusFrames));
        ImGui::ProgressBar(float(fill) / float(kShmBusFrames), ImVec2(-1, 7), "");
        ImGui::TextDisabled("%s: %llu кадрів | переповнень %llu | недоборів %llu | переякорень %llu",
                            tx_side ? "Відправлено" : "Доставлено",
                            (unsigned long long)(tx_side ? st.tx_frames : st.rx_frames),
                            (unsigned long long)st.tx_overruns, (unsigned long long)st.rx_underruns,
                            (unsigned long long)st.reanchors);
    } else {
        ImGui::TextColored(kColAmber, "Очікування DAW: виберіть пристрій «LanVid»");
        ImGui::TextDisabled("Windows: ASIO «LanVid Virtual ASIO» | macOS: «LanVid Virtual 64ch» | Linux: lanvid-audio-bridge");
    }
}

// ---------------------------------------------------------------- Full-duplex 64x64 AoIP node

void LanVidStudioApp::scan_aoip_nodes() {
    if (nodes_scanning_.exchange(true)) return;
    if (nodes_thread_.joinable()) nodes_thread_.join();
    nodes_thread_ = std::thread([this] {
        std::vector<LV_AudioNode> found(32);
        const int n = lv_discovery_scan_audio_nodes(600, 0, found.data(), int(found.size()));
        found.resize(size_t(std::max(n, 0)));
        {
            std::lock_guard<std::mutex> lk(nodes_mu_);
            nodes_ = std::move(found);
        }
        nodes_scanning_.store(false);
    });
}

void LanVidStudioApp::start_duplex() {
    if (!duplex_) return;
    // The duplex node owns both rings of the virtual device: release the single-direction bridges.
    stop_vdev_tx();
    vdev_rx_.stop();
    cfg_.dpx_peer_host = dpx_peer_buf_;
    save_app_config(cfg_);
    LV_DuplexConfig dc;
    lv_duplex_config_default(&dc);
    std::snprintf(dc.peer_host, sizeof(dc.peer_host), "%s", dpx_peer_buf_);
    dc.peer_port = uint16_t(std::clamp(cfg_.dpx_peer_port, 1024, 65535));
    dc.listen_port = uint16_t(std::clamp(cfg_.dpx_listen_port, 1024, 65535));
    dc.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    dc.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
    dc.audio_codec = std::clamp(cfg_.audio_codec, 0, 8);
    dc.adaptive_jitter = cfg_.adaptive_jitter ? 1 : 0;
    const int rc = lv_duplex_start(duplex_, &dc);
    duplex_running_ = rc == LV_OK;
    if (duplex_running_) {
        if (cfg_.audio_bitrate_kbps > 0) lv_duplex_set_audio_bitrate(duplex_, cfg_.audio_bitrate_kbps);
        lv_duplex_set_wifi_redundancy(duplex_, cfg_.wifi_redundancy ? 1 : 0, 1500);
        lv_duplex_set_network_interfaces(
            duplex_, cfg_.primary_nic_ip.c_str(),
            cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip.c_str() : "");
        lv_audio_set_tx_routing(duplex_, route_[0]);
        lv_audio_set_rx_routing(duplex_, route_[1]);
        broadcast_control_sync();  // Automatic handshake on start
        toast_msg_ = "Duplex 64x64: DAW -> " + std::string(dpx_peer_buf_) + ":" + std::to_string(dc.peer_port) +
                     " | :" + std::to_string(dc.listen_port) + " -> DAW";
    } else {
        toast_msg_ = std::string("Не вдалося запустити duplex: ") + lv_error_string(rc);
    }
    toast_until_us_ = now_us() + 3500000ull;
}

void LanVidStudioApp::stop_duplex() {
    if (duplex_) lv_duplex_stop(duplex_);
    duplex_running_ = false;
}

void LanVidStudioApp::draw_patchbay_block() {
    ImGui::TextColored(kColAccentInd, "Channel Routing & Solo/Mute");
    ImGui::SameLine();
    ImGui::RadioButton("TX (DAW -> мережа)", &patch_dir_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("RX (мережа -> DAW)", &patch_dir_, 1);
    const LV_AudioDirection dir = patch_dir_ == 0 ? LV_AUDIO_DIR_TX : LV_AUDIO_DIR_RX;
    const int d = patch_dir_;

    // Routing editor
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("Джерело", &patch_src_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("-> Призначення (0 = вимк.)", &patch_dst_);
    patch_src_ = std::clamp(patch_src_, 1, 64);
    patch_dst_ = std::clamp(patch_dst_, 0, 64);
    bool apply = false;
    if (gradient_pill_button("Призначити", ImVec2(0, 28))) { route_[d][patch_src_ - 1] = int8_t(patch_dst_ - 1); apply = true; }
    ImGui::SameLine();
    if (status_pill_button("1:1", false, ImVec2(0, 28))) { for (int c = 0; c < 64; ++c) route_[d][c] = int8_t(c); apply = true; }
    ImGui::SameLine();
    if (status_pill_button("Реверс 1..N", false, ImVec2(0, 28))) {
        const int n = std::clamp(cfg_.aoip_channels, 1, 64);
        for (int c = 0; c < 64; ++c) route_[d][c] = int8_t(c < n ? n - 1 - c : c);
        apply = true;
    }
    ImGui::SameLine();
    if (status_pill_button("Скинути M/S", false, ImVec2(0, 28))) {
        for (int c = 0; c < 64; ++c) {
            mute_[d][c] = solo_[d][c] = false;
            if (duplex_) lv_audio_set_channel_mute_solo(duplex_, dir, uint16_t(c), 0, 0);
        }
    }
    if (apply && duplex_) {
        if (d == 0) lv_audio_set_tx_routing(duplex_, route_[0]);
        else lv_audio_set_rx_routing(duplex_, route_[1]);
    }
    std::string routes;
    for (int c = 0; c < 64; ++c)
        if (route_[d][c] != c) routes += std::to_string(c + 1) + "->" + (route_[d][c] < 0 ? std::string("off") : std::to_string(route_[d][c] + 1)) + "  ";
    ImGui::TextDisabled("Маршрути: %s", routes.empty() ? "1:1 (без змін)" : routes.c_str());

    // 8 x 8 grid of channels with M / S toggles (5 ms ramps in the core: no clicks)
    const int n = std::clamp(cfg_.aoip_channels, 1, 64);
    const ImVec4 on_m = kColPink, on_s = kColAmber, off = ImVec4(0.929f, 0.914f, 0.996f, 1.0f);
    for (int c = 0; c < n; ++c) {
        ImGui::PushID(d * 64 + c);
        if (c % 8) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%2d", c + 1);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, mute_[d][c] ? on_m : off);
        ImGui::PushStyleColor(ImGuiCol_Text, mute_[d][c] ? kColWhite : kColTextMain);
        if (ImGui::SmallButton("M")) {
            mute_[d][c] = !mute_[d][c];
            if (duplex_) lv_audio_set_channel_mute_solo(duplex_, dir, uint16_t(c), mute_[d][c] ? 1 : 0, -1);
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, solo_[d][c] ? on_s : off);
        ImGui::PushStyleColor(ImGuiCol_Text, solo_[d][c] ? kColWhite : kColTextMain);
        if (ImGui::SmallButton("S")) {
            solo_[d][c] = !solo_[d][c];
            if (duplex_) lv_audio_set_channel_mute_solo(duplex_, dir, uint16_t(c), -1, solo_[d][c] ? 1 : 0);
        }
        ImGui::PopStyleColor(2);
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

void LanVidStudioApp::draw_duplex_tab() {
    const uint64_t now = now_us();
    if (duplex_running_ && now - dpx_stats_us_ > 250000) {
        dpx_stats_us_ = now;
        lv_duplex_get_stats(duplex_, &dpx_stats_);
    }
    begin_glass_card("##DpxLeft", ImVec2(420.0f, 0));
    draw_gradient_icon_tile("##DpxIcon", IconKind::kDuplex, 32.0f, cfg_.aoip_channels, false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColTextMain, "Full-Duplex 64x64 AoIP Transceiver");
    ImGui::TextDisabled("DAW-виходи -> вузол-партнер | партнер -> DAW-входи");
    ImGui::EndGroup();
    ImGui::Separator();

    if (gradient_pill_button(nodes_scanning_.load() ? "Пошук..." : "Знайти AoIP-вузли в LAN", ImVec2(-1, 30))) {
        scan_aoip_nodes();
    }
    {
        std::lock_guard<std::mutex> lk(nodes_mu_);
        const char* preview = nodes_.empty() ? "(натисніть «Знайти»)" : "Виберіть вузол...";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##DiscoveredAoIP", preview)) {
            for (size_t i = 0; i < nodes_.size(); ++i) {
                const LV_AudioNode& nd = nodes_[i];
                char lbl[320];
                std::snprintf(lbl, sizeof(lbl), "%s  %s:%u  %uch %s  [%s%s]", nd.name, nd.ip, nd.rx_port, nd.channels,
                              nd.duplex ? "duplex" : "rx", nd.audio_driver[0] ? nd.audio_driver : "без DAW",
                              nd.driver_connected ? ", DAW on" : "");
                if (ImGui::Selectable(lbl, false)) {  // one click: take the node's receive port as our target
                    std::snprintf(dpx_peer_buf_, sizeof(dpx_peer_buf_), "%s", nd.ip);
                    cfg_.dpx_peer_port = nd.rx_port;
                    if (nd.duplex && nd.tx_port) cfg_.dpx_listen_port = nd.tx_port;  // where it sends to us
                    config_dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Discovered AoIP Nodes: %zu", nodes_.size());
    }
    if (ImGui::InputText("IP партнера (вручну)", dpx_peer_buf_, sizeof(dpx_peer_buf_))) config_dirty_ = true;
    if (ImGui::InputInt("Порт партнера", &cfg_.dpx_peer_port)) {
        cfg_.dpx_peer_port = std::clamp(cfg_.dpx_peer_port, 1024, 65535);
        config_dirty_ = true;
    }
    if (ImGui::InputInt("Мій порт прийому", &cfg_.dpx_listen_port)) {
        cfg_.dpx_listen_port = std::clamp(cfg_.dpx_listen_port, 1024, 65535);
        config_dirty_ = true;
    }
    {
        int bus_idx = 1;
        for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
        const char* bus_items[] = {"2 канали", "8 каналів", "16 каналів", "32 канали", "64 канали"};
        if (ImGui::Combo("Канали AoIP", &bus_idx, bus_items, 5)) {
            cfg_.aoip_channels = kBusChoices[bus_idx];
            config_dirty_ = true;
        }
    }
    if (ImGui::SliderInt("Затримка", &cfg_.aoip_latency_ms, 2, 2000, "%d мс", ImGuiSliderFlags_Logarithmic)) {
        if (duplex_running_) lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    draw_latency_mode_controls("DpxLatMode", ImGui::CalcItemWidth());
    draw_actual_buffer_line();
    ImGui::TextDisabled("Бітрейт аудіо на канал:");
    draw_audio_bitrate_pills("DpxBitrate");
    if (ImGui::Checkbox("Audio Only (Disable Video)", &cfg_.audio_only)) {
        config_dirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Duplex 64x64 завжди передає лише AoIP без відео; перемикач також вимикає відео у Sender/Receiver.");
    }
    ImGui::Spacing();
    if (!duplex_running_) {
        if (gradient_pill_button("Старт Duplex 64x64", ImVec2(-1, 36), kColAccentBlue, kColAccentPurp)) start_duplex();
    } else if (gradient_pill_button("Стоп Duplex", ImVec2(-1, 36), kColPink, kColRed)) {
        stop_duplex();
    }
    ImGui::Separator();
    if (duplex_running_) {
        const LV_DuplexStats& st = dpx_stats_;
        st.driver_connected ? ImGui::TextColored(kColGreen, "DAW: %s | %u Гц | буфер %u",
                                                 st.driver_name, st.sample_rate, st.buffer_size)
                            : ImGui::TextColored(kColAmber, "Очікування DAW на пристрої «LanVid»");
        ImGui::Text("Партнер -> ми: годинник %s | RTT %.2f мс | %+.1f ppm", st.rx_clock_synced ? "synced" : "free-run",
                    double(st.rx_rtt_us) / 1000.0, st.rx_clock_skew_ppm);
        ImGui::TextDisabled("  пакети %llu | втрачено %llu | у DAW %llu кадрів | недоборів %llu",
                            (unsigned long long)st.rx_packets, (unsigned long long)st.rx_lost,
                            (unsigned long long)st.rx_frames_to_daw, (unsigned long long)st.rx_underruns);
        ImGui::Text("Ми -> партнер: пакети %llu | з DAW %llu кадрів", (unsigned long long)st.tx_packets,
                    (unsigned long long)st.tx_frames_from_daw);
        ImGui::TextDisabled("  переповнень %llu | відповідей clock-sync %llu (годинник цього напрямку вимірює партнер)",
                            (unsigned long long)st.tx_overruns, (unsigned long long)st.tx_pongs_sent);
    } else {
        ImGui::TextDisabled("Партнер запускає дзеркальні порти: його «Порт партнера» = мій порт прийому.");
    }
    end_glass_card();
    ImGui::SameLine();
    begin_glass_card("##DpxPatch", ImVec2(0, 0));
    draw_patchbay_block();
    end_glass_card();
}

// ---------------------------------------------------------------- Multi-Device Audio Matrix (64x64)

void LanVidStudioApp::init_audio_matrix() {
    audio_matrix_.clear_slots();
    audio_matrix_.set_buffer_size(cfg_.matrix_buffer_size);
    audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);

    if (cfg_.matrix_slots.empty()) {
        MatrixSlotConfig daw{};
        daw.id = 0;
        daw.type = MatrixSlotType::kVirtualBus;
        daw.role = MatrixSlotRole::kDuplex;
        daw.name = "DAW Virtual Bus (64ch)";
        daw.channels = 64;
        daw.buffer_size = cfg_.matrix_buffer_size;
        daw.active = true;
        audio_matrix_.add_slot(daw);

        MatrixSlotConfig net{};
        net.id = 1;
        net.type = MatrixSlotType::kNetworkAoip;
        net.role = MatrixSlotRole::kDuplex;
        net.name = "Network AoIP (64ch)";
        net.channels = 64;
        net.buffer_size = cfg_.matrix_buffer_size;
        net.active = true;
        audio_matrix_.add_slot(net);

        MatrixSlotConfig hp{};
        hp.id = 2;
        hp.type = MatrixSlotType::kSystemAudio;
        hp.role = MatrixSlotRole::kOutput;
#if defined(__APPLE__)
        hp.name = "Mac Headphones / Output (CoreAudio)";
#else
        hp.name = "PC Headphones / Output";
#endif
        hp.device_index = -1;
        hp.channels = 2;
        hp.buffer_size = cfg_.matrix_buffer_size;
        hp.active = true;
        audio_matrix_.add_slot(hp);

        audio_matrix_.apply_preset(0, 1, 0, 2);
        sync_matrix_to_config();
    } else {
        for (const auto& s : cfg_.matrix_slots) audio_matrix_.add_slot(s);
        for (const auto& c : cfg_.matrix_crosspoints) {
            audio_matrix_.set_crosspoint(c.src_slot, c.src_ch, c.dst_slot, c.dst_ch, c.gain_db, c.mute,
                                         !cfg_.matrix_feedback_guard);
        }
    }

    // Restore or initialize MIDI slots (Network MIDI TX/RX + system virtual DAW port + saved OS MIDI devices)
    audio_matrix_.clear_midi_slots();
    if (cfg_.midi_slots.empty()) {
        audio_matrix_.add_midi_slot({0, MidiSlotType::kNetworkMidiTx, "Network MIDI TX (MIDI2)", "", -1, 0, true});
        audio_matrix_.add_midi_slot({1, MidiSlotType::kNetworkMidiRx, "Network MIDI RX (MIDI2)", "", -1, 0, true});
        const int daw_midi_id = audio_matrix_.add_midi_slot(
            {2, MidiSlotType::kVirtualDawMidi, "LanVid MIDI (DAW)", "LanVid MIDI", -1, 0, true});
        if (daw_midi_id >= 0) {
            audio_matrix_.set_midi_crosspoint(1, daw_midi_id, 0, false);  // Network MIDI RX -> LanVid MIDI (DAW)
            audio_matrix_.set_midi_crosspoint(daw_midi_id, 0, 0, false);  // LanVid MIDI (DAW) -> Network MIDI TX
        }
    } else {
        bool has_virtual_daw = false;
        for (const auto& ms : cfg_.midi_slots) {
            if (ms.type == MidiSlotType::kVirtualDawMidi) has_virtual_daw = true;
            audio_matrix_.add_midi_slot(ms);
        }
        if (!has_virtual_daw) {
            const int daw_midi_id = audio_matrix_.add_midi_slot(
                {-1, MidiSlotType::kVirtualDawMidi, "LanVid MIDI (DAW)", "LanVid MIDI", -1, 0, true});
            if (daw_midi_id >= 0) {
                for (const auto& s : audio_matrix_.midi_slots()) {
                    if (s.type == MidiSlotType::kNetworkMidiRx) {
                        audio_matrix_.set_midi_crosspoint(s.id, daw_midi_id, 0, false);
                    } else if (s.type == MidiSlotType::kNetworkMidiTx) {
                        audio_matrix_.set_midi_crosspoint(daw_midi_id, s.id, 0, false);
                    }
                }
            }
        }
        for (const auto& mc : cfg_.midi_crosspoints) {
            audio_matrix_.set_midi_crosspoint(mc.src_slot, mc.dst_slot, mc.channel_filter, mc.mute);
        }
    }

    audio_matrix_.set_network_io(
        [this](const float* const* planes, uint32_t ch, uint32_t frames) {
            if (monitor_server_.is_running()) {
                monitor_server_.process_and_send(planes, int(ch), frames);
            }
            if (sender_ && sender_running_ && !cfg_.midi_only &&
                matrix_has_net_tx_routes_.load(std::memory_order_relaxed)) {
                lv_sender_push_audio_multichannel(sender_, planes, uint16_t(ch), frames,
                                                  LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR, LV_MEDIA_TS_AUTO);
            }
        },
        [this](float* const* planes, uint32_t ch, uint32_t frames) {
            if (receiver_ && receiver_running_) {
                static thread_local std::vector<float> tmp;
                tmp.resize(size_t(ch) * frames);
                // Never hand out a stale block (the previous one would be replayed as a buzz).
                if (lv_receiver_pop_audio_multichannel(receiver_, tmp.data(), uint16_t(ch), frames,
                                                       LV_AUDIO_LAYOUT_PLANAR) < 0)
                    std::fill(tmp.begin(), tmp.end(), 0.0f);
                for (uint32_t c = 0; c < ch; ++c) {
                    std::copy(tmp.begin() + size_t(c) * frames, tmp.begin() + size_t(c + 1) * frames, planes[c]);
                }
            }
        });
}

void LanVidStudioApp::sync_matrix_to_config() {
    cfg_.matrix_buffer_size = audio_matrix_.buffer_size();
    cfg_.matrix_feedback_guard = audio_matrix_.feedback_guard();
    cfg_.matrix_slots = audio_matrix_.slots();
    cfg_.matrix_crosspoints = audio_matrix_.crosspoints();
    cfg_.midi_slots = audio_matrix_.midi_slots();
    cfg_.midi_crosspoints = audio_matrix_.midi_crosspoints();
}

void LanVidStudioApp::draw_segmented_vu(float dbfs, float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    constexpr int kSegs = 20;
    const float gap = 2.5f;
    const float seg_w = std::max(2.0f, (width - gap * float(kSegs - 1)) / float(kSegs));
    const float norm = std::clamp((dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
    const int lit = int(std::round(norm * float(kSegs)));
    for (int i = 0; i < kSegs; ++i) {
        const float x0 = p0.x + float(i) * (seg_w + gap);
        const float frac = float(i) / float(kSegs - 1);
        ImU32 col = IM_COL32(237, 233, 254, 255);  // #EDE9FE pastel lavender track
        if (i < lit) {
            // Smooth gradient: Sky Blue (#60A5FA) -> Violet (#A855F7) -> Soft Raspberry Pink (#EC4899)
            ImVec4 c = (frac < 0.65f) ? lerp_col(kColAccentBlue, kColAccentViol, frac / 0.65f)
                                      : lerp_col(kColAccentViol, kColPink, (frac - 0.65f) / 0.35f);
            col = ImGui::ColorConvertFloat4ToU32(c);
        }
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x0 + seg_w, p0.y + height), col, 4.0f);
    }
    ImGui::Dummy(ImVec2(width, height));
}

void LanVidStudioApp::draw_sidebar() {
    float sidebar_w = 204.0f;
    {
        const char* labels[] = {"З'єднання", "Маршрути", "Пристрої", "Налаштування"};
        for (const char* lbl : labels) {
            const float need = 16.0f + 34.0f + 12.0f + ImGui::CalcTextSize(lbl).x + 28.0f;
            sidebar_w = std::max(sidebar_w, need);
        }
    }

    const ImVec2 sb_p0 = ImGui::GetCursorScreenPos();
    const float sb_h = ImGui::GetContentRegionAvail().y;
    draw_soft_shadow(ImGui::GetWindowDrawList(), sb_p0, ImVec2(sb_p0.x + sidebar_w, sb_p0.y + sb_h), 16.0f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColSidebar);
    ImGui::PushStyleColor(ImGuiCol_Border, kColBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 14.0f));
    ImGui::BeginChild("##Sidebar", ImVec2(sidebar_w, 0), true, ImGuiWindowFlags_NoScrollbar);

#ifndef LANVID_VERSION_STRING
#define LANVID_VERSION_STRING "2.6.0"
#endif
    draw_gradient_icon_tile("##BrandTile", IconKind::kWaveform, 36.0f, -1, false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColAccentPurp, "LanVid");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(kColTextMain, "Studio");
    ImGui::TextColored(kColTextMuted, "v%s", LANVID_VERSION_STRING);
    ImGui::SameLine(0.0f, 6.0f);
    {
        const bool synced_live = sender_running_ || receiver_running_ || duplex_running_ || monitor_server_.is_running();
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        const char* badge = synced_live ? "Synced" : "Active";
        const ImVec2 bsz = ImGui::CalcTextSize(badge);
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        bdl->AddRectFilled(ImVec2(sp.x, sp.y - 1.0f), ImVec2(sp.x + bsz.x + 12.0f, sp.y + bsz.y + 3.0f),
                           IM_COL32(16, 185, 129, 42), 9.0f);
        bdl->AddRect(ImVec2(sp.x, sp.y - 1.0f), ImVec2(sp.x + bsz.x + 12.0f, sp.y + bsz.y + 3.0f),
                     IM_COL32(16, 185, 129, 175), 9.0f, 0, 1.0f);
        bdl->AddText(ImVec2(sp.x + 6.0f, sp.y + 1.0f), IM_COL32(16, 185, 129, 255), badge);
        ImGui::Dummy(ImVec2(bsz.x + 14.0f, bsz.y + 3.0f));
    }
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    struct NavItem {
        int id;
        IconKind icon;
        const char* title;
        const char* subtitle;
    };
    const NavItem nav[] = {
        {0, IconKind::kSend,       "З'єднання",    "Connect"},
        {1, IconKind::kMatrix,     "Маршрути",     "Patchbay"},
        {2, IconKind::kHeadphones, "Пристрої",     "Devices"},
        {3, IconKind::kGear,       "Налаштування", "Settings"},
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& item : nav) {
        const bool selected = (cfg_.active_tab == item.id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = 54.0f;
        if (ImGui::InvisibleButton(item.subtitle, ImVec2(w, h))) {
            if (cfg_.active_tab != item.id) {
                cfg_.active_tab = item.id;
                config_dirty_ = true;
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 p1(pos.x + w, pos.y + h);
        if (selected) {
            draw_soft_shadow(dl, pos, p1, 14.0f, 0.75f);
            draw_rounded_gradient_rect(dl, pos, p1, 14.0f, kColAccentInd, kColAccentViol, false);
            dl->AddRect(pos, p1, IM_COL32(255, 255, 255, 75), 14.0f, 0, 1.0f);
        } else if (hovered) {
            dl->AddRectFilled(pos, p1,
                              g_dark_theme ? IM_COL32(32, 39, 56, 220) : IM_COL32(237, 233, 254, 220), 14.0f);
            dl->AddRect(pos, p1, ImGui::ColorConvertFloat4ToU32(kColBorder), 14.0f, 0, 1.0f);
        }

        // Large soft gradient icon tile inside nav item
        const ImVec2 ic_p0(pos.x + 9.0f, pos.y + 10.0f);
        const ImVec2 ic_p1(ic_p0.x + 34.0f, ic_p0.y + 34.0f);
        if (selected) {
            dl->AddRectFilled(ic_p0, ic_p1, IM_COL32(255, 255, 255, 55), 10.0f);
        } else {
            draw_rounded_gradient_rect(dl, ic_p0, ic_p1, 10.0f, kColAccentViol, kColAccentInd, true);
        }
        const ImVec2 ic_c((ic_p0.x + ic_p1.x) * 0.5f, (ic_p0.y + ic_p1.y) * 0.5f);
        const float u = 9.0f;
        const ImU32 wc = IM_COL32(255, 255, 255, 245);
        if (item.icon == IconKind::kSend) {
            dl->AddLine(ImVec2(ic_c.x - u * 0.6f, ic_c.y + u * 0.4f), ImVec2(ic_c.x + u * 0.5f, ic_c.y - u * 0.5f), wc, 1.9f);
            dl->AddTriangleFilled(ImVec2(ic_c.x + u * 0.72f, ic_c.y - u * 0.62f),
                                  ImVec2(ic_c.x + u * 0.10f, ic_c.y - u * 0.58f),
                                  ImVec2(ic_c.x + u * 0.50f, ic_c.y - u * 0.02f), wc);
        } else if (item.icon == IconKind::kMatrix) {
            for (int r = -1; r <= 1; ++r)
                for (int k = -1; k <= 1; ++k)
                    dl->AddCircleFilled(ImVec2(ic_c.x + float(k) * 5.0f, ic_c.y + float(r) * 5.0f),
                                        (r == k) ? 2.3f : 1.3f, (r == k) ? wc : IM_COL32(255, 255, 255, 130));
        } else if (item.icon == IconKind::kHeadphones) {
            dl->PathClear();
            dl->PathArcTo(ImVec2(ic_c.x, ic_c.y + 0.5f), u * 0.78f, kPi, 2.0f * kPi, 16);
            dl->PathStroke(wc, 0, 1.9f);
            dl->AddRectFilled(ImVec2(ic_c.x - u * 0.92f, ic_c.y + 0.5f), ImVec2(ic_c.x - u * 0.48f, ic_c.y + u * 0.75f), wc, 2.5f);
            dl->AddRectFilled(ImVec2(ic_c.x + u * 0.48f, ic_c.y + 0.5f), ImVec2(ic_c.x + u * 0.92f, ic_c.y + u * 0.75f), wc, 2.5f);
        } else {
            dl->AddCircle(ic_c, u * 0.52f, wc, 16, 1.8f);
            for (int g = 0; g < 8; ++g) {
                const float ang = float(g) * (2.0f * kPi / 8.0f);
                dl->AddLine(ImVec2(ic_c.x + std::cos(ang) * u * 0.62f, ic_c.y + std::sin(ang) * u * 0.62f),
                            ImVec2(ic_c.x + std::cos(ang) * u * 0.98f, ic_c.y + std::sin(ang) * u * 0.98f), wc, 1.9f);
            }
        }

        const ImU32 title_col = selected ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(kColTextMain);
        const ImU32 sub_col = selected ? IM_COL32(238, 235, 255, 225) : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
        dl->AddText(ImVec2(pos.x + 52.0f, pos.y + 10.0f), title_col, item.title);
        dl->AddText(ImVec2(pos.x + 52.0f, pos.y + 29.0f), sub_col, item.subtitle);
        ImGui::Spacing();
    }

    // Bottom connection & latency card pinned to bottom of sidebar
    const float rem = ImGui::GetContentRegionAvail().y;
    if (rem > 92.0f) ImGui::Dummy(ImVec2(1.0f, rem - 88.0f));
    const ImVec2 bp0 = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x;
    const ImVec2 bp1(bp0.x + bw, bp0.y + 78.0f);
    dl->AddRectFilled(bp0, bp1, ImGui::ColorConvertFloat4ToU32(kColCard), 14.0f);
    dl->AddRect(bp0, bp1, ImGui::ColorConvertFloat4ToU32(kColBorder), 14.0f, 0, 1.0f);

    const bool any_active = sender_running_ || receiver_running_ || duplex_running_ || audio_matrix_.is_running() ||
                            monitor_server_.is_running();
    dl->AddCircleFilled(ImVec2(bp0.x + 14.0f, bp0.y + 19.0f), 4.5f,
                        ImGui::ColorConvertFloat4ToU32(any_active ? kColGreen : kColTextMuted));
    dl->AddText(ImVec2(bp0.x + 25.0f, bp0.y + 12.0f),
                ImGui::ColorConvertFloat4ToU32(any_active ? kColGreen : kColTextMuted),
                any_active ? "Connected" : "Очікування");
    char lat_txt[64];
    if (actual_buf_valid_) std::snprintf(lat_txt, sizeof(lat_txt), "LATENCY: %.1f ms", actual_buf_ms_);
    else std::snprintf(lat_txt, sizeof(lat_txt), "LATENCY: %d ms", cfg_.aoip_latency_ms);
    dl->AddText(ImVec2(bp0.x + 14.0f, bp0.y + 34.0f), ImGui::ColorConvertFloat4ToU32(kColTextMuted), lat_txt);
    const char* lat_mode = cfg_.adaptive_jitter ? "Авто для Wi-Fi" : "Фіксована (кабель)";
    dl->AddText(ImVec2(bp0.x + 14.0f, bp0.y + 52.0f),
                ImGui::ColorConvertFloat4ToU32(cfg_.adaptive_jitter ? kColGreen : kColAccentInd), lat_mode);
    ImGui::Dummy(ImVec2(bw, 78.0f));

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void LanVidStudioApp::apply_latency_mode() {
    cfg_.adaptive_jitter = cfg_.adaptive_jitter ? 1 : 0;
    // Live: no stream restart. On an idle receiver handle the choice is kept for the next start.
    if (receiver_) lv_receiver_set_adaptive_jitter(receiver_, cfg_.adaptive_jitter);
    if (duplex_) lv_duplex_set_adaptive_jitter(duplex_, cfg_.adaptive_jitter);
}

void LanVidStudioApp::poll_latency_telemetry(uint64_t now) {
    if (now - actual_buf_us_ < 100000ull) return;  // 10 Hz
    actual_buf_us_ = now;
    actual_buf_valid_ = false;
    midi_dejitter_valid_ = false;
    if (duplex_running_ && duplex_) {
        lv_duplex_get_stats(duplex_, &dpx_stats_);
        dpx_stats_us_ = now;
        actual_buf_ms_ = dpx_stats_.effective_latency_ms;
        actual_target_ms_ = dpx_stats_.effective_target_ms;
        actual_buf_valid_ = dpx_stats_.running && actual_buf_ms_ > 0.0;
        midi_dejitter_valid_ = (lv_duplex_get_midi_dejitter_stats(duplex_, &midi_dejitter_stats_) == LV_OK);
    } else if (receiver_running_ && receiver_) {
        actual_buf_ms_ = lv_receiver_get_effective_latency_ms(receiver_);
        actual_target_ms_ = rx_stats_.effective_target_ms;
        actual_buf_valid_ = actual_buf_ms_ > 0.0;
        midi_dejitter_valid_ = (lv_receiver_get_midi_dejitter_stats(receiver_, &midi_dejitter_stats_) == LV_OK);
    }
}

void LanVidStudioApp::draw_actual_buffer_line() {
    if (!actual_buf_valid_) {
        ImGui::TextColored(kColTextMuted, "ACTUAL BUFFER: -- ms");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("No AoIP stream is being played out yet.");
    } else {
        ImVec4 col = kColCyan;  // Fixed: held at the configured latency
        const char* state = "fixed";
        if (cfg_.adaptive_jitter) {
            const double lat = double(cfg_.aoip_latency_ms);
            const bool grown = double(actual_target_ms_) > lat + 0.5 ||
                               actual_buf_ms_ > lat + std::max(1.5, 0.05 * lat);
            col = grown ? kColAmber : kColGreen;
            state = grown ? "expanded (Wi-Fi burst)" : "nominal";
        }
        ImGui::TextColored(col, "ACTUAL BUFFER: %.1f ms", actual_buf_ms_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Measured jitter-buffer playout delay: %.1f ms (%s)\nTarget: %d ms%s", actual_buf_ms_,
                              state, cfg_.aoip_latency_ms,
                              cfg_.adaptive_jitter ? ", Adaptive range up to 200 ms" : ", Fixed");
    }

    // MIDI De-Jitter Playout Queue telemetry (synchronized with audio latency settings or MIDI Only)
    const bool midi_only_active = cfg_.midi_only || (midi_dejitter_valid_ && midi_dejitter_stats_.midi_only_mode != 0);
    const double midi_delay =
        midi_dejitter_valid_ ? midi_dejitter_stats_.playout_delay_ms : (midi_only_active ? 1.5 : double(cfg_.aoip_latency_ms));
    const bool midi_fixed = midi_dejitter_valid_ ? (midi_dejitter_stats_.fixed_mode != 0) : !cfg_.adaptive_jitter;
    const double midi_jit = midi_dejitter_valid_ ? midi_dejitter_stats_.absorbed_jitter_ms : 0.0;
    const uint64_t midi_late = midi_dejitter_valid_ ? midi_dejitter_stats_.late_events : 0;
    const ImVec4 mcol = midi_only_active ? kColGreen
                                         : (midi_late > 0 ? kColAmber : (midi_dejitter_valid_ ? kColAccentPurp : kColTextMuted));
    if (midi_only_active) {
        ImGui::TextColored(mcol, "MIDI Only • Ultra-Low Latency (~%.1f ms)", midi_delay);
    } else {
        ImGui::TextColored(mcol, "MIDI Playout: %.1f ms (%s • Synced with Audio)", midi_delay,
                           midi_fixed ? "Fixed" : "Adaptive");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Sample-Accurate 48 kHz MIDI De-Jitter Queue\n"
            "Playout Delay: %.1f ms (%s)\n"
            "Absorbed Wi-Fi Jitter: %.1f ms | Late packets: %llu | Queued: %u",
            midi_delay, midi_fixed ? "Fixed" : "Adaptive", midi_jit,
            static_cast<unsigned long long>(midi_late),
            midi_dejitter_valid_ ? midi_dejitter_stats_.queued_events : 0u);
    }
}

void LanVidStudioApp::draw_latency_mode_controls(const char* id, float combo_width) {
    ImGui::PushID(id);
    int mode = cfg_.adaptive_jitter ? 1 : 0;
    const char* items[] = {"Fixed (Ethernet)", "Adaptive (Wi-Fi)"};
    ImGui::SetNextItemWidth(combo_width);
    if (ImGui::Combo(combo_width < 0.0f ? "##LatencyMode" : "Latency Mode", &mode, items, 2)) {
        cfg_.adaptive_jitter = mode;
        apply_latency_mode();
        config_dirty_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fixed (Ethernet): the buffer is held exactly at the selected latency - wired LAN, live monitoring.\n"
                          "Adaptive (Wi-Fi): grows up to 200 ms on Wi-Fi jitter bursts and glides back via +/-0.05%%\n"
                          "micro-resampling. Switches live, without restarting the stream.");
    if (combo_width < 0.0f) ImGui::TextDisabled("Latency Mode");
    ImGui::PopID();
}

void LanVidStudioApp::draw_audio_bitrate_pills(const char* id) {
    ImGui::PushID(id);
    struct BitrateOpt {
        int kbps;
        const char* label;
        const char* tip;
    };
    const BitrateOpt opts[6] = {
        {0,   "Lossless", "Без стиснення (PCM 32-bit/24-bit, студійний еталон для Ethernet)"},
        {256, "256 kbps", "Прозора студійна якість (Opus CELT 2,5/5 мс, Fullband 48 кГц на канал)"},
        {192, "192 kbps", "Оптимально для Wi-Fi 6 (Opus CELT, Fullband 48 кГц на канал)"},
        {128, "128 kbps", "Збалансований потік (Opus CELT, Fullband 48 кГц, низьке навантаження на Wi-Fi)"},
        {96,  "96 kbps",  "Економний Wi-Fi (Opus CELT, 48 кГц, стабільна передача)"},
        {64,  "64 kbps",  "Максимальна економія трафіку (Opus CELT, 48 кГц, для слабкого Wi-Fi)"},
    };

    const float avail_x = ImGui::GetContentRegionAvail().x;
    float row_x = 0.0f;
    for (int i = 0; i < 6; ++i) {
        const float bw = (i == 0) ? 84.0f : 76.0f;
        if (i > 0 && row_x + bw + 6.0f <= avail_x) {
            ImGui::SameLine(0.0f, 6.0f);
        } else if (i > 0) {
            row_x = 0.0f;
        }
        const bool active = (cfg_.audio_bitrate_kbps == opts[i].kbps);
        if (status_pill_button(opts[i].label, active, ImVec2(bw, 28.0f))) {
            cfg_.audio_bitrate_kbps = opts[i].kbps;
            if (cfg_.audio_bitrate_kbps == 0) {
                if (cfg_.audio_codec > 3) cfg_.audio_codec = 0;
                if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
                if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
            } else {
                if (sender_) lv_sender_set_audio_bitrate(sender_, cfg_.audio_bitrate_kbps);
                if (duplex_running_) lv_duplex_set_audio_bitrate(duplex_, cfg_.audio_bitrate_kbps);
            }
            config_dirty_ = true;
            broadcast_control_sync();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opts[i].tip);
        row_x += bw + 6.0f;
    }

    const double est_mbps = estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec, cfg_.audio_bitrate_kbps);
    if (cfg_.audio_bitrate_kbps > 0) {
        ImGui::TextDisabled("Бітрейт: %d кбіт/с на канал  ·  Загальний потік (%d кан.): ~%.1f Мбіт/с",
                            cfg_.audio_bitrate_kbps, cfg_.aoip_channels, est_mbps);
    } else {
        ImGui::TextDisabled("Бітрейт: Lossless PCM  ·  Загальний потік (%d кан.): ~%.1f Мбіт/с",
                            cfg_.aoip_channels, est_mbps);
    }
    ImGui::PopID();
}

const char* LanVidStudioApp::video_source_warning() const {
    if (!sender_running_ || cfg_.audio_only) return nullptr;
    switch (tx_stats_.video_source_error) {
        case LV_VIDEO_SOURCE_SCREEN_UNAVAILABLE:
#if defined(__APPLE__) && TARGET_OS_OSX
            if (!CGPreflightScreenCaptureAccess())
                return "Надайте дозвіл на запис екрана в Системних параметрах macOS (Конфіденційність і безпека -> "
                       "Запис екрана -> LanVid Studio). Звук передається далі без відео.";
#endif
            return "Захоплення екрана недоступне на цьому Mac / платформі. Звук передається далі без відео - "
                   "оберіть джерело «Тестовий патерн» або «Файл».";
        case LV_VIDEO_SOURCE_CAMERA_UNAVAILABLE:
            return "Камера недоступна (немає дозволу або пристрій зайнятий). Звук передається далі без відео.";
        case LV_VIDEO_SOURCE_FILE_UNAVAILABLE:
            return "Не вдалося відкрити відеофайл. Звук передається далі без відео.";
        default:
            return nullptr;
    }
}

void LanVidStudioApp::apply_stream_mode() {
    if (cfg_.midi_only) cfg_.audio_only = true;
    if (sender_) lv_sender_set_midi_only(sender_, cfg_.midi_only ? 1 : 0);
    if (receiver_) lv_receiver_set_midi_only(receiver_, cfg_.midi_only ? 1 : 0);
    if (duplex_) lv_duplex_set_midi_only(duplex_, cfg_.midi_only ? 1 : 0);
}

void LanVidStudioApp::draw_stream_mode_selector(const char* id) {
    ImGui::PushID(id);
    const int cur_mode = cfg_.midi_only ? 2 : (cfg_.audio_only ? 1 : 0);
    int next_mode = cur_mode;
    if (status_pill_button("Відео + Аудіо + MIDI", cur_mode == 0, ImVec2(0, 26))) next_mode = 0;
    ImGui::SameLine();
    if (status_pill_button("Тільки Аудіо", cur_mode == 1, ImVec2(0, 26))) next_mode = 1;
    ImGui::SameLine();
    if (status_pill_button("Тільки MIDI", cur_mode == 2, ImVec2(0, 26))) next_mode = 2;

    if (next_mode != cur_mode) {
        const bool was_video = !cfg_.audio_only;
        cfg_.midi_only = (next_mode == 2);
        cfg_.audio_only = (next_mode >= 1);
        const bool now_video = !cfg_.audio_only;
        apply_stream_mode();
        config_dirty_ = true;
        broadcast_control_sync();
        if (was_video != now_video) {
            if (sender_running_) start_sender();
            if (receiver_running_) start_receiver();
        }
        toast_msg_ = (next_mode == 2)
                         ? "Режим: Тільки MIDI (Ultra-Low Latency ~1.5 ms, 0 аудіо/відео трафіку)"
                         : (next_mode == 1 ? "Режим: Тільки Аудіо + MIDI (відео вимкнено)"
                                           : "Режим: Відео + Аудіо + MIDI");
        toast_until_us_ = now_us() + 2800000ull;
    }
    ImGui::PopID();
}

void LanVidStudioApp::draw_mobile_monitor_section() {
    ImGui::TextColored(kColAccentPurp, "МОБІЛЬНИЙ МОНІТОРИНГ (SMARTPHONES)");
    bool srv_on = monitor_server_.is_running();
    if (studio_toggle("##MonSrvToggle", &srv_on, kColGreen)) {
        if (srv_on) {
            if (monitor_server_.start(kDefaultMonitorPort)) {
                const bool adv = monitor_mdns_.start(
                    "LanVid Studio (" + local_host_name() + ")", monitor_server_.bound_port(),
                    {{"role", "monitor"}, {"ver", LANVID_VERSION_STRING}, {"ch", std::to_string(cfg_.aoip_channels)}});
                toast_msg_ = "Сервер мобільного моніторингу запущено на UDP :" +
                             std::to_string(monitor_server_.bound_port()) +
                             (adv ? " (автопошук mDNS увімкнено)" : " (підключення за IP)");
            } else {
                toast_msg_ = "Не вдалося запустити сервер мобільного моніторингу";
            }
            toast_until_us_ = now_us() + 2800000ull;
        } else {
            monitor_mdns_.stop();
            monitor_server_.stop();
        }
    }
    ImGui::SameLine();
    if (monitor_server_.is_running()) {
        ImGui::TextColored(kColGreen, "Активний  %s:%u",
                           local_lan_ip_.c_str(), unsigned(monitor_server_.bound_port()));
    } else {
        ImGui::TextDisabled("Вимкнено (до 8 смартфонів, DSP на ПК)");
    }

    if (monitor_server_.is_running()) {
        const auto phones = monitor_server_.clients();
        ImGui::TextDisabled("  Адреса ПК: %s:%u  |  Пошук: %s + UDP :%u  |  RX: %llu  |  Disc: %llu",
                            local_lan_ip_.c_str(), unsigned(monitor_server_.bound_port()),
                            monitor_mdns_.running() ? "mDNS" : "Broadcast",
                            unsigned(monitor_server_.discovery_port()),
                            static_cast<unsigned long long>(monitor_server_.total_control_rx()),
                            static_cast<unsigned long long>(monitor_server_.discovery_replies()));
        if (studio_toggle("##MonWifiRedund", &cfg_.wifi_redundancy, kColAccentInd)) {
            monitor_server_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
            tally_intercom_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
            if (sender_) lv_sender_set_wifi_redundancy(sender_, cfg_.wifi_redundancy ? 1 : 0, 1500);
            if (duplex_running_) lv_duplex_set_wifi_redundancy(duplex_, cfg_.wifi_redundancy ? 1 : 0, 1500);
            config_dirty_ = true;
            toast_msg_ = cfg_.wifi_redundancy
                             ? "Wi-Fi Redundancy (1+1 Duplicate · +1.5 мс) увімкнено"
                             : "Wi-Fi Redundancy (1+1 Duplicate) вимкнено";
            toast_until_us_ = now_us() + 2600000ull;
        }
        ImGui::SameLine();
        ImGui::TextColored(cfg_.wifi_redundancy ? kColAccentInd : kColTextMain,
                           "Wi-Fi Redundancy (1+1 Duplicate)");
        if (monitor_server_.flow_packets_tx() > 0 || monitor_server_.dup_packets_tx() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("Flow: %llu · Dup: %llu",
                                static_cast<unsigned long long>(monitor_server_.flow_packets_tx()),
                                static_cast<unsigned long long>(monitor_server_.dup_packets_tx()));
        }
        if (phones.empty()) {
            ImGui::TextDisabled("  Очікування підключення смартфонів (%s:%u)...",
                                local_lan_ip_.c_str(), unsigned(monitor_server_.bound_port()));
        } else {
            for (const auto& ph : phones) {
                ImGui::PushID(9000 + ph.slot);
                ImGui::TextColored(ph.connected ? kColGreen : kColTextMuted, "●");
                ImGui::SameLine(0.0f, 5.0f);
                ImGui::TextColored(kColTextMain, "%s", ph.name.c_str());
                ImGui::SameLine();
                if (ph.bitrate_kbps == kMonitorBitrateLossless)
                    ImGui::TextDisabled("%u ms · Lossless", unsigned(ph.latency_ms));
                else
                    ImGui::TextDisabled("%u ms · %u kbps", unsigned(ph.latency_ms), unsigned(ph.bitrate_kbps));
                ImGui::SameLine();
                if (ImGui::SmallButton(ph.muted ? "UNMUTE" : "MUTE")) {
                    monitor_server_.set_client_mute(ph.slot, !ph.muted);
                }
                ImGui::PopID();
            }
        }
    }
}

void LanVidStudioApp::draw_cameras_tally_intercom_section() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kColAccentPurp, "STUDIO CAMERAS · TALLY & INTERCOM");

    bool ti_on = tally_intercom_.is_running();
    if (studio_toggle("##TallyIcToggle", &ti_on, kColGreen)) {
        if (ti_on) {
            if (tally_intercom_.start_studio(kDefaultTallyIntercomPort)) {
                toast_msg_ = "Tally & Duplex Intercom запущено на UDP :" +
                             std::to_string(tally_intercom_.bound_port());
            } else {
                toast_msg_ = "Не вдалося запустити Tally & Intercom сервер";
            }
            toast_until_us_ = now_us() + 2800000ull;
        } else {
            tally_intercom_.stop();
        }
    }
    ImGui::SameLine();
    if (tally_intercom_.is_running()) {
        ImGui::TextColored(kColGreen, "Активний (UDP :%u)", unsigned(tally_intercom_.bound_port()));
    } else {
        ImGui::TextDisabled("Вимкнено (Tally STBY/PVW/PGM + Talkback IFB)");
        return;
    }

    tally_intercom_.poll();

    // Hotkeys: F1..F8 = Direct Call Talkback to CAM 1..8; Space = Momentary All Call (when not typing)
    if (!ImGui::GetIO().WantTextInput) {
        const bool space_held = ImGui::IsKeyDown(ImGuiKey_Space);
        if (space_held != intercom_all_call_ && space_held) {
            tally_intercom_.studio_set_camera_intercom(0, true, true, true);
        }
        for (int k = 0; k < 8; ++k) {
            const ImGuiKey fkey = static_cast<ImGuiKey>(ImGuiKey_F1 + k);
            if (ImGui::IsKeyPressed(fkey, false)) {
                tally_intercom_.studio_set_camera_intercom(uint32_t(k + 1), true, true, intercom_all_call_);
            } else if (ImGui::IsKeyReleased(fkey)) {
                tally_intercom_.studio_set_camera_intercom(uint32_t(k + 1), false, true, intercom_all_call_);
            }
        }
    }

    // Director Intercom controls (ALL CALL + IFB Gain + Mic/Out selection)
    if (status_pill_button(intercom_all_call_ ? "ALL CALL (ON)" : "ALL CALL (Усім)", intercom_all_call_,
                           ImVec2(0, 26))) {
        intercom_all_call_ = !intercom_all_call_;
        tally_intercom_.studio_set_camera_intercom(0, intercom_all_call_, true, intercom_all_call_);
        tally_intercom_.set_intercom_state(intercom_all_call_, intercom_listen_muted_, intercom_ifb_gain_, 0.0f);
    }
    ImGui::SameLine();
    if (status_pill_button(intercom_listen_muted_ ? "IFB MUTE" : "IFB ON", !intercom_listen_muted_, ImVec2(0, 26))) {
        intercom_listen_muted_ = !intercom_listen_muted_;
        tally_intercom_.set_intercom_state(intercom_all_call_, intercom_listen_muted_, intercom_ifb_gain_, 0.0f);
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##IfbGain", &intercom_ifb_gain_, 0.0f, 1.5f, "IFB Гучність: %.0f%%")) {
        tally_intercom_.set_intercom_state(intercom_all_call_, intercom_listen_muted_, intercom_ifb_gain_, 0.0f);
    }
    ImGui::TextDisabled("Mic/Out: System Default · Гарячі клавіші: F1..F8 (Talk), Space (All Call)");

    // Connected cameras list with 1-click Tally (STBY / PVW / PGM), REC, TALK, LISTEN
    const auto cams = tally_intercom_.studio_cameras();
    if (cams.empty()) {
        ImGui::TextDisabled("  Очікування LanVid Camera (%s:%u)...",
                            local_lan_ip_.c_str(), unsigned(tally_intercom_.bound_port()));
    } else {
        for (const auto& cam : cams) {
            ImGui::PushID(int(9500 + cam.camera_id));
            const ImVec4 tcol = (cam.tally_state == kTallyProgram)
                                    ? kColRed
                                    : (cam.tally_state == kTallyPreview ? kColGreen : kColTextMuted);
            ImGui::TextColored(tcol, "●");
            ImGui::SameLine(0.0f, 5.0f);
            ImGui::TextColored(kColTextMain, "%s", cam.label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%u%% · %d°C", unsigned(cam.battery_pct), int(cam.temp_c));
            if (cam.operator_speaking) {
                ImGui::SameLine();
                ImGui::TextColored(kColGreen, "[VOICE]");
            }

            if (status_pill_button("STBY", cam.tally_state == kTallyStandby, ImVec2(0, 22))) {
                tally_intercom_.studio_set_camera_tally(cam.camera_id, kTallyStandby, cam.label, cam.rec_trigger);
            }
            ImGui::SameLine(0.0f, 4.0f);
            if (status_pill_button("PVW", cam.tally_state == kTallyPreview, ImVec2(0, 22))) {
                tally_intercom_.studio_set_camera_tally(cam.camera_id, kTallyPreview, cam.label, cam.rec_trigger);
            }
            ImGui::SameLine(0.0f, 4.0f);
            if (status_pill_button("PGM", cam.tally_state == kTallyProgram, ImVec2(0, 22))) {
                tally_intercom_.studio_set_camera_tally(cam.camera_id, kTallyProgram, cam.label, cam.rec_trigger);
            }
            ImGui::SameLine(0.0f, 6.0f);
            if (status_pill_button(cam.rec_trigger ? "REC ●" : "REC", cam.rec_trigger, ImVec2(0, 22))) {
                tally_intercom_.studio_set_camera_tally(cam.camera_id, cam.tally_state, cam.label, !cam.rec_trigger);
            }
            ImGui::SameLine(0.0f, 6.0f);
            if (status_pill_button("TALK", cam.talk_to_cam, ImVec2(0, 22))) {
                tally_intercom_.studio_set_camera_intercom(cam.camera_id, !cam.talk_to_cam, cam.listen_from_cam,
                                                           intercom_all_call_);
            }
            ImGui::PopID();
        }
    }
}

void LanVidStudioApp::draw_studio_config_panel() {
    begin_glass_card("##StudioConfig", ImVec2(0, 0));

    // Header with gradient Gear icon tile
    draw_gradient_icon_tile("##CfgGearTile", IconKind::kGear, 32.0f, -1, false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColTextMain, "STUDIO CONFIG");
    ImGui::TextDisabled("Network · Audio Bus · Telemetry");
    ImGui::EndGroup();
    ImGui::Separator();

    // ---- NETWORK ----
    ImGui::TextColored(kColAccentPurp, "NETWORK");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##CfgIP", ip_buf_, sizeof(ip_buf_))) config_dirty_ = true;
    ImGui::TextDisabled("IP Address");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##CfgPort", &cfg_.dest_port, 0, 0)) {
        cfg_.dest_port = std::clamp(cfg_.dest_port, 1024, 65535);
        cfg_.listen_port = cfg_.dest_port;
        config_dirty_ = true;
    }
    ImGui::TextDisabled("UDP Port (TX / RX)");

    const bool running = audio_matrix_.is_running();
    const float btn_avail = ImGui::GetContentRegionAvail().x;
    if (!running) {
        if (draw_gradient_icon_tile("##PlayMxTile", IconKind::kPlay, 34.0f, -1, true)) {
            stop_vdev_tx();
            vdev_rx_.stop();
            audio_matrix_.start();
            toast_msg_ = "Audio Matrix active (48 kHz, buf " + std::to_string(audio_matrix_.buffer_size()) + ")";
            toast_until_us_ = now_us() + 3000000ull;
        }
        ImGui::SameLine();
        if (gradient_pill_button("Start Audio Matrix", ImVec2(std::max(110.0f, btn_avail - 82.0f), 34.0f),
                                 kColAccentBlue, kColAccentPurp)) {
            stop_vdev_tx();
            vdev_rx_.stop();
            audio_matrix_.start();
            toast_msg_ = "Audio Matrix active (48 kHz, buf " + std::to_string(audio_matrix_.buffer_size()) + ")";
            toast_until_us_ = now_us() + 3000000ull;
        }
    } else {
        if (draw_gradient_icon_tile("##StopMxTile", IconKind::kStop, 34.0f, -1, true)) {
            audio_matrix_.stop();
        }
        ImGui::SameLine();
        if (gradient_pill_button("Stop Audio Matrix", ImVec2(std::max(110.0f, btn_avail - 82.0f), 34.0f),
                                 kColPink, kColRed)) {
            audio_matrix_.stop();
        }
    }
    ImGui::SameLine();
    if (draw_gradient_icon_tile("##PanicCfgTile", IconKind::kPanic, 34.0f, -1, true)) {
        audio_matrix_.send_midi_panic(-1);
        toast_msg_ = "MIDI PANIC: CC 123 (All Notes Off) sent";
        toast_until_us_ = now_us() + 2500000ull;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MIDI PANIC (All Notes Off)");

    ImGui::Spacing();
    ImGui::Separator();

    // Virtual DAW MIDI Port status ("LanVid MIDI")
    for (const auto& ms : audio_matrix_.midi_slots()) {
        if (ms.type != MidiSlotType::kVirtualDawMidi) continue;
        const auto mst = audio_matrix_.midi_slot_status(ms.id);
        ImGui::Spacing();
        ImGui::TextColored(mst.connected ? kColGreen : kColAmber, "●");
        ImGui::SameLine(0.0f, 5.0f);
        ImGui::TextWrapped("Віртуальний порт для DAW: %s",
                           mst.status_text.empty() ? "LanVid MIDI" : mst.status_text.c_str());
        break;
    }

    // ---- AUDIO BUS ----
    ImGui::TextColored(kColAccentPurp, "AUDIO BUS");
    int bus_idx = 4;
    for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
    const char* bus_items[] = {"2 Channels", "8 Channels", "16 Channels", "32 Channels", "64 Channels"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##CfgBusCh", &bus_idx, bus_items, 5)) {
        cfg_.aoip_channels = kBusChoices[bus_idx];
        config_dirty_ = true;
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##CfgLat", &cfg_.aoip_latency_ms, 2, 2000, "LATENCY: %d ms", ImGuiSliderFlags_Logarithmic)) {
        if (sender_) lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
        if (receiver_) lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
        if (duplex_running_) lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    if (status_pill_button("5 ms", cfg_.aoip_latency_ms == 5, ImVec2(0, 24))) { cfg_.aoip_latency_ms = 5; config_dirty_ = true; }
    ImGui::SameLine();
    if (status_pill_button("10 ms", cfg_.aoip_latency_ms == 10, ImVec2(0, 24))) { cfg_.aoip_latency_ms = 10; config_dirty_ = true; }
    ImGui::SameLine();
    if (status_pill_button("20 ms", cfg_.aoip_latency_ms == 20, ImVec2(0, 24))) { cfg_.aoip_latency_ms = 20; config_dirty_ = true; }

    draw_latency_mode_controls("CfgLatMode", -1.0f);
    draw_actual_buffer_line();

    int buf_idx = (cfg_.matrix_buffer_size <= 128) ? 0 : (cfg_.matrix_buffer_size <= 256 ? 1 : 2);
    const char* buf_items[] = {"Buffer: 128 (2.7 ms)", "Buffer: 256 (5.3 ms)", "Buffer: 512 (10.7 ms)"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##CfgBuf", &buf_idx, buf_items, 3)) {
        cfg_.matrix_buffer_size = (buf_idx == 0) ? 128 : (buf_idx == 1 ? 256 : 512);
        audio_matrix_.set_buffer_size(cfg_.matrix_buffer_size);
        config_dirty_ = true;
    }

    ImGui::TextDisabled("Бітрейт аудіо на канал:");
    draw_audio_bitrate_pills("CfgBitrate");

    if (ImGui::Checkbox("Feedback Loop Guard", &cfg_.matrix_feedback_guard)) {
        audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);
        config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- STREAM MODE ----
    ImGui::TextColored(kColAccentPurp, "STREAM MODE");
    draw_stream_mode_selector("CfgStreamMode");
    ImGui::TextDisabled(cfg_.midi_only
                            ? "MIDI Only: ~1.5 ms Ultra-Low Latency (0 audio/video)"
                            : (cfg_.audio_only ? "Audio Only: video disabled (pure AoIP + MIDI)"
                                               : "Video + Audio + MIDI streaming enabled"));

    ImGui::Spacing();
    ImGui::Separator();

    // ---- PERSONAL MOBILE MONITOR SERVER ----
    draw_mobile_monitor_section();

    // ---- STUDIO CAMERAS: TALLY & DUPLEX INTERCOM ----
    draw_cameras_tally_intercom_section();

    ImGui::Spacing();
    ImGui::Separator();

    // ---- LIVE TELEMETRY (3 Donut Ring Gauges on White Cards) ----
    ImGui::TextColored(kColAccentPurp, "LIVE TELEMETRY");
    const double total_mbps = tx_stats_.tx_mbps + rx_stats_.rx_mbps + dpx_tx_mbps_ + dpx_rx_mbps_;
    const double loss_pct = std::clamp(receiver_running_ ? rx_stats_.loss_pct : tx_stats_.rx_loss_pct, 0.0, 100.0);
    const double health_pct = std::clamp(100.0 - loss_pct, 0.0, 100.0);
    const double est_cap_mbps = std::max(10.0, estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec));
    const float bw_frac = float(std::clamp(total_mbps / std::max(100.0, est_cap_mbps), 0.0, 1.0));
    const double buf_ms = actual_buf_valid_ ? actual_buf_ms_
                                            : (receiver_running_ ? rx_stats_.audio_buffer_ms
                                                                 : double(cfg_.matrix_buffer_size) * 1000.0 / 48000.0);
    const double buf_denom = std::max(2.0, cfg_.adaptive_jitter ? std::max<double>(cfg_.aoip_latency_ms, actual_target_ms_)
                                                                : double(cfg_.aoip_latency_ms));
    const float buf_frac = float(std::clamp(buf_ms / buf_denom, 0.0, 1.0));
    uint64_t total_xruns = 0;
    for (const auto& s : audio_matrix_.slots()) total_xruns += audio_matrix_.slot_status(s.id).xruns;

    const float tel_avail = ImGui::GetContentRegionAvail().x;
    const float gap = 8.0f;
    const float card_w = std::max(72.0f, (tel_avail - gap * 2.0f) / 3.0f);
    const float card_h = 104.0f;

    char bw_val[24], bw_sub[24];
    std::snprintf(bw_val, sizeof(bw_val), "%.0f%%", bw_frac * 100.0f);
    std::snprintf(bw_sub, sizeof(bw_sub), "%.1fM", total_mbps);
    draw_ring_gauge("Bandwidth", bw_val, bw_sub, std::max(0.06f, bw_frac), card_w, card_h, bw_frac > 0.85f, false);

    ImGui::SameLine(0.0f, gap);
    char buf_val[24], buf_sub[24];
    std::snprintf(buf_val, sizeof(buf_val), "%.1f", buf_ms);
    std::snprintf(buf_sub, sizeof(buf_sub), "ms");
    draw_ring_gauge("Buffer Fill", buf_val, buf_sub, std::max(0.08f, buf_frac), card_w, card_h, buf_frac > 0.85f, false);

    ImGui::SameLine(0.0f, gap);
    char hlth_val[24];
    std::snprintf(hlth_val, sizeof(hlth_val), "%.0f%%", health_pct);
    draw_ring_gauge("Health", hlth_val, "OK", float(health_pct / 100.0), card_w, card_h, loss_pct > 1.0, true);

    ImGui::Spacing();
    ImGui::TextDisabled("Underruns: %llu  ·  Crosspoints: %zu active",
                        (unsigned long long)total_xruns, audio_matrix_.crosspoints().size());

    end_glass_card();
}

void LanVidStudioApp::draw_audio_matrix_tab() {
    auto slots = audio_matrix_.slots();
    auto find_slot_by_type = [&](MatrixSlotType t, MatrixSlotRole role_hint = MatrixSlotRole::kDuplex,
                                 bool check_role = false) -> int {
        for (const auto& s : slots) {
            if (s.type == t && (!check_role || s.role == role_hint)) return s.id;
        }
        return -1;
    };

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float col1_w = std::max(270.0f, total_w * 0.30f);
    const float col2_w = std::max(360.0f, total_w * 0.45f);

    // ==================== COLUMN 1: DEVICES & SLOTS (~30%) ====================
    begin_glass_card("##MatrixDevices", ImVec2(col1_w, 0));
    draw_gradient_icon_tile("##DevSecTile", IconKind::kWaveform, 30.0f, int(slots.size()), false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColTextMain, "DEVICES & SLOTS");
    ImGui::TextDisabled("%zu / %d Audio  ·  %zu MIDI", slots.size(), kMatrixMaxSlots, audio_matrix_.midi_slots().size());
    ImGui::EndGroup();
    ImGui::Separator();

    if (gradient_pill_button("+ Audio", ImVec2(col1_w * 0.30f, 30.0f), kColAccentBlue, kColAccentInd)) ImGui::OpenPopup("##AddAudioDev");
    ImGui::SameLine();
    if (gradient_pill_button("+ Add MIDI Device", ImVec2(col1_w * 0.44f, 30.0f), kColAccentInd, kColAccentPurp)) ImGui::OpenPopup("##AddMidiDev");
    ImGui::SameLine();
    if (status_pill_button("Ref", false, ImVec2(-1, 30.0f))) refresh_devices();

    if (ImGui::BeginPopup("##AddMidiDev")) {
        ImGui::TextColored(kColCyan, "MIDI Inputs (Keyboards / E-Drums / Controllers)");
        if (midi_in_devs_.empty()) ImGui::TextDisabled("  No OS MIDI inputs found");
        for (const auto& m : midi_in_devs_) {
            if (ImGui::Selectable((m.name + " [MIDI IN]").c_str())) {
                audio_matrix_.add_midi_slot({-1, MidiSlotType::kMidiInput, m.name, m.name, m.index, 0, true});
                config_dirty_ = true;
            }
        }
        ImGui::Separator();
        ImGui::TextColored(kColTeal, "MIDI Outputs (DAW / Synths)");
        if (midi_out_devs_.empty()) ImGui::TextDisabled("  No OS MIDI outputs found");
        for (const auto& m : midi_out_devs_) {
            if (ImGui::Selectable((m.name + " [MIDI OUT]").c_str())) {
                audio_matrix_.add_midi_slot({-1, MidiSlotType::kMidiOutput, m.name, m.name, m.index, 0, true});
                config_dirty_ = true;
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##AddAudioDev")) {
#if defined(__APPLE__)
        auto add_sys_slot = [this](const SystemAudioDeviceEntry* d, MatrixSlotRole role) {
            MatrixSlotConfig s{};
            s.id = -1;
            s.type = MatrixSlotType::kSystemAudio;
            s.role = role;
            s.name = d ? d->name : (role == MatrixSlotRole::kOutput ? "Default Output (CoreAudio)" : "Default Input (CoreAudio)");
            s.device_id = d ? d->uid : "";
            s.device_index = -1;
            s.channels = d ? std::clamp(d->channels, 1, 64) : 2;
            s.buffer_size = cfg_.matrix_buffer_size;
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        };
        ImGui::TextColored(kColCyan, "CoreAudio Outputs (Speakers / Headphones / USB)");
        if (ImGui::Selectable("Default Output (CoreAudio)")) add_sys_slot(nullptr, MatrixSlotRole::kOutput);
        for (const auto& d : sys_out_devs_) {
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch, %u Hz)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [default]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kOutput);
        }
        ImGui::Separator();
        ImGui::TextColored(kColTeal, "CoreAudio Inputs (Mic / Line-In / USB)");
        if (ImGui::Selectable("Default Input (CoreAudio)")) add_sys_slot(nullptr, MatrixSlotRole::kInput);
        for (const auto& d : sys_in_devs_) {
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch, %u Hz)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [default]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kInput);
        }
#else
        ImGui::TextColored(kColCyan, "WASAPI Outputs (Headphones / Interfaces)");
        if (ImGui::Selectable("Default PC Output (WASAPI)")) {
            MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                               "PC Headphones / Output", "", -1, 2, cfg_.matrix_buffer_size, true};
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : loop_devs_) {
            if (ImGui::Selectable((d.name + " [OUT]").c_str())) {
                MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                                   d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true};
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        ImGui::Separator();
        ImGui::TextColored(kColTeal, "WASAPI Inputs (Mic / Line-In)");
        if (ImGui::Selectable("Default Mic / Line-In (WASAPI)")) {
            MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                               "System Mic / Line-In", "", -1, 2, cfg_.matrix_buffer_size, true};
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : mic_devs_) {
            if (ImGui::Selectable((d.name + " [IN]").c_str())) {
                MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                                   d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true};
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        if (!asio_devs_.empty()) {
            ImGui::Separator();
            ImGui::TextColored(kColAmber, "External ASIO Drivers (Duplex)");
            for (const auto& a : asio_devs_) {
                if (ImGui::Selectable((a.name + " [ASIO]").c_str())) {
                    MatrixSlotConfig s{-1, MatrixSlotType::kExternalAsio, MatrixSlotRole::kDuplex,
                                       a.name, a.name, -1, 16, cfg_.matrix_buffer_size, true};
                    audio_matrix_.add_slot(s);
                    config_dirty_ = true;
                }
            }
        }
#endif
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    slots = audio_matrix_.slots();
    for (auto s : slots) {
        ImGui::PushID(s.id);
        const auto st = audio_matrix_.slot_status(s.id);
        begin_glass_card("##SlotCard", ImVec2(0, 92.0f), true, ImGuiWindowFlags_NoScrollbar);

        const char* badge = (s.type == MatrixSlotType::kVirtualBus)
                                ? "ASIO SHM"
                                : (s.type == MatrixSlotType::kNetworkAoip
                                       ? "AoIP"
                                       : (s.type == MatrixSlotType::kExternalAsio ? "EXT ASIO" : "WASAPI"));
        const IconKind ik = (s.type == MatrixSlotType::kSystemAudio) ? IconKind::kHeadphones : IconKind::kWaveform;
        draw_gradient_icon_tile("##SlotIcon", ik, 36.0f, s.channels, false);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(kColTextMain, "%s", s.name.c_str());
        ImGui::TextColored(st.connected ? kColGreen : kColTextMuted, "%s",
                           s.active ? (st.connected ? "ACTIVE" : "READY") : "OFF");
        ImGui::SameLine();
        ImGui::TextDisabled("| %s | %u Hz", badge, st.device_sample_rate);
        ImGui::EndGroup();

        ImGui::SameLine(std::max(140.0f, ImGui::GetWindowContentRegionMax().x - 58.0f));
        bool act = s.active;
        if (studio_toggle("##SlotAct", &act, kColAccentInd)) {
            s.active = act;
            audio_matrix_.update_slot(s.id, s);
            config_dirty_ = true;
        }
        if (s.type != MatrixSlotType::kVirtualBus && s.type != MatrixSlotType::kNetworkAoip) {
            ImGui::SameLine(std::max(100.0f, ImGui::GetWindowContentRegionMax().x - 84.0f));
            if (ImGui::SmallButton("x")) {
                audio_matrix_.remove_slot(s.id);
                config_dirty_ = true;
                end_glass_card();
                ImGui::PopID();
                break;
            }
        }

        const float vu_w = ImGui::GetContentRegionAvail().x;
        const float vu_db = (s.role == MatrixSlotRole::kOutput) ? st.peak_out_dbfs
                                                                : std::max(st.peak_in_dbfs, st.peak_out_dbfs);
        draw_segmented_vu(vu_db, vu_w, 9.0f);

        end_glass_card();
        ImGui::PopID();
    }

    // ---- MIDI DEVICES SECTION IN COLUMN 1 ----
    const auto mslots = audio_matrix_.midi_slots();
    if (!mslots.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kColAccentPurp, "MIDI DEVICES (%zu / %d)", mslots.size(), kMatrixMaxMidiSlots);
        ImGui::SameLine(std::max(140.0f, col1_w - 110.0f));
        if (gradient_pill_button("PANIC ALL", ImVec2(88.0f, 24.0f), kColPink, kColRed)) {
            audio_matrix_.send_midi_panic(-1);
            toast_msg_ = "MIDI PANIC: CC 123 (All Notes Off) sent to all 16 channels";
            toast_until_us_ = now_us() + 2500000ull;
        }
        const uint64_t now_t = now_us();
        for (auto ms : mslots) {
            ImGui::PushID(2000 + ms.id);
            const auto mst = audio_matrix_.midi_slot_status(ms.id);
            const bool blinking = (mst.last_activity_us > 0 && now_t - mst.last_activity_us < 150000ull);
            begin_glass_card("##MidiCard", ImVec2(0, 68.0f), true, ImGuiWindowFlags_NoScrollbar);

            const int ev_badge = (mst.events_count > 0) ? int(std::min<uint64_t>(mst.events_count, 99)) : -1;
            draw_gradient_icon_tile("##MidiIcon", IconKind::kMidiPlug, 34.0f, ev_badge, false);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(blinking ? kColGreen : kColTextMuted, "●");
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextColored(kColTextMain, "%s", ms.name.c_str());

            const char* mtag = (ms.type == MidiSlotType::kMidiInput)
                                   ? "MIDI IN"
                                   : (ms.type == MidiSlotType::kMidiOutput
                                          ? "MIDI OUT"
                                          : (ms.type == MidiSlotType::kVirtualDawMidi
                                                 ? "DAW VIRT"
                                                 : (ms.type == MidiSlotType::kNetworkMidiTx ? "NET TX" : "NET RX")));
            ImGui::TextColored(blinking ? kColAccentPurp : kColTextMuted, "%llu ev · %s",
                               (unsigned long long)mst.events_count, mtag);
            if (ms.type == MidiSlotType::kMidiOutput || ms.type == MidiSlotType::kNetworkMidiTx ||
                ms.type == MidiSlotType::kVirtualDawMidi) {
                ImGui::SameLine();
                if (ImGui::SmallButton("PANIC")) audio_matrix_.send_midi_panic(ms.id);
            }
            if (ms.type == MidiSlotType::kMidiInput || ms.type == MidiSlotType::kMidiOutput) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    audio_matrix_.remove_midi_slot(ms.id);
                    config_dirty_ = true;
                    ImGui::EndGroup();
                    end_glass_card();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::EndGroup();

            ImGui::SameLine(std::max(140.0f, ImGui::GetWindowContentRegionMax().x - 56.0f));
            bool act = ms.active;
            if (studio_toggle("##MidiAct", &act, kColAccentPurp)) {
                ms.active = act;
                audio_matrix_.update_midi_slot(ms.id, ms);
                config_dirty_ = true;
            }

            end_glass_card();
            ImGui::PopID();
        }
    }
    end_glass_card();

    // ==================== COLUMN 2: PATCHBAY MATRIX GRID (~45%) ====================
    ImGui::SameLine();
    begin_glass_card("##MatrixGrid", ImVec2(col2_w, 0));
    draw_gradient_icon_tile("##MxGridTile", matrix_patch_mode_ == 0 ? IconKind::kMatrix : IconKind::kMidiPlug,
                            30.0f, int(audio_matrix_.crosspoints().size()), false);
    ImGui::SameLine();
    ImGui::TextColored(kColTextMain, matrix_patch_mode_ == 0 ? "AUDIO PATCHBAY GRID" : "MIDI PATCHBAY GRID");
    ImGui::SameLine(std::max(190.0f, col2_w - 220.0f));
    if (status_pill_button("Audio Patch", matrix_patch_mode_ == 0, ImVec2(0, 28))) matrix_patch_mode_ = 0;
    ImGui::SameLine();
    if (status_pill_button("MIDI Patch", matrix_patch_mode_ == 1, ImVec2(0, 28))) matrix_patch_mode_ = 1;
    ImGui::Separator();

    if (matrix_patch_mode_ == 1) {
        // ---- MIDI ROUTING MATRIX (many-to-1 merge & 1-to-many split with channel filter) ----
        const char* ch_filters[17] = {"All Ch", "Ch 1",  "Ch 2",  "Ch 3",  "Ch 4",  "Ch 5",  "Ch 6",  "Ch 7", "Ch 8",
                                      "Ch 9",   "Ch 10", "Ch 11", "Ch 12", "Ch 13", "Ch 14", "Ch 15", "Ch 16"};
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Channel Filter", &midi_channel_filter_, ch_filters, 17);
        ImGui::SameLine();
        if (status_pill_button("Clear MIDI Routes", false, ImVec2(0, 26))) {
            audio_matrix_.clear_midi_crosspoints();
            config_dirty_ = true;
        }
        ImGui::Spacing();

        std::vector<MidiSlotConfig> srcs, dsts;
        for (const auto& s : audio_matrix_.midi_slots()) {
            if (s.type == MidiSlotType::kMidiInput || s.type == MidiSlotType::kNetworkMidiRx ||
                s.type == MidiSlotType::kVirtualDawMidi)
                srcs.push_back(s);
            if (s.type == MidiSlotType::kMidiOutput || s.type == MidiSlotType::kNetworkMidiTx ||
                s.type == MidiSlotType::kVirtualDawMidi)
                dsts.push_back(s);
        }
        if (srcs.empty() || dsts.empty()) {
            ImGui::TextDisabled("Add MIDI Input / Output devices on the left to patch MIDI routes.");
        } else if (ImGui::BeginTable("##MidiPatchTbl", int(dsts.size()) + 1,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("MIDI Source \\ Dest");
            for (const auto& d : dsts) ImGui::TableSetupColumn(d.name.c_str());
            ImGui::TableHeadersRow();
            for (const auto& s : srcs) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", s.name.c_str());
                for (size_t j = 0; j < dsts.size(); ++j) {
                    ImGui::TableSetColumnIndex(int(j) + 1);
                    if (s.id == dsts[j].id) {
                        ImGui::TextDisabled("  —");
                        continue;
                    }
                    MidiCrosspoint mcp{};
                    const bool linked = audio_matrix_.has_midi_crosspoint(s.id, dsts[j].id, &mcp);
                    ImGui::PushID(s.id * 32 + dsts[j].id);
                    const ImVec4 bcol = linked ? (mcp.mute ? kColPink : kColAccentNode) : kColCardSoft;
                    ImGui::PushStyleColor(ImGuiCol_Button, bcol);
                    ImGui::PushStyleColor(ImGuiCol_Text, linked ? kColWhite : kColTextMuted);
                    char blbl[16];
                    if (!linked) std::snprintf(blbl, sizeof(blbl), "·");
                    else if (mcp.mute) std::snprintf(blbl, sizeof(blbl), "M");
                    else if (mcp.channel_filter == 0) std::snprintf(blbl, sizeof(blbl), "ALL");
                    else std::snprintf(blbl, sizeof(blbl), "C%d", mcp.channel_filter);
                    if (ImGui::SmallButton(blbl)) {
                        if (linked) audio_matrix_.remove_midi_crosspoint(s.id, dsts[j].id);
                        else audio_matrix_.set_midi_crosspoint(s.id, dsts[j].id, midi_channel_filter_, false);
                        config_dirty_ = true;
                    }
                    if (linked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        audio_matrix_.set_midi_crosspoint(s.id, dsts[j].id, mcp.channel_filter, !mcp.mute);
                        config_dirty_ = true;
                    }
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Many-to-1 Merge & 1-to-Many Split | LMB: Patch/Unpatch | RMB: Mute");
        end_glass_card();
        ImGui::SameLine();
        draw_studio_config_panel();
        return;
    }

    const int net_id = find_slot_by_type(MatrixSlotType::kNetworkAoip);
    const int daw_id = find_slot_by_type(MatrixSlotType::kVirtualBus);
    const int mon_id = find_slot_by_type(MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, true);

    auto preset_pill = [&](int idx, const char* label) {
        if (status_pill_button(label, matrix_active_preset_ == idx, ImVec2(0, 28))) {
            matrix_active_preset_ = idx;
            audio_matrix_.apply_preset(idx, net_id, daw_id, mon_id);
            config_dirty_ = true;
        }
    };
    preset_pill(0, "Mac/Net -> DAW + HP");
    ImGui::SameLine();
    preset_pill(1, "Full-Duplex 64x64");
    ImGui::SameLine();
    preset_pill(2, "Direct Patch 1:1");
    ImGui::SameLine();
    if (status_pill_button("Clear", false, ImVec2(0, 28))) {
        matrix_active_preset_ = -1;
        audio_matrix_.clear_crosspoints();
        config_dirty_ = true;
    }

    ImGui::Spacing();
    slots = audio_matrix_.slots();
    MatrixSlotConfig src_cfg{}, dst_cfg{};
    bool has_src = false, has_dst = false;
    for (const auto& s : slots) {
        if (s.id == matrix_src_slot_) { src_cfg = s; has_src = true; }
        if (s.id == matrix_dst_slot_) { dst_cfg = s; has_dst = true; }
    }
    if (!has_src && !slots.empty()) { src_cfg = slots.front(); matrix_src_slot_ = src_cfg.id; has_src = true; }
    if (!has_dst && !slots.empty()) { dst_cfg = slots.back(); matrix_dst_slot_ = dst_cfg.id; has_dst = true; }

    ImGui::SetNextItemWidth(165.0f);
    if (ImGui::BeginCombo("##MxSrc", has_src ? src_cfg.name.c_str() : "Source")) {
        for (const auto& s : slots) {
            if (s.role == MatrixSlotRole::kOutput) continue;
            if (ImGui::Selectable(s.name.c_str(), s.id == matrix_src_slot_)) {
                matrix_src_slot_ = s.id;
                matrix_src_page_ = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(kColAccentPurp, "->");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(165.0f);
    if (ImGui::BeginCombo("##MxDst", has_dst ? dst_cfg.name.c_str() : "Destination")) {
        for (const auto& s : slots) {
            if (s.role == MatrixSlotRole::kInput) continue;
            if (ImGui::Selectable(s.name.c_str(), s.id == matrix_dst_slot_)) {
                matrix_dst_slot_ = s.id;
                matrix_dst_page_ = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(86.0f);
    ImGui::SliderFloat("##MxGain", &matrix_new_gain_db_, -24.0f, 6.0f, "%.0fdB");

    const char* page_lbl[] = {"1-16", "17-32", "33-48", "49-64"};
    if (has_src && src_cfg.channels > 16) {
        ImGui::SetNextItemWidth(76.0f);
        ImGui::Combo("Src##SrcPg", &matrix_src_page_, page_lbl, std::min(4, (src_cfg.channels + 15) / 16));
        ImGui::SameLine();
    } else {
        matrix_src_page_ = 0;
    }
    if (has_dst && dst_cfg.channels > 16) {
        ImGui::SetNextItemWidth(76.0f);
        ImGui::Combo("Dst##DstPg", &matrix_dst_page_, page_lbl, std::min(4, (dst_cfg.channels + 15) / 16));
    } else {
        matrix_dst_page_ = 0;
    }

    // Soft Pastel Lavender Patchbay Grid with Violet-Blue glowing nodes & #EDE9FE crosshair
    if (has_src && has_dst) {
        const int r0 = matrix_src_page_ * 16;
        const int r1 = std::min(src_cfg.channels, r0 + 16);
        const int c0 = matrix_dst_page_ * 16;
        const int c1 = std::min(dst_cfg.channels, c0 + 16);
        const int rows = r1 - r0;
        const int cols = c1 - c0;

        if (rows > 0 && cols > 0) {
            const float label_w = 46.0f;
            const float header_h = 24.0f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float cell = std::clamp(std::min((avail.x - label_w - 8.0f) / float(cols),
                                                   (avail.y - header_h - 26.0f) / float(rows)),
                                          18.0f, 28.0f);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Subtle light lavender backing plate for the grid
            const ImVec2 grid_p0(origin.x + label_w - 2.0f, origin.y + header_h - 2.0f);
            const ImVec2 grid_p1(origin.x + label_w + float(cols) * cell + 2.0f,
                                 origin.y + header_h + float(rows) * cell + 2.0f);
            dl->AddRectFilled(grid_p0, grid_p1, IM_COL32(250, 248, 255, 255), 12.0f);
            dl->AddRect(grid_p0, grid_p1, IM_COL32(221, 214, 254, 220), 12.0f, 0, 1.0f);

            const ImVec2 mouse = ImGui::GetIO().MousePos;
            int hov_r = -1, hov_c = -1;
            if (ImGui::IsWindowHovered()) {
                const float mx = mouse.x - (origin.x + label_w);
                const float my = mouse.y - (origin.y + header_h);
                if (mx >= 0 && my >= 0 && mx < float(cols) * cell && my < float(rows) * cell) {
                    hov_c = int(mx / cell);
                    hov_r = int(my / cell);
                }
            }

            // Soft lavender (#EDE9FE) highlight for hovered row & column
            if (hov_r >= 0) {
                dl->AddRectFilled(ImVec2(origin.x, origin.y + header_h + float(hov_r) * cell),
                                  ImVec2(origin.x + label_w + float(cols) * cell,
                                         origin.y + header_h + float(hov_r + 1) * cell),
                                  IM_COL32(237, 233, 254, 220), 6.0f);
            }
            if (hov_c >= 0) {
                dl->AddRectFilled(ImVec2(origin.x + label_w + float(hov_c) * cell, origin.y),
                                  ImVec2(origin.x + label_w + float(hov_c + 1) * cell,
                                         origin.y + header_h + float(rows) * cell),
                                  IM_COL32(237, 233, 254, 220), 6.0f);
            }

            // Column headers (Destinations)
            for (int j = 0; j < cols; ++j) {
                char hdr[8];
                std::snprintf(hdr, sizeof(hdr), "%d", c0 + j + 1);
                const ImU32 col = (j == hov_c) ? ImGui::ColorConvertFloat4ToU32(kColAccentPurp)
                                               : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
                dl->AddText(ImVec2(origin.x + label_w + float(j) * cell + 5.0f, origin.y + 4.0f), col, hdr);
            }

            // Row labels + clickable violet-blue claymorphic nodes
            for (int i = 0; i < rows; ++i) {
                const int sc = r0 + i;
                const float ry = origin.y + header_h + float(i) * cell;
                char rlbl[12];
                std::snprintf(rlbl, sizeof(rlbl), "Ch %02d", sc + 1);
                const ImU32 rcol = (i == hov_r) ? ImGui::ColorConvertFloat4ToU32(kColAccentPurp)
                                                : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
                dl->AddText(ImVec2(origin.x + 2.0f, ry + (cell - 14.0f) * 0.5f), rcol, rlbl);

                for (int j = 0; j < cols; ++j) {
                    const int dc = c0 + j;
                    const float cx = origin.x + label_w + float(j) * cell + cell * 0.5f;
                    const float cy = ry + cell * 0.5f;
                    MatrixCrosspoint cp{};
                    const bool linked = audio_matrix_.has_crosspoint(src_cfg.id, sc, dst_cfg.id, dc, &cp);

                    ImGui::SetCursorScreenPos(ImVec2(origin.x + label_w + float(j) * cell, ry));
                    ImGui::PushID(sc * 100 + dc);
                    if (ImGui::InvisibleButton("##Node", ImVec2(cell, cell))) {
                        matrix_active_preset_ = -1;
                        if (linked) {
                            audio_matrix_.remove_crosspoint(src_cfg.id, sc, dst_cfg.id, dc);
                            config_dirty_ = true;
                        } else {
                            const auto res = audio_matrix_.set_crosspoint(
                                src_cfg.id, sc, dst_cfg.id, dc, matrix_new_gain_db_, false, !cfg_.matrix_feedback_guard);
                            if (res == CrosspointResult::kBlockedFeedbackLoop) {
                                toast_msg_ = "Feedback Loop Guard: self-loop blocked!";
                                toast_until_us_ = now_us() + 3500000ull;
                            } else if (res == CrosspointResult::kOk) {
                                config_dirty_ = true;
                            }
                        }
                    }
                    if (linked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        audio_matrix_.set_crosspoint(src_cfg.id, sc, dst_cfg.id, dc, cp.gain_db, !cp.mute, true);
                        config_dirty_ = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        if (linked) {
                            ImGui::SetTooltip("%s Ch%d -> %s Ch%d\nGain: %.1f dB | %s\n(LMB: Remove, RMB: Mute)",
                                              src_cfg.name.c_str(), sc + 1, dst_cfg.name.c_str(), dc + 1, cp.gain_db,
                                              cp.mute ? "MUTED" : "ACTIVE");
                        } else {
                            ImGui::SetTooltip("Patch %s Ch%d -> %s Ch%d (%.1f dB)",
                                              src_cfg.name.c_str(), sc + 1, dst_cfg.name.c_str(), dc + 1,
                                              matrix_new_gain_db_);
                        }
                    }
                    ImGui::PopID();

                    const float rad = cell * 0.29f;
                    if (linked) {
                        if (cp.mute) {
                            dl->AddCircleFilled(ImVec2(cx, cy), rad + 3.0f, IM_COL32(236, 72, 153, 55));
                            dl->AddCircleFilled(ImVec2(cx, cy), rad, ImGui::ColorConvertFloat4ToU32(kColPink));
                        } else {
                            // Soft violet-blue node (#8B5CF6 -> #3B82F6) with pastel halo + highlight
                            const float t_diag = float(i + j) / std::max(1.0f, float(rows + cols - 2));
                            const ImVec4 ncol = lerp_col(kColAccentNode, kColAccentSky, t_diag);
                            dl->AddCircleFilled(ImVec2(cx, cy), rad + 3.5f, IM_COL32(139, 92, 246, 52));
                            dl->AddCircleFilled(ImVec2(cx, cy), rad, ImGui::ColorConvertFloat4ToU32(ncol));
                            dl->AddCircleFilled(ImVec2(cx - rad * 0.28f, cy - rad * 0.28f), rad * 0.26f,
                                                IM_COL32(255, 255, 255, 165));
                        }
                    } else {
                        const ImU32 dot = (i == hov_r && j == hov_c) ? IM_COL32(139, 92, 246, 170)
                                                                     : IM_COL32(221, 214, 254, 240);
                        dl->AddCircleFilled(ImVec2(cx, cy), rad * 0.62f, dot);
                    }
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header_h + float(rows) * cell + 6.0f));
        }
    }
    ImGui::TextDisabled("LMB: Toggle Node  |  RMB: Mute Node  |  Hover: Crosshair");
    end_glass_card();

    // ==================== COLUMN 3: STUDIO CONFIG (~25%) ====================
    ImGui::SameLine();
    draw_studio_config_panel();
}

void LanVidStudioApp::stop_tone() {
    tone_stop_.store(true);
    if (tone_thread_.joinable()) tone_thread_.join();
}

void LanVidStudioApp::stop_sender() {
    stop_tone();
    stop_vdev_tx();
    if (sender_) lv_sender_stop(sender_);
    sender_running_ = false;
}

void LanVidStudioApp::start_receiver() {
    if (!receiver_) return;
    save_app_config(cfg_);

    LvReceiverConfig rc;
    lv_receiver_config_default(&rc);
    rc.bind_port = uint16_t(std::clamp(cfg_.listen_port, 1024, 65535));
    rc.conceal_mode = cfg_.conceal_mode;
    rc.audio_target_ms = cfg_.audio_target_ms;
    rc.enable_audio = 1;
    rc.enable_discovery = 1;
    rc.audio_only = cfg_.audio_only ? 1 : 0;
    rc.adaptive_jitter = cfg_.adaptive_jitter ? 1 : 0;

    apply_receiver_bus();

    last_rx_fid_ = 0;
    lv_receiver_set_volume(receiver_, cfg_.volume);
    if (lv_receiver_start(receiver_, &rc)) {
        receiver_running_ = true;
        lv_receiver_set_midi_only(receiver_, cfg_.midi_only ? 1 : 0);
        apply_receiver_output();
        toast_msg_ = std::string(cfg_.audio_only ? "Прийом [Audio Only] на UDP :" : "Прийом запущено на UDP :") +
                     std::to_string(rc.bind_port) + " (LAN автопошук увімкнено)";
        toast_until_us_ = now_us() + 3000000ull;
    } else {
        receiver_running_ = false;
        toast_msg_ = "Не вдалося відкрити UDP порт " + std::to_string(rc.bind_port) + " (можливо, зайнятий)";
        toast_until_us_ = now_us() + 4500000ull;
    }
}

void LanVidStudioApp::apply_receiver_bus() {
    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    bus.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
    lv_receiver_configure_audio_bus(receiver_, &bus);
}

void LanVidStudioApp::draw_aoip_sender_controls() {
    ImGui::Spacing();
    ImGui::TextColored(kColAccentPurp, "AoIP шина (до 64 каналів, Master Clock)");
    ImGui::Separator();
    int bus_idx = 1;
    for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
    const char* bus_items[] = {"2 канали", "8 каналів", "16 каналів", "32 канали", "64 канали"};
    if (ImGui::Combo("Канали", &bus_idx, bus_items, 5)) {
        cfg_.aoip_channels = kBusChoices[bus_idx];
        tone_channels_.store(cfg_.aoip_channels);
        config_dirty_ = true;
    }
    if (ImGui::SliderInt("Буфер затримки та надійності", &cfg_.aoip_latency_ms, 2, 2000, "%d мс",
                         ImGuiSliderFlags_Logarithmic)) {
        if (sender_) lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    ImGui::TextDisabled("Бітрейт аудіо на канал:");
    draw_audio_bitrate_pills("SndBitrate");
    ImGui::TextDisabled("2 мс — Ultra Low Latency  <->  2000 мс — Bulletproof");
    ImGui::TextWrapped("%s", latency_mode_label(cfg_.aoip_latency_ms));
    const char* src_items[] = {"Built-in Test Tone Generator", "Virtual Audio Device (ASIO / CoreAudio / PipeWire)"};
    if (ImGui::Combo("Джерело шини", &cfg_.aoip_source, src_items, 2)) {
        if (sender_running_) {
            stop_tone();
            stop_vdev_tx();
            if (cfg_.aoip_source == 1) start_vdev_tx();
            else if (cfg_.aoip_tone) start_tone();
        }
        config_dirty_ = true;
    }
    if (cfg_.aoip_source == 0) {
        if (ImGui::Checkbox("Генерувати тон (110 Гц x номер каналу)", &cfg_.aoip_tone)) {
            if (sender_running_) {
                if (cfg_.aoip_tone) start_tone();
                else stop_tone();
            }
            config_dirty_ = true;
        }
    } else if (sender_running_) {
        draw_vdev_status(vdev_tx_.status(), true);
    } else {
        ImGui::TextDisabled("DAW -> виходи «LanVid Out 1..%d» -> мережа (після «Старт передачі»)", cfg_.aoip_channels);
    }
}

void LanVidStudioApp::draw_aoip_meter_grid() {
    const int n = std::clamp(cfg_.aoip_channels, 1, 64);
    const int cols = std::min(n, 16);
    const int rows = (n + cols - 1) / cols;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cell_w = std::max(18.0f, (avail - 4.0f) / float(cols)) - 4.0f;
    const float cell_h = 22.0f;
    for (int c = 0; c < n; ++c) {
        const int r = c / cols, k = c % cols;
        const ImVec2 p0(origin.x + float(k) * (cell_w + 4.0f), origin.y + float(r) * (cell_h + 4.0f));
        const ImVec2 p1(p0.x + cell_w, p0.y + cell_h);
        const LV_AudioChannelStats& st = ch_stats_[c];
        dl->AddRectFilled(p0, p1, IM_COL32(237, 233, 254, 255), 4.0f);
        if (st.active) {
            // -60..0 dBFS -> smooth gradient fill: Blue (#60A5FA) -> Violet (#A855F7) -> Pink (#EC4899)
            const float t = std::clamp((st.level_dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
            const ImVec4 c_end = (t < 0.65f) ? lerp_col(kColAccentBlue, kColAccentViol, t / 0.65f)
                                             : lerp_col(kColAccentViol, kColPink, (t - 0.65f) / 0.35f);
            draw_rounded_gradient_rect(dl, p0, ImVec2(p0.x + std::max(4.0f, cell_w * t), p1.y), 4.0f,
                                       kColAccentBlue, c_end, false);
        }
        const bool trouble = st.packets_lost > 0 || st.underruns > 0;
        dl->AddRect(p0, p1, trouble ? ImGui::ColorConvertFloat4ToU32(kColPink)
                                    : ImGui::ColorConvertFloat4ToU32(kColBorder), 4.0f);
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%d", c + 1);
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 3.0f),
                    st.active && st.level_dbfs > -36.0f ? IM_COL32(255, 255, 255, 250)
                                                        : ImGui::ColorConvertFloat4ToU32(kColTextMain),
                    lbl);
        if (ImGui::IsMouseHoveringRect(p0, p1)) {
            ImGui::SetTooltip("Канал %d: %.1f dBFS%s\nПакети: %llu | втрачено: %llu\nFEC: %llu | NACK: %llu (запитів %llu)\n"
                              "Underruns: %llu | пізні: %llu | джитер: %.2f мс",
                              c + 1, st.level_dbfs, st.active ? "" : " (неактивний)",
                              (unsigned long long)st.packets_rx, (unsigned long long)st.packets_lost,
                              (unsigned long long)st.fec_recovered, (unsigned long long)st.nack_recovered,
                              (unsigned long long)st.nacks_sent, (unsigned long long)st.underruns,
                              (unsigned long long)st.late_packets, double(st.jitter_us) / 1000.0);
        }
    }
    ImGui::Dummy(ImVec2(avail, float(rows) * (cell_h + 4.0f)));
}

void LanVidStudioApp::stop_receiver() {
    vdev_rx_.stop();
    if (receiver_) lv_receiver_stop(receiver_);
    receiver_running_ = false;
}

void LanVidStudioApp::poll_telemetry_and_frames() {
    const uint64_t now = now_us();
    apply_pending_remote_sync();
    enforce_audio_ownership(now);

    // Fetch latest GPU preview frame from Sender
    if (sender_ && sender_running_ && !cfg_.audio_only && sender_tex_) {
        int w = 0, h = 0;
        const int want_rgba = sender_tex_->wants_rgba() ? 1 : 0;
        int rc = lv_sender_get_preview_frame(sender_, sender_frame_buf_.data(), sender_frame_buf_.size(),
                                             &w, &h, &last_tx_preview_fid_, want_rgba);
        if (rc == -1 && w > 0 && h > 0) {
            sender_frame_buf_.resize(size_t(w) * size_t(h) * 4);
            rc = lv_sender_get_preview_frame(sender_, sender_frame_buf_.data(), sender_frame_buf_.size(),
                                             &w, &h, &last_tx_preview_fid_, want_rgba);
        }
        // Only a complete, sane frame reaches the GL texture (and the aspect-ratio math).
        if (rc == 1 && w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
            sender_frame_buf_.size() >= size_t(w) * size_t(h) * 4) {
            sender_tex_->update(sender_frame_buf_.data(), w, h);
        }
    }

    // Fetch latest decoded video frame from Receiver
    if (receiver_ && receiver_running_ && !cfg_.audio_only && receiver_tex_) {
        int w = 0, h = 0;
        const int want_rgba = receiver_tex_->wants_rgba() ? 1 : 0;
        int rc = lv_receiver_get_latest_frame(receiver_, receiver_frame_buf_.data(),
                                              receiver_frame_buf_.size(), &w, &h, &last_rx_fid_,
                                              want_rgba);
        if (rc == -1 && w > 0 && h > 0) {
            receiver_frame_buf_.resize(size_t(w) * size_t(h) * 4);
            rc = lv_receiver_get_latest_frame(receiver_, receiver_frame_buf_.data(),
                                              receiver_frame_buf_.size(), &w, &h, &last_rx_fid_,
                                              want_rgba);
        }
        // Only a complete, sane frame reaches the GL texture (and the aspect-ratio math).
        if (rc == 1 && w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
            receiver_frame_buf_.size() >= size_t(w) * size_t(h) * 4) {
            receiver_tex_->update(receiver_frame_buf_.data(), w, h);
        }
    }

    poll_latency_telemetry(now);

    // Update stats + rolling graphs every 500 ms
    if (now - last_stat_poll_us_ >= 500000ull) {
        last_stat_poll_us_ = now;
        if (sender_) {
            lv_sender_get_stats(sender_, &tx_stats_);
            sender_running_ = (tx_stats_.running != 0);
            if (tx_stats_.video_source_error != last_video_error_) {
                last_video_error_ = tx_stats_.video_source_error;
                if (const char* w = video_source_warning()) {
                    toast_msg_ = w;
                    toast_until_us_ = now + 6000000ull;
                }
            }
            if (sender_running_) {
                push_hist(tx_mbps_hist_, kHistLen, float(tx_stats_.tx_mbps));
                push_hist(tx_loss_hist_, kHistLen, float(tx_stats_.rx_loss_pct));
            }
        }
        if (receiver_ && receiver_running_ && now - ch_stats_us_ >= 100000) {
            ch_stats_us_ = now;
            for (int c = 0; c < std::clamp(cfg_.aoip_channels, 1, 64); ++c)
                lv_receiver_get_channel_stats(receiver_, uint16_t(c), &ch_stats_[c]);
        }
        if (receiver_) {
            lv_receiver_get_stats(receiver_, &rx_stats_);
            receiver_running_ = (rx_stats_.running != 0);
            if (receiver_running_) {
                push_hist(rx_lat_hist_, kHistLen, float(rx_stats_.latency_p50_ms));
                push_hist(rx_mbps_hist_, kHistLen, float(rx_stats_.rx_mbps));
            }
        }
        if (duplex_running_ && duplex_) {
            lv_duplex_get_stats(duplex_, &dpx_stats_);
            if (dpx_rate_us_ > 0 && now > dpx_rate_us_) {
                const double dt_s = double(now - dpx_rate_us_) / 1e6;
                const double bps_per_pkt = estimate_aoip_mbps(1, cfg_.audio_codec) * 1e6 / 200.0;  // ~5 ms pkt
                const uint64_t dtx = dpx_stats_.tx_packets >= dpx_prev_tx_pkts_ ? (dpx_stats_.tx_packets - dpx_prev_tx_pkts_) : 0;
                const uint64_t drx = dpx_stats_.rx_packets >= dpx_prev_rx_pkts_ ? (dpx_stats_.rx_packets - dpx_prev_rx_pkts_) : 0;
                dpx_tx_mbps_ = (double(dtx) * bps_per_pkt) / (dt_s * 1e6);
                dpx_rx_mbps_ = (double(drx) * bps_per_pkt) / (dt_s * 1e6);
            }
            dpx_prev_tx_pkts_ = dpx_stats_.tx_packets;
            dpx_prev_rx_pkts_ = dpx_stats_.rx_packets;
            dpx_rate_us_ = now;
        } else {
            dpx_tx_mbps_ = dpx_rx_mbps_ = 0.0;
            dpx_rate_us_ = 0;
        }
    }

    // Auto-save config debounced every 2s when modified
    if (config_dirty_ && (now - last_save_us_ >= 2000000ull)) {
        cfg_.dest_ip = ip_buf_;
        cfg_.file_path = file_buf_;
        cfg_.dpx_peer_host = dpx_peer_buf_;
        sync_matrix_to_config();
        save_app_config(cfg_);
        config_dirty_ = false;
        last_save_us_ = now;
    }
}

void LanVidStudioApp::draw_aspect_image(IGpuTexture* tex, const ImVec2& avail, const char* empty_label) {
    const float aw = std::max(1.0f, avail.x);
    const float ah = std::max(1.0f, avail.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + aw, p0.y + ah);
    const bool has_video = (tex && tex->width() > 0 && tex->height() > 0 && tex->imgui_id());
    if (has_video) {
        dl->AddRectFilled(p0, p1, IM_COL32(18, 16, 38, 255), 14.0f);
        const float src_ar = float(tex->width()) / float(tex->height());
        const float dst_ar = aw / ah;
        float dw = aw, dh = ah;
        if (dst_ar > src_ar) {
            dh = ah;
            dw = ah * src_ar;
        } else {
            dw = aw;
            dh = aw / src_ar;
        }
        const float dx = (aw - dw) * 0.5f;
        const float dy = (ah - dh) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + dx, p0.y + dy));
        ImGui::Image(tex->imgui_id(), ImVec2(dw, dh));
    } else {
        draw_rounded_gradient_rect(dl, p0, p1, 14.0f, kColCardSoft, kColBgTopRight, true);
        dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(kColBorder), 14.0f, 0, 1.0f);
        const ImVec2 tsz = ImGui::CalcTextSize(empty_label);
        dl->AddText(ImVec2(p0.x + (aw - tsz.x) * 0.5f, p0.y + (ah - tsz.y) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(kColTextMuted), empty_label);
        ImGui::Dummy(ImVec2(aw, ah));
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
}

void LanVidStudioApp::draw_top_bar() {
    begin_glass_card("##TopBar", ImVec2(0, 62), false,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const int tab_idx = std::clamp(cfg_.active_tab, 0, 3);
    const IconKind mode_icons[] = {IconKind::kSend, IconKind::kMatrix, IconKind::kHeadphones, IconKind::kGear};
    draw_gradient_icon_tile("##TopModeTile", mode_icons[tab_idx], 32.0f, -1, false);
    ImGui::SameLine();

    // Human-friendly interactive live status banner
    const bool grid_active = (cfg_.network_engine_mode != 0) && grid_node_ && grid_node_->is_open();
    const bool any_live = sender_running_ || receiver_running_ || duplex_running_ || audio_matrix_.is_running() ||
                          grid_active;
    const bool has_routes = !audio_matrix_.crosspoints().empty() || !audio_matrix_.midi_crosspoints().empty();
    const char* target_ip = (cfg_.connect_role == 2) ? dpx_peer_buf_ : ip_buf_;
    const int lat_disp = actual_buf_valid_ ? int(std::round(actual_buf_ms_)) : cfg_.aoip_latency_ms;
    const bool has_midi = !audio_matrix_.midi_slots().empty();

    char banner[256];
    ImVec4 banner_col = kColTextMain;
    if (cfg_.network_engine_mode == 1) {
        std::snprintf(banner, sizeof(banner),
                      "LanVid Grid L2 (%dch Ethernet · 0x88B6)  ·  %s",
                      cfg_.grid_channels,
                      grid_active ? ("MAC " + grid_node_->mac().to_string()).c_str()
                                  : grid_backend_status_.c_str());
        banner_col = grid_active ? kColGreen : kColAmber;
    } else if (!has_routes) {
        std::snprintf(banner, sizeof(banner),
                      "Підказка: оберіть навушники у «Пристрої» або увімкніть пресет маршрутизації у «Маршрути»");
        banner_col = kColAmber;
    } else if (any_live) {
        std::snprintf(banner, sizeof(banner),
                      "%s: %d каналів%s -> %s (Затримка %d мс · %s)",
                      cfg_.network_engine_mode == 2 ? "Hybrid L2+L3" : "Працює",
                      cfg_.aoip_channels, has_midi ? " + MIDI" : "", target_ip, lat_disp,
                      cfg_.adaptive_jitter ? "Авто Wi-Fi" : "Фіксована");
        banner_col = kColGreen;
    } else {
        std::snprintf(banner, sizeof(banner),
                      "Готово до роботи: %d каналів%s -> %s (Затримка %d мс)",
                      cfg_.aoip_channels, has_midi ? " + MIDI" : "", target_ip, lat_disp);
    }

    ImGui::BeginGroup();
    ImGui::TextColored(banner_col, "%s", banner);
    ImGui::TextDisabled("IP: %s  ·  48 кГц  ·  Рушій: %s",
                        local_lan_ip_.c_str(),
                        cfg_.network_engine_mode == 0 ? "Classic (UDP/Wi-Fi)"
                                                      : (cfg_.network_engine_mode == 1 ? "Grid (L2 256ch)" : "Hybrid (L2+L3)"));
    ImGui::EndGroup();

    // Quick 1-click Engine Switcher (Classic UDP <-> Grid L2 <-> Hybrid) in Top Bar
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 14.0f, ImGui::GetWindowWidth() - 420.0f));
    if (status_pill_button("Classic UDP", cfg_.network_engine_mode == 0, ImVec2(92.0f, 28.0f))) {
        apply_engine_mode(0);
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (status_pill_button("Grid L2", cfg_.network_engine_mode == 1, ImVec2(72.0f, 28.0f))) {
        apply_engine_mode(1);
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (status_pill_button("Hybrid", cfg_.network_engine_mode == 2, ImVec2(66.0f, 28.0f))) {
        apply_engine_mode(2);
    }
    ImGui::SameLine(0.0f, 10.0f);

    // Live status pill on far right
    const char* badge = any_live ? "● Активно" : "● Готово";
    const ImVec2 tsz = ImGui::CalcTextSize(badge);
    const float pill_w = tsz.x + 24.0f;
    const float pill_h = 28.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 pp0 = ImGui::GetCursorScreenPos();
    const ImVec2 pp1(pp0.x + pill_w, pp0.y + pill_h);
    if (any_live) {
        dl->AddRectFilled(pp0, pp1, IM_COL32(16, 185, 129, 32), pill_h * 0.5f);
        dl->AddRect(pp0, pp1, IM_COL32(16, 185, 129, 140), pill_h * 0.5f, 0, 1.0f);
    } else {
        dl->AddRectFilled(pp0, pp1,
                          g_dark_theme ? IM_COL32(32, 39, 56, 235) : IM_COL32(237, 233, 254, 235), pill_h * 0.5f);
        dl->AddRect(pp0, pp1, ImGui::ColorConvertFloat4ToU32(kColBorder), pill_h * 0.5f, 0, 1.0f);
    }
    dl->AddText(ImVec2(pp0.x + 12.0f, pp0.y + (pill_h - tsz.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(any_live ? kColGreen : kColTextMuted), badge);
    ImGui::Dummy(ImVec2(pill_w, pill_h));
    end_glass_card();
}

void LanVidStudioApp::draw_sender_tab() {
    const float left_w = std::min(430.0f, ImGui::GetContentRegionAvail().x * 0.44f);

    // ---- LEFT COLUMN: Controls ----
    begin_glass_card("##SenderControls", ImVec2(left_w, 0));

    ImGui::TextColored(kColAccentPurp, "1. Джерело відео та аудіо");
    ImGui::Separator();

    if (ImGui::Checkbox("Audio Only (Disable Video)", &cfg_.audio_only)) {
        config_dirty_ = true;
        if (sender_running_) start_sender();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Аналог CLI --audio-only: без захоплення відео, енкодера та відеопакетів (лише AoIP + MasterClock).");
    }

    ImGui::BeginDisabled(cfg_.audio_only);
    const char* src_items[] = {"Тестовий патерн", "Екран / Монітор", "Веб-камера", "Файл (.y4m / .ppm)"};
    int src_idx = std::clamp(cfg_.source_type - 1, 0, 3);
    if (ImGui::Combo("Джерело", &src_idx, src_items, 4)) {
        cfg_.source_type = src_idx + 1;
        config_dirty_ = true;
    }

    if (cfg_.source_type == LV_SOURCE_SCREEN) {
        if (!monitors_.empty()) {
            const auto& m = monitors_[size_t(std::clamp(cfg_.monitor_index, 0, int(monitors_.size()) - 1))];
            char cur_lbl[128];
            std::snprintf(cur_lbl, sizeof(cur_lbl), "[%d] %dx%d%s", m.index, m.width, m.height,
                          m.primary ? " (Основний)" : "");
            if (ImGui::BeginCombo("Монітор", cur_lbl)) {
                for (size_t i = 0; i < monitors_.size(); ++i) {
                    char lbl[128];
                    std::snprintf(lbl, sizeof(lbl), "[%d] %s %dx%d%s", monitors_[i].index,
                                  monitors_[i].name.c_str(), monitors_[i].width, monitors_[i].height,
                                  monitors_[i].primary ? " (Основний)" : "");
                    if (ImGui::Selectable(lbl, cfg_.monitor_index == int(i))) {
                        cfg_.monitor_index = int(i);
                        config_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("Захоплення екрана недоступне на цій платформі");
        }
        const char* cap_be[] = {"Auto (DXGI -> GDI)", "DXGI Desktop Duplication", "Win32 GDI"};
        if (ImGui::Combo("Захоплення", &cfg_.capture_backend, cap_be, 3)) config_dirty_ = true;
    } else if (cfg_.source_type == LV_SOURCE_CAMERA) {
        if (!cameras_.empty()) {
            const auto& c = cameras_[size_t(std::clamp(cfg_.camera_index, 0, int(cameras_.size()) - 1))];
            if (ImGui::BeginCombo("Камера", c.name.c_str())) {
                for (size_t i = 0; i < cameras_.size(); ++i) {
                    if (ImGui::Selectable(cameras_[i].name.c_str(), cfg_.camera_index == int(i))) {
                        cfg_.camera_index = int(i);
                        config_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("Веб-камери не знайдено");
        }
    } else if (cfg_.source_type == LV_SOURCE_FILE) {
        if (ImGui::InputText("Шлях файлу", file_buf_, sizeof(file_buf_))) config_dirty_ = true;
    } else {
        const char* res_lbl[] = {"1280x720 (HD)", "1920x1080 (Full HD)", "3840x2160 (4K UHD)"};
        int ridx = (cfg_.pattern_width >= 3840) ? 2 : (cfg_.pattern_width >= 1920 ? 1 : 0);
        if (ImGui::Combo("Роздільна здатність", &ridx, res_lbl, 3)) {
            if (ridx == 0) { cfg_.pattern_width = 1280; cfg_.pattern_height = 720; }
            else if (ridx == 1) { cfg_.pattern_width = 1920; cfg_.pattern_height = 1080; }
            else { cfg_.pattern_width = 3840; cfg_.pattern_height = 2160; }
            config_dirty_ = true;
        }
    }
    ImGui::EndDisabled();

    const char* aud_items[] = {"Вимкнено", "Системний звук (Loopback)", "Мікрофон"};
    if (ImGui::Combo("Аудіо", &cfg_.audio_mode, aud_items, 3)) {
        cfg_.audio_device_index = -1;
        config_dirty_ = true;
    }
    if (cfg_.audio_mode != LV_AUDIO_NONE) {
        const auto& adevs = (cfg_.audio_mode == LV_AUDIO_LOOPBACK) ? loop_devs_ : mic_devs_;
        std::string cur_aud = "Системний за замовчуванням";
        if (cfg_.audio_device_index >= 0 && cfg_.audio_device_index < int(adevs.size())) {
            cur_aud = adevs[size_t(cfg_.audio_device_index)].name;
        }
        if (ImGui::BeginCombo("Аудіопристрій", cur_aud.c_str())) {
            if (ImGui::Selectable("Системний за замовчуванням", cfg_.audio_device_index == -1)) {
                cfg_.audio_device_index = -1;
                config_dirty_ = true;
            }
            for (size_t i = 0; i < adevs.size(); ++i) {
                if (ImGui::Selectable(adevs[i].name.c_str(), cfg_.audio_device_index == int(i))) {
                    cfg_.audio_device_index = int(i);
                    config_dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        int chunk_idx = (cfg_.audio_chunk_ms <= 3.0) ? 0 : 1;
        const char* chunk_items[] = {"2.5 ms (Ultra-Low Latency)", "5.0 ms (Standard LAN)"};
        if (ImGui::Combo("Пакет аудіо", &chunk_idx, chunk_items, 2)) {
            cfg_.audio_chunk_ms = (chunk_idx == 0) ? 2.5 : 5.0;
            config_dirty_ = true;
        }
    }

    draw_aoip_sender_controls();

    ImGui::Spacing();
    ImGui::TextColored(kColAccentPurp, "2. Отримувач у LAN");
    ImGui::Separator();

    if (gradient_pill_button(scanning_.load() ? "Сканування LAN..." : "Сканувати LAN", ImVec2(0, 28))) {
        start_lan_scan();
    }
    ImGui::SameLine();
    if (status_pill_button("Оновити пристрої", false, ImVec2(0, 28))) refresh_devices();

    // Discovered receivers clickable list
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColCardSoft);
    ImGui::BeginChild("##DiscoveredList", ImVec2(0, 74), true);
    {
        std::lock_guard<std::mutex> lk(disc_mu_);
        if (discovered_.empty()) {
            ImGui::TextDisabled("Активних Receiver у LAN не знайдено. Запустіть прийом або введіть IP.");
        } else {
            for (const auto& d : discovered_) {
                char row[256];
                std::snprintf(row, sizeof(row), "%s [%s]  %s:%u  (%u ms)", d.device_name.c_str(),
                              d.os.c_str(), d.ip.c_str(), unsigned(d.stream_port), unsigned(d.rtt_ms));
                if (ImGui::Selectable(row, cfg_.dest_ip == d.ip && cfg_.dest_port == int(d.stream_port))) {
                    cfg_.dest_ip = d.ip;
                    cfg_.dest_port = int(d.stream_port);
                    std::snprintf(ip_buf_, sizeof(ip_buf_), "%s", d.ip.c_str());
                    config_dirty_ = true;
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SetNextItemWidth(left_w * 0.56f);
    if (ImGui::InputText("IP", ip_buf_, sizeof(ip_buf_))) config_dirty_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(92.0f);
    if (ImGui::InputInt("Порт", &cfg_.dest_port, 0, 0)) {
        cfg_.dest_port = std::clamp(cfg_.dest_port, 1024, 65535);
        config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::TextColored(kColAccentPurp, "3. Кодек та якість");
    ImGui::Separator();

    ImGui::BeginDisabled(cfg_.audio_only);
    const char* codec_items[] = {
        "LanVid DCT (~2 ms, без затримки)",
        "H.264 / AVC (Low Bandwidth, HW)",
        "H.265 / HEVC (Ultra-Low Bandwidth, HW)"
    };
    if (ImGui::Combo("Відеокодек", &cfg_.codec, codec_items, 3)) config_dirty_ = true;

    if (status_pill_button("Low Latency 1080p60", cfg_.preset == 0, ImVec2(0, 26))) apply_preset(0);
    ImGui::SameLine();
    if (status_pill_button("Ultra 4K60", cfg_.preset == 1, ImVec2(0, 26))) apply_preset(1);
    ImGui::SameLine();
    if (status_pill_button("Wi-Fi Adaptive", cfg_.preset == 2, ImVec2(0, 26))) apply_preset(2);

    if (cfg_.codec == LV_CODEC_LANVID_DCT) {
        if (ImGui::SliderInt("Якість (Q)", &cfg_.quality, 20, 98)) config_dirty_ = true;
    } else {
        if (ImGui::SliderInt("Цільовий бітрейт", &cfg_.bitrate_mbps, 1, 50, "%d Mbps")) {
            config_dirty_ = true;
            if (sender_ && sender_running_) {
                lv_sender_set_bitrate(sender_, uint32_t(cfg_.bitrate_mbps * 1000));
            }
        }
        if (ImGui::SliderInt("Інтервал IDR", &cfg_.keyframe_interval, 15, 300, "%d кадр.")) {
            config_dirty_ = true;
        }
        if (sender_running_ && tx_stats_.codec_backend[0]) {
            ImGui::TextColored(tx_stats_.codec_is_hardware ? kColGreen : kColAmber,
                               "Енкодер: %s", tx_stats_.codec_backend);
        }
    }
    if (ImGui::SliderInt("FPS", &cfg_.fps, 15, 120)) config_dirty_ = true;
    if (ImGui::SliderInt("XOR FEC група (0=вимк)", &cfg_.fec_group, 0, 12)) config_dirty_ = true;
    if (ImGui::Checkbox("Адаптивний контроль (AIMD для Wi-Fi)", &cfg_.adaptive_quality)) config_dirty_ = true;
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (!sender_running_) {
        if (gradient_pill_button("СТАРТ ПЕРЕДАЧІ", ImVec2(-1, 42), kColAccentBlue, kColAccentPurp)) start_sender();
    } else {
        if (gradient_pill_button("СТОП ПЕРЕДАЧІ", ImVec2(-1, 42), kColPink, kColRed)) stop_sender();
    }
    end_glass_card();

    // ---- RIGHT COLUMN: Live Sender Preview + Bitrate/Loss Graphs ----
    ImGui::SameLine();
    begin_glass_card("##SenderPreview", ImVec2(0, 0));

    ImGui::TextColored(kColAccentPurp, "Живий міні-прев'ю передачі");
    ImGui::SameLine();
    if (sender_running_ && tx_stats_.width > 0) {
        ImGui::TextDisabled("(%dx%d @ %.1f FPS | enc+tx: %.2f ms | Q=%d)",
                            tx_stats_.width, tx_stats_.height, tx_stats_.tx_fps,
                            tx_stats_.encode_tx_ms, tx_stats_.active_quality);
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float prev_h = std::max(180.0f, avail.y - 215.0f);
    const char* vwarn = video_source_warning();
    draw_aspect_image((sender_running_ && !cfg_.audio_only && !vwarn) ? sender_tex_ : nullptr, ImVec2(avail.x, prev_h),
                      cfg_.audio_only ? "Режим Audio Only (відео вимкнено) — активна AoIP шина"
                      : vwarn         ? vwarn
                                      : "Натисніть «СТАРТ ПЕРЕДАЧІ» для живого прев'ю");

    ImGui::Spacing();
    ImGui::Columns(4, "##TxCards", false);
    ImGui::TextDisabled("TX FPS");
    ImGui::Text("%.1f", tx_stats_.tx_fps);
    ImGui::NextColumn();
    ImGui::TextDisabled("Бітрейт");
    ImGui::Text("%.1f Mbps", tx_stats_.tx_mbps);
    ImGui::NextColumn();
    ImGui::TextDisabled("Втрати RX / FEC");
    ImGui::Text("%.2f%% / %.2f%%", tx_stats_.rx_loss_pct, tx_stats_.rx_fec_pct);
    ImGui::NextColumn();
    ImGui::TextDisabled("Аудіо");
    ImGui::Text("%.0f kbps", tx_stats_.audio_kbps);
    ImGui::Columns(1);

    ImGui::Spacing();
    char mbps_ov[64];
    std::snprintf(mbps_ov, sizeof(mbps_ov), "Бітрейт: %.1f Mbps", tx_stats_.tx_mbps);
    ImGui::PlotLines("##TxMbps", tx_mbps_hist_, kHistLen, 0, mbps_ov, 0.0f, FLT_MAX, ImVec2(-1, 62));

    char loss_ov[64];
    std::snprintf(loss_ov, sizeof(loss_ov), "Втрати пакетів RX: %.2f%%", tx_stats_.rx_loss_pct);
    ImGui::PlotLines("##TxLoss", tx_loss_hist_, kHistLen, 0, loss_ov, 0.0f, 10.0f, ImVec2(-1, 52));

    end_glass_card();
}

void LanVidStudioApp::draw_receiver_tab() {
    // Top compact control bar
    // Two rows: transport / view controls, then the AoIP bus (channels, latency, latency mode, actual buffer).
    begin_glass_card("##RxControls", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2.0f + 22.0f), false,
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::SetNextItemWidth(86.0f);
    if (ImGui::InputInt("Порт", &cfg_.listen_port, 0, 0)) {
        cfg_.listen_port = std::clamp(cfg_.listen_port, 1024, 65535);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (!receiver_running_) {
        if (gradient_pill_button("Старт прийому", ImVec2(0, 28), kColAccentBlue, kColAccentPurp)) start_receiver();
    } else {
        if (gradient_pill_button("Стоп прийому", ImVec2(0, 28), kColPink, kColRed)) stop_receiver();
    }

    ImGui::SameLine();
    if (status_pill_button("Повний екран (F11)", false, ImVec2(0, 28))) {
        if (host_.set_fullscreen && host_.is_fullscreen) {
            host_.set_fullscreen(host_.ctx, !host_.is_fullscreen(host_.ctx));
        }
    }

    ImGui::SameLine();
    if (status_pill_button("Скріншот", false, ImVec2(0, 28))) {
        char fname[128];
        std::snprintf(fname, sizeof(fname), "lanvid_shot_%04d.ppm", ++screenshot_counter_);
        if (receiver_ && lv_receiver_save_screenshot(receiver_, fname)) {
            toast_msg_ = std::string("Збережено скріншот: ") + fname;
        } else {
            toast_msg_ = "Немає активного кадру для скріншота";
        }
        toast_until_us_ = now_us() + 3000000ull;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int vol_pct = int(cfg_.volume * 100.0f + 0.5f);
    if (ImGui::SliderInt("Гучність", &vol_pct, 0, 150, "%d%%")) {
        cfg_.volume = float(vol_pct) / 100.0f;
        if (receiver_) lv_receiver_set_volume(receiver_, cfg_.volume);
        config_dirty_ = true;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    const char* conc_items[] = {"Чорний", "Попер. кадр"};
    if (ImGui::Combo("Втрати", &cfg_.conceal_mode, conc_items, 2)) {
        if (receiver_) lv_receiver_set_concealment(receiver_, cfg_.conceal_mode);
        config_dirty_ = true;
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("HUD", &cfg_.show_hud)) config_dirty_ = true;

    ImGui::SameLine();
    if (ImGui::Checkbox("Audio Only (Disable Video)", &cfg_.audio_only)) {
        config_dirty_ = true;
        if (receiver_running_) start_receiver();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Аналог CLI --audio-only: ігнорувати відеопакети до декодування (чистий AoIP-міст).");
    }

    ImGui::SetNextItemWidth(90.0f);
    int bus_idx = 1;
    for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
    const char* bus_items[] = {"2 кан.", "8 кан.", "16 кан.", "32 кан.", "64 кан."};
    if (ImGui::Combo("AoIP", &bus_idx, bus_items, 5)) {
        cfg_.aoip_channels = kBusChoices[bus_idx];
        if (receiver_) apply_receiver_bus();
        config_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderInt("Затримка", &cfg_.aoip_latency_ms, 2, 2000, "%d мс", ImGuiSliderFlags_Logarithmic)) {
        if (receiver_) lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", latency_mode_label(cfg_.aoip_latency_ms));
    ImGui::SameLine();
    draw_latency_mode_controls("RxLatMode", 160.0f);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    draw_actual_buffer_line();
    end_glass_card();

    // 64-channel LED meter grid (phase-aligned playout bus)
    const int bus_n = std::clamp(cfg_.aoip_channels, 1, 64);
    const int bus_rows = (bus_n + 15) / 16;
    const float meter_h = 34.0f + float(bus_rows) * 26.0f + (cfg_.aoip_output == 1 ? 22.0f : 0.0f);
    begin_glass_card("##AoipMeters", ImVec2(0, meter_h), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("AoIP шина: %d кан. | затримка %d мс — %s", bus_n, cfg_.aoip_latency_ms,
                        cfg_.aoip_latency_ms <= 10 ? "Ultra Low" : (cfg_.aoip_latency_ms <= 100 ? "Balanced" : "Bulletproof"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    const char* out_items[] = {"Лише індикатори", "Virtual Audio Device (DAW inputs)"};
    if (ImGui::Combo("Вихід шини", &cfg_.aoip_output, out_items, 2)) {
        apply_receiver_output();
        config_dirty_ = true;
    }
    if (cfg_.aoip_output == 1 && receiver_running_) {
        const auto vs = vdev_rx_.status();
        vs.driver_connected ? ImGui::TextColored(kColGreen, "DAW: %s | %u Гц | буфер %u | RX %u кадрів",
                                                 vs.driver_name.c_str(), vs.sample_rate, vs.buffer_size, vs.rx_fill)
                            : ImGui::TextColored(kColAmber, "Очікування DAW на пристрої «LanVid»");
        vdev_rx_.publish_network_state(uint32_t(cfg_.aoip_latency_ms), rx_stats_.running != 0, 0.0);
    }
    draw_aoip_meter_grid();
    end_glass_card();

    // Live GPU Video Viewport
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float vid_h = std::max(160.0f, avail.y - 86.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##RxCanvas", ImVec2(0, vid_h), false, ImGuiWindowFlags_NoScrollbar);
    draw_aspect_image((receiver_running_ && !cfg_.audio_only) ? receiver_tex_ : nullptr,
                      ImGui::GetContentRegionAvail(),
                      cfg_.audio_only
                          ? (receiver_running_ ? "Режим Audio Only (відео вимкнено) — AoIP прийом активний"
                                               : "Режим Audio Only — натисніть «Старт прийому» для AoIP")
                          : (receiver_running_ ? "Очікування відеопотоку по UDP..."
                                               : "Натисніть «Старт прийому», щоб приймати відео та звук"));
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Bottom Live Telemetry Strip
    begin_glass_card("##RxTelemetry", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::Columns(6, "##RxCols", false);
    ImGui::TextDisabled("Потік / FPS");
    ImGui::Text("%dx%d  %.1f FPS", rx_stats_.width, rx_stats_.height, rx_stats_.fps);
    ImGui::NextColumn();
    ImGui::TextDisabled("Кодек / Бекенд");
    {
        const char* cname = (rx_stats_.active_codec == LV_CODEC_H265)
                                ? "H.265"
                                : (rx_stats_.active_codec == LV_CODEC_H264 ? "H.264" : "DCT");
        const char* accel = rx_stats_.codec_is_hardware ? "HW" : "SW";
        ImGui::TextColored(rx_stats_.codec_is_hardware ? kColGreen : kColAccentInd,
                           "%s [%s]", cname, accel);
    }
    ImGui::NextColumn();
    ImGui::TextDisabled("E2E Latency (p50 / p99)");
    ImGui::TextColored(rx_stats_.latency_p50_ms < 6.0 ? kColGreen : kColAmber,
                       "%.2f / %.2f ms", rx_stats_.latency_p50_ms, rx_stats_.latency_p99_ms);
    ImGui::NextColumn();
    ImGui::TextDisabled("Бітрейт / PLI");
    if (rx_stats_.active_codec == LV_CODEC_LANVID_DCT) {
        ImGui::Text("%.1f Mbps (Q=%d)", rx_stats_.rx_mbps, rx_stats_.active_quality);
    } else {
        ImGui::Text("%.2f Mbps (PLI:%llu)", rx_stats_.rx_mbps,
                    static_cast<unsigned long long>(rx_stats_.pli_requests));
    }
    ImGui::NextColumn();
    ImGui::TextDisabled("Втрати / FEC");
    ImGui::Text("%.2f%% (FEC:%llu)", rx_stats_.loss_pct,
                static_cast<unsigned long long>(rx_stats_.fec_recovered));
    ImGui::NextColumn();
    ImGui::TextDisabled("Аудіо / Буфер");
    ImGui::Text("%.0f kbps (%.1f ms)", rx_stats_.audio_kbps, rx_stats_.audio_buffer_ms);
    ImGui::Columns(1);
    end_glass_card();
}

void LanVidStudioApp::draw_fullscreen_receiver() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##FullscreenCanvas", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_aspect_image(receiver_tex_, vp->Size, "Очікування відеопотоку по UDP...");

    if (cfg_.show_hud) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 16, vp->Pos.y + 16));
        ImGui::SetNextWindowBgAlpha(0.68f);
        ImGui::Begin("##FsHud", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings);
        const char* cname = (rx_stats_.active_codec == LV_CODEC_H265)
                                ? "H.265"
                                : (rx_stats_.active_codec == LV_CODEC_H264 ? "H.264" : "DCT");
        ImGui::Text("[%s %s] %dx%d  %.1f FPS | E2E %.2f ms | %.2f Mbps | FEC:%llu | PLI:%llu | Loss:%.2f%% | Aud:%.0f kbps",
                    cname, rx_stats_.codec_is_hardware ? "HW" : "SW",
                    rx_stats_.width, rx_stats_.height, rx_stats_.fps, rx_stats_.latency_p50_ms,
                    rx_stats_.rx_mbps, static_cast<unsigned long long>(rx_stats_.fec_recovered),
                    static_cast<unsigned long long>(rx_stats_.pli_requests),
                    rx_stats_.loss_pct, rx_stats_.audio_kbps);
        ImGui::SameLine();
        if (ImGui::SmallButton("Вийти (ESC/F11)")) {
            if (host_.set_fullscreen) host_.set_fullscreen(host_.ctx, false);
        }
        ImGui::End();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void LanVidStudioApp::draw_add_device_popups() {
    auto finish_add_slot = [this](const MatrixSlotConfig& s) {
        audio_matrix_.add_slot(s);
        sync_matrix_to_config();
        save_app_config(cfg_);
        config_dirty_ = false;
        toast_msg_ = "Додано аудіопристрій: " + s.name;
        toast_until_us_ = now_us() + 2800000ull;
    };
    auto finish_add_midi = [this](const MidiSlotConfig& m) {
        audio_matrix_.add_midi_slot(m);
        sync_matrix_to_config();
        save_app_config(cfg_);
        config_dirty_ = false;
        toast_msg_ = "Додано MIDI-пристрій: " + m.name;
        toast_until_us_ = now_us() + 2800000ull;
    };

    if (ImGui::BeginPopup("##AddMidiDev")) {
        ImGui::TextColored(kColAccentInd, "MIDI-входи (Клавіатури / Барабани / Контролери)");
        if (midi_in_devs_.empty()) ImGui::TextDisabled("  MIDI-входів не знайдено");
        for (size_t i = 0; i < midi_in_devs_.size(); ++i) {
            const auto& m = midi_in_devs_[i];
            if (m.name == "LanVid MIDI Internal") continue;
            ImGui::PushID(int(i) + 1000);
            if (ImGui::Selectable((m.name + " [MIDI IN]").c_str())) {
                finish_add_midi({-1, MidiSlotType::kMidiInput, m.name, m.name, m.index, 0, true});
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextColored(kColAccentPurp, "MIDI-виходи (DAW / Синтезатори)");
        if (midi_out_devs_.empty()) ImGui::TextDisabled("  MIDI-виходів не знайдено");
        for (size_t i = 0; i < midi_out_devs_.size(); ++i) {
            const auto& m = midi_out_devs_[i];
            if (m.name == "LanVid MIDI Internal") continue;
            ImGui::PushID(int(i) + 2000);
            if (ImGui::Selectable((m.name + " [MIDI OUT]").c_str())) {
                finish_add_midi({-1, MidiSlotType::kMidiOutput, m.name, m.name, m.index, 0, true});
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##AddAudioDev")) {
#if defined(__APPLE__)
        auto add_sys_slot = [&](const SystemAudioDeviceEntry* d, MatrixSlotRole role) {
            MatrixSlotConfig s{};
            s.id = -1;
            s.type = MatrixSlotType::kSystemAudio;
            s.role = role;
            s.name = d ? d->name : (role == MatrixSlotRole::kOutput ? "Основний вихід (CoreAudio)" : "Основний вхід (CoreAudio)");
            s.device_id = d ? d->uid : "";
            s.device_index = -1;
            s.channels = d ? std::clamp(d->channels, 1, 64) : 2;
            s.buffer_size = cfg_.matrix_buffer_size;
            finish_add_slot(s);
        };
        ImGui::TextColored(kColAccentInd, "Виходи CoreAudio (Навушники / Колонки / USB)");
        if (ImGui::Selectable("Основний вихід (CoreAudio)##DefOut")) add_sys_slot(nullptr, MatrixSlotRole::kOutput);
        for (size_t i = 0; i < sys_out_devs_.size(); ++i) {
            const auto& d = sys_out_devs_[i];
            ImGui::PushID(int(i) + 100);
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d кан., %u Гц)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [основний]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kOutput);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextColored(kColAccentPurp, "Входи CoreAudio (Мікрофон / Лінійний вхід)");
        if (ImGui::Selectable("Основний вхід (CoreAudio)##DefIn")) add_sys_slot(nullptr, MatrixSlotRole::kInput);
        for (size_t i = 0; i < sys_in_devs_.size(); ++i) {
            const auto& d = sys_in_devs_[i];
            ImGui::PushID(int(i) + 200);
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d кан., %u Гц)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [основний]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kInput);
            ImGui::PopID();
        }
#else
        ImGui::TextColored(kColAccentInd, "Виходи звуку (Навушники / Аудіоінтерфейси)");
        if (ImGui::Selectable("Основні навушники / колонки ПК (WASAPI)##DefOut")) {
            finish_add_slot({-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                             "Навушники / Вихід ПК", "", -1, 2, cfg_.matrix_buffer_size, true});
        }
        for (size_t i = 0; i < loop_devs_.size(); ++i) {
            const auto& d = loop_devs_[i];
            ImGui::PushID(int(i) + 100);
            if (ImGui::Selectable((d.name + " [ВИХІД]").c_str())) {
                finish_add_slot({-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                                 d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true});
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextColored(kColAccentPurp, "Входи звуку (Мікрофон / Лінійний вхід)");
        if (ImGui::Selectable("Основний мікрофон / вхід ПК (WASAPI)##DefIn")) {
            finish_add_slot({-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                             "Мікрофон / Вхід ПК", "", -1, 2, cfg_.matrix_buffer_size, true});
        }
        for (size_t i = 0; i < mic_devs_.size(); ++i) {
            const auto& d = mic_devs_[i];
            ImGui::PushID(int(i) + 200);
            if (ImGui::Selectable((d.name + " [ВХІД]").c_str())) {
                finish_add_slot({-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                                 d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true});
            }
            ImGui::PopID();
        }
        if (!asio_devs_.empty()) {
            ImGui::Separator();
            ImGui::TextColored(kColAmber, "Зовнішні ASIO-драйвери (Студійні карти)");
            for (size_t i = 0; i < asio_devs_.size(); ++i) {
                const auto& a = asio_devs_[i];
                ImGui::PushID(int(i) + 300);
                if (ImGui::Selectable((a.name + " [ASIO]").c_str())) {
                    finish_add_slot({-1, MatrixSlotType::kExternalAsio, MatrixSlotRole::kDuplex,
                                     a.name, a.name, -1, 16, cfg_.matrix_buffer_size, true});
                }
                ImGui::PopID();
            }
        }
#endif
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("##ConfirmClearAll", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kColTextMain, "Очистити всі аудіо та MIDI маршрути?");
        ImGui::TextDisabled("Ви завжди зможете відновити їх одним кліком у швидких сценаріях.");
        ImGui::Spacing();
        if (gradient_pill_button("Так, очистити", ImVec2(140, 32), kColPink, kColRed)) {
            matrix_active_preset_ = -1;
            audio_matrix_.clear_crosspoints();
            audio_matrix_.clear_midi_crosspoints();
            config_dirty_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (status_pill_button("Скасувати", false, ImVec2(110, 32))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void LanVidStudioApp::draw_connect_tab() {
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float board_w = std::max(480.0f, total_w * 0.62f);

    // Helper: animated cubic Bezier patch cable with glowing traffic dot + pill label
    auto draw_animated_bezier_cable = [](ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col,
                                         const char* badge, bool active, float speed, bool reverse_dot) {
        const float dx = std::max(55.0f, std::fabs(p1.x - p0.x) * 0.45f);
        const ImVec2 cp0(p0.x + dx, p0.y);
        const ImVec2 cp1(p1.x - dx, p1.y);
        // Subtle glow under cable
        const ImU32 glow_col = (col & 0x00FFFFFFu) | (active ? 0x44000000u : 0x1E000000u);
        dl->AddBezierCubic(p0, cp0, cp1, p1, glow_col, active ? 7.0f : 4.5f, 36);
        dl->AddBezierCubic(p0, cp0, cp1, p1, active ? col : ((col & 0x00FFFFFFu) | 0x88000000u),
                           active ? 2.6f : 1.8f, 36);

        // Moving traffic pulse dot along cubic Bezier
        if (active) {
            const float raw_t = std::fmod(float(ImGui::GetTime()) * speed, 1.0f);
            const float t = reverse_dot ? (1.0f - raw_t) : raw_t;
            const float u = 1.0f - t;
            const ImVec2 dot(
                u * u * u * p0.x + 3.0f * u * u * t * cp0.x + 3.0f * u * t * t * cp1.x + t * t * t * p1.x,
                u * u * u * p0.y + 3.0f * u * u * t * cp0.y + 3.0f * u * t * t * cp1.y + t * t * t * p1.y);
            dl->AddCircleFilled(dot, 6.5f, (col & 0x00FFFFFFu) | 0x55000000u);
            dl->AddCircleFilled(dot, 3.6f, IM_COL32(255, 255, 255, 245));
        }

        // Midpoint pill badge
        if (badge && badge[0]) {
            const ImVec2 mid(
                0.125f * p0.x + 0.375f * cp0.x + 0.375f * cp1.x + 0.125f * p1.x,
                0.125f * p0.y + 0.375f * cp0.y + 0.375f * cp1.y + 0.125f * p1.y);
            const ImVec2 tsz = ImGui::CalcTextSize(badge);
            const ImVec2 b0(mid.x - tsz.x * 0.5f - 8.0f, mid.y - tsz.y * 0.5f - 3.0f);
            const ImVec2 b1(mid.x + tsz.x * 0.5f + 8.0f, mid.y + tsz.y * 0.5f + 3.0f);
            dl->AddRectFilled(b0, b1, g_dark_theme ? IM_COL32(20, 18, 34, 235) : IM_COL32(250, 248, 255, 242), 10.0f);
            dl->AddRect(b0, b1, col, 10.0f, 0, 1.1f);
            dl->AddText(ImVec2(b0.x + 8.0f, b0.y + 3.0f),
                        g_dark_theme ? IM_COL32(236, 233, 255, 245) : IM_COL32(48, 38, 92, 245), badge);
        }
    };

    // Snapshot live peers, cameras, monitors & audio slots
    const auto studio_cams = tally_intercom_.is_running() ? tally_intercom_.studio_cameras()
                                                          : std::vector<StudioCameraInfo>{};
    const auto mon_phones = monitor_server_.is_running() ? monitor_server_.clients()
                                                         : std::vector<MonitorClientInfo>{};
    const auto slots = audio_matrix_.slots();
    std::string remote_name = "Віддалена студія (LAN)";
    std::string remote_ip = (cfg_.connect_role == 2) ? std::string(dpx_peer_buf_) : std::string(ip_buf_);
    int remote_ch = cfg_.aoip_channels;
    {
        std::lock_guard<std::mutex> lk(nodes_mu_);
        if (!nodes_.empty()) {
            remote_name = nodes_[0].name;
            remote_ip = nodes_[0].ip;
            remote_ch = nodes_[0].channels;
        }
    }
    if (remote_name == "Віддалена студія (LAN)") {
        std::lock_guard<std::mutex> lk(disc_mu_);
        if (!discovered_.empty()) {
            remote_name = discovered_[0].device_name;
            remote_ip = discovered_[0].ip;
        }
    }

    const bool role_running = (cfg_.connect_role == 0) ? sender_running_
                                                       : (cfg_.connect_role == 1 ? receiver_running_ : duplex_running_);

    // ---- LEFT / CENTER: INTERACTIVE NETWORK CONNECTION BOARD ----
    begin_glass_card("##ConnectBoardRoot", ImVec2(board_w, 0));

    // Board header bar
    ImGui::TextColored(kColAccentPurp, "МЕРЕЖЕВА ДОШКА ПІДКЛЮЧЕНЬ (LAN TOPOLOGY)");
    ImGui::SameLine(std::max(300.0f, ImGui::GetContentRegionAvail().x - 240.0f));
    const bool is_scanning = scanning_.load() || nodes_scanning_.load();
    if (status_pill_button(is_scanning ? "Пошук..." : "Знайти", false, ImVec2(82.0f, 26.0f))) {
        start_lan_scan();
        scan_aoip_nodes();
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (!role_running) {
        if (gradient_pill_button("Старт", ImVec2(128.0f, 26.0f), kColAccentBlue, kColAccentPurp)) {
            if (cfg_.connect_role == 0) {
                start_sender();
                if (!audio_matrix_.is_running()) audio_matrix_.start();
            } else if (cfg_.connect_role == 1) {
                start_receiver();
                if (!audio_matrix_.is_running()) audio_matrix_.start();
            } else {
                audio_matrix_.stop();
                start_duplex();
            }
        }
    } else {
        if (gradient_pill_button("Зупинити", ImVec2(128.0f, 26.0f), kColPink, kColRed)) {
            if (cfg_.connect_role == 0) stop_sender();
            else if (cfg_.connect_role == 1) stop_receiver();
            else stop_duplex();
        }
    }

    ImGui::Spacing();

    // Interactive Topology Canvas (ImDrawList)
    const float canvas_h = std::max(300.0f, ImGui::GetContentRegionAvail().y - 118.0f);
    const ImVec2 c_p0 = ImGui::GetCursorScreenPos();
    const float c_w = ImGui::GetContentRegionAvail().x;
    const ImVec2 c_p1(c_p0.x + c_w, c_p0.y + canvas_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Subtle soft-lavender studio dot-grid background
    dl->AddRectFilled(c_p0, c_p1,
                      g_dark_theme ? IM_COL32(18, 16, 29, 230) : IM_COL32(244, 240, 255, 220), 16.0f);
    dl->AddRect(c_p0, c_p1, ImGui::ColorConvertFloat4ToU32(kColBorder), 16.0f, 0, 1.1f);
    const ImU32 grid_col = g_dark_theme ? IM_COL32(139, 92, 246, 26) : IM_COL32(124, 58, 237, 28);
    for (float gx = c_p0.x + 24.0f; gx < c_p1.x - 12.0f; gx += 28.0f) {
        for (float gy = c_p0.y + 24.0f; gy < c_p1.y - 12.0f; gy += 28.0f) {
            dl->AddCircleFilled(ImVec2(gx, gy), 1.3f, grid_col);
        }
    }

    auto draw_topology_node = [&](int node_id, ImVec2 pos, ImVec2 sz, const char* title, const char* subtitle,
                                  const char* badge, ImU32 accent, bool tally_border, ImU32 tally_col) {
        const bool sel = (connect_selected_node_ == node_id);
        const ImVec2 p1(pos.x + sz.x, pos.y + sz.y);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(4000 + node_id);
        if (ImGui::InvisibleButton("##TopoNode", sz)) {
            connect_selected_node_ = node_id;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();

        draw_soft_shadow(dl, pos, p1, 14.0f, sel ? 0.9f : 0.45f);
        const ImU32 bg = g_dark_theme ? IM_COL32(27, 24, 44, 245) : IM_COL32(255, 255, 255, 248);
        dl->AddRectFilled(pos, p1, bg, 14.0f);
        const ImU32 border = tally_border ? tally_col : (sel || hov ? accent : ImGui::ColorConvertFloat4ToU32(kColBorder));
        dl->AddRect(pos, p1, border, 14.0f, 0, (tally_border || sel) ? 2.4f : 1.1f);

        // Accent top pill bar
        dl->AddRectFilled(ImVec2(pos.x + 12.0f, pos.y + 10.0f), ImVec2(pos.x + 16.0f, pos.y + sz.y - 10.0f), accent, 3.0f);
        dl->AddText(ImVec2(pos.x + 24.0f, pos.y + 11.0f), ImGui::ColorConvertFloat4ToU32(kColTextMain), title);
        dl->AddText(ImVec2(pos.x + 24.0f, pos.y + 30.0f), ImGui::ColorConvertFloat4ToU32(kColTextMuted), subtitle);
        if (badge && badge[0]) {
            const ImVec2 bsz = ImGui::CalcTextSize(badge);
            const ImVec2 bp0(pos.x + 24.0f, pos.y + sz.y - 24.0f);
            const ImVec2 bp1(bp0.x + bsz.x + 12.0f, bp0.y + 16.0f);
            dl->AddRectFilled(bp0, bp1, (accent & 0x00FFFFFFu) | 0x33000000u, 8.0f);
            dl->AddText(ImVec2(bp0.x + 6.0f, bp0.y + 1.0f), accent, badge);
        }
        // Connection port dots (left & right)
        dl->AddCircleFilled(ImVec2(pos.x, pos.y + sz.y * 0.5f), 4.5f, accent);
        dl->AddCircleFilled(ImVec2(p1.x, pos.y + sz.y * 0.5f), 4.5f, accent);
    };

    const float node_w = std::clamp(c_w * 0.36f, 175.0f, 235.0f);
    const float node_h = 74.0f;
    const float left_x = c_p0.x + 20.0f;
    const float right_x = c_p1.x - node_w - 20.0f;

    // Left Column Nodes: 1) Local Host, 2) Studio Audio Interface (Virtual ASIO / CoreAudio)
    const ImVec2 local_pos(left_x, c_p0.y + 28.0f);
    const ImVec2 audio_pos(left_x, c_p0.y + canvas_h - node_h - 28.0f);

    // Right Column Nodes: 1) Remote Studio PC/Mac, 2) Mobile Camera / Monitor (Tally + Intercom)
    const ImVec2 remote_pos(right_x, c_p0.y + 28.0f);
    const ImVec2 mobile_pos(right_x, c_p0.y + canvas_h - node_h - 28.0f);

    // Draw animated Bezier cables first (behind node cards)
    const ImU32 col_violet = IM_COL32(139, 92, 246, 255);
    const ImU32 col_teal = IM_COL32(20, 184, 166, 255);
    const ImU32 col_indigo = IM_COL32(99, 102, 241, 255);

    char main_cable_lbl[64];
    const double disp_lat = actual_buf_valid_ ? actual_buf_ms_ : double(cfg_.aoip_latency_ms);
    if (cfg_.midi_only) {
        std::snprintf(main_cable_lbl, sizeof(main_cable_lbl), "MIDI Only · %.1f ms", disp_lat);
    } else if (cfg_.audio_only) {
        std::snprintf(main_cable_lbl, sizeof(main_cable_lbl), "%dch AoIP · %.0f ms · 48k", cfg_.aoip_channels, disp_lat);
    } else {
        std::snprintf(main_cable_lbl, sizeof(main_cable_lbl), "4K Video + %dch AoIP", cfg_.aoip_channels);
    }

    // Helper: animated dashed cubic Bezier cable for Secondary / Redundant Failover NIC
    auto draw_dashed_bezier_cable = [](ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col,
                                       const char* badge, bool active, float speed, bool reverse_dot) {
        const float dx = std::max(55.0f, std::fabs(p1.x - p0.x) * 0.45f);
        const ImVec2 cp0(p0.x + dx, p0.y);
        const ImVec2 cp1(p1.x - dx, p1.y);
        auto eval = [&](float t) -> ImVec2 {
            const float u = 1.0f - t;
            return ImVec2(
                u * u * u * p0.x + 3.0f * u * u * t * cp0.x + 3.0f * u * t * t * cp1.x + t * t * t * p1.x,
                u * u * u * p0.y + 3.0f * u * u * t * cp0.y + 3.0f * u * t * t * cp1.y + t * t * t * p1.y);
        };
        constexpr int kSegs = 28;
        const ImU32 line_col = active ? col : ((col & 0x00FFFFFFu) | 0x77000000u);
        for (int i = 0; i < kSegs; i += 2) {
            const ImVec2 a = eval(float(i) / float(kSegs));
            const ImVec2 b = eval(float(std::min(kSegs, i + 1)) / float(kSegs));
            dl->AddLine(a, b, (col & 0x00FFFFFFu) | 0x30000000u, active ? 5.0f : 3.2f);
            dl->AddLine(a, b, line_col, active ? 2.1f : 1.5f);
        }
        if (active) {
            const float raw_t = std::fmod(float(ImGui::GetTime()) * speed + 0.35f, 1.0f);
            const float t = reverse_dot ? (1.0f - raw_t) : raw_t;
            const ImVec2 dot = eval(t);
            dl->AddCircleFilled(dot, 5.5f, (col & 0x00FFFFFFu) | 0x55000000u);
            dl->AddCircleFilled(dot, 3.0f, IM_COL32(255, 255, 255, 240));
        }
        if (badge && badge[0]) {
            const ImVec2 mid = eval(0.5f);
            const ImVec2 tsz = ImGui::CalcTextSize(badge);
            const ImVec2 b0(mid.x - tsz.x * 0.5f - 7.0f, mid.y - tsz.y * 0.5f - 2.5f);
            const ImVec2 b1(mid.x + tsz.x * 0.5f + 7.0f, mid.y + tsz.y * 0.5f + 2.5f);
            dl->AddRectFilled(b0, b1, g_dark_theme ? IM_COL32(14, 28, 34, 235) : IM_COL32(240, 253, 250, 242), 9.0f);
            dl->AddRect(b0, b1, col, 9.0f, 0, 1.0f);
            dl->AddText(ImVec2(b0.x + 7.0f, b0.y + 2.5f), col, badge);
        }
    };

    // Cable 1: Local Host <-> Remote Studio (Primary + optional dashed turquoise Secondary 1+1 Failover cable!)
    const bool dual_nic_on = cfg_.dual_nic_redundancy_enabled && !cfg_.secondary_nic_ip.empty();
    draw_animated_bezier_cable(
        dl,
        ImVec2(local_pos.x + node_w, local_pos.y + node_h * (dual_nic_on ? 0.28f : 0.42f)),
        ImVec2(remote_pos.x, remote_pos.y + node_h * (dual_nic_on ? 0.28f : 0.42f)),
        col_violet, main_cable_lbl, role_running, 0.45f, cfg_.connect_role == 1);
    if (dual_nic_on) {
        draw_dashed_bezier_cable(
            dl,
            ImVec2(local_pos.x + node_w, local_pos.y + node_h * 0.52f),
            ImVec2(remote_pos.x, remote_pos.y + node_h * 0.52f),
            col_teal, "1+1 Backup · Secondary NIC", role_running || monitor_server_.is_running(),
            0.42f, cfg_.connect_role == 1);
    }

    // Cable 2: Local Host <-> Mobile Camera / Personal Monitor (Tally + Intercom + IFB)
    const bool mobile_active = !studio_cams.empty() || !mon_phones.empty() || monitor_server_.is_running();
    const char* mob_lbl = !studio_cams.empty() ? "Tally · Duplex Intercom"
                                               : (!mon_phones.empty() ? "Personal Monitor IFB" : "UDP :5020 / :5022");
    draw_animated_bezier_cable(
        dl,
        ImVec2(local_pos.x + node_w, local_pos.y + node_h * 0.68f),
        ImVec2(mobile_pos.x, mobile_pos.y + node_h * 0.50f),
        col_teal, mob_lbl, mobile_active, 0.38f, false);
    if (dual_nic_on) {
        draw_dashed_bezier_cable(
            dl,
            ImVec2(local_pos.x + node_w, local_pos.y + node_h * 0.84f),
            ImVec2(mobile_pos.x, mobile_pos.y + node_h * 0.72f),
            col_teal, nullptr, mobile_active, 0.35f, false);
    }

    // Cable 3: Audio Interface <-> Local Host (64ch Virtual Bus)
    draw_animated_bezier_cable(
        dl,
        ImVec2(audio_pos.x + node_w * 0.5f, audio_pos.y),
        ImVec2(local_pos.x + node_w * 0.5f, local_pos.y + node_h),
        col_indigo, "64ch Bus · MIDI", audio_matrix_.is_running() || duplex_running_, 0.55f, false);

    // Draw the 4 primary topology node cards
    char local_sub[96];
    std::snprintf(local_sub, sizeof(local_sub), "%s · %s", local_host_name().c_str(), local_lan_ip_.c_str());
    draw_topology_node(0, local_pos, ImVec2(node_w, node_h),
                       "LanVid Studio (This PC)", local_sub, "64ch IN/OUT · Host",
                       col_violet, false, 0);

    char audio_sub[96];
    std::snprintf(audio_sub, sizeof(audio_sub), "Virtual ASIO / CoreAudio (%zu слотів)", slots.size());
    draw_topology_node(2, audio_pos, ImVec2(node_w, node_h),
                       "Studio Audio Interface", audio_sub,
                       audio_matrix_.is_running() ? "64ch Active" : "Ready",
                       col_indigo, false, 0);

    char rem_sub[96], rem_badge[48];
    std::snprintf(rem_sub, sizeof(rem_sub), "%s:%d",
                  remote_ip.empty() ? "192.168.1.x" : remote_ip.c_str(),
                  cfg_.connect_role == 2 ? cfg_.dpx_peer_port : cfg_.dest_port);
    std::snprintf(rem_badge, sizeof(rem_badge), "%dch AoIP · %s", remote_ch, role_running ? "Synced" : "Standby");
    draw_topology_node(1, remote_pos, ImVec2(node_w, node_h),
                       remote_name.c_str(), rem_sub, rem_badge,
                       role_running ? IM_COL32(16, 185, 129, 255) : col_violet, false, 0);

    // Mobile Camera / Personal Monitor node (shows live Tally color border!)
    std::string mob_title = "iPhone / Android Studio";
    char mob_sub[96] = "Monitor :5020 · Tally :5022";
    const char* mob_badge = "Personal Monitor · Tally";
    bool mob_tally_border = false;
    ImU32 mob_tally_col = col_teal;
    if (!studio_cams.empty()) {
        const auto& c = studio_cams[0];
        mob_title = c.label.empty() ? ("CAM " + std::to_string(c.camera_id)) : c.label;
        std::snprintf(mob_sub, sizeof(mob_sub), "%s · %u%% · %d°C",
                      c.address.c_str(), unsigned(c.battery_pct), int(c.temp_c));
        if (c.tally_state == kTallyProgram) {
            mob_badge = "ON AIR (PGM) · Intercom";
            mob_tally_border = true;
            mob_tally_col = IM_COL32(239, 68, 68, 255);
        } else if (c.tally_state == kTallyPreview) {
            mob_badge = "PREVIEW (PVW) · Intercom";
            mob_tally_border = true;
            mob_tally_col = IM_COL32(16, 185, 129, 255);
        } else {
            mob_badge = "STANDBY · Duplex Intercom";
        }
    } else if (!mon_phones.empty()) {
        mob_title = mon_phones[0].name;
        std::snprintf(mob_sub, sizeof(mob_sub), "%s · %u ms", mon_phones[0].address.c_str(),
                      unsigned(mon_phones[0].latency_ms));
        mob_badge = "Personal Monitor Active";
    }
    draw_topology_node(10, mobile_pos, ImVec2(node_w, node_h),
                       mob_title.c_str(), mob_sub, mob_badge,
                       col_teal, mob_tally_border, mob_tally_col);

    ImGui::SetCursorScreenPos(ImVec2(c_p0.x, c_p1.y + 8.0f));

    // ---- BOTTOM QUICK ACTION PILL BAR ----
    auto find_slot_by_type = [&](MatrixSlotType t, MatrixSlotRole role_hint = MatrixSlotRole::kDuplex,
                                 bool check_role = false) -> int {
        for (const auto& s : slots) {
            if (s.type == t && (!check_role || s.role == role_hint)) return s.id;
        }
        return -1;
    };
    const int net_id = find_slot_by_type(MatrixSlotType::kNetworkAoip);
    const int daw_id = find_slot_by_type(MatrixSlotType::kVirtualBus);
    const int mon_id = find_slot_by_type(MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, true);

    if (status_pill_button("Слухати на ПК", cfg_.connect_role == 1, ImVec2(0, 28))) {
        cfg_.connect_role = 1;
        audio_matrix_.apply_preset(0, net_id, daw_id, mon_id);
        config_dirty_ = true;
        toast_msg_ = "Режим: Слухати на ПК (Мережа -> Навушники + DAW)";
        toast_until_us_ = now_us() + 2400000ull;
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (status_pill_button("Студійний міст 1:1", cfg_.connect_role == 2, ImVec2(0, 28))) {
        cfg_.connect_role = 2;
        audio_matrix_.apply_preset(1, net_id, daw_id, mon_id);
        config_dirty_ = true;
        toast_msg_ = "Режим: Студійний міст 1:1 (64x64 Full-Duplex)";
        toast_until_us_ = now_us() + 2400000ull;
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (status_pill_button("Камера + інтерком", tally_intercom_.is_running(), ImVec2(0, 28))) {
        if (!tally_intercom_.is_running()) tally_intercom_.start_studio(kDefaultTallyIntercomPort);
        connect_selected_node_ = 10;
        toast_msg_ = "Tally & Duplex Intercom активний на UDP :" + std::to_string(tally_intercom_.bound_port());
        toast_until_us_ = now_us() + 2400000ull;
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (status_pill_button("Очистити все", false, ImVec2(0, 28))) {
        req_open_clear_all_popup_ = true;
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (gradient_pill_button("+ Add Network Device", ImVec2(0, 28), kColAccentBlue, kColAccentInd)) {
        connect_show_manual_ip_ = !connect_show_manual_ip_;
    }

    if (connect_show_manual_ip_) {
        ImGui::Spacing();
        ImGui::SetNextItemWidth(160.0f);
        if (cfg_.connect_role == 2) {
            if (ImGui::InputText("IP партнера", dpx_peer_buf_, sizeof(dpx_peer_buf_))) config_dirty_ = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(76.0f);
            if (ImGui::InputInt("TX", &cfg_.dpx_peer_port, 0, 0)) {
                cfg_.dpx_peer_port = std::clamp(cfg_.dpx_peer_port, 1024, 65535);
                config_dirty_ = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(76.0f);
            if (ImGui::InputInt("RX", &cfg_.dpx_listen_port, 0, 0)) {
                cfg_.dpx_listen_port = std::clamp(cfg_.dpx_listen_port, 1024, 65535);
                config_dirty_ = true;
            }
        } else {
            if (ImGui::InputText("IP отримувача", ip_buf_, sizeof(ip_buf_))) config_dirty_ = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(86.0f);
            if (ImGui::InputInt("Порт", &cfg_.dest_port, 0, 0)) {
                cfg_.dest_port = std::clamp(cfg_.dest_port, 1024, 65535);
                cfg_.listen_port = cfg_.dest_port;
                config_dirty_ = true;
            }
        }
    }

    end_glass_card();

    // ---- RIGHT INSPECTOR PANEL: Студійний моніторинг & Налаштування ----
    ImGui::SameLine();
    begin_glass_card("##ConnectInspector", ImVec2(0, 0));

    // 1. CONNECTED Block
    ImGui::TextColored(kColAccentPurp, "CONNECTED");
    ImGui::SameLine(std::max(120.0f, ImGui::GetContentRegionAvail().x - 110.0f));
    ImGui::TextColored(role_running ? kColGreen : kColTextMuted,
                       role_running ? "● Synced" : "○ Standby");
    ImGui::Text("%s (%s)", remote_name.c_str(), remote_ip.empty() ? "LAN" : remote_ip.c_str());
    draw_audio_bitrate_pills("ConnectBitrate");
    ImGui::Separator();

    // 1b. NETWORK ADAPTERS & DUAL-NIC FAILOVER (Primary + Secondary)
    draw_network_adapters_section("ConnectNic", true);
    ImGui::Separator();

    // 2. STUDIO CAMERAS · TALLY & INTERCOM Block
    ImGui::TextColored(kColAccentPurp, "STUDIO CAMERAS · TALLY & INTERCOM");
    if (!tally_intercom_.is_running()) {
        if (status_pill_button("Увімкнути Tally & Intercom (UDP :5022)", false, ImVec2(-1, 28))) {
            tally_intercom_.start_studio(kDefaultTallyIntercomPort);
        }
    } else {
        uint32_t cam_id = studio_cams.empty() ? 1u : studio_cams[0].camera_id;
        uint8_t cur_tally = studio_cams.empty() ? kTallyStandby : studio_cams[0].tally_state;
        const std::string cam_lbl = studio_cams.empty() ? "CAM 1" : studio_cams[0].label;
        if (!studio_cams.empty()) {
            ImGui::TextDisabled("%s  ·  Bat %u%%  ·  %d°C  ·  %s",
                                cam_lbl.c_str(), unsigned(studio_cams[0].battery_pct),
                                int(studio_cams[0].temp_c),
                                studio_cams[0].operator_speaking ? "🎙 Operator Talking" : "Idle");
        } else {
            ImGui::TextDisabled("Очікування камер на %s:%u...", local_lan_ip_.c_str(),
                                unsigned(tally_intercom_.bound_port()));
        }
        if (status_pill_button("STBY", cur_tally == kTallyStandby, ImVec2(62, 26))) {
            tally_intercom_.studio_set_camera_tally(cam_id, kTallyStandby, cam_lbl, false);
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (status_pill_button("PVW", cur_tally == kTallyPreview, ImVec2(62, 26))) {
            tally_intercom_.studio_set_camera_tally(cam_id, kTallyPreview, cam_lbl, false);
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (status_pill_button("PGM", cur_tally == kTallyProgram, ImVec2(62, 26))) {
            tally_intercom_.studio_set_camera_tally(cam_id, kTallyProgram, cam_lbl, false);
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (status_pill_button(intercom_all_call_ ? "TALK ●" : "TALK", intercom_all_call_, ImVec2(76, 26))) {
            intercom_all_call_ = !intercom_all_call_;
            tally_intercom_.studio_set_camera_intercom(cam_id, intercom_all_call_, true, intercom_all_call_);
            tally_intercom_.set_intercom_state(intercom_all_call_, intercom_listen_muted_, intercom_ifb_gain_, 0.0f);
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (status_pill_button(intercom_latch_mode_ ? "Latch" : "PTT", intercom_latch_mode_, ImVec2(56, 26))) {
            intercom_latch_mode_ = !intercom_latch_mode_;
        }
        ImGui::TextDisabled("Director Voice:");
        ImGui::SameLine();
        draw_segmented_vu(tally_intercom_.tx_peak_dbfs(), std::max(100.0f, ImGui::GetContentRegionAvail().x - 8.0f), 10.0f);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // 3. LIVE TELEMETRY (3 Circular Donut Gauges + Volume / Ducking / Mute Panic)
    ImGui::TextColored(kColAccentPurp, "LIVE TELEMETRY");
    const double total_mbps = tx_stats_.tx_mbps + rx_stats_.rx_mbps + dpx_tx_mbps_ + dpx_rx_mbps_;
    const double loss_pct = std::clamp(receiver_running_ ? rx_stats_.loss_pct : tx_stats_.rx_loss_pct, 0.0, 100.0);
    const double health_pct = std::clamp(100.0 - loss_pct, 0.0, 100.0);
    const double est_mbps = estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec);
    const float bw_frac = float(std::clamp(total_mbps / std::max(100.0, est_mbps), 0.0, 1.0));
    const double buf_ms = actual_buf_valid_ ? actual_buf_ms_ : double(cfg_.aoip_latency_ms);
    const float lat_frac = float(std::clamp(buf_ms / 100.0, 0.05, 1.0));

    const float gw_avail = ImGui::GetContentRegionAvail().x;
    const float ggap = 8.0f;
    const float card_w = std::max(84.0f, (gw_avail - ggap * 2.0f) / 3.0f);
    const float card_h = 118.0f;

    char hlth_val[24], lat_val[24], tr_val[24];
    std::snprintf(hlth_val, sizeof(hlth_val), "%.0f%%", health_pct);
    std::snprintf(lat_val, sizeof(lat_val), "%.0f", buf_ms);
    std::snprintf(tr_val, sizeof(tr_val), "%.1f", total_mbps);

    draw_ring_gauge("Network", hlth_val, loss_pct > 0.5 ? "Loss" : "OK",
                    float(health_pct / 100.0), card_w, card_h, loss_pct > 1.0, true);
    ImGui::SameLine(0.0f, ggap);
    draw_ring_gauge("Latency", lat_val, "ms", lat_frac, card_w, card_h, buf_ms > 50.0, false);
    ImGui::SameLine(0.0f, ggap);
    draw_ring_gauge("Traffic", tr_val, "Mbps", std::max(0.06f, bw_frac), card_w, card_h, bw_frac > 0.85f, false);

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##MasterVol", &connect_master_vol_, 0.0f, 1.5f, "Volume: %.0f%%")) {
        if (receiver_) lv_receiver_set_volume(receiver_, connect_mute_panic_ ? 0.0f : connect_master_vol_);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##DuckDb", &connect_ducking_db_, -24.0f, 0.0f, "Ducking: %.0f dB");

    if (status_pill_button(connect_mute_panic_ ? "PANIC MUTED (Click to Unmute)" : "Mute / Panic",
                           connect_mute_panic_, ImVec2(-1, 28))) {
        connect_mute_panic_ = !connect_mute_panic_;
        if (receiver_) lv_receiver_set_volume(receiver_, connect_mute_panic_ ? 0.0f : connect_master_vol_);
        if (connect_mute_panic_) audio_matrix_.send_midi_panic(-1);
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Відеопотік та розширені параметри")) {
        if (cfg_.connect_role == 0) draw_sender_tab();
        else if (cfg_.connect_role == 1) draw_receiver_tab();
        else draw_duplex_tab();
    }

    end_glass_card();
}

void LanVidStudioApp::draw_devices_tab() {
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float half_w = std::max(340.0f, (total_w - 10.0f) * 0.5f);

    // ---- LEFT: AUDIO DEVICES ----
    auto slots = audio_matrix_.slots();
    begin_glass_card("##DevAudioCol", ImVec2(half_w, 0));
    draw_gradient_icon_tile("##DevAudTile", IconKind::kHeadphones, 34.0f, int(slots.size()), false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColTextMain, "Аудіопристрої та звукові карти");
    ImGui::TextDisabled("Навушники, мікрофони, ASIO-драйвери та шина DAW (%zu / %d)", slots.size(), kMatrixMaxSlots);
    ImGui::EndGroup();
    ImGui::Separator();

    if (gradient_pill_button("+ Додати аудіопристрій", ImVec2(0, 32), kColAccentBlue, kColAccentPurp)) {
        if (now_us() - last_dev_refresh_us_ > 3000000ull) refresh_devices();
        req_open_add_audio_popup_ = true;
    }
    ImGui::SameLine();
    if (status_pill_button("Оновити список", false, ImVec2(0, 32))) refresh_devices();
    ImGui::SameLine();
    const bool mx_on = audio_matrix_.is_running();
    if (!mx_on) {
        if (gradient_pill_button("Увімкнути звук", ImVec2(-1, 32), kColAccentBlue, kColGreen)) {
            stop_vdev_tx();
            vdev_rx_.stop();
            audio_matrix_.start();
            toast_msg_ = "Аудіоматрицю запущено (48 кГц)";
            toast_until_us_ = now_us() + 2500000ull;
        }
    } else {
        if (gradient_pill_button("Зупинити звук", ImVec2(-1, 32), kColPink, kColRed)) {
            audio_matrix_.stop();
        }
    }

    ImGui::Spacing();
    for (auto s : slots) {
        ImGui::PushID(s.id);
        const auto st = audio_matrix_.slot_status(s.id);
        begin_glass_card("##DevSlotCard", ImVec2(0, 96.0f), true,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const char* role_lbl = (s.type == MatrixSlotType::kVirtualBus)
                                   ? "Віртуальна шина DAW"
                                   : (s.type == MatrixSlotType::kNetworkAoip
                                          ? "Мережевий потік (AoIP)"
                                          : (s.type == MatrixSlotType::kExternalAsio ? "Зовнішній ASIO" : "Системний звук"));
        const IconKind ik = (s.type == MatrixSlotType::kSystemAudio) ? IconKind::kHeadphones : IconKind::kWaveform;
        draw_gradient_icon_tile("##DevSlotIco", ik, 38.0f, s.channels, false);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(kColTextMain, "%s", s.name.c_str());
        ImGui::TextColored(st.connected ? kColGreen : kColTextMuted, "%s",
                           s.active ? (st.connected ? "Активний" : "Готовий") : "Вимкнено");
        ImGui::SameLine();
        ImGui::TextDisabled("· %s · %u Гц", role_lbl, st.device_sample_rate);
        ImGui::EndGroup();

        const float card_w = ImGui::GetWindowWidth();
        if (s.type != MatrixSlotType::kVirtualBus && s.type != MatrixSlotType::kNetworkAoip) {
            ImGui::SameLine(std::max(140.0f, card_w - 94.0f));
            if (ImGui::SmallButton("x##RmSlot")) {
                audio_matrix_.remove_slot(s.id);
                config_dirty_ = true;
                end_glass_card();
                ImGui::PopID();
                break;
            }
        }
        ImGui::SameLine(std::max(170.0f, card_w - 60.0f));
        bool act = s.active;
        if (studio_toggle("##DevAct", &act, kColAccentInd)) {
            s.active = act;
            audio_matrix_.update_slot(s.id, s);
            config_dirty_ = true;
        }

        const float vu_w = ImGui::GetContentRegionAvail().x;
        const float vu_db = (s.role == MatrixSlotRole::kOutput) ? st.peak_out_dbfs
                                                                : std::max(st.peak_in_dbfs, st.peak_out_dbfs);
        draw_segmented_vu(vu_db, vu_w, 10.0f);
        end_glass_card();
        ImGui::PopID();
    }
    end_glass_card();

    // ---- RIGHT: MIDI DEVICES ----
    ImGui::SameLine();
    const auto mslots = audio_matrix_.midi_slots();
    begin_glass_card("##DevMidiCol", ImVec2(0, 0));
    draw_gradient_icon_tile("##DevMidTile", IconKind::kMidiPlug, 34.0f, int(mslots.size()), false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColTextMain, "MIDI-контролери та порти");
    ImGui::TextDisabled("Клавіатури, електробарабани, DAW та мережевий MIDI (%zu / %d)",
                        mslots.size(), kMatrixMaxMidiSlots);
    ImGui::EndGroup();
    ImGui::Separator();

    if (gradient_pill_button("+ Додати MIDI-пристрій", ImVec2(0, 32), kColAccentInd, kColAccentPurp)) {
        if (now_us() - last_dev_refresh_us_ > 3000000ull) refresh_devices();
        req_open_add_midi_popup_ = true;
    }
    ImGui::SameLine();
    if (gradient_pill_button("PANIC (Скинути всі ноти)", ImVec2(-1, 32), kColPink, kColRed)) {
        audio_matrix_.send_midi_panic(-1);
        broadcast_control_sync(true);
        toast_msg_ = "MIDI PANIC: надіслано All Notes Off на всі 16 каналів";
        toast_until_us_ = now_us() + 2500000ull;
    }

    ImGui::Spacing();
    // Status banner for the system virtual DAW MIDI port ("LanVid MIDI")
    for (const auto& ms : mslots) {
        if (ms.type != MidiSlotType::kVirtualDawMidi) continue;
        const auto mst = audio_matrix_.midi_slot_status(ms.id);
        begin_glass_card("##VirtDawMidiBanner", ImVec2(0, 48.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(mst.connected ? kColGreen : kColAmber, "●");
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(kColTextMain, "Віртуальний порт для DAW:");
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(mst.connected ? kColAccentPurp : kColTextMuted, "%s",
                           mst.status_text.empty() ? "LanVid MIDI" : mst.status_text.c_str());
        end_glass_card();
        ImGui::Spacing();
        break;
    }

    const uint64_t now_t = now_us();
    for (auto ms : mslots) {
        ImGui::PushID(4000 + ms.id);
        const auto mst = audio_matrix_.midi_slot_status(ms.id);
        const bool blinking = (mst.last_activity_us > 0 && now_t - mst.last_activity_us < 180000ull);
        begin_glass_card("##DevMidiCard", ImVec2(0, 74.0f), true, ImGuiWindowFlags_NoScrollbar);

        const int ev_badge = (mst.events_count > 0) ? int(std::min<uint64_t>(mst.events_count, 99)) : -1;
        draw_gradient_icon_tile("##DevMidIco", IconKind::kMidiPlug, 36.0f, ev_badge, false);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(blinking ? kColGreen : (mst.connected ? kColAccentInd : kColTextMuted), "●");
        ImGui::SameLine(0.0f, 5.0f);
        ImGui::TextColored(kColTextMain, "%s", ms.name.c_str());

        const char* mtag = (ms.type == MidiSlotType::kMidiInput)
                               ? "Вхід MIDI"
                               : (ms.type == MidiSlotType::kMidiOutput
                                      ? "Вихід MIDI"
                                      : (ms.type == MidiSlotType::kVirtualDawMidi
                                             ? "DAW Віртуальний (In/Out)"
                                             : (ms.type == MidiSlotType::kNetworkMidiTx ? "У мережу (MIDI2)"
                                                                                        : "З мережі (MIDI2)")));
        ImGui::TextColored(blinking ? kColAccentPurp : kColTextMuted, "%s · %llu подій",
                           mtag, (unsigned long long)mst.events_count);
        if (ms.type == MidiSlotType::kMidiOutput || ms.type == MidiSlotType::kNetworkMidiTx ||
            ms.type == MidiSlotType::kVirtualDawMidi) {
            ImGui::SameLine();
            if (ImGui::SmallButton("PANIC")) audio_matrix_.send_midi_panic(ms.id);
        }
        if (ms.type == MidiSlotType::kMidiInput || ms.type == MidiSlotType::kMidiOutput) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                audio_matrix_.remove_midi_slot(ms.id);
                config_dirty_ = true;
                ImGui::EndGroup();
                end_glass_card();
                ImGui::PopID();
                break;
            }
        }
        ImGui::EndGroup();

        ImGui::SameLine(std::max(160.0f, ImGui::GetWindowWidth() - 62.0f));
        bool act = ms.active;
        if (studio_toggle("##DevMidAct", &act, kColAccentPurp)) {
            ms.active = act;
            audio_matrix_.update_midi_slot(ms.id, ms);
            config_dirty_ = true;
        }
        end_glass_card();
        ImGui::PopID();
    }
    end_glass_card();
}

void LanVidStudioApp::draw_settings_tab() {
    begin_glass_card("##SettingsRoot", ImVec2(0, 0));

    // Top header + Theme Switch in top-right corner
    draw_gradient_icon_tile("##SetGear", IconKind::kGear, 34.0f, -1, false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColTextMain, "Налаштування LanVid Studio");
    ImGui::TextDisabled("Зрозумілі параметри якості звуку, затримки та оформлення");
    ImGui::EndGroup();

    // Theme switcher pinned to top-right corner
    const char* theme_lbl = cfg_.dark_theme ? "Тема: Темна Студія" : "Тема: Світла Лаванда";
    const float tw = ImGui::CalcTextSize(theme_lbl).x + 56.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - tw - 20.0f));
    ImGui::BeginGroup();
    ImGui::TextColored(kColAccentPurp, "%s", theme_lbl);
    ImGui::SameLine();
    if (studio_toggle("##ThemeToggle", &cfg_.dark_theme, kColAccentInd)) {
        apply_theme(cfg_.dark_theme);
        config_dirty_ = true;
    }
    ImGui::EndGroup();

    ImGui::Separator();
    ImGui::Spacing();

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float col_gap = 14.0f;
    const float left_w = std::max(380.0f, (avail_w - col_gap) * 0.54f);

    // ---- LEFT COLUMN: Per-Channel Bitrate Pills + Lossless Quality Cards + Channel Count Pills ----
    ImGui::BeginGroup();
    ImGui::TextColored(kColAccentPurp, "1. Бітрейт кодування на канал (Lossless / kbps)");
    ImGui::TextDisabled("Оберіть цільовий бітрейт на один канал — перемикається на льоту без обриву звуку");
    ImGui::Spacing();
    draw_audio_bitrate_pills("SettingsBitrate");
    ImGui::Spacing();

    if (cfg_.audio_bitrate_kbps == 0) {
        struct QualityCard {
            int codec;
            const char* title;
            const char* desc;
        };
        const QualityCard qcards[3] = {
            {0, "Максимальна (Lossless 32-bit Float)", "Для кабелю Ethernet (~98 Мбіт/с на 64 кан.)"},
            {1, "Студійна (Lossless 24-bit PCM)",      "Оптимально для швидкого Wi-Fi 6 (-25% трафіку)"},
            {2, "Збалансована (PCM 16-bit)",           "Стабільно для звичайного Wi-Fi (-50% трафіку)"},
        };
        for (const auto& q : qcards) {
            ImGui::PushID(q.codec);
            if (selectable_choice_card("##QCard", q.title, q.desc, cfg_.audio_codec == q.codec, ImVec2(left_w, 56.0f))) {
                cfg_.audio_codec = q.codec;
                if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
                if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
                config_dirty_ = true;
            }
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Big channel count pill buttons: 2 (Стерео), 8, 16, 32, 64
    ImGui::TextColored(kColAccentPurp, "2. Кількість каналів звуку");
    ImGui::TextDisabled("Скільки доріжок передавати одночасно");
    ImGui::Spacing();
    const char* ch_labels[5] = {"2 (Стерео)", "8", "16", "32", "64"};
    for (int i = 0; i < 5; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 8.0f);
        if (status_pill_button(ch_labels[i], cfg_.aoip_channels == kBusChoices[i],
                               ImVec2(i == 0 ? 116.0f : 62.0f, 36.0f))) {
            cfg_.aoip_channels = kBusChoices[i];
            tone_channels_.store(cfg_.aoip_channels);
            if (receiver_) apply_receiver_bus();
            config_dirty_ = true;
            broadcast_control_sync();
        }
    }
    ImGui::EndGroup();

    // ---- RIGHT COLUMN: Latency Mode Cards + Buffer & Stream Options ----
    ImGui::SameLine(0.0f, col_gap);
    ImGui::BeginGroup();
    const float right_w = std::max(320.0f, ImGui::GetContentRegionAvail().x);
    ImGui::TextColored(kColAccentPurp, "3. Режим затримки");
    ImGui::TextDisabled("Перемикається на льоту без переривання звуку");
    ImGui::Spacing();

    char auto_desc[128];
    if (actual_buf_valid_) {
        std::snprintf(auto_desc, sizeof(auto_desc),
                      "Сам підлаштовується під Wi-Fi (поточна затримка: %.0f мс)", actual_buf_ms_);
    } else {
        std::snprintf(auto_desc, sizeof(auto_desc),
                      "Сам підлаштовується під якість Wi-Fi (ціль: %d мс)", cfg_.aoip_latency_ms);
    }
    if (selectable_choice_card("##LatAuto", "Авто для Wi-Fi (Без обривів)", auto_desc,
                               cfg_.adaptive_jitter != 0, ImVec2(right_w, 64.0f))) {
        cfg_.adaptive_jitter = 1;
        apply_latency_mode();
        config_dirty_ = true;
        broadcast_control_sync();
    }
    if (selectable_choice_card("##LatFixed", "Фіксована для кабелю (Для живої гри)",
                               "Жорстко тримає вибрані 5 / 10 / 20 мс без змін",
                               cfg_.adaptive_jitter == 0, ImVec2(right_w, 64.0f))) {
        cfg_.adaptive_jitter = 0;
        apply_latency_mode();
        config_dirty_ = true;
        broadcast_control_sync();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Цільова затримка:");
    const int lat_presets[4] = {5, 10, 20, 50};
    const char* lat_lbls[4] = {"5 мс", "10 мс", "20 мс", "50 мс"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 8.0f);
        if (status_pill_button(lat_lbls[i], cfg_.aoip_latency_ms == lat_presets[i], ImVec2(72.0f, 32.0f))) {
            cfg_.aoip_latency_ms = lat_presets[i];
            if (sender_) lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
            if (receiver_) lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
            if (duplex_running_) lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
            config_dirty_ = true;
            broadcast_control_sync();
        }
    }
    ImGui::SetNextItemWidth(right_w);
    if (ImGui::SliderInt("##SetLatSlider", &cfg_.aoip_latency_ms, 2, 2000, "Точна затримка: %d мс",
                         ImGuiSliderFlags_Logarithmic)) {
        if (sender_) lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
        if (receiver_) lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
        if (duplex_running_) lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    draw_actual_buffer_line();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(kColAccentPurp, "4. Режим потоку та додаткові налаштування");
    ImGui::Spacing();
    draw_stream_mode_selector("SetStreamMode");
    ImGui::TextDisabled("«Тільки MIDI» вимикає аудіо/відео трафік і вмикає ультранизьку затримку ~1.5 мс");

    ImGui::Spacing();
    if (studio_toggle("##SetWifiRedund", &cfg_.wifi_redundancy, kColAccentInd)) {
        monitor_server_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
        tally_intercom_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
        if (sender_) lv_sender_set_wifi_redundancy(sender_, cfg_.wifi_redundancy ? 1 : 0, 1500);
        if (duplex_running_) lv_duplex_set_wifi_redundancy(duplex_, cfg_.wifi_redundancy ? 1 : 0, 1500);
        config_dirty_ = true;
        toast_msg_ = cfg_.wifi_redundancy
                         ? "Wi-Fi Redundancy (1+1 Duplicate · +1.5 мс) увімкнено — 0 NACK-пауз при втратах Wi-Fi"
                         : "Wi-Fi Redundancy (1+1 Duplicate) вимкнено";
        toast_until_us_ = now_us() + 2800000ull;
    }
    ImGui::SameLine();
    ImGui::TextColored(kColTextMain, "Wi-Fi Redundancy (1+1 Duplicate · +1.5 мс)");
    ImGui::TextDisabled("Проактивно дублює аудіо, Tally та Intercom пакети (+1.5 мс, DSCP CS7/EF) — без пауз NACK");

    ImGui::Spacing();
    if (studio_toggle("##SetFbGuard", &cfg_.matrix_feedback_guard, kColAccentPurp)) {
        audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(kColTextMain, "Захист від петлі зворотного зв'язку");
    ImGui::TextDisabled("Блокує випадкове замикання виходу пристрою на власний вхід");

    ImGui::Spacing();
    if (studio_toggle("##SetSyncPeer", &cfg_.sync_settings, kColGreen)) {
        config_dirty_ = true;
        if (cfg_.sync_settings) broadcast_control_sync();
    }
    ImGui::SameLine();
    ImGui::TextColored(kColTextMain, "Синхронізувати налаштування між пристроями");
    ImGui::TextDisabled("Миттєво вирівнює бітрейт, канали, затримку та PANIC між Mac і ПК");
    ImGui::BeginDisabled(!cfg_.sync_settings);
    if (ImGui::Checkbox("Також синхронізувати маршрути матриці", &cfg_.sync_matrix_routing)) {
        config_dirty_ = true;
        if (cfg_.sync_matrix_routing) broadcast_control_sync();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("Розмір буфера аудіоматриці:");
    const int bufs[3] = {128, 256, 512};
    const char* buf_l[3] = {"128 (2.7 мс)", "256 (5.3 мс)", "512 (10.7 мс)"};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 8.0f);
        if (status_pill_button(buf_l[i], cfg_.matrix_buffer_size == bufs[i], ImVec2(106.0f, 30.0f))) {
            cfg_.matrix_buffer_size = bufs[i];
            audio_matrix_.set_buffer_size(cfg_.matrix_buffer_size);
            config_dirty_ = true;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_network_adapters_section("SettingsNic", false);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_grid_engine_section();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(kColAccentPurp, "Про програму");
    ImGui::TextColored(kColTextMain, "LanVid Studio v%s", LANVID_VERSION_STRING);
    ImGui::TextDisabled("Classic UDP/Opus + LanVid Grid L2 (256ch · 0x88B6 · DSP Server) · Soft Lavender UI");
    ImGui::EndGroup();

    end_glass_card();
}

void LanVidStudioApp::apply_engine_mode(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (cfg_.network_engine_mode == mode) return;
    cfg_.network_engine_mode = mode;
    config_dirty_ = true;
    if (mode == 0) {
        stop_grid_node();
        toast_msg_ = "Режим рушія: Classic (UDP / IP Layer 3 · Wi-Fi сумісний)";
    } else {
        const bool ok = start_grid_node();
        toast_msg_ = (mode == 1 ? "Режим рушія: LanVid Grid (Layer 2 Ethernet · 256ch)"
                                : "Режим рушія: Hybrid (Grid L2 Ethernet + Classic UDP Bridge)") +
                     std::string(ok ? " — Активний" : (" — " + grid_last_err_));
    }
    toast_until_us_ = now_us() + 3000000ull;
}

bool LanVidStudioApp::start_grid_node() {
    stop_grid_node();
    grid_adapters_ = lv::grid::list_l2_adapters();
    lv::grid::l2_native_backend_available(&grid_backend_status_);

    lv::grid::L2AdapterInfo chosen{};
    bool found = false;
    for (const auto& a : grid_adapters_) {
        if (!cfg_.grid_nic_name.empty() && a.device_name == cfg_.grid_nic_name) {
            chosen = a;
            found = true;
            break;
        }
    }
    if (!found) found = lv::grid::pick_default_l2_adapter(grid_adapters_, chosen);  // wired Ethernet with carrier
    if (!found) {
        grid_last_err_ = "Не знайдено Ethernet-адаптера";
        return false;
    }
    cfg_.grid_nic_name = chosen.device_name;

    std::string err;
    auto transport = lv::grid::create_native_l2_transport(chosen, &err);
    if (!transport) {
        grid_last_err_ = err.empty() ? grid_backend_status_ : err;
        return false;
    }

    lv::grid::GridNodeConfig gcfg{};
    gcfg.node_name = "LanVid Studio (" + local_host_name() + ")";
    gcfg.role = lv::grid::GridNodeRole::kStudioHost;
    gcfg.max_channels = uint16_t(std::clamp(cfg_.grid_channels, 64, 256));
    gcfg.sample_rate = 48000;
    gcfg.vlan = {cfg_.grid_vlan_enabled, uint16_t(std::clamp(cfg_.grid_vlan_id, 0, 4094)),
                 uint8_t(std::clamp(cfg_.grid_vlan_pcp, 0, 7))};
    gcfg.clock_master = cfg_.grid_clock_master;

    grid_node_ = std::make_unique<lv::grid::GridNode>(std::move(transport), gcfg);
    grid_node_->broadcast_discovery_probe();
    grid_last_err_.clear();
    return true;
}

void LanVidStudioApp::stop_grid_node() {
    grid_node_.reset();
}

void LanVidStudioApp::refresh_network_interfaces() {
    net_ifaces_ = enumerate_network_interfaces(/*include_disconnected=*/false, /*show_all=*/false);
    // Reconcile saved adapter IDs to their latest IPv4 addresses
    if (!cfg_.primary_nic_id.empty()) {
        bool found = false;
        for (const auto& nic : net_ifaces_) {
            if (nic.id == cfg_.primary_nic_id || nic.ipv4_address == cfg_.primary_nic_ip) {
                cfg_.primary_nic_id = nic.id;
                cfg_.primary_nic_ip = nic.ipv4_address;
                local_lan_ip_ = nic.ipv4_address;
                found = true;
                break;
            }
        }
        if (!found) cfg_.primary_nic_ip.clear();
    } else if (!net_ifaces_.empty()) {
        local_lan_ip_ = net_ifaces_[0].ipv4_address;
    }
    if (!cfg_.secondary_nic_id.empty()) {
        bool found = false;
        for (const auto& nic : net_ifaces_) {
            if (nic.id == cfg_.secondary_nic_id || nic.ipv4_address == cfg_.secondary_nic_ip) {
                cfg_.secondary_nic_id = nic.id;
                cfg_.secondary_nic_ip = nic.ipv4_address;
                found = true;
                break;
            }
        }
        if (!found) {
            cfg_.secondary_nic_ip.clear();
            cfg_.dual_nic_redundancy_enabled = false;
        }
    }
}

void LanVidStudioApp::apply_network_interfaces() {
    const std::string pri_ip = cfg_.primary_nic_ip;
    const std::string sec_ip = cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : std::string();
    if (!pri_ip.empty()) local_lan_ip_ = pri_ip;
    else if (!net_ifaces_.empty()) local_lan_ip_ = net_ifaces_[0].ipv4_address;

    monitor_server_.set_network_interfaces(pri_ip, sec_ip);
    if (sender_) lv_sender_set_network_interfaces(sender_, pri_ip.c_str(), sec_ip.c_str());
    if (duplex_) lv_duplex_set_network_interfaces(duplex_, pri_ip.c_str(), sec_ip.c_str());
    config_dirty_ = true;
}

void LanVidStudioApp::draw_network_adapters_section(const char* id_prefix, bool compact) {
    ImGui::PushID(id_prefix);

    // Update per-NIC live TX Mbps & failover packet counters (~2 Hz)
    const uint64_t now = now_us();
    if (now - nic_rate_us_ >= 500'000ull) {
        uint64_t sender_fp = 0, sender_sec_b = 0;
        if (sender_) lv_sender_get_failover_stats(sender_, &sender_fp, &sender_sec_b);
        const uint64_t pri_b = monitor_server_.primary_tx_bytes();
        const uint64_t sec_b = monitor_server_.secondary_tx_bytes() + sender_sec_b;
        const double dt_s = (nic_rate_us_ > 0) ? double(now - nic_rate_us_) / 1e6 : 0.5;
        if (nic_rate_us_ > 0 && dt_s > 0.05) {
            const uint64_t d_pri = (pri_b >= nic_prev_pri_bytes_) ? (pri_b - nic_prev_pri_bytes_) : 0;
            const uint64_t d_sec = (sec_b >= nic_prev_sec_bytes_) ? (sec_b - nic_prev_sec_bytes_) : 0;
            nic_pri_tx_mbps_ = double(d_pri) * 8.0 / (dt_s * 1e6) + tx_stats_.tx_mbps + dpx_tx_mbps_;
            nic_sec_tx_mbps_ = double(d_sec) * 8.0 / (dt_s * 1e6);
        }
        nic_prev_pri_bytes_ = pri_b;
        nic_prev_sec_bytes_ = sec_b;
        nic_failover_pkts_ = monitor_server_.failover_packets() + sender_fp;
        nic_rate_us_ = now;
    }

    auto format_nic_label = [](const NetworkInterfaceInfo& nic) -> std::string {
        char buf[256];
        if (nic.link_speed_mbps > 0) {
            std::snprintf(buf, sizeof(buf), "[%s] %s — %s (%u Mbps) • %s",
                          network_interface_type_tag(nic.type), nic.friendly_name.c_str(),
                          nic.ipv4_address.c_str(), unsigned(nic.link_speed_mbps),
                          nic.is_up ? "Active" : "Down");
        } else {
            std::snprintf(buf, sizeof(buf), "[%s] %s — %s • %s",
                          network_interface_type_tag(nic.type), nic.friendly_name.c_str(),
                          nic.ipv4_address.c_str(), nic.is_up ? "Active" : "Down");
        }
        return buf;
    };

    ImGui::TextColored(kColAccentPurp, compact ? "МЕРЕЖЕВІ АДАПТЕРИ (PRIMARY / 1+1)"
                                               : "Мережеві адаптери та Резервування (Ethernet / Wi-Fi)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Оновити NICs")) {
        refresh_network_interfaces();
        toast_msg_ = "Знайдено активних адаптерів: " + std::to_string(net_ifaces_.size());
        toast_until_us_ = now_us() + 2200000ull;
    }

    // 1. Primary Stream NIC dropdown
    std::string pri_preview = "Автоматично (Системний маршрут за замовчуванням)";
    bool pri_is_up = true;
    for (const auto& nic : net_ifaces_) {
        if (!cfg_.primary_nic_id.empty() && nic.id == cfg_.primary_nic_id) {
            pri_preview = format_nic_label(nic);
            pri_is_up = nic.is_up;
            break;
        }
    }
    if (!compact) ImGui::TextDisabled("Основна мережева карта (Primary Stream NIC):");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##PrimaryNicCombo", pri_preview.c_str())) {
        const bool auto_sel = cfg_.primary_nic_id.empty();
        if (ImGui::Selectable("Автоматично (Системний маршрут за замовчуванням)", auto_sel)) {
            cfg_.primary_nic_id.clear();
            cfg_.primary_nic_ip.clear();
            apply_network_interfaces();
        }
        for (const auto& nic : net_ifaces_) {
            const bool sel = (!cfg_.primary_nic_id.empty() && nic.id == cfg_.primary_nic_id);
            const std::string lbl = format_nic_label(nic);
            if (ImGui::Selectable(lbl.c_str(), sel)) {
                cfg_.primary_nic_id = nic.id;
                cfg_.primary_nic_ip = nic.ipv4_address;
                if (cfg_.secondary_nic_id == nic.id) {
                    cfg_.secondary_nic_id.clear();
                    cfg_.secondary_nic_ip.clear();
                    cfg_.dual_nic_redundancy_enabled = false;
                }
                apply_network_interfaces();
                toast_msg_ = "Основний адаптер: " + nic.friendly_name + " (" + nic.ipv4_address + ")";
                toast_until_us_ = now_us() + 2600000ull;
            }
        }
        ImGui::EndCombo();
    }

    // 2. Secondary / Redundant Failover NIC dropdown
    std::string sec_preview = cfg_.wifi_redundancy
                                  ? "1+1 у тій самій мережі (+1.5 мс)"
                                  : "Вимкнено (Тільки основний канал)";
    bool sec_is_up = false;
    for (const auto& nic : net_ifaces_) {
        if (cfg_.dual_nic_redundancy_enabled && !cfg_.secondary_nic_id.empty() && nic.id == cfg_.secondary_nic_id) {
            sec_preview = format_nic_label(nic);
            sec_is_up = nic.is_up;
            break;
        }
    }
    if (!compact) ImGui::TextDisabled("Резервна мережева карта (Redundant / Failover NIC):");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##SecondaryNicCombo", sec_preview.c_str())) {
        const bool off_sel = !cfg_.dual_nic_redundancy_enabled || cfg_.secondary_nic_id.empty();
        if (ImGui::Selectable("Вимкнено (Тільки основний канал)", off_sel && !cfg_.wifi_redundancy)) {
            cfg_.secondary_nic_id.clear();
            cfg_.secondary_nic_ip.clear();
            cfg_.dual_nic_redundancy_enabled = false;
            apply_network_interfaces();
        }
        if (ImGui::Selectable("1+1 у тій самій мережі (Wi-Fi Duplicate +1.5 мс)", off_sel && cfg_.wifi_redundancy)) {
            cfg_.secondary_nic_id.clear();
            cfg_.secondary_nic_ip.clear();
            cfg_.dual_nic_redundancy_enabled = false;
            cfg_.wifi_redundancy = true;
            monitor_server_.set_wifi_redundancy(true, 1500);
            tally_intercom_.set_wifi_redundancy(true, 1500);
            if (sender_) lv_sender_set_wifi_redundancy(sender_, 1, 1500);
            if (duplex_running_) lv_duplex_set_wifi_redundancy(duplex_, 1, 1500);
            apply_network_interfaces();
        }
        for (const auto& nic : net_ifaces_) {
            if (!cfg_.primary_nic_id.empty() && nic.id == cfg_.primary_nic_id) continue;
            const bool sel = cfg_.dual_nic_redundancy_enabled && (nic.id == cfg_.secondary_nic_id);
            const std::string lbl = format_nic_label(nic);
            if (ImGui::Selectable(lbl.c_str(), sel)) {
                cfg_.secondary_nic_id = nic.id;
                cfg_.secondary_nic_ip = nic.ipv4_address;
                cfg_.dual_nic_redundancy_enabled = true;
                apply_network_interfaces();
                toast_msg_ = "Dual-NIC 1+1 Failover увімкнено: резерв " + nic.friendly_name;
                toast_until_us_ = now_us() + 2800000ull;
            }
        }
        ImGui::EndCombo();
    }

    // 3. Live link status indicators (UP/DOWN, TX Mbps, Failover Packets)
    ImGui::TextColored(pri_is_up ? kColGreen : kColRed, "● Primary %s", pri_is_up ? "UP" : "DOWN");
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f Mbps", nic_pri_tx_mbps_);
    if (cfg_.dual_nic_redundancy_enabled && !cfg_.secondary_nic_ip.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(sec_is_up ? kColAccentBlue : kColRed, "  ● Backup %s", sec_is_up ? "UP" : "DOWN");
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f Mbps · Failover: %llu", nic_sec_tx_mbps_,
                            static_cast<unsigned long long>(nic_failover_pkts_));
    } else if (!compact) {
        ImGui::SameLine();
        ImGui::TextDisabled("  |  Адаптерів: %zu  |  Failover: %llu", net_ifaces_.size(),
                            static_cast<unsigned long long>(nic_failover_pkts_));
    }

    ImGui::PopID();
}

void LanVidStudioApp::draw_grid_engine_section() {
    ImGui::TextColored(kColAccentPurp, "5. Мережевий рушій: Classic (UDP) ↔ LanVid Grid (Layer 2 · 256ch)");
    ImGui::TextDisabled("Повна сумісність: стара система (UDP/Wi-Fi) працює без змін, перемикання в 1 клік");
    ImGui::Spacing();

    if (status_pill_button("Classic (UDP/Wi-Fi)", cfg_.network_engine_mode == 0, ImVec2(0, 30))) apply_engine_mode(0);
    ImGui::SameLine(0.0f, 6.0f);
    if (status_pill_button("Grid (L2 Ethernet · 256ch)", cfg_.network_engine_mode == 1, ImVec2(0, 30))) apply_engine_mode(1);
    ImGui::SameLine(0.0f, 6.0f);
    if (status_pill_button("Hybrid (L2 + L3 Bridge)", cfg_.network_engine_mode == 2, ImVec2(0, 30))) apply_engine_mode(2);

    if (cfg_.network_engine_mode != 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("L2 Backend: %s", grid_backend_status_.c_str());
        if (!grid_last_err_.empty()) {
            ImGui::TextColored(kColAmber, "Статус L2: %s", grid_last_err_.c_str());
        }

        // Adapter picker
        std::string cur_nic = "Оберіть Ethernet-карту";
        for (const auto& a : grid_adapters_) {
            if (a.device_name == cfg_.grid_nic_name) {
                cur_nic = a.friendly_name + " (" + a.mac.to_string() + ")";
                break;
            }
        }
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("NIC (Layer 2)", cur_nic.c_str())) {
            for (const auto& a : grid_adapters_) {
                const std::string lbl = a.friendly_name + " [" + a.mac.to_string() + "]";
                if (ImGui::Selectable(lbl.c_str(), a.device_name == cfg_.grid_nic_name)) {
                    cfg_.grid_nic_name = a.device_name;
                    config_dirty_ = true;
                    start_grid_node();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Сканувати L2")) {
            if (!grid_node_) start_grid_node();
            if (grid_node_) grid_node_->broadcast_discovery_probe();
        }

        // Channel count (64 / 128 / 256)
        ImGui::TextDisabled("Канали Grid L2:");
        for (int ch : {64, 128, 256}) {
            ImGui::SameLine(0.0f, 6.0f);
            char lb[24];
            std::snprintf(lb, sizeof(lb), "%d ch", ch);
            if (status_pill_button(lb, cfg_.grid_channels == ch, ImVec2(64, 24))) {
                cfg_.grid_channels = ch;
                config_dirty_ = true;
            }
        }

        if (ImGui::Checkbox("L2 Clock Master (синхроімпульс 1 мс)", &cfg_.grid_clock_master)) {
            config_dirty_ = true;
            if (grid_node_) grid_node_->set_clock_master(cfg_.grid_clock_master);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("802.1Q / 802.1p QoS (PCP=6)", &cfg_.grid_vlan_enabled)) {
            config_dirty_ = true;
            if (grid_node_) {
                grid_node_->set_vlan_qos({cfg_.grid_vlan_enabled, uint16_t(cfg_.grid_vlan_id),
                                          uint8_t(cfg_.grid_vlan_pcp)});
            }
        }

        if (grid_node_ && grid_node_->is_open()) {
            const auto peers = grid_node_->discovered_nodes();
            const auto clk = grid_node_->clock_status();
            ImGui::TextColored(kColGreen, "● Grid L2 Активний (MAC %s) · Clock: %s · Знайдено L2-вузлів: %zu",
                               grid_node_->mac().to_string().c_str(),
                               clk.is_master ? "MASTER" : (clk.locked ? "LOCKED" : "FREE"),
                               peers.size());
            for (const auto& p : peers) {
                const char* r = (p.role == lv::grid::GridNodeRole::kDspServer) ? "DSP Server"
                              : (p.role == lv::grid::GridNodeRole::kDawDriver) ? "DAW Driver"
                              : (p.role == lv::grid::GridNodeRole::kStageboxIo) ? "Stagebox"
                                                                                : "Studio Host";
                ImGui::TextDisabled("  • %s [%s] · %s · %u ch · CPU %.1f%%",
                                    p.name.c_str(), p.mac.to_string().c_str(), r,
                                    unsigned(p.max_channels), p.cpu_load_pct);
            }
        }
    }
}

void LanVidStudioApp::draw_patchbay_tab() {
    auto slots = audio_matrix_.slots();
    auto find_slot_by_type = [&](MatrixSlotType t, MatrixSlotRole role_hint = MatrixSlotRole::kDuplex,
                                 bool check_role = false) -> int {
        for (const auto& s : slots) {
            if (s.type == t && (!check_role || s.role == role_hint)) return s.id;
        }
        return -1;
    };
    const int net_id = find_slot_by_type(MatrixSlotType::kNetworkAoip);
    const int daw_id = find_slot_by_type(MatrixSlotType::kVirtualBus);
    const int mon_id = find_slot_by_type(MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, true);

    begin_glass_card("##PatchbayRoot", ImVec2(0, 0));

    // ---- 1. QUICK ROUTINGS (1-Click Scenarios) ----
    ImGui::TextColored(kColAccentPurp, "Швидкі сценарії в 1 клік");
    ImGui::Spacing();
    const float qgap = 10.0f;
    const float qw = std::max(145.0f, (ImGui::GetContentRegionAvail().x - qgap * 3.0f) / 4.0f);
    if (selectable_choice_card("##QrListen", "Слухати на цьому ПК", "Мережа -> Навушники + DAW",
                               matrix_active_preset_ == 0, ImVec2(qw, 56.0f))) {
        matrix_active_preset_ = 0;
        matrix_patch_mode_ = 0;
        audio_matrix_.apply_preset(0, net_id, daw_id, mon_id);
        config_dirty_ = true;
        toast_msg_ = "Сценарій: Мережа -> Навушники + DAW";
        toast_until_us_ = now_us() + 2500000ull;
    }
    ImGui::SameLine(0.0f, qgap);
    if (selectable_choice_card("##QrBridge", "Студійний міст 1:1", "DAW <-> Мережа + Моніторинг",
                               matrix_active_preset_ == 1, ImVec2(qw, 56.0f))) {
        matrix_active_preset_ = 1;
        matrix_patch_mode_ = 0;
        audio_matrix_.apply_preset(1, net_id, daw_id, mon_id);
        config_dirty_ = true;
        toast_msg_ = "Сценарій: Студійний міст 1:1 (DAW <-> Мережа)";
        toast_until_us_ = now_us() + 2500000ull;
    }
    ImGui::SameLine(0.0f, qgap);
    if (selectable_choice_card("##QrMidi", "MIDI-контролер у DAW", "LanVid MIDI (DAW) <-> Мережа",
                               matrix_active_preset_ == 3, ImVec2(qw, 56.0f))) {
        matrix_active_preset_ = 3;
        matrix_patch_mode_ = 1;
        const auto mslots = audio_matrix_.midi_slots();
        for (const auto& s : mslots) {
            if (s.type != MidiSlotType::kMidiInput && s.type != MidiSlotType::kNetworkMidiRx &&
                s.type != MidiSlotType::kVirtualDawMidi)
                continue;
            for (const auto& d : mslots) {
                if (d.type != MidiSlotType::kMidiOutput && d.type != MidiSlotType::kNetworkMidiTx &&
                    d.type != MidiSlotType::kVirtualDawMidi)
                    continue;
                if (s.id == d.id) continue;
                if (s.type == MidiSlotType::kNetworkMidiRx && d.type == MidiSlotType::kNetworkMidiTx) continue;
                audio_matrix_.set_midi_crosspoint(s.id, d.id, 0, false);
            }
        }
        config_dirty_ = true;
        toast_msg_ = "Сценарій: MIDI-контролер у DAW (LanVid MIDI <-> Мережа)";
        toast_until_us_ = now_us() + 2500000ull;
    }
    ImGui::SameLine(0.0f, qgap);
    if (selectable_choice_card("##QrClear", "Очистити все", "Видалити всі маршрути",
                               false, ImVec2(qw, 56.0f))) {
        req_open_clear_all_popup_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- 2. MODE & VIEW SWITCHER (Visual Patchbay <-> Detailed 64x64 Matrix <-> MIDI) ----
    if (status_pill_button("Візуальний Patchbay", matrix_patch_mode_ == 0 && patchbay_view_mode_ == 0, ImVec2(0, 30))) {
        matrix_patch_mode_ = 0;
        patchbay_view_mode_ = 0;
    }
    ImGui::SameLine();
    if (status_pill_button("Матриця 64×64 (Детально)", matrix_patch_mode_ == 0 && patchbay_view_mode_ == 1, ImVec2(0, 30))) {
        matrix_patch_mode_ = 0;
        patchbay_view_mode_ = 1;
    }
    ImGui::SameLine();
    if (status_pill_button("MIDI-маршрути", matrix_patch_mode_ == 1, ImVec2(0, 30))) {
        matrix_patch_mode_ = 1;
    }

    if (matrix_patch_mode_ == 0 && patchbay_view_mode_ == 1) {
        ImGui::SameLine(0.0f, 18.0f);
        if (status_pill_button("Стерео-пари (1-2, 3-4...)", cfg_.matrix_stereo_pairs, ImVec2(0, 30))) {
            cfg_.matrix_stereo_pairs = true;
            config_dirty_ = true;
        }
        ImGui::SameLine();
        if (status_pill_button("Поканально (1, 2, 3...)", !cfg_.matrix_stereo_pairs, ImVec2(0, 30))) {
            cfg_.matrix_stereo_pairs = false;
            config_dirty_ = true;
        }
    }

    // ---- MODE A: VISUAL AUDIO PATCHBAY BOARD (Drag-and-Drop Bezier Cables + DSP Inspector) ----
    if (matrix_patch_mode_ == 0 && patchbay_view_mode_ == 0) {
        ImGui::Spacing();
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float insp_w = std::clamp(avail_w * 0.30f, 250.0f, 320.0f);
        const float board_w = std::max(380.0f, avail_w - insp_w - 10.0f);
        const float board_h = std::max(300.0f, ImGui::GetContentRegionAvail().y - 44.0f);

        std::vector<MatrixSlotConfig> src_slots, dst_slots;
        for (const auto& s : slots) {
            if (s.role != MatrixSlotRole::kOutput) src_slots.push_back(s);
            if (s.role != MatrixSlotRole::kInput) dst_slots.push_back(s);
        }
        const auto links = audio_matrix_.slot_patch_links();
        if (patch_sel_src_ < 0 && !links.empty()) {
            patch_sel_src_ = links[0].src_slot;
            patch_sel_dst_ = links[0].dst_slot;
        }

        // Left/Center Visual Patchbay Canvas
        begin_glass_card("##VisualPatchBoard", ImVec2(board_w, board_h), true);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float cw = ImGui::GetContentRegionAvail().x;
        const float ch = ImGui::GetContentRegionAvail().y;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Column headers
        const float col_w = std::clamp(cw * 0.34f, 155.0f, 220.0f);
        const float left_x = p0.x + 8.0f;
        const float right_x = p0.x + cw - col_w - 8.0f;
        dl->AddText(ImVec2(left_x + 4.0f, p0.y), ImGui::ColorConvertFloat4ToU32(kColAccentInd), "ДЖЕРЕЛА / SOURCES");
        dl->AddText(ImVec2(right_x + 4.0f, p0.y), ImGui::ColorConvertFloat4ToU32(kColAccentPurp), "ПРИЙМАЧІ / DESTINATIONS");

        struct PortPos { int slot_id; ImVec2 pos; };
        std::vector<PortPos> src_ports, dst_ports;

        const float card_h = 62.0f;
        const float card_gap = 10.0f;
        float sy = p0.y + 24.0f;
        for (const auto& s : src_slots) {
            const ImVec2 cp0(left_x, sy);
            const ImVec2 cp1(left_x + col_w, sy + card_h);
            const ImVec2 port(cp1.x, sy + card_h * 0.5f);
            src_ports.push_back({s.id, port});

            const bool is_drag = (patch_drag_src_slot_ == s.id);
            const auto st = audio_matrix_.slot_status(s.id);
            dl->AddRectFilled(cp0, cp1, g_dark_theme ? IM_COL32(27, 24, 44, 240) : IM_COL32(255, 255, 255, 246), 12.0f);
            dl->AddRect(cp0, cp1, is_drag ? IM_COL32(139, 92, 246, 255) : ImGui::ColorConvertFloat4ToU32(kColBorder),
                        12.0f, 0, is_drag ? 2.2f : 1.0f);
            dl->AddText(ImVec2(cp0.x + 10.0f, cp0.y + 8.0f), ImGui::ColorConvertFloat4ToU32(kColTextMain), s.name.c_str());
            char sb[48];
            std::snprintf(sb, sizeof(sb), "%d ch · %s", s.channels, st.connected ? "Active" : "Ready");
            dl->AddText(ImVec2(cp0.x + 10.0f, cp0.y + 26.0f), ImGui::ColorConvertFloat4ToU32(kColTextMuted), sb);

            // Mini live VU bar
            const float norm = std::clamp((std::max(st.peak_in_dbfs, st.peak_out_dbfs) + 54.0f) / 54.0f, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(cp0.x + 10.0f, cp1.y - 12.0f), ImVec2(cp1.x - 20.0f, cp1.y - 6.0f),
                              IM_COL32(120, 120, 150, 45), 3.0f);
            if (norm > 0.02f) {
                dl->AddRectFilled(ImVec2(cp0.x + 10.0f, cp1.y - 12.0f),
                                  ImVec2(cp0.x + 10.0f + (cp1.x - cp0.x - 30.0f) * norm, cp1.y - 6.0f),
                                  IM_COL32(16, 185, 129, 230), 3.0f);
            }

            // Draggable output port button
            ImGui::SetCursorScreenPos(ImVec2(cp0.x, cp0.y));
            ImGui::PushID(7000 + s.id);
            ImGui::InvisibleButton("##SrcPort", ImVec2(col_w + 10.0f, card_h));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                patch_drag_src_slot_ = s.id;
            }
            ImGui::PopID();
            dl->AddCircleFilled(port, 6.0f, IM_COL32(139, 92, 246, 255));
            dl->AddCircleFilled(port, 2.6f, IM_COL32(255, 255, 255, 240));
            sy += card_h + card_gap;
        }

        float dy = p0.y + 24.0f;
        int hovered_dst_slot = -1;
        for (const auto& d : dst_slots) {
            const ImVec2 cp0(right_x, dy);
            const ImVec2 cp1(right_x + col_w, dy + card_h);
            const ImVec2 port(cp0.x, dy + card_h * 0.5f);
            dst_ports.push_back({d.id, port});

            ImGui::SetCursorScreenPos(ImVec2(cp0.x - 10.0f, cp0.y));
            ImGui::PushID(7200 + d.id);
            ImGui::InvisibleButton("##DstPort", ImVec2(col_w + 10.0f, card_h));
            const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (hov) hovered_dst_slot = d.id;
            ImGui::PopID();

            const auto st = audio_matrix_.slot_status(d.id);
            dl->AddRectFilled(cp0, cp1, g_dark_theme ? IM_COL32(27, 24, 44, 240) : IM_COL32(255, 255, 255, 246), 12.0f);
            dl->AddRect(cp0, cp1, (patch_drag_src_slot_ >= 0 && hov) ? IM_COL32(20, 184, 166, 255)
                                                                     : ImGui::ColorConvertFloat4ToU32(kColBorder),
                        12.0f, 0, (patch_drag_src_slot_ >= 0 && hov) ? 2.4f : 1.0f);
            dl->AddText(ImVec2(cp0.x + 14.0f, cp0.y + 8.0f), ImGui::ColorConvertFloat4ToU32(kColTextMain), d.name.c_str());
            char db[48];
            std::snprintf(db, sizeof(db), "%d ch · %s", d.channels, st.connected ? "Active" : "Ready");
            dl->AddText(ImVec2(cp0.x + 14.0f, cp0.y + 26.0f), ImGui::ColorConvertFloat4ToU32(kColTextMuted), db);
            dl->AddCircleFilled(port, 6.0f, IM_COL32(20, 184, 166, 255));
            dl->AddCircleFilled(port, 2.6f, IM_COL32(255, 255, 255, 240));
            dy += card_h + card_gap;
        }

        // Complete drag-and-drop cable creation on mouse release
        if (patch_drag_src_slot_ >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (hovered_dst_slot >= 0) {
                const int nch = (patch_block_mode_ == 0) ? 2 : 64;
                const auto res = audio_matrix_.connect_slot_block(patch_drag_src_slot_, hovered_dst_slot, nch,
                                                                  matrix_new_gain_db_, false, false);
                if (res == CrosspointResult::kBlockedFeedbackLoop) {
                    toast_msg_ = "Feedback Loop Guard: заблоковано петлю зворотного зв'язку!";
                } else if (res == CrosspointResult::kOk) {
                    patch_sel_src_ = patch_drag_src_slot_;
                    patch_sel_dst_ = hovered_dst_slot;
                    matrix_active_preset_ = -1;
                    config_dirty_ = true;
                    toast_msg_ = "Маршрут створено (" + std::to_string(nch) + " ch)";
                }
                toast_until_us_ = now_us() + 2500000ull;
            }
            patch_drag_src_slot_ = -1;
        }

        auto find_port = [](const std::vector<PortPos>& vec, int id, ImVec2& out) -> bool {
            for (const auto& p : vec) if (p.slot_id == id) { out = p.pos; return true; }
            return false;
        };

        // Draw active Bezier cables from unified audio_matrix_.slot_patch_links()
        for (size_t i = 0; i < links.size(); ++i) {
            const auto& lk = links[i];
            ImVec2 a{}, b{};
            if (!find_port(src_ports, lk.src_slot, a) || !find_port(dst_ports, lk.dst_slot, b)) continue;
            const bool sel = (lk.src_slot == patch_sel_src_ && lk.dst_slot == patch_sel_dst_);
            const ImU32 col = lk.all_muted ? IM_COL32(239, 68, 68, 220)
                                           : (sel ? IM_COL32(16, 185, 129, 255) : IM_COL32(139, 92, 246, 235));
            const float dx = std::max(50.0f, std::fabs(b.x - a.x) * 0.45f);
            const ImVec2 cp0(a.x + dx, a.y), cp1(b.x - dx, b.y);
            dl->AddBezierCubic(a, cp0, cp1, b, (col & 0x00FFFFFFu) | 0x38000000u, sel ? 8.0f : 5.5f, 32);
            dl->AddBezierCubic(a, cp0, cp1, b, col, sel ? 3.0f : 2.1f, 32);

            // Clickable midpoint badge to select cable in right inspector
            const ImVec2 mid(0.125f * a.x + 0.375f * cp0.x + 0.375f * cp1.x + 0.125f * b.x,
                             0.125f * a.y + 0.375f * cp0.y + 0.375f * cp1.y + 0.125f * b.y);
            char lbl[40];
            if (lk.all_muted) std::snprintf(lbl, sizeof(lbl), "MUTE (%dch)", lk.channel_count);
            else std::snprintf(lbl, sizeof(lbl), "%dch · %.0f dB", lk.channel_count, lk.gain_db);
            const ImVec2 tsz = ImGui::CalcTextSize(lbl);
            const ImVec2 b0(mid.x - tsz.x * 0.5f - 7.0f, mid.y - tsz.y * 0.5f - 3.0f);
            const ImVec2 b1(mid.x + tsz.x * 0.5f + 7.0f, mid.y + tsz.y * 0.5f + 3.0f);
            ImGui::SetCursorScreenPos(b0);
            ImGui::PushID(7600 + int(i));
            if (ImGui::InvisibleButton("##CableHit", ImVec2(b1.x - b0.x, b1.y - b0.y))) {
                patch_sel_src_ = lk.src_slot;
                patch_sel_dst_ = lk.dst_slot;
            }
            ImGui::PopID();
            dl->AddRectFilled(b0, b1, g_dark_theme ? IM_COL32(20, 18, 34, 240) : IM_COL32(252, 250, 255, 245), 9.0f);
            dl->AddRect(b0, b1, col, 9.0f, 0, sel ? 1.8f : 1.0f);
            dl->AddText(ImVec2(b0.x + 7.0f, b0.y + 3.0f),
                        g_dark_theme ? IM_COL32(240, 238, 255, 245) : IM_COL32(45, 35, 88, 245), lbl);
        }

        // Live preview cable while dragging
        if (patch_drag_src_slot_ >= 0) {
            ImVec2 a{};
            if (find_port(src_ports, patch_drag_src_slot_, a)) {
                const ImVec2 b = ImGui::GetMousePos();
                const float dx = std::max(45.0f, std::fabs(b.x - a.x) * 0.45f);
                dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b,
                                   IM_COL32(20, 184, 166, 235), 2.8f, 28);
            }
        }
        (void)ch;
        end_glass_card();

        // Right Inspector: Керування маршрутом & DSP
        ImGui::SameLine();
        begin_glass_card("##PatchInspector", ImVec2(0, board_h), true);
        ImGui::TextColored(kColAccentPurp, "КЕРУВАННЯ МАРШРУТОМ & DSP");
        ImGui::Separator();
        ImGui::Spacing();

        const SlotPatchLink* sel_link = nullptr;
        for (const auto& lk : links) {
            if (lk.src_slot == patch_sel_src_ && lk.dst_slot == patch_sel_dst_) { sel_link = &lk; break; }
        }
        if (sel_link) {
            float g = sel_link->gain_db;
            bool m = sel_link->all_muted;
            ImGui::Text("Активний кабель: Slot %d -> Slot %d (%d ch)",
                        sel_link->src_slot, sel_link->dst_slot, sel_link->channel_count);
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##PatchGain", &g, -60.0f, 12.0f, "Patch Gain: %.1f dB")) {
                audio_matrix_.set_slot_block_gain_mute(sel_link->src_slot, sel_link->dst_slot, g, m);
                config_dirty_ = true;
            }
            if (status_pill_button(m ? "UNMUTE" : "MUTE", m, ImVec2(100, 28))) {
                audio_matrix_.set_slot_block_gain_mute(sel_link->src_slot, sel_link->dst_slot, g, !m);
                config_dirty_ = true;
            }
            ImGui::SameLine();
            if (status_pill_button(patch_solo_active_ ? "SOLO ON" : "SOLO", patch_solo_active_, ImVec2(100, 28))) {
                patch_solo_active_ = !patch_solo_active_;
                for (const auto& other : links) {
                    const bool is_this = (other.src_slot == sel_link->src_slot && other.dst_slot == sel_link->dst_slot);
                    audio_matrix_.set_slot_block_gain_mute(other.src_slot, other.dst_slot, other.gain_db,
                                                           patch_solo_active_ ? !is_this : false);
                }
                config_dirty_ = true;
            }
            ImGui::Spacing();
            if (status_pill_button("Видалити маршрут", false, ImVec2(-1, 28))) {
                audio_matrix_.disconnect_slot_block(sel_link->src_slot, sel_link->dst_slot);
                patch_sel_src_ = patch_sel_dst_ = -1;
                config_dirty_ = true;
            }
        } else {
            ImGui::TextDisabled("Перетягніть кабель від Джерела до Приймача або клікніть по кабелю.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool guard = audio_matrix_.feedback_guard();
        if (studio_toggle("##LoopGuard", &guard, kColGreen)) {
            audio_matrix_.set_feedback_guard(guard);
            config_dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::Text("Feedback Loop Guard");

        ImGui::Spacing();
        ImGui::TextDisabled("Режим нових кабелів:");
        if (status_pill_button("Стерео (Ch 1-2)", patch_block_mode_ == 0, ImVec2(0, 26))) patch_block_mode_ = 0;
        ImGui::SameLine();
        if (status_pill_button("64ch 1:1", patch_block_mode_ == 1, ImVec2(0, 26))) patch_block_mode_ = 1;
        ImGui::Spacing();
        if (gradient_pill_button("Поканально (Матриця 64×64) ->", ImVec2(-1, 30), kColAccentBlue, kColAccentPurp)) {
            patchbay_view_mode_ = 1;
            cfg_.matrix_stereo_pairs = false;
        }
        end_glass_card();

        // Bottom quick preset bar
        ImGui::Spacing();
        if (status_pill_button("Стерео-пари / Поканально (Матриця 64×64)", false, ImVec2(0, 28))) {
            patchbay_view_mode_ = 1;
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (status_pill_button("+ Додати пристрій", false, ImVec2(0, 28))) {
            req_open_add_audio_popup_ = true;
        }
        end_glass_card();
        return;
    }

    // ---- MIDI PATCHBAY VIEW ----
    if (matrix_patch_mode_ == 1) {
        ImGui::SameLine(0.0f, 18.0f);
        const char* ch_filters[17] = {"Усі канали", "Ch 1", "Ch 2", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7", "Ch 8",
                                      "Ch 9", "Ch 10", "Ch 11", "Ch 12", "Ch 13", "Ch 14", "Ch 15", "Ch 16"};
        ImGui::SetNextItemWidth(120.0f);
        ImGui::Combo("Фільтр MIDI", &midi_channel_filter_, ch_filters, 17);
        ImGui::Spacing();

        std::vector<MidiSlotConfig> srcs, dsts;
        for (const auto& s : audio_matrix_.midi_slots()) {
            if (s.type == MidiSlotType::kMidiInput || s.type == MidiSlotType::kNetworkMidiRx ||
                s.type == MidiSlotType::kVirtualDawMidi)
                srcs.push_back(s);
            if (s.type == MidiSlotType::kMidiOutput || s.type == MidiSlotType::kNetworkMidiTx ||
                s.type == MidiSlotType::kVirtualDawMidi)
                dsts.push_back(s);
        }
        if (srcs.empty() || dsts.empty()) {
            ImGui::TextDisabled("Додайте MIDI-клавіатуру або вихід у вкладці «Пристрої», щоб налаштувати маршрути.");
        } else if (ImGui::BeginTable("##MidiPatchTbl2", int(dsts.size()) + 1,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("ЗВІДКИ (Джерела) \\ КУДИ (Приймачі)");
            for (const auto& d : dsts) ImGui::TableSetupColumn(d.name.c_str());
            ImGui::TableHeadersRow();
            const uint64_t now_t = now_us();
            for (const auto& s : srcs) {
                const auto mst = audio_matrix_.midi_slot_status(s.id);
                const bool midi_pulse = (mst.last_activity_us > 0 && now_t - mst.last_activity_us < 220000ull);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(midi_pulse ? kColGreen : kColAccentPurp, "●");
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::Text("%s", s.name.c_str());
                for (size_t j = 0; j < dsts.size(); ++j) {
                    ImGui::TableSetColumnIndex(int(j) + 1);
                    if (s.id == dsts[j].id) {
                        ImGui::TextDisabled("  —");
                        continue;
                    }
                    MidiCrosspoint mcp{};
                    const bool linked = audio_matrix_.has_midi_crosspoint(s.id, dsts[j].id, &mcp);
                    ImGui::PushID(s.id * 32 + dsts[j].id);
                    const ImVec4 bcol = linked ? (mcp.mute ? kColPink : (midi_pulse ? kColGreen : kColAccentNode))
                                               : kColCardSoft;
                    ImGui::PushStyleColor(ImGuiCol_Button, bcol);
                    ImGui::PushStyleColor(ImGuiCol_Text, linked ? kColWhite : kColTextMuted);
                    char blbl[16];
                    if (!linked) std::snprintf(blbl, sizeof(blbl), "·");
                    else if (mcp.mute) std::snprintf(blbl, sizeof(blbl), "MUTE");
                    else if (mcp.channel_filter == 0) std::snprintf(blbl, sizeof(blbl), "УСІ");
                    else std::snprintf(blbl, sizeof(blbl), "Ch%d", mcp.channel_filter);
                    if (ImGui::Button(blbl, ImVec2(58.0f, 26.0f))) {
                        if (linked) audio_matrix_.remove_midi_crosspoint(s.id, dsts[j].id);
                        else audio_matrix_.set_midi_crosspoint(s.id, dsts[j].id, midi_channel_filter_, false);
                        config_dirty_ = true;
                    }
                    if (linked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        audio_matrix_.set_midi_crosspoint(s.id, dsts[j].id, mcp.channel_filter, !mcp.mute);
                        config_dirty_ = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Направити MIDI: %s -> %s\nКлік — увімкнути/вимкнути, ПКМ — Mute",
                                          s.name.c_str(), dsts[j].name.c_str());
                    }
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Підказка: зелена пульсація означає, що зараз надходять MIDI-ноти.");
        end_glass_card();
        return;
    }

    // ---- 3. AUDIO SOURCE / DESTINATION SELECTORS + RANGE PILLS ----
    ImGui::Spacing();
    MatrixSlotConfig src_cfg{}, dst_cfg{};
    bool has_src = false, has_dst = false;
    for (const auto& s : slots) {
        if (s.id == matrix_src_slot_) { src_cfg = s; has_src = true; }
        if (s.id == matrix_dst_slot_) { dst_cfg = s; has_dst = true; }
    }
    if (!has_src && !slots.empty()) { src_cfg = slots.front(); matrix_src_slot_ = src_cfg.id; has_src = true; }
    if (!has_dst && !slots.empty()) { dst_cfg = slots.back(); matrix_dst_slot_ = dst_cfg.id; has_dst = true; }

    ImGui::TextColored(kColAccentInd, "● ЗВІДКИ (Джерело):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##PbSrc", has_src ? src_cfg.name.c_str() : "Оберіть джерело")) {
        for (const auto& s : slots) {
            if (s.role == MatrixSlotRole::kOutput) continue;
            if (ImGui::Selectable(s.name.c_str(), s.id == matrix_src_slot_)) {
                matrix_src_slot_ = s.id;
                matrix_src_page_ = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextColored(kColAccentPurp, "● КУДИ (Приймач):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##PbDst", has_dst ? dst_cfg.name.c_str() : "Оберіть приймач")) {
        for (const auto& s : slots) {
            if (s.role == MatrixSlotRole::kInput) continue;
            if (ImGui::Selectable(s.name.c_str(), s.id == matrix_dst_slot_)) {
                matrix_dst_slot_ = s.id;
                matrix_dst_page_ = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("##PbGain", &matrix_new_gain_db_, -24.0f, 6.0f, "Гучність %.0f дБ");

    // Range filter pills (1-16, 17-32, 33-48, 49-64)
    const char* page_lbl[] = {"1-16", "17-32", "33-48", "49-64"};
    const int max_src_pages = has_src ? std::min(4, (src_cfg.channels + 15) / 16) : 1;
    const int max_dst_pages = has_dst ? std::min(4, (dst_cfg.channels + 15) / 16) : 1;
    if (matrix_src_page_ >= max_src_pages) matrix_src_page_ = 0;
    if (matrix_dst_page_ >= max_dst_pages) matrix_dst_page_ = 0;
    if (max_src_pages > 1 || max_dst_pages > 1) {
        ImGui::TextDisabled("Діапазон каналів:");
        const int max_p = std::max(max_src_pages, max_dst_pages);
        for (int p = 0; p < max_p; ++p) {
            ImGui::SameLine(0.0f, 6.0f);
            const bool cur = (matrix_src_page_ == p || (max_src_pages == 1 && matrix_dst_page_ == p));
            if (status_pill_button(page_lbl[p], cur, ImVec2(66.0f, 24.0f))) {
                if (p < max_src_pages) matrix_src_page_ = p;
                if (p < max_dst_pages) matrix_dst_page_ = p;
            }
        }
    }

    ImGui::Spacing();

    // ---- 4. INTERACTIVE AUDIO PATCHBAY GRID (Stereo-Pairs or Per-Channel + Signal Pulse) ----
    if (has_src && has_dst) {
        const int step = cfg_.matrix_stereo_pairs ? 2 : 1;
        const int r0 = matrix_src_page_ * 16;
        const int r1 = std::min(src_cfg.channels, r0 + 16);
        const int c0 = matrix_dst_page_ * 16;
        const int c1 = std::min(dst_cfg.channels, c0 + 16);
        const int rows = (r1 - r0 + step - 1) / step;
        const int cols = (c1 - c0 + step - 1) / step;

        // Real audio signal pulse factor from source/dest live peak levels
        const auto src_st = audio_matrix_.slot_status(src_cfg.id);
        const auto dst_st = audio_matrix_.slot_status(dst_cfg.id);
        const float peak_db = std::max(std::max(src_st.peak_in_dbfs, src_st.peak_out_dbfs),
                                       std::max(dst_st.peak_in_dbfs, dst_st.peak_out_dbfs));
        const float sig_norm = std::clamp((peak_db + 54.0f) / 54.0f, 0.0f, 1.0f);
        const float pulse_wave = 0.5f + 0.5f * std::sin(float(ImGui::GetTime() * 8.0));
        const float sig_pulse = sig_norm * (0.65f + 0.35f * pulse_wave);

        if (rows > 0 && cols > 0) {
            const float label_w = 88.0f;
            const float header_h = 34.0f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float max_cell = cfg_.matrix_stereo_pairs ? 44.0f : 30.0f;
            const float cell = std::clamp(std::min((avail.x - label_w - 12.0f) / float(cols),
                                                   (avail.y - header_h - 32.0f) / float(rows)),
                                          22.0f, max_cell);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Backing plate
            const ImVec2 grid_p0(origin.x + label_w - 3.0f, origin.y + header_h - 3.0f);
            const ImVec2 grid_p1(origin.x + label_w + float(cols) * cell + 3.0f,
                                 origin.y + header_h + float(rows) * cell + 3.0f);
            dl->AddRectFilled(grid_p0, grid_p1,
                              g_dark_theme ? IM_COL32(18, 22, 34, 255) : IM_COL32(250, 248, 255, 255), 14.0f);
            dl->AddRect(grid_p0, grid_p1, ImGui::ColorConvertFloat4ToU32(kColBorder), 14.0f, 0, 1.0f);

            // Top-left corner legend
            dl->AddText(ImVec2(origin.x + 2.0f, origin.y + 10.0f),
                        ImGui::ColorConvertFloat4ToU32(kColAccentInd), "ЗВІДКИ \\ КУДИ");

            const ImVec2 mouse = ImGui::GetIO().MousePos;
            int hov_r = -1, hov_c = -1;
            if (ImGui::IsWindowHovered()) {
                const float mx = mouse.x - (origin.x + label_w);
                const float my = mouse.y - (origin.y + header_h);
                if (mx >= 0 && my >= 0 && mx < float(cols) * cell && my < float(rows) * cell) {
                    hov_c = int(mx / cell);
                    hov_r = int(my / cell);
                }
            }

            const ImU32 hov_fill = g_dark_theme ? IM_COL32(99, 102, 241, 40) : IM_COL32(237, 233, 254, 230);
            if (hov_r >= 0) {
                dl->AddRectFilled(ImVec2(origin.x, origin.y + header_h + float(hov_r) * cell),
                                  ImVec2(origin.x + label_w + float(cols) * cell,
                                         origin.y + header_h + float(hov_r + 1) * cell),
                                  hov_fill, 6.0f);
            }
            if (hov_c >= 0) {
                dl->AddRectFilled(ImVec2(origin.x + label_w + float(hov_c) * cell, origin.y),
                                  ImVec2(origin.x + label_w + float(hov_c + 1) * cell,
                                         origin.y + header_h + float(rows) * cell),
                                  hov_fill, 6.0f);
            }

            // Column headers (Destinations)
            for (int j = 0; j < cols; ++j) {
                const int dc = c0 + j * step;
                char hdr[16];
                if (step == 2 && dc + 1 < dst_cfg.channels) std::snprintf(hdr, sizeof(hdr), "%d-%d", dc + 1, dc + 2);
                else std::snprintf(hdr, sizeof(hdr), "%d", dc + 1);
                const ImVec2 hsz = ImGui::CalcTextSize(hdr);
                const ImU32 col = (j == hov_c) ? ImGui::ColorConvertFloat4ToU32(kColAccentPurp)
                                               : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
                dl->AddText(ImVec2(origin.x + label_w + float(j) * cell + (cell - hsz.x) * 0.5f, origin.y + 10.0f),
                            col, hdr);
            }

            // Rows + clickable nodes
            for (int i = 0; i < rows; ++i) {
                const int sc = r0 + i * step;
                const float ry = origin.y + header_h + float(i) * cell;
                char rlbl[20];
                if (step == 2 && sc + 1 < src_cfg.channels) std::snprintf(rlbl, sizeof(rlbl), "Ch %d-%d", sc + 1, sc + 2);
                else std::snprintf(rlbl, sizeof(rlbl), "Ch %02d", sc + 1);
                const ImU32 rcol = (i == hov_r) ? ImGui::ColorConvertFloat4ToU32(kColAccentInd)
                                                : ImGui::ColorConvertFloat4ToU32(kColTextMain);
                dl->AddText(ImVec2(origin.x + 6.0f, ry + (cell - 14.0f) * 0.5f), rcol, rlbl);

                for (int j = 0; j < cols; ++j) {
                    const int dc = c0 + j * step;
                    const float cx = origin.x + label_w + float(j) * cell + cell * 0.5f;
                    const float cy = ry + cell * 0.5f;

                    MatrixCrosspoint cp0{}, cp1{};
                    const bool l0 = audio_matrix_.has_crosspoint(src_cfg.id, sc, dst_cfg.id, dc, &cp0);
                    const bool has_pair = (step == 2 && sc + 1 < src_cfg.channels && dc + 1 < dst_cfg.channels);
                    const bool l1 = has_pair && audio_matrix_.has_crosspoint(src_cfg.id, sc + 1, dst_cfg.id, dc + 1, &cp1);
                    const bool linked = l0 || l1;
                    const bool muted = linked && (l0 ? cp0.mute : cp1.mute);

                    ImGui::SetCursorScreenPos(ImVec2(origin.x + label_w + float(j) * cell, ry));
                    ImGui::PushID(sc * 100 + dc);
                    if (ImGui::InvisibleButton("##PbNode", ImVec2(cell, cell))) {
                        matrix_active_preset_ = -1;
                        if (linked) {
                            audio_matrix_.remove_crosspoint(src_cfg.id, sc, dst_cfg.id, dc);
                            if (has_pair) audio_matrix_.remove_crosspoint(src_cfg.id, sc + 1, dst_cfg.id, dc + 1);
                            config_dirty_ = true;
                        } else {
                            const auto res = audio_matrix_.set_crosspoint(
                                src_cfg.id, sc, dst_cfg.id, dc, matrix_new_gain_db_, false, !cfg_.matrix_feedback_guard);
                            if (res == CrosspointResult::kBlockedFeedbackLoop) {
                                toast_msg_ = "Захист від петлі: не можна замкнути пристрій сам на себе!";
                                toast_until_us_ = now_us() + 3500000ull;
                            } else if (res == CrosspointResult::kOk) {
                                if (has_pair) {
                                    audio_matrix_.set_crosspoint(src_cfg.id, sc + 1, dst_cfg.id, dc + 1,
                                                                 matrix_new_gain_db_, false, !cfg_.matrix_feedback_guard);
                                }
                                config_dirty_ = true;
                            }
                        }
                    }
                    if (linked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        audio_matrix_.set_crosspoint(src_cfg.id, sc, dst_cfg.id, dc, cp0.gain_db, !muted, true);
                        if (has_pair) {
                            audio_matrix_.set_crosspoint(src_cfg.id, sc + 1, dst_cfg.id, dc + 1, cp1.gain_db, !muted, true);
                        }
                        config_dirty_ = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        char sc_s[16], dc_s[16];
                        if (step == 2 && sc + 1 < src_cfg.channels) std::snprintf(sc_s, sizeof(sc_s), "Ch %d-%d", sc + 1, sc + 2);
                        else std::snprintf(sc_s, sizeof(sc_s), "Ch %d", sc + 1);
                        if (step == 2 && dc + 1 < dst_cfg.channels) std::snprintf(dc_s, sizeof(dc_s), "Ch %d-%d", dc + 1, dc + 2);
                        else std::snprintf(dc_s, sizeof(dc_s), "Ch %d", dc + 1);

                        if (linked) {
                            ImGui::SetTooltip("Маршрут: %s %s -> %s %s\nГучність: %.1f дБ | %s\n(Клік — вимкнути, ПКМ — Mute)",
                                              src_cfg.name.c_str(), sc_s, dst_cfg.name.c_str(), dc_s,
                                              cp0.gain_db, muted ? "ЗАГЛУШЕНО" : "АКТИВНИЙ");
                        } else {
                            ImGui::SetTooltip("Направити: %s %s -> %s %s\n(Клік — увімкнути, ПКМ — Mute)",
                                              src_cfg.name.c_str(), sc_s, dst_cfg.name.c_str(), dc_s);
                        }
                    }
                    ImGui::PopID();

                    const float base_rad = cell * 0.28f;
                    if (linked) {
                        if (muted) {
                            dl->AddCircleFilled(ImVec2(cx, cy), base_rad + 3.0f, IM_COL32(236, 72, 153, 55));
                            dl->AddCircleFilled(ImVec2(cx, cy), base_rad, ImGui::ColorConvertFloat4ToU32(kColPink));
                        } else {
                            // Soft violet-blue node pulsing gently with live audio signal level
                            const float rad = base_rad * (1.0f + 0.20f * sig_pulse);
                            const float halo = rad + 3.5f + 4.5f * sig_pulse;
                            const float t_diag = float(i + j) / std::max(1.0f, float(rows + cols - 2));
                            const ImVec4 ncol = lerp_col(kColAccentNode, kColAccentSky, t_diag);
                            dl->AddCircleFilled(ImVec2(cx, cy), halo,
                                                IM_COL32(139, 92, 246, int(52 + 75 * sig_pulse)));
                            dl->AddCircleFilled(ImVec2(cx, cy), rad, ImGui::ColorConvertFloat4ToU32(ncol));
                            dl->AddCircleFilled(ImVec2(cx - rad * 0.28f, cy - rad * 0.28f), rad * 0.26f,
                                                IM_COL32(255, 255, 255, 175));
                        }
                    } else {
                        const ImU32 dot = (i == hov_r && j == hov_c)
                                              ? IM_COL32(139, 92, 246, 175)
                                              : (g_dark_theme ? IM_COL32(55, 65, 88, 220) : IM_COL32(221, 214, 254, 240));
                        dl->AddCircleFilled(ImVec2(cx, cy), base_rad * 0.58f, dot);
                    }
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header_h + float(rows) * cell + 8.0f));
        }
    }
    ImGui::TextDisabled("Клік: з'єднати / роз'єднати  ·  ПКМ: Mute  ·  Пульсація вузла = є живий звук");
    end_glass_card();
}

void LanVidStudioApp::render_frame() {
    if (monitor_server_.is_running()) monitor_server_.poll_control();
    if (tally_intercom_.is_running()) tally_intercom_.poll();
    if (grid_node_ && grid_node_->is_open()) {
        grid_node_->poll(128);
        const uint64_t now = now_us();
        if (cfg_.grid_clock_master && (now - grid_last_sync_us_) >= 10000ull) {
            grid_clock_sample_ += 480;
            grid_node_->emit_clock_sync(grid_clock_sample_);
            grid_last_sync_us_ = now;
        }
    }
    poll_telemetry_and_frames();

    // Global hotkeys (F11 fullscreen toggle, ESC exit fullscreen)
    const bool is_fs = host_.is_fullscreen && host_.is_fullscreen(host_.ctx);
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        if (host_.set_fullscreen) host_.set_fullscreen(host_.ctx, !is_fs);
    }
    if (is_fs && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (host_.set_fullscreen) host_.set_fullscreen(host_.ctx, false);
    }

    if (is_fs) {
        draw_fullscreen_receiver();
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##LanVidMain", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    // Soft Pastel Lavender -> Blue-Purple diagonal backdrop (#DDD6FE -> #E0E7FF / #EDE9FE)
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
        ImGui::ColorConvertFloat4ToU32(kColBgTopLeft),
        ImGui::ColorConvertFloat4ToU32(kColBgTopRight),
        ImGui::ColorConvertFloat4ToU32(kColBgBotRight),
        ImGui::ColorConvertFloat4ToU32(kColBgBotLeft));

    draw_sidebar();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##MainWorkspace", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_top_bar();
    ImGui::Spacing();

    switch (std::clamp(cfg_.active_tab, 0, 3)) {
        case 0: draw_connect_tab(); break;
        case 1: draw_patchbay_tab(); break;
        case 2: draw_devices_tab(); break;
        default: draw_settings_tab(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Root-level popups & modals (guaranteed matching ID stack from any child panel)
    if (req_open_add_audio_popup_) {
        req_open_add_audio_popup_ = false;
        ImGui::OpenPopup("##AddAudioDev");
    }
    if (req_open_add_midi_popup_) {
        req_open_add_midi_popup_ = false;
        ImGui::OpenPopup("##AddMidiDev");
    }
    if (req_open_clear_all_popup_) {
        req_open_clear_all_popup_ = false;
        ImGui::OpenPopup("##ConfirmClearAll");
    }
    draw_add_device_popups();

    // Soft glass toast overlay
    if (!toast_msg_.empty() && now_us() < toast_until_us_) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 20.0f, vp->Pos.y + vp->Size.y - 20.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, kColBorder);
        ImGui::Begin("##Toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(kColAccentPurp, "●");
        ImGui::SameLine();
        ImGui::TextColored(kColTextMain, "%s", toast_msg_.c_str());
        ImGui::End();
        ImGui::PopStyleColor(2);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace lv::gui
