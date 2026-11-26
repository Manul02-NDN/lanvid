#include "audio_app.hpp"

#include <algorithm>
#include <cstdio>

#include "common_ui/qr_code.hpp"
#include "common_ui/theme.hpp"
#include "imgui.h"

namespace lv::apps {

namespace {
constexpr const char* kAudioPages[] = {
    "AoIP та Grid L2",
    "Patchbay 64x64",
    "Персональний монітор",
    "MIDI2 та Налаштування",
};
constexpr const char* kCodecLabels[] = {"PCM32F", "PCM24", "PCM16", "Opus CELT"};
constexpr int kChannelChoices[] = {2, 8, 16, 32, 64};
}  // namespace

LanVidAudioApp::LanVidAudioApp(std::string config_path_override) : svc_(std::move(config_path_override)) {}
LanVidAudioApp::~LanVidAudioApp() { shutdown(); }

void LanVidAudioApp::init() {
    svc_.load_config();
    auto& c = svc_.config();
    std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", c.peer_ip.c_str());
    std::snprintf(studio_name_buf_, sizeof(studio_name_buf_), "%s", c.studio_name.c_str());
    lv::ui::apply_theme(c.dark_theme);
    nics_ = svc_.network_interfaces();
    svc_.start_services(uint16_t(c.listen_port), uint16_t(c.monitor_port), uint16_t(c.discovery_port));
}

void LanVidAudioApp::shutdown() {
    svc_.config().peer_ip = peer_ip_buf_;
    svc_.config().studio_name = studio_name_buf_;
    svc_.stop_services();
    svc_.save_config();
}

void LanVidAudioApp::render_frame() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidAudioRoot", nullptr, wf);
    lv::ui::draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##AudioSide", ImVec2(220.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##AudioMain", ImVec2(0.0f, avail_h), false);
    const int p = svc_.config().active_page;
    if (p == 0) draw_aoip_grid_page();
    else if (p == 1) draw_patchbay_page();
    else if (p == 2) draw_monitor_page();
    else draw_midi_settings_page();
    ImGui::EndChild();

    ImGui::End();
}

void LanVidAudioApp::draw_top_bar() {
    const auto& p = lv::ui::theme_palette();
    auto& c = svc_.config();
    ImGui::TextColored(p.accent_purp, "LanVid Audio v2.7.0");
    ImGui::SameLine();
    lv::ui::draw_status_pill(svc_.services_running() ? "ENGINE LIVE" : "STOPPED",
                             svc_.services_running() ? p.green : p.red, true);
    ImGui::SameLine();
    lv::ui::draw_status_pill(svc_.duplex_running() ? "AOIP 64ch TX/RX" : "AOIP IDLE",
                             svc_.duplex_running() ? p.accent_node : p.text_muted, false);
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "Audio :%u  |  Monitor :%u  |  Disc :%u",
                       unsigned(svc_.bound_audio_port()), unsigned(svc_.bound_monitor_port()),
                       unsigned(svc_.bound_discovery_port()));
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - 180.0f));
    if (ImGui::SmallButton(svc_.duplex_running() ? "Зупинити потік" : "Запустити потік")) {
        c.peer_ip = peer_ip_buf_;
        if (svc_.duplex_running()) svc_.stop_duplex_stream();
        else svc_.start_duplex_stream();
    }
}

void LanVidAudioApp::draw_sidebar() {
    const auto& p = lv::ui::theme_palette();
    auto& c = svc_.config();
    for (int i = 0; i < 4; ++i) {
        const bool sel = (c.active_page == i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        if (ImGui::Button(kAudioPages[i], ImVec2(-1.0f, 38.0f))) c.active_page = i;
        ImGui::PopStyleColor(2);
    }
    ImGui::Separator();
    const auto st = svc_.duplex_stats();
    ImGui::Text("DAW буфер: %u", st.buffer_size);
    ImGui::Text("RTT: %.2f ms", double(st.rx_rtt_us) / 1000.0);
    ImGui::Text("Монітор клієнтів: %zu", svc_.monitor_clients().size());
    if (ImGui::SmallButton(c.dark_theme ? "Світла Лаванда" : "Темна Студія")) {
        c.dark_theme = !c.dark_theme;
        lv::ui::apply_theme(c.dark_theme);
    }
}

void LanVidAudioApp::draw_aoip_grid_page() {
    const auto& p = lv::ui::theme_palette();
    auto& c = svc_.config();
    ImGui::TextColored(p.accent_node, "Мережевий міст AoIP & LanVid Grid L2 (256ch)");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("IP вузла-партнера", peer_ip_buf_, sizeof(peer_ip_buf_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("Порт", &c.peer_port);

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("Канали", (std::to_string(c.channels) + " ch").c_str())) {
        for (int ch : kChannelChoices) {
            if (ImGui::Selectable((std::to_string(ch) + " ch").c_str(), c.channels == ch)) {
                c.channels = ch;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Затримка (ms)", &c.latency_ms, 2, 200)) svc_.apply_live_settings();
    if (ImGui::Checkbox("Adaptive Wi-Fi", &c.adaptive_jitter)) svc_.apply_live_settings();
    ImGui::SameLine();
    if (ImGui::Checkbox("1+1 Wi-Fi Redundancy", &c.wifi_redundancy)) svc_.apply_live_settings();

    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("Аудіокодек", &c.audio_codec, kCodecLabels, 4)) svc_.apply_live_settings();

    ImGui::Separator();
    ImGui::TextColored(p.accent_node, "Виявлені AoIP-вузли у LAN");
    ImGui::SameLine();
    if (ImGui::SmallButton("Сканувати")) svc_.scan_nodes();
    for (const auto& nd : svc_.discovered_nodes()) {
        if (ImGui::Selectable((std::string(nd.name) + " (" + nd.ip + ":" + std::to_string(nd.rx_port) + ")").c_str())) {
            std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", nd.ip);
            c.peer_port = nd.rx_port;
        }
    }

    ImGui::Separator();
    ImGui::Text("Grid L2 статус: %s", svc_.grid_status().c_str());
    for (int m = 0; m < 3; ++m) {
        const char* ml[] = {"Classic UDP", "Grid L2 (256ch)", "Hybrid"};
        if (m > 0) ImGui::SameLine();
        if (ImGui::RadioButton(ml[m], c.network_engine_mode == m)) svc_.toggle_grid_l2(m);
    }
}

void LanVidAudioApp::draw_patchbay_page() {
    const auto& p = lv::ui::theme_palette();
    ImGui::TextColored(p.accent_node, "Матриця маршрутизації 64x64 (Virtual ASIO / CoreAudio HAL <-> Network)");
    if (ImGui::SmallButton("Скинути 1:1")) svc_.reset_routing_identity();

    const int show_ch = std::min(svc_.config().channels, 16);
    if (ImGui::BeginTable("##Patch", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Канал");
        ImGui::TableSetupColumn("TX -> Канал");
        ImGui::TableSetupColumn("RX -> Канал");
        ImGui::TableSetupColumn("Mute");
        ImGui::TableSetupColumn("Solo");
        ImGui::TableHeadersRow();
        for (int i = 0; i < show_ch; ++i) {
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Ch %d", i + 1);
            ImGui::TableSetColumnIndex(1);
            int txd = svc_.tx_routing[size_t(i)];
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputInt("##tx", &txd)) svc_.set_channel_routing(true, i, std::clamp(txd, -1, 63));
            ImGui::TableSetColumnIndex(2);
            int rxd = svc_.rx_routing[size_t(i)];
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputInt("##rx", &rxd)) svc_.set_channel_routing(false, i, std::clamp(rxd, -1, 63));
            ImGui::TableSetColumnIndex(3);
            bool m = svc_.ch_mute[size_t(i)];
            if (ImGui::Checkbox("##m", &m)) svc_.set_channel_mute_solo(true, uint16_t(i), m ? 1 : 0, -1);
            ImGui::TableSetColumnIndex(4);
            bool s = svc_.ch_solo[size_t(i)];
            if (ImGui::Checkbox("##s", &s)) svc_.set_channel_mute_solo(true, uint16_t(i), -1, s ? 1 : 0);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void LanVidAudioApp::draw_monitor_page() {
    const auto& p = lv::ui::theme_palette();
    const std::string ip = nics_.empty() ? "127.0.0.1" : nics_[0].ipv4_address;
    const std::string qr =
        "lanvid://monitor?host=" + ip + "&port=" + std::to_string(svc_.bound_monitor_port());
    ImGui::BeginChild("##MonCli", ImVec2(ImGui::GetContentRegionAvail().x * 0.62f, 0.0f), true);
    ImGui::TextColored(p.accent_node, "Підключені смартфони LanVid Monitor (порт :%u)",
                       unsigned(svc_.bound_monitor_port()));
    if (ImGui::Checkbox("Multi-Channel Dante Flow", &svc_.config().monitor_flow_enabled)) svc_.apply_live_settings();
    ImGui::Separator();
    const auto clients = svc_.monitor_clients();
    if (clients.empty()) {
        ImGui::TextDisabled("Поки немає підключених моніторів — відскануйте QR-код у LanVid Monitor.");
    } else {
        for (const auto& cli : clients) {
            ImGui::Text("%s (%s) • %u kbps • %u ms • %llu pkts", cli.name, cli.address,
                        unsigned(cli.bitrate_kbps), unsigned(cli.latency_ms),
                        static_cast<unsigned long long>(cli.packets_tx));
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##MonQr", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.accent_purp, "QR для LanVid Monitor");
    lv::ui::draw_qr_box(qr, 200.0f);
    ImGui::TextWrapped("%s", qr.c_str());
    ImGui::EndChild();
}

void LanVidAudioApp::draw_midi_settings_page() {
    const auto& p = lv::ui::theme_palette();
    ImGui::TextColored(p.accent_node, "MIDI2 over IP та Системні параметри");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Назва вузла", studio_name_buf_, sizeof(studio_name_buf_));
    if (ImGui::Checkbox("MIDI Only (1.5 ms, без аудіо)", &svc_.config().midi_only)) svc_.apply_live_settings();
    if (ImGui::Button("Тест MIDI Note On")) svc_.send_test_midi_note(60, true);
    ImGui::SameLine();
    if (ImGui::Button("Note Off")) svc_.send_test_midi_note(60, false);

    ImGui::Separator();
    ImGui::Text("Системні аудіовиходи: %zu | Входи: %zu", svc_.system_outputs().size(),
                svc_.system_inputs().size());
    for (const auto& n : nics_) {
        ImGui::Text("NIC: %s — %s (%u Mbps)", n.friendly_name, n.ipv4_address, unsigned(n.link_speed_mbps));
    }
}

}  // namespace lv::apps
