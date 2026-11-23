#include "gui/lanvid_studio.hpp"

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
    "Ultra-Light (Opus / ADPCM · -80%)"
};

double estimate_aoip_mbps(int channels, int codec) {
    const double bits_per_sample = (codec == 0) ? 32.0 : (codec == 1 ? 24.0 : (codec == 2 ? 16.0 : 4.2));
    return double(std::clamp(channels, 1, 64)) * 48000.0 * bits_per_sample / 1e6;
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
    const float s = std::max(0.5f, elevation);
    if (g_dark_theme) {
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 3.0f * s), ImVec2(p1.x, p1.y + 6.0f * s),
                          IM_COL32(0, 0, 0, 55), rounding + 2.0f);
        return;
    }
    // Multi-layer soft lavender drop shadow rgba(124, 58, 237, 0.04..0.10)
    dl->AddRectFilled(ImVec2(p0.x - 1.0f * s, p0.y + 3.0f * s), ImVec2(p1.x + 1.0f * s, p1.y + 7.0f * s),
                      IM_COL32(124, 58, 237, 10), rounding + 2.0f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 2.0f * s), ImVec2(p1.x, p1.y + 4.5f * s),
                      IM_COL32(124, 58, 237, 16), rounding + 1.0f);
    dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f * s), ImVec2(p1.x - 1.0f, p1.y + 2.5f * s),
                      IM_COL32(124, 58, 237, 25), rounding);
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
    init_audio_matrix();
    if (monitors_.empty() && cfg_.source_type == LV_SOURCE_SCREEN) {
        cfg_.source_type = LV_SOURCE_PATTERN;
    }
    start_lan_scan();
}

void LanVidStudioApp::shutdown() {
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
    audio_matrix_.set_midi_sink([this](int dst_slot, uint8_t st, uint8_t d1, uint8_t d2, uint8_t port, uint64_t) {
        bool to_net = false;
        for (const auto& s : audio_matrix_.midi_slots())
            if (s.id == dst_slot && s.type == MidiSlotType::kNetworkMidiTx) to_net = true;
        if (!to_net) return;  // OS MIDI outputs are driven by the matrix itself
        if (duplex_running_ && duplex_) lv_duplex_send_midi(duplex_, st, d1, d2, port, 0);
        else if (sender_running_ && sender_) lv_sender_push_midi(sender_, st, d1, d2, port, 0);
    });
    if (receiver_) lv_receiver_set_midi_callback(receiver_, &LanVidStudioApp::on_network_midi, this);
    if (duplex_) lv_duplex_set_midi_callback(duplex_, &LanVidStudioApp::on_network_midi, this);
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
    sc.audio_codec = std::clamp(cfg_.audio_codec, 0, 3);

    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    bus.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
    lv_sender_configure_audio_bus(sender_, &bus);

    last_tx_preview_fid_ = 0;
    if (lv_sender_start(sender_, &sc)) {
        sender_running_ = true;
        if (cfg_.aoip_tone && cfg_.aoip_source == 0) start_tone();
        if (cfg_.aoip_source == 1) start_vdev_tx();
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
    if (!receiver_running_) return;
    if (cfg_.aoip_output == 1) {
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
    }
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
    dc.audio_codec = std::clamp(cfg_.audio_codec, 0, 3);
    dc.adaptive_jitter = cfg_.adaptive_jitter ? 1 : 0;
    const int rc = lv_duplex_start(duplex_, &dc);
    duplex_running_ = rc == LV_OK;
    if (duplex_running_) {
        lv_audio_set_tx_routing(duplex_, route_[0]);
        lv_audio_set_rx_routing(duplex_, route_[1]);
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
    if (ImGui::Combo("Audio Compression", &cfg_.audio_codec, kAudioCodecItems, 4)) {
        if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        config_dirty_ = true;
    }
    ImGui::TextDisabled("Est. AoIP TX: %.1f Mbps (%d ch @ 48 kHz)",
                        estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec), cfg_.aoip_channels);
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

    // Restore or initialize MIDI slots (Network MIDI TX/RX + saved OS MIDI devices)
    audio_matrix_.clear_midi_slots();
    if (cfg_.midi_slots.empty()) {
        audio_matrix_.add_midi_slot({0, MidiSlotType::kNetworkMidiTx, "Network MIDI TX (MIDI2)", "", -1, 0, true});
        audio_matrix_.add_midi_slot({1, MidiSlotType::kNetworkMidiRx, "Network MIDI RX (MIDI2)", "", -1, 0, true});
    } else {
        for (const auto& ms : cfg_.midi_slots) audio_matrix_.add_midi_slot(ms);
        for (const auto& mc : cfg_.midi_crosspoints) {
            audio_matrix_.set_midi_crosspoint(mc.src_slot, mc.dst_slot, mc.channel_filter, mc.mute);
        }
    }

    audio_matrix_.set_network_io(
        [this](const float* const* planes, uint32_t ch, uint32_t frames) {
            if (sender_ && sender_running_) {
                lv_sender_push_audio_multichannel(sender_, planes, uint16_t(ch), frames,
                                                  LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR, LV_MEDIA_TS_AUTO);
            }
        },
        [this](float* const* planes, uint32_t ch, uint32_t frames) {
            if (receiver_ && receiver_running_) {
                static thread_local std::vector<float> tmp;
                tmp.resize(size_t(ch) * frames);
                lv_receiver_pop_audio_multichannel(receiver_, tmp.data(), uint16_t(ch), frames,
                                                   LV_AUDIO_LAYOUT_PLANAR);
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
#define LANVID_VERSION_STRING "2.5.0"
#endif
    draw_gradient_icon_tile("##BrandTile", IconKind::kWaveform, 36.0f, -1, false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColAccentPurp, "LanVid");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(kColTextMain, "Studio");
    ImGui::TextColored(kColTextMuted, "v%s", LANVID_VERSION_STRING);
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

    const bool any_active = sender_running_ || receiver_running_ || duplex_running_ || audio_matrix_.is_running();
    dl->AddCircleFilled(ImVec2(bp0.x + 14.0f, bp0.y + 19.0f), 4.5f,
                        ImGui::ColorConvertFloat4ToU32(any_active ? kColGreen : kColTextMuted));
    dl->AddText(ImVec2(bp0.x + 25.0f, bp0.y + 12.0f),
                ImGui::ColorConvertFloat4ToU32(any_active ? kColGreen : kColTextMuted),
                any_active ? "На зв'язку" : "Очікування");
    char lat_txt[48];
    if (actual_buf_valid_) std::snprintf(lat_txt, sizeof(lat_txt), "Затримка: %.0f мс", actual_buf_ms_);
    else std::snprintf(lat_txt, sizeof(lat_txt), "Затримка: %d мс", cfg_.aoip_latency_ms);
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
    if (duplex_running_ && duplex_) {
        lv_duplex_get_stats(duplex_, &dpx_stats_);
        dpx_stats_us_ = now;
        actual_buf_ms_ = dpx_stats_.effective_latency_ms;
        actual_target_ms_ = dpx_stats_.effective_target_ms;
        actual_buf_valid_ = dpx_stats_.running && actual_buf_ms_ > 0.0;
    } else if (receiver_running_ && receiver_) {
        actual_buf_ms_ = lv_receiver_get_effective_latency_ms(receiver_);
        actual_target_ms_ = rx_stats_.effective_target_ms;
        actual_buf_valid_ = actual_buf_ms_ > 0.0;
    }
}

void LanVidStudioApp::draw_actual_buffer_line() {
    if (!actual_buf_valid_) {
        ImGui::TextColored(kColTextMuted, "ACTUAL BUFFER: -- ms");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("No AoIP stream is being played out yet.");
        return;
    }
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
        ImGui::SetTooltip("Measured jitter-buffer playout delay: %.1f ms (%s)\nTarget: %d ms%s", actual_buf_ms_, state,
                          cfg_.aoip_latency_ms,
                          cfg_.adaptive_jitter ? ", Adaptive range up to 200 ms" : ", Fixed");
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

    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##CfgAudioCodec", &cfg_.audio_codec, kAudioCodecItems, 4)) {
        if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        config_dirty_ = true;
    }
    ImGui::TextDisabled("Audio Compression · Est: %.1f Mbps (%d ch)",
                        estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec), cfg_.aoip_channels);

    if (ImGui::Checkbox("Feedback Loop Guard", &cfg_.matrix_feedback_guard)) {
        audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);
        config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- STREAM MODE ----
    ImGui::TextColored(kColAccentPurp, "STREAM MODE");
    if (studio_toggle("##AudioOnlyToggle", &cfg_.audio_only, kColAccentInd)) {
        config_dirty_ = true;
        if (sender_running_) start_sender();
        if (receiver_running_) start_receiver();
    }
    ImGui::SameLine();
    ImGui::TextColored(cfg_.audio_only ? kColAccentPurp : kColTextMain, "AUDIO ONLY");
    ImGui::TextDisabled(cfg_.audio_only ? "Video disabled (~0 Mbps video, pure AoIP)"
                                        : "Video + Audio streaming enabled");

    ImGui::Spacing();
    ImGui::Separator();

    // ---- LIVE TELEMETRY (3 Donut Ring Gauges on White Cards) ----
    ImGui::TextColored(kColAccentPurp, "LIVE TELEMETRY");
    const double total_mbps = tx_stats_.tx_mbps + rx_stats_.rx_mbps +
                              (tx_stats_.audio_kbps + rx_stats_.audio_kbps) / 1000.0;
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
                                          : (ms.type == MidiSlotType::kNetworkMidiTx ? "NET TX" : "NET RX"));
            ImGui::TextColored(blinking ? kColAccentPurp : kColTextMuted, "%llu ev · %s",
                               (unsigned long long)mst.events_count, mtag);
            if (ms.type == MidiSlotType::kMidiOutput || ms.type == MidiSlotType::kNetworkMidiTx) {
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
            if (s.type == MidiSlotType::kMidiInput || s.type == MidiSlotType::kNetworkMidiRx) srcs.push_back(s);
            if (s.type == MidiSlotType::kMidiOutput || s.type == MidiSlotType::kNetworkMidiTx) dsts.push_back(s);
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
    if (ImGui::Combo("Audio Compression", &cfg_.audio_codec, kAudioCodecItems, 4)) {
        if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        config_dirty_ = true;
    }
    ImGui::TextDisabled("Est. AoIP: %.1f Mbps (%d ch @ 48 kHz)",
                        estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec), cfg_.aoip_channels);
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
        if (rc == 1 && w > 0 && h > 0) {
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
        if (rc == 1 && w > 0 && h > 0) {
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
    begin_glass_card("##TopBar", ImVec2(0, 56), false, ImGuiWindowFlags_NoScrollbar);
    const int tab_idx = std::clamp(cfg_.active_tab, 0, 3);
    const IconKind mode_icons[] = {IconKind::kSend, IconKind::kMatrix, IconKind::kHeadphones, IconKind::kGear};
    draw_gradient_icon_tile("##TopModeTile", mode_icons[tab_idx], 32.0f, -1, false);
    ImGui::SameLine();

    // Human-friendly interactive live status banner
    const bool any_live = sender_running_ || receiver_running_ || duplex_running_ || audio_matrix_.is_running();
    const bool has_routes = !audio_matrix_.crosspoints().empty() || !audio_matrix_.midi_crosspoints().empty();
    const char* target_ip = (cfg_.connect_role == 2) ? dpx_peer_buf_ : ip_buf_;
    const int lat_disp = actual_buf_valid_ ? int(std::round(actual_buf_ms_)) : cfg_.aoip_latency_ms;
    const bool has_midi = !audio_matrix_.midi_slots().empty();

    char banner[256];
    ImVec4 banner_col = kColTextMain;
    if (!has_routes) {
        std::snprintf(banner, sizeof(banner),
                      "Підказка: оберіть навушники у «Пристрої» або увімкніть пресет маршрутизації у «Маршрути»");
        banner_col = kColAmber;
    } else if (any_live) {
        std::snprintf(banner, sizeof(banner),
                      "Працює: %d каналів%s -> %s (Затримка %d мс · %s)",
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
    ImGui::TextDisabled("Ваш IP у локальній мережі: %s  ·  48 кГц  ·  %s",
                        local_lan_ip_.c_str(), cfg_.audio_only ? "Лише звук" : "Звук + Відео");
    ImGui::EndGroup();

    // Live status pill on right side
    const char* badge = any_live ? "● Активно" : "● Готово";
    const ImVec2 tsz = ImGui::CalcTextSize(badge);
    const float pill_w = tsz.x + 24.0f;
    const float pill_h = 28.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - pill_w - 16.0f));
    const ImVec2 pp0 = ImGui::GetCursorScreenPos();
    const ImVec2 pp1(pp0.x + pill_w, pp0.y + pill_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
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
    draw_aspect_image((sender_running_ && !cfg_.audio_only) ? sender_tex_ : nullptr, ImVec2(avail.x, prev_h),
                      cfg_.audio_only ? "Режим Audio Only (відео вимкнено) — активна AoIP шина"
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
    if (ImGui::BeginPopup("##AddMidiDev")) {
        ImGui::TextColored(kColAccentInd, "MIDI-входи (Клавіатури / Барабани / Контролери)");
        if (midi_in_devs_.empty()) ImGui::TextDisabled("  MIDI-входів не знайдено");
        for (const auto& m : midi_in_devs_) {
            if (ImGui::Selectable((m.name + " [MIDI IN]").c_str())) {
                audio_matrix_.add_midi_slot({-1, MidiSlotType::kMidiInput, m.name, m.name, m.index, 0, true});
                config_dirty_ = true;
            }
        }
        ImGui::Separator();
        ImGui::TextColored(kColAccentPurp, "MIDI-виходи (DAW / Синтезатори)");
        if (midi_out_devs_.empty()) ImGui::TextDisabled("  MIDI-виходів не знайдено");
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
            s.name = d ? d->name : (role == MatrixSlotRole::kOutput ? "Основний вихід (CoreAudio)" : "Основний вхід (CoreAudio)");
            s.device_id = d ? d->uid : "";
            s.device_index = -1;
            s.channels = d ? std::clamp(d->channels, 1, 64) : 2;
            s.buffer_size = cfg_.matrix_buffer_size;
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        };
        ImGui::TextColored(kColAccentInd, "Виходи CoreAudio (Навушники / Колонки / USB)");
        if (ImGui::Selectable("Основний вихід (CoreAudio)")) add_sys_slot(nullptr, MatrixSlotRole::kOutput);
        for (const auto& d : sys_out_devs_) {
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d кан., %u Гц)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [основний]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kOutput);
        }
        ImGui::Separator();
        ImGui::TextColored(kColAccentPurp, "Входи CoreAudio (Мікрофон / Лінійний вхід)");
        if (ImGui::Selectable("Основний вхід (CoreAudio)")) add_sys_slot(nullptr, MatrixSlotRole::kInput);
        for (const auto& d : sys_in_devs_) {
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d кан., %u Гц)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [основний]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kInput);
        }
#else
        ImGui::TextColored(kColAccentInd, "Виходи звуку (Навушники / Аудіоінтерфейси)");
        if (ImGui::Selectable("Основні навушники / колонки ПК (WASAPI)")) {
            MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                               "Навушники / Вихід ПК", "", -1, 2, cfg_.matrix_buffer_size, true};
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : loop_devs_) {
            if (ImGui::Selectable((d.name + " [ВИХІД]").c_str())) {
                MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                                   d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true};
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        ImGui::Separator();
        ImGui::TextColored(kColAccentPurp, "Входи звуку (Мікрофон / Лінійний вхід)");
        if (ImGui::Selectable("Основний мікрофон / вхід ПК (WASAPI)")) {
            MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                               "Мікрофон / Вхід ПК", "", -1, 2, cfg_.matrix_buffer_size, true};
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : mic_devs_) {
            if (ImGui::Selectable((d.name + " [ВХІД]").c_str())) {
                MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                                   d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true};
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        if (!asio_devs_.empty()) {
            ImGui::Separator();
            ImGui::TextColored(kColAmber, "Зовнішні ASIO-драйвери (Студійні карти)");
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
}

void LanVidStudioApp::draw_connect_tab() {
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float left_w = std::max(440.0f, total_w * 0.56f);

    // ---- LEFT COLUMN: Quick Start, Role, One-Click Start, Auto-Discovery ----
    begin_glass_card("##ConnectMain", ImVec2(left_w, 0));

    ImGui::TextColored(kColAccentPurp, "1. Що має робити цей комп'ютер?");
    ImGui::Spacing();
    const float role_gap = 10.0f;
    const float role_w = std::max(120.0f, (ImGui::GetContentRegionAvail().x - role_gap * 2.0f) / 3.0f);
    if (selectable_choice_card("##RoleSend", "Віддавати звук", "З цього ПК -> у мережу",
                               cfg_.connect_role == 0, ImVec2(role_w, 60.0f))) {
        cfg_.connect_role = 0;
        config_dirty_ = true;
    }
    ImGui::SameLine(0.0f, role_gap);
    if (selectable_choice_card("##RoleRecv", "Приймати звук", "З мережі -> на цей ПК",
                               cfg_.connect_role == 1, ImVec2(role_w, 60.0f))) {
        cfg_.connect_role = 1;
        config_dirty_ = true;
    }
    ImGui::SameLine(0.0f, role_gap);
    if (selectable_choice_card("##RoleDpx", "Обидва напрямки", "Студійний міст 64x64",
                               cfg_.connect_role == 2, ImVec2(role_w, 60.0f))) {
        cfg_.connect_role = 2;
        config_dirty_ = true;
    }

    ImGui::Spacing();
    const bool role_running = (cfg_.connect_role == 0) ? sender_running_
                                                       : (cfg_.connect_role == 1 ? receiver_running_ : duplex_running_);
    if (!role_running) {
        if (gradient_pill_button("Підключитись / Старт", ImVec2(-1, 48.0f), kColAccentBlue, kColAccentPurp)) {
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
        if (gradient_pill_button("Зупинити з'єднання", ImVec2(-1, 48.0f), kColPink, kColRed)) {
            if (cfg_.connect_role == 0) stop_sender();
            else if (cfg_.connect_role == 1) stop_receiver();
            else stop_duplex();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Automatic LAN / Bonjour Discovery (1 click) ----
    ImGui::TextColored(kColAccentPurp, "2. Пристрої поруч у мережі (Автопошук)");
    ImGui::SameLine(std::max(240.0f, ImGui::GetContentRegionAvail().x - 175.0f));
    const bool is_scanning = scanning_.load() || nodes_scanning_.load();
    if (gradient_pill_button(is_scanning ? "Пошук..." : "Знайти пристрої", ImVec2(165.0f, 28.0f),
                             kColAccentBlue, kColAccentInd)) {
        start_lan_scan();
        scan_aoip_nodes();
    }
    ImGui::Spacing();

    begin_glass_card("##DiscoveredCards", ImVec2(0, 136.0f), true);
    int found_total = 0;
    {
        std::lock_guard<std::mutex> lk(disc_mu_);
        for (size_t i = 0; i < discovered_.size(); ++i) {
            const auto& d = discovered_[i];
            ++found_total;
            char title[128], sub[128];
            std::snprintf(title, sizeof(title), "%s (%s)", d.device_name.c_str(), d.os.c_str());
            std::snprintf(sub, sizeof(sub), "%s:%u  ·  пінг %u мс  ·  клік щоб обрати",
                          d.ip.c_str(), unsigned(d.stream_port), unsigned(d.rtt_ms));
            const bool is_sel = (cfg_.dest_ip == d.ip && cfg_.dest_port == int(d.stream_port));
            ImGui::PushID(int(i) + 100);
            if (selectable_choice_card("##DiscPeer", title, sub, is_sel, ImVec2(-1, 56.0f))) {
                cfg_.dest_ip = d.ip;
                cfg_.dest_port = int(d.stream_port);
                std::snprintf(ip_buf_, sizeof(ip_buf_), "%s", d.ip.c_str());
                std::snprintf(dpx_peer_buf_, sizeof(dpx_peer_buf_), "%s", d.ip.c_str());
                config_dirty_ = true;
                toast_msg_ = "Обрано пристрій: " + d.device_name + " (" + d.ip + ")";
                toast_until_us_ = now_us() + 2500000ull;
            }
            ImGui::PopID();
        }
    }
    {
        std::lock_guard<std::mutex> lk(nodes_mu_);
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const auto& nd = nodes_[i];
            ++found_total;
            char title[128], sub[160];
            std::snprintf(title, sizeof(title), "%s (AoIP %u кан.)", nd.name, nd.channels);
            std::snprintf(sub, sizeof(sub), "%s:%u  ·  %s  ·  клік щоб обрати",
                          nd.ip, nd.rx_port, nd.driver_connected ? "DAW підключена" : "Готовий");
            const bool is_sel = (std::strcmp(dpx_peer_buf_, nd.ip) == 0);
            ImGui::PushID(int(i) + 500);
            if (selectable_choice_card("##AoipPeer", title, sub, is_sel, ImVec2(-1, 56.0f))) {
                std::snprintf(ip_buf_, sizeof(ip_buf_), "%s", nd.ip);
                std::snprintf(dpx_peer_buf_, sizeof(dpx_peer_buf_), "%s", nd.ip);
                cfg_.dest_ip = nd.ip;
                cfg_.dpx_peer_host = nd.ip;
                cfg_.dest_port = nd.rx_port;
                cfg_.dpx_peer_port = nd.rx_port;
                if (nd.duplex && nd.tx_port) cfg_.dpx_listen_port = nd.tx_port;
                config_dirty_ = true;
                toast_msg_ = std::string("Обрано AoIP-вузол: ") + nd.name;
                toast_until_us_ = now_us() + 2500000ull;
            }
            ImGui::PopID();
        }
    }
    if (found_total == 0) {
        ImGui::TextDisabled("Поки що не знайдено інших комп'ютерів із LanVid у вашій мережі.");
        ImGui::TextDisabled("Натисніть «Знайти пристрої» або введіть IP-адресу вручну нижче.");
    }
    end_glass_card();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Ввести IP-адресу та порт вручну")) {
        ImGui::SetNextItemWidth(200.0f);
        if (cfg_.connect_role == 2) {
            if (ImGui::InputText("IP партнера", dpx_peer_buf_, sizeof(dpx_peer_buf_))) config_dirty_ = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            if (ImGui::InputInt("Порт TX", &cfg_.dpx_peer_port, 0, 0)) {
                cfg_.dpx_peer_port = std::clamp(cfg_.dpx_peer_port, 1024, 65535);
                config_dirty_ = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            if (ImGui::InputInt("Порт RX", &cfg_.dpx_listen_port, 0, 0)) {
                cfg_.dpx_listen_port = std::clamp(cfg_.dpx_listen_port, 1024, 65535);
                config_dirty_ = true;
            }
        } else {
            if (ImGui::InputText("IP отримувача", ip_buf_, sizeof(ip_buf_))) config_dirty_ = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(96.0f);
            if (ImGui::InputInt("Порт UDP", &cfg_.dest_port, 0, 0)) {
                cfg_.dest_port = std::clamp(cfg_.dest_port, 1024, 65535);
                cfg_.listen_port = cfg_.dest_port;
                config_dirty_ = true;
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Відеопотік, прев'ю та просунуті параметри")) {
        if (cfg_.connect_role == 0) {
            draw_sender_tab();
        } else if (cfg_.connect_role == 1) {
            draw_receiver_tab();
        } else {
            draw_duplex_tab();
        }
    }

    end_glass_card();

    // ---- RIGHT COLUMN: 3 Large Telemetry Donut Gauges + Live Quick Summary ----
    ImGui::SameLine();
    begin_glass_card("##ConnectTelemetry", ImVec2(0, 0));
    draw_gradient_icon_tile("##TelIcon", IconKind::kWaveform, 32.0f, cfg_.aoip_channels, false);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kColTextMain, "Стан з'єднання в реальному часі");
    ImGui::TextDisabled("Якість мережі · Затримка · Трафік");
    ImGui::EndGroup();
    ImGui::Separator();
    ImGui::Spacing();

    const double total_mbps = tx_stats_.tx_mbps + rx_stats_.rx_mbps +
                              (tx_stats_.audio_kbps + rx_stats_.audio_kbps) / 1000.0;
    const double loss_pct = std::clamp(receiver_running_ ? rx_stats_.loss_pct : tx_stats_.rx_loss_pct, 0.0, 100.0);
    const double health_pct = std::clamp(100.0 - loss_pct, 0.0, 100.0);
    const double est_mbps = estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec);
    const float bw_frac = float(std::clamp(total_mbps / std::max(100.0, est_mbps), 0.0, 1.0));
    const double buf_ms = actual_buf_valid_ ? actual_buf_ms_ : double(cfg_.aoip_latency_ms);
    const float lat_frac = float(std::clamp(buf_ms / 100.0, 0.05, 1.0));

    const float gw_avail = ImGui::GetContentRegionAvail().x;
    const float ggap = 10.0f;
    const float card_w = std::max(100.0f, (gw_avail - ggap * 2.0f) / 3.0f);
    const float card_h = 136.0f;

    char hlth_val[24];
    std::snprintf(hlth_val, sizeof(hlth_val), "%.0f%%", health_pct);
    draw_ring_gauge("Якість мережі", hlth_val, loss_pct > 0.5 ? "Втрати" : "Чудово",
                    float(health_pct / 100.0), card_w, card_h, loss_pct > 1.0, true);

    ImGui::SameLine(0.0f, ggap);
    char lat_val[24];
    std::snprintf(lat_val, sizeof(lat_val), "%.0f", buf_ms);
    draw_ring_gauge("Затримка", lat_val, "мс", lat_frac, card_w, card_h, buf_ms > 50.0, false);

    ImGui::SameLine(0.0f, ggap);
    char tr_val[24], tr_sub[24];
    std::snprintf(tr_val, sizeof(tr_val), "%.1f", total_mbps);
    std::snprintf(tr_sub, sizeof(tr_sub), "Mbps");
    draw_ring_gauge("Трафік", tr_val, tr_sub, std::max(0.06f, bw_frac), card_w, card_h, bw_frac > 0.85f, false);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Quick status summary cards
    begin_glass_card("##QuickSummary", ImVec2(0, 150.0f), true);
    ImGui::TextColored(kColAccentPurp, "Поточна конфігурація");
    ImGui::Spacing();
    const char* q_names[4] = {"Максимальна (32-bit)", "Студійна (24-bit)", "Збалансована (16-bit)", "Економна (Wi-Fi)"};
    ImGui::Text("Канали: %d кан. @ 48 кГц  ·  %s", cfg_.aoip_channels, q_names[std::clamp(cfg_.audio_codec, 0, 3)]);
    ImGui::Text("Очікуваний трафік: ~%.1f Мбіт/с", est_mbps);
    ImGui::Text("Режим затримки: %s (%d мс)",
                cfg_.adaptive_jitter ? "Авто для Wi-Fi" : "Фіксована для кабелю", cfg_.aoip_latency_ms);
    ImGui::Text("Активні маршрути: %zu аудіо  ·  %zu MIDI",
                audio_matrix_.crosspoints().size(), audio_matrix_.midi_crosspoints().size());
    ImGui::Spacing();
    if (status_pill_button("Відкрити маршрути ->", false, ImVec2(0, 28))) { cfg_.active_tab = 1; config_dirty_ = true; }
    ImGui::SameLine();
    if (status_pill_button("Налаштування ->", false, ImVec2(0, 28))) { cfg_.active_tab = 3; config_dirty_ = true; }
    end_glass_card();

    // Compact live meters preview if receiver is running
    if (receiver_running_) {
        ImGui::Spacing();
        ImGui::TextColored(kColAccentPurp, "Живі рівні каналів");
        draw_aoip_meter_grid();
    }

    end_glass_card();
}

void LanVidStudioApp::draw_devices_tab() {
    draw_add_device_popups();

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
        ImGui::OpenPopup("##AddAudioDev");
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
        begin_glass_card("##DevSlotCard", ImVec2(0, 96.0f), true, ImGuiWindowFlags_NoScrollbar);

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

        ImGui::SameLine(std::max(160.0f, ImGui::GetWindowWidth() - 62.0f));
        bool act = s.active;
        if (studio_toggle("##DevAct", &act, kColAccentInd)) {
            s.active = act;
            audio_matrix_.update_slot(s.id, s);
            config_dirty_ = true;
        }
        if (s.type != MatrixSlotType::kVirtualBus && s.type != MatrixSlotType::kNetworkAoip) {
            ImGui::SameLine(std::max(120.0f, ImGui::GetWindowWidth() - 94.0f));
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
        ImGui::OpenPopup("##AddMidiDev");
    }
    ImGui::SameLine();
    if (gradient_pill_button("PANIC (Скинути всі ноти)", ImVec2(-1, 32), kColPink, kColRed)) {
        audio_matrix_.send_midi_panic(-1);
        toast_msg_ = "MIDI PANIC: надіслано All Notes Off на всі 16 каналів";
        toast_until_us_ = now_us() + 2500000ull;
    }

    ImGui::Spacing();
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
        ImGui::TextColored(blinking ? kColGreen : kColTextMuted, "●");
        ImGui::SameLine(0.0f, 5.0f);
        ImGui::TextColored(kColTextMain, "%s", ms.name.c_str());

        const char* mtag = (ms.type == MidiSlotType::kMidiInput)
                               ? "Вхід MIDI"
                               : (ms.type == MidiSlotType::kMidiOutput
                                      ? "Вихід MIDI"
                                      : (ms.type == MidiSlotType::kNetworkMidiTx ? "У мережу (MIDI2)" : "З мережі (MIDI2)"));
        ImGui::TextColored(blinking ? kColAccentPurp : kColTextMuted, "%s · %llu подій",
                           mtag, (unsigned long long)mst.events_count);
        if (ms.type == MidiSlotType::kMidiOutput || ms.type == MidiSlotType::kNetworkMidiTx) {
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

    // ---- LEFT COLUMN: Audio Quality Cards + Channel Count Pills ----
    ImGui::BeginGroup();
    ImGui::TextColored(kColAccentPurp, "1. Якість звуку і трафік мережі");
    ImGui::TextDisabled("Оберіть баланс між точністю звучання та навантаженням на Wi-Fi чи Ethernet");
    ImGui::Spacing();

    struct QualityCard {
        int codec;
        const char* title;
        const char* desc;
    };
    const QualityCard qcards[4] = {
        {0, "Максимальна (Без втрат 32-bit)", "Для кабелю Ethernet (~98 Мбіт/с на 64 кан.)"},
        {1, "Студійна (24-bit)",              "Оптимально для швидкого Wi-Fi 6 (-25% трафіку)"},
        {2, "Збалансована (16-bit)",          "Стабільно для звичайного Wi-Fi (-50% трафіку)"},
        {3, "Економна (Стиснення для Wi-Fi)", "Мінімальне навантаження на мережу (-80% трафіку)"},
    };
    for (const auto& q : qcards) {
        ImGui::PushID(q.codec);
        if (selectable_choice_card("##QCard", q.title, q.desc, cfg_.audio_codec == q.codec, ImVec2(left_w, 60.0f))) {
            cfg_.audio_codec = q.codec;
            if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
            if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
            config_dirty_ = true;
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("Орієнтовний трафік для %d каналів: %.1f Мбіт/с",
                        cfg_.aoip_channels, estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec));

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
    }
    if (selectable_choice_card("##LatFixed", "Фіксована для кабелю (Для живої гри)",
                               "Жорстко тримає вибрані 5 / 10 / 20 мс без змін",
                               cfg_.adaptive_jitter == 0, ImVec2(right_w, 64.0f))) {
        cfg_.adaptive_jitter = 0;
        apply_latency_mode();
        config_dirty_ = true;
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

    ImGui::TextColored(kColAccentPurp, "4. Додаткові налаштування");
    ImGui::Spacing();
    if (studio_toggle("##SetAudioOnly", &cfg_.audio_only, kColAccentInd)) {
        config_dirty_ = true;
        if (sender_running_) start_sender();
        if (receiver_running_) start_receiver();
    }
    ImGui::SameLine();
    ImGui::TextColored(kColTextMain, "Лише звук (Вимкнути відео)");
    ImGui::TextDisabled("Економить процесор і трафік, якщо потрібен тільки звук та MIDI");

    ImGui::Spacing();
    if (studio_toggle("##SetFbGuard", &cfg_.matrix_feedback_guard, kColAccentPurp)) {
        audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(kColTextMain, "Захист від петлі зворотного зв'язку");
    ImGui::TextDisabled("Блокує випадкове замикання виходу пристрою на власний вхід");

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
    ImGui::EndGroup();

    end_glass_card();
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
    if (selectable_choice_card("##QrMidi", "MIDI-контролер у DAW", "Усі MIDI -> DAW / Мережа",
                               matrix_active_preset_ == 3, ImVec2(qw, 56.0f))) {
        matrix_active_preset_ = 3;
        matrix_patch_mode_ = 1;
        const auto mslots = audio_matrix_.midi_slots();
        for (const auto& s : mslots) {
            if (s.type != MidiSlotType::kMidiInput && s.type != MidiSlotType::kNetworkMidiRx) continue;
            for (const auto& d : mslots) {
                if (d.type != MidiSlotType::kMidiOutput && d.type != MidiSlotType::kNetworkMidiTx) continue;
                if (s.type == MidiSlotType::kNetworkMidiRx && d.type == MidiSlotType::kNetworkMidiTx) continue;
                audio_matrix_.set_midi_crosspoint(s.id, d.id, 0, false);
            }
        }
        config_dirty_ = true;
        toast_msg_ = "Сценарій: Усі MIDI-входи направлено в DAW та мережу";
        toast_until_us_ = now_us() + 2500000ull;
    }
    ImGui::SameLine(0.0f, qgap);
    if (selectable_choice_card("##QrClear", "Очистити все", "Видалити всі маршрути",
                               false, ImVec2(qw, 56.0f))) {
        ImGui::OpenPopup("##ConfirmClearAll");
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- 2. MODE & SCALE CONTROLS ----
    if (status_pill_button("Аудіо-маршрути", matrix_patch_mode_ == 0, ImVec2(0, 30))) matrix_patch_mode_ = 0;
    ImGui::SameLine();
    if (status_pill_button("MIDI-маршрути", matrix_patch_mode_ == 1, ImVec2(0, 30))) matrix_patch_mode_ = 1;

    if (matrix_patch_mode_ == 0) {
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
            if (s.type == MidiSlotType::kMidiInput || s.type == MidiSlotType::kNetworkMidiRx) srcs.push_back(s);
            if (s.type == MidiSlotType::kMidiOutput || s.type == MidiSlotType::kNetworkMidiTx) dsts.push_back(s);
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
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

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
    ImGui::BeginChild("##MainWorkspace", ImVec2(0, 0), false);
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
