#pragma once
// Shared Soft Pastel Lavender / Dark Studio Glass UI components and desktop window host
// used by both LanVid Audio (`lanvid_audio_studio`) and LanVid Video (`lanvid_video_studio`).

#include <functional>
#include <string>
#include <vector>

#include "gui/gpu_texture.hpp"
#include "imgui.h"
#include "net/discovery.hpp"
#include "net/network_interfaces.hpp"

namespace lv::gui {

struct StudioThemePalette {
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

// Applies Soft Pastel Lavender (default) or Dark Studio visual theme to ImGui.
void apply_studio_theme(bool dark = false);
const StudioThemePalette& studio_palette();

// Draws the subtle lavender or dark studio window backdrop.
void draw_lavender_backdrop(ImDrawList* dl, ImVec2 pos, ImVec2 size);

// Draws a smooth cubic Bezier cable (solid or dashed) with optional animated flow dots.
void draw_studio_bezier_cable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 color,
                              float thickness = 3.0f, bool animated = true, float time_sec = 0.0f);
void draw_studio_dashed_bezier_cable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 color,
                                     float thickness = 2.2f, float time_sec = 0.0f);

// Draws a circular telemetry ring gauge card (CPU, Bitrate, Jitter, Health, Buffer).
void draw_telemetry_ring_gauge(const char* title, const char* value_str, const char* sub_str,
                               float fraction, float card_w = 136.0f, float card_h = 112.0f,
                               bool warn = false, bool health_mode = false);

// Draws a compact rounded status badge ("LIVE", "STBY", "[AUDIO]", "[VIDEO]", etc.).
void draw_status_pill(const char* label, const ImVec4& color, bool filled = true);

// Draws a list of LAN-discovered LanVid nodes with [AUDIO] / [VIDEO] role badges.
// If `filter_role` is non-empty ("audio" or "video"), highlights matching nodes first.
void draw_discovered_nodes_list(const char* id,
                                const std::vector<lv::DiscoveredReceiver>& nodes,
                                const std::string& filter_role,
                                const std::function<void(const lv::DiscoveredReceiver&)>& on_select);

// Draws the reusable Primary + Secondary NIC selector block (Ethernet / Wi-Fi / 1+1 Failover).
// Returns true if user changed NIC selection.
bool draw_nic_selector_panel(const char* id_prefix,
                             const std::vector<lv::NetworkInterfaceInfo>& nics,
                             std::string& primary_nic_id,
                             std::string& primary_nic_ip,
                             std::string& secondary_nic_id,
                             std::string& secondary_nic_ip,
                             bool& dual_nic_enabled,
                             uint64_t failover_packets,
                             const std::function<void()>& on_refresh);

// Interface implemented by split desktop apps (LanVidAudioStudioApp, LanVidVideoStudioApp).
class IStudioDesktopApp {
public:
    virtual ~IStudioDesktopApp() = default;
    virtual void set_platform_host(const PlatformHost& host) = 0;
    virtual void init() = 0;
    virtual void shutdown() = 0;
    virtual void render_frame() = 0;
    virtual const char* app_display_name() const = 0;
    virtual std::string active_config_path() const = 0;
};

struct StudioWindowSpec {
    const char* window_title = "LanVid Studio";
    const char* window_class = "LanVidStudioWindowClass";
    int width = 1280;
    int height = 780;
    int smoke_test_ms = 0;
};

// Launches the native OS window + GPU renderer (Win32/D3D11 or SDL2/OpenGL3) and runs `app`.
int run_studio_desktop_app(const StudioWindowSpec& spec, IStudioDesktopApp& app);

}  // namespace lv::gui
