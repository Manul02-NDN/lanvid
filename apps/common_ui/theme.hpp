#pragma once
// Soft Pastel Lavender & Dark Studio Glass palette + neutral Dear ImGui styling helpers.

#include "imgui.h"

namespace lv::ui {

struct ThemePalette {
    bool dark = false;
    ImVec4 bg_top_left{};
    ImVec4 bg_top_right{};
    ImVec4 bg_bot_right{};
    ImVec4 bg_bot_left{};
    ImVec4 sidebar{};
    ImVec4 card{};
    ImVec4 card_soft{};
    ImVec4 border{};
    ImVec4 accent_blue{};
    ImVec4 accent_ind{};
    ImVec4 accent_node{};
    ImVec4 accent_purp{};
    ImVec4 accent_teal{};
    ImVec4 green{};
    ImVec4 amber{};
    ImVec4 red{};
    ImVec4 text_main{};
    ImVec4 text_muted{};
};

void apply_theme(bool dark = false);
const ThemePalette& theme_palette();

void draw_lavender_backdrop(ImDrawList* dl, ImVec2 pos, ImVec2 size);
void draw_status_pill(const char* label, const ImVec4& color, bool filled = true);
void draw_vu_bar(float dbfs, float width = 140.0f, float height = 12.0f);
void draw_telemetry_ring(const char* title, const char* value_str, const char* sub_str, float fraction,
                         float card_w = 136.0f, float card_h = 112.0f, bool warn = false);
bool draw_subtab_pills(int& active_idx, const char* const* labels, int count, const char* id_prefix);

}  // namespace lv::ui
