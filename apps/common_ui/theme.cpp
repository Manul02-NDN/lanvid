#include "common_ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lv::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;
ThemePalette g_pal{};
bool g_init = false;

void populate(bool dark) {
    g_pal.dark = dark;
    if (!dark) {
        g_pal.bg_top_left  = ImVec4(0.867f, 0.839f, 0.996f, 1.00f);  // #DDD6FE
        g_pal.bg_top_right = ImVec4(0.878f, 0.906f, 1.000f, 1.00f);  // #E0E7FF
        g_pal.bg_bot_right = ImVec4(0.929f, 0.914f, 0.996f, 1.00f);  // #EDE9FE
        g_pal.bg_bot_left  = ImVec4(0.894f, 0.878f, 1.000f, 1.00f);  // #E4E0FF
        g_pal.sidebar      = ImVec4(0.961f, 0.953f, 1.000f, 0.94f);  // #F5F3FF
        g_pal.card         = ImVec4(1.000f, 1.000f, 1.000f, 0.97f);  // #FFFFFF
        g_pal.card_soft    = ImVec4(0.953f, 0.937f, 0.996f, 0.96f);  // #F3EFFE
        g_pal.border       = ImVec4(0.867f, 0.839f, 0.996f, 1.00f);  // #DDD6FE
        g_pal.accent_blue  = ImVec4(0.376f, 0.647f, 0.980f, 1.00f);  // #60A5FA
        g_pal.accent_ind   = ImVec4(0.388f, 0.400f, 0.945f, 1.00f);  // #6366F1
        g_pal.accent_node  = ImVec4(0.545f, 0.361f, 0.965f, 1.00f);  // #8B5CF6
        g_pal.accent_purp  = ImVec4(0.576f, 0.200f, 0.918f, 1.00f);  // #9333EA
        g_pal.accent_teal  = ImVec4(0.078f, 0.722f, 0.651f, 1.00f);  // #14B8A6
        g_pal.green        = ImVec4(0.063f, 0.725f, 0.506f, 1.00f);  // #10B981
        g_pal.amber        = ImVec4(0.961f, 0.620f, 0.043f, 1.00f);  // #F59E0B
        g_pal.red          = ImVec4(0.937f, 0.267f, 0.267f, 1.00f);  // #EF4444
        g_pal.text_main    = ImVec4(0.118f, 0.106f, 0.294f, 1.00f);  // #1E1B4B
        g_pal.text_muted   = ImVec4(0.486f, 0.451f, 0.600f, 1.00f);  // #7C7399
    } else {
        g_pal.bg_top_left  = ImVec4(0.071f, 0.063f, 0.122f, 1.00f);
        g_pal.bg_top_right = ImVec4(0.082f, 0.078f, 0.145f, 1.00f);
        g_pal.bg_bot_right = ImVec4(0.094f, 0.082f, 0.161f, 1.00f);
        g_pal.bg_bot_left  = ImVec4(0.075f, 0.067f, 0.129f, 1.00f);
        g_pal.sidebar      = ImVec4(0.110f, 0.098f, 0.184f, 0.96f);
        g_pal.card         = ImVec4(0.137f, 0.122f, 0.227f, 0.96f);
        g_pal.card_soft    = ImVec4(0.165f, 0.145f, 0.271f, 0.96f);
        g_pal.border       = ImVec4(0.255f, 0.224f, 0.400f, 1.00f);
        g_pal.accent_blue  = ImVec4(0.376f, 0.647f, 0.980f, 1.00f);
        g_pal.accent_ind   = ImVec4(0.506f, 0.549f, 0.973f, 1.00f);
        g_pal.accent_node  = ImVec4(0.655f, 0.545f, 0.980f, 1.00f);
        g_pal.accent_purp  = ImVec4(0.753f, 0.518f, 0.988f, 1.00f);
        g_pal.accent_teal  = ImVec4(0.176f, 0.831f, 0.749f, 1.00f);
        g_pal.green        = ImVec4(0.063f, 0.725f, 0.506f, 1.00f);
        g_pal.amber        = ImVec4(0.961f, 0.620f, 0.043f, 1.00f);
        g_pal.red          = ImVec4(0.937f, 0.267f, 0.267f, 1.00f);
        g_pal.text_main    = ImVec4(0.945f, 0.937f, 0.984f, 1.00f);
        g_pal.text_muted   = ImVec4(0.647f, 0.616f, 0.765f, 1.00f);
    }
    g_init = true;
}

}  // namespace

const ThemePalette& theme_palette() {
    if (!g_init) populate(false);
    return g_pal;
}

void apply_theme(bool dark) {
    populate(dark);
    if (!ImGui::GetCurrentContext()) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 12.0f;
    st.ChildRounding = 12.0f;
    st.FrameRounding = 8.0f;
    st.PopupRounding = 10.0f;
    st.ScrollbarRounding = 8.0f;
    st.GrabRounding = 8.0f;
    st.TabRounding = 8.0f;
    st.WindowPadding = ImVec2(14.0f, 12.0f);
    st.FramePadding = ImVec2(10.0f, 6.0f);
    st.ItemSpacing = ImVec2(8.0f, 7.0f);
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = g_pal.text_main;
    c[ImGuiCol_TextDisabled] = g_pal.text_muted;
    c[ImGuiCol_WindowBg] = g_pal.bg_bot_right;
    c[ImGuiCol_ChildBg] = g_pal.card;
    c[ImGuiCol_PopupBg] = g_pal.card;
    c[ImGuiCol_Border] = g_pal.border;
    c[ImGuiCol_FrameBg] = g_pal.card_soft;
    c[ImGuiCol_FrameBgHovered] = ImVec4(g_pal.accent_node.x, g_pal.accent_node.y, g_pal.accent_node.z, 0.22f);
    c[ImGuiCol_FrameBgActive] = ImVec4(g_pal.accent_node.x, g_pal.accent_node.y, g_pal.accent_node.z, 0.34f);
    c[ImGuiCol_Button] = ImVec4(g_pal.accent_node.x, g_pal.accent_node.y, g_pal.accent_node.z, 0.85f);
    c[ImGuiCol_ButtonHovered] = g_pal.accent_node;
    c[ImGuiCol_ButtonActive] = g_pal.accent_purp;
    c[ImGuiCol_Header] = ImVec4(g_pal.accent_node.x, g_pal.accent_node.y, g_pal.accent_node.z, 0.24f);
    c[ImGuiCol_HeaderHovered] = ImVec4(g_pal.accent_node.x, g_pal.accent_node.y, g_pal.accent_node.z, 0.36f);
    c[ImGuiCol_HeaderActive] = ImVec4(g_pal.accent_node.x, g_pal.accent_node.y, g_pal.accent_node.z, 0.48f);
    c[ImGuiCol_CheckMark] = g_pal.accent_purp;
    c[ImGuiCol_SliderGrab] = g_pal.accent_node;
    c[ImGuiCol_SliderGrabActive] = g_pal.accent_purp;
    c[ImGuiCol_Separator] = g_pal.border;
    c[ImGuiCol_Tab] = g_pal.card_soft;
    c[ImGuiCol_TabHovered] = ImVec4(g_pal.accent_node.x, g_pal.accent_node.y, g_pal.accent_node.z, 0.30f);
    c[ImGuiCol_TabActive] = g_pal.accent_node;
}

void draw_lavender_backdrop(ImDrawList* dl, ImVec2 pos, ImVec2 size) {
    if (!dl) return;
    const auto& p = theme_palette();
    dl->AddRectFilledMultiColor(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                ImGui::GetColorU32(p.bg_top_left), ImGui::GetColorU32(p.bg_top_right),
                                ImGui::GetColorU32(p.bg_bot_right), ImGui::GetColorU32(p.bg_bot_left));
}

void draw_status_pill(const char* label, const ImVec4& color, bool filled) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pad(8.0f, 3.0f);
    const ImVec2 txt_sz = ImGui::CalcTextSize(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 sz(txt_sz.x + pad.x * 2.0f, txt_sz.y + pad.y * 2.0f);
    const ImU32 bg = ImGui::GetColorU32(filled ? color : ImVec4(color.x, color.y, color.z, 0.16f));
    const ImU32 fg = filled ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(color);
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), bg, sz.y * 0.5f);
    if (!filled) dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ImGui::GetColorU32(color), sz.y * 0.5f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), fg, label);
    ImGui::Dummy(sz);
}

void draw_vu_bar(float dbfs, float width, float height) {
    const auto& p = theme_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float norm = std::clamp((dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(p.card_soft), 4.0f);
    if (norm > 0.01f) {
        const ImVec4 col = (norm > 0.88f) ? p.red : (norm > 0.65f ? p.amber : p.accent_node);
        dl->AddRectFilled(pos, ImVec2(pos.x + width * norm, pos.y + height), ImGui::GetColorU32(col), 4.0f);
    }
    ImGui::Dummy(ImVec2(width, height));
}

void draw_telemetry_ring(const char* title, const char* value_str, const char* sub_str, float fraction,
                         float card_w, float card_h, bool warn) {
    const auto& p = theme_palette();
    ImGui::BeginChild(title, ImVec2(card_w, card_h), true, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const ImVec2 center(win_pos.x + card_w * 0.5f, win_pos.y + card_h * 0.48f);
    const float radius = std::min(card_w, card_h) * 0.28f;
    const float frac = std::clamp(fraction, 0.0f, 1.0f);

    const ImVec4 active_col = warn ? p.red : p.accent_node;
    dl->AddCircle(center, radius, ImGui::GetColorU32(p.border), 32, 5.0f);
    if (frac > 0.005f) {
        const float a0 = -kPi * 0.5f;
        const float a1 = a0 + 2.0f * kPi * frac;
        dl->PathClear();
        dl->PathArcTo(center, radius, a0, a1, 32);
        dl->PathStroke(ImGui::GetColorU32(active_col), 0, 5.0f);
    }

    const ImVec2 val_sz = ImGui::CalcTextSize(value_str);
    dl->AddText(ImVec2(center.x - val_sz.x * 0.5f, center.y - val_sz.y * 0.5f),
                ImGui::GetColorU32(p.text_main), value_str);
    const ImVec2 ttl_sz = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(win_pos.x + (card_w - ttl_sz.x) * 0.5f, win_pos.y + 6.0f),
                ImGui::GetColorU32(p.text_muted), title);
    if (sub_str && *sub_str) {
        const ImVec2 sub_sz = ImGui::CalcTextSize(sub_str);
        dl->AddText(ImVec2(win_pos.x + (card_w - sub_sz.x) * 0.5f, win_pos.y + card_h - sub_sz.y - 6.0f),
                    ImGui::GetColorU32(p.text_muted), sub_str);
    }
    ImGui::EndChild();
}

bool draw_subtab_pills(int& active_idx, const char* const* labels, int count, const char* id_prefix) {
    const auto& p = theme_palette();
    bool changed = false;
    active_idx = std::clamp(active_idx, 0, count - 1);
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool sel = (active_idx == i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        char bid[96];
        std::snprintf(bid, sizeof(bid), "%s##%s_%d", labels[i], id_prefix, i);
        if (ImGui::Button(bid, ImVec2(0.0f, 30.0f))) {
            active_idx = i;
            changed = true;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::Separator();
    return changed;
}

}  // namespace lv::ui
