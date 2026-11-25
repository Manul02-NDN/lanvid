#include "gui/studio_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lv::gui {

namespace {

constexpr float kPi = 3.14159265358979323846f;
StudioThemePalette g_pal{};
bool g_pal_init = false;

void populate_palette(bool dark) {
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
    g_pal_init = true;
}

ImVec2 cubic_bezier(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) {
    const float u = 1.0f - t;
    const float tt = t * t;
    const float uu = u * u;
    const float uuu = uu * u;
    const float ttt = tt * t;
    return ImVec2(uuu * p0.x + 3.0f * uu * t * p1.x + 3.0f * u * tt * p2.x + ttt * p3.x,
                  uuu * p0.y + 3.0f * uu * t * p1.y + 3.0f * u * tt * p2.y + ttt * p3.y);
}

}  // namespace

const StudioThemePalette& studio_palette() {
    if (!g_pal_init) populate_palette(false);
    return g_pal;
}

void apply_studio_theme(bool dark) {
    populate_palette(dark);
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
    const auto& p = studio_palette();
    dl->AddRectFilledMultiColor(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                ImGui::GetColorU32(p.bg_top_left),
                                ImGui::GetColorU32(p.bg_top_right),
                                ImGui::GetColorU32(p.bg_bot_right),
                                ImGui::GetColorU32(p.bg_bot_left));
}

void draw_studio_bezier_cable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 color,
                              float thickness, bool animated, float time_sec) {
    if (!dl) return;
    const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.45f);
    const ImVec2 c0(p0.x + dx, p0.y);
    const ImVec2 c1(p1.x - dx, p1.y);
    dl->AddBezierCubic(p0, c0, c1, p1, color, thickness, 28);
    if (animated) {
        const float t = std::fmod(time_sec * 0.55f, 1.0f);
        const ImVec2 dot = cubic_bezier(p0, c0, c1, p1, t);
        dl->AddCircleFilled(dot, thickness * 1.4f, IM_COL32(255, 255, 255, 220));
    }
}

void draw_studio_dashed_bezier_cable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 color,
                                     float thickness, float time_sec) {
    if (!dl) return;
    const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.45f);
    const ImVec2 c0(p0.x + dx, p0.y + 18.0f);
    const ImVec2 c1(p1.x - dx, p1.y + 18.0f);
    constexpr int kSegs = 24;
    const float phase = std::fmod(time_sec * 1.2f, 1.0f);
    for (int i = 0; i < kSegs; i += 2) {
        const float t0 = std::clamp((float(i) + phase) / float(kSegs), 0.0f, 1.0f);
        const float t1 = std::clamp((float(i + 1) + phase) / float(kSegs), 0.0f, 1.0f);
        if (t1 <= t0) continue;
        dl->AddLine(cubic_bezier(p0, c0, c1, p1, t0), cubic_bezier(p0, c0, c1, p1, t1), color, thickness);
    }
}

void draw_telemetry_ring_gauge(const char* title, const char* value_str, const char* sub_str,
                               float fraction, float card_w, float card_h,
                               bool warn, bool health_mode) {
    const auto& p = studio_palette();
    ImGui::BeginChild(title, ImVec2(card_w, card_h), true, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const ImVec2 center(win_pos.x + card_w * 0.5f, win_pos.y + card_h * 0.48f);
    const float radius = std::min(card_w, card_h) * 0.28f;
    const float frac = std::clamp(fraction, 0.0f, 1.0f);

    const ImVec4 active_col = warn ? p.red : (health_mode ? p.green : p.accent_node);
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

void draw_status_pill(const char* label, const ImVec4& color, bool filled) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pad(8.0f, 3.0f);
    const ImVec2 txt_sz = ImGui::CalcTextSize(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 sz(txt_sz.x + pad.x * 2.0f, txt_sz.y + pad.y * 2.0f);
    const ImU32 bg = ImGui::GetColorU32(filled ? color : ImVec4(color.x, color.y, color.z, 0.16f));
    const ImU32 fg = filled ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(color);
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), bg, sz.y * 0.5f);
    if (!filled) {
        dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ImGui::GetColorU32(color), sz.y * 0.5f);
    }
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), fg, label);
    ImGui::Dummy(sz);
}

void draw_discovered_nodes_list(const char* id,
                                const std::vector<lv::DiscoveredReceiver>& nodes,
                                const std::string& filter_role,
                                const std::function<void(const lv::DiscoveredReceiver&)>& on_select) {
    const auto& p = studio_palette();
    ImGui::PushID(id);
    if (nodes.empty()) {
        ImGui::TextColored(p.text_muted, "Вузлів у LAN не знайдено — натисніть «Оновити».");
    } else {
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto& n = nodes[i];
            if (!filter_role.empty() && !n.role.empty() && n.role != filter_role) continue;
            const char* role_badge = (n.role == "audio") ? "[AUDIO]"
                                                         : (n.role == "video" ? "[VIDEO]" : "[LANVID]");
            const ImVec4 badge_col = (n.role == "audio") ? p.accent_node
                                                         : (n.role == "video" ? p.accent_blue : p.text_muted);
            ImGui::PushID(int(i));
            draw_status_pill(role_badge, badge_col, false);
            ImGui::SameLine();
            char label[256];
            std::snprintf(label, sizeof(label), "%s  (%s:%u · %u ms)",
                          n.device_name.c_str(), n.ip.c_str(), unsigned(n.stream_port), unsigned(n.rtt_ms));
            if (ImGui::Selectable(label, false)) {
                if (on_select) on_select(n);
            }
            ImGui::PopID();
        }
    }
    ImGui::PopID();
}

bool draw_nic_selector_panel(const char* id_prefix,
                             const std::vector<lv::NetworkInterfaceInfo>& nics,
                             std::string& primary_nic_id,
                             std::string& primary_nic_ip,
                             std::string& secondary_nic_id,
                             std::string& secondary_nic_ip,
                             bool& dual_nic_enabled,
                             uint64_t failover_packets,
                             const std::function<void()>& on_refresh) {
    const auto& p = studio_palette();
    bool changed = false;
    ImGui::PushID(id_prefix);
    ImGui::TextColored(p.text_main, "Мережеві адаптери та Резервування (Ethernet / Wi-Fi)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Оновити NICs")) {
        if (on_refresh) on_refresh();
    }

    auto format_nic = [](const lv::NetworkInterfaceInfo& n) -> std::string {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[%s] %s — %s (%u Mbps)",
                      lv::network_interface_type_tag(n.type),
                      n.friendly_name.c_str(), n.ipv4_address.c_str(), unsigned(n.link_speed_mbps));
        return buf;
    };

    const std::string pri_preview = primary_nic_ip.empty() ? "Автоматично (OS Default)" : primary_nic_ip;
    if (ImGui::BeginCombo("Основний адаптер (Primary NIC)", pri_preview.c_str())) {
        if (ImGui::Selectable("Автоматично (OS Default)", primary_nic_ip.empty())) {
            primary_nic_id.clear();
            primary_nic_ip.clear();
            changed = true;
        }
        for (const auto& n : nics) {
            const std::string lbl = format_nic(n);
            const bool sel = (primary_nic_ip == n.ipv4_address);
            if (ImGui::Selectable(lbl.c_str(), sel)) {
                primary_nic_id = n.id;
                primary_nic_ip = n.ipv4_address;
                if (secondary_nic_ip == primary_nic_ip) {
                    secondary_nic_id.clear();
                    secondary_nic_ip.clear();
                    dual_nic_enabled = false;
                }
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    const std::string sec_preview = secondary_nic_ip.empty() ? "Вимкнено (1 NIC)" : secondary_nic_ip;
    if (ImGui::BeginCombo("Резервний адаптер (Secondary 1+1)", sec_preview.c_str())) {
        if (ImGui::Selectable("Вимкнено (1 NIC)", secondary_nic_ip.empty())) {
            secondary_nic_id.clear();
            secondary_nic_ip.clear();
            dual_nic_enabled = false;
            changed = true;
        }
        for (const auto& n : nics) {
            if (!primary_nic_ip.empty() && n.ipv4_address == primary_nic_ip) continue;
            const std::string lbl = format_nic(n);
            const bool sel = (secondary_nic_ip == n.ipv4_address);
            if (ImGui::Selectable(lbl.c_str(), sel)) {
                secondary_nic_id = n.id;
                secondary_nic_ip = n.ipv4_address;
                dual_nic_enabled = true;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    const bool pri_up = primary_nic_ip.empty() ? !nics.empty() : lv::is_interface_ip_up(primary_nic_ip);
    const bool sec_up = !secondary_nic_ip.empty() && lv::is_interface_ip_up(secondary_nic_ip);
    draw_status_pill(pri_up ? "Primary: UP" : "Primary: DOWN", pri_up ? p.green : p.red, false);
    if (!secondary_nic_ip.empty()) {
        ImGui::SameLine();
        draw_status_pill(sec_up ? "Secondary: UP" : "Secondary: DOWN", sec_up ? p.accent_teal : p.amber, false);
        ImGui::SameLine();
        ImGui::TextColored(p.text_muted, "Failover Pkts: %llu", static_cast<unsigned long long>(failover_packets));
    }

    ImGui::PopID();
    return changed;
}

}  // namespace lv::gui
