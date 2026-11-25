#include "gui/audio_studio_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/clock.hpp"
#include "imgui.h"

namespace lv::gui {

LanVidAudioStudioApp::LanVidAudioStudioApp(std::string config_path_override)
    : config_path_(config_path_override.empty() ? config_file_path_for("lanvid_audio_config.json")
                                                : std::move(config_path_override)) {
    cfg_.dest_port = lv::kAudioStreamPort;
    cfg_.listen_port = lv::kAudioStreamPort;
    cfg_.dpx_listen_port = lv::kAudioStreamPort;
    cfg_.dpx_peer_port = lv::kAudioStreamPort;
    cfg_.audio_only = true;
}

LanVidAudioStudioApp::~LanVidAudioStudioApp() {
    shutdown();
}

void LanVidAudioStudioApp::init() {
    load_app_config(cfg_, config_path_);
    if (cfg_.listen_port == 5000) cfg_.listen_port = lv::kAudioStreamPort;
    if (cfg_.dest_port == 5000) cfg_.dest_port = lv::kAudioStreamPort;
    cfg_.audio_only = true;
    std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", cfg_.dest_ip.c_str());

    apply_studio_theme(cfg_.dark_theme);
    refresh_nics();

    // Seed default virtual & network slots in Patchbay if empty
    if (cfg_.matrix_slots.empty()) {
        MatrixSlotConfig s0{};
        s0.id = 0; s0.name = "LanVid Virtual ASIO/HAL In"; s0.type = MatrixSlotType::kVirtualBus;
        s0.role = MatrixSlotRole::kInput; s0.channels = 64; s0.active = true;
        MatrixSlotConfig s1{};
        s1.id = 1; s1.name = "LAN AoIP Bus RX"; s1.type = MatrixSlotType::kNetworkAoip;
        s1.role = MatrixSlotRole::kDuplex; s1.channels = 64; s1.active = true;
        MatrixSlotConfig s2{};
        s2.id = 2; s2.name = "Personal Monitor Bus (Wi-Fi)"; s2.type = MatrixSlotType::kVirtualBus;
        s2.role = MatrixSlotRole::kOutput; s2.channels = 64; s2.active = true;
        cfg_.matrix_slots = {s0, s1, s2};
        MatrixCrosspoint cp{};
        cp.src_slot = 0; cp.src_ch = 0; cp.dst_slot = 1; cp.dst_ch = 0; cp.gain_db = 0.0f;
        cfg_.matrix_crosspoints.push_back(cp);
    }

    start_services(uint16_t(cfg_.listen_port), lv::kAudioMonitorPort, lv::kAudioDiscoveryPort);
    start_lan_scan();
}

void LanVidAudioStudioApp::shutdown() {
    stop_services();
    if (scan_thread_.joinable()) scan_thread_.join();
    save_app_config(cfg_, config_path_);
}

bool LanVidAudioStudioApp::start_services(uint16_t audio_port, uint16_t monitor_port, uint16_t discovery_port) {
    stop_services();

    // 1. Bind dedicated AoIP Audio UDP port (default 5004)
    if (!audio_sock_.open()) return false;
    const std::string bind_ip = cfg_.primary_nic_ip.empty() ? "0.0.0.0" : cfg_.primary_nic_ip;
    if (!audio_sock_.bind(bind_ip, audio_port)) {
        if (!audio_sock_.bind("0.0.0.0", audio_port)) {
            audio_sock_.close();
            return false;
        }
    }
    audio_sock_.set_nonblocking(true);
    bound_audio_port_ = audio_sock_.local_port();

    // 2. Start PersonalMonitorServer on dedicated Monitor UDP port (default 5020)
    //    Disable internal discovery port (0) because DiscoveryResponder owns port 5021 with role="audio".
    monitor_srv_ = std::make_unique<lv::PersonalMonitorServer>(48000);
    monitor_srv_->set_studio_name(studio_name_buf_);
    monitor_srv_->set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
    monitor_srv_->set_multichannel_flow(true, 2);
    if (!monitor_srv_->start(monitor_port, bind_ip, 0)) {
        if (!monitor_srv_->start(monitor_port, "0.0.0.0", 0)) {
            monitor_srv_.reset();
            audio_sock_.close();
            return false;
        }
    }
    if (!cfg_.primary_nic_ip.empty() || !cfg_.secondary_nic_ip.empty()) {
        monitor_srv_->set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    }

    // 3. Start DiscoveryResponder on Audio Discovery port (default 5021) advertising role="audio"
    discovery_.set_role("audio");
    discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    const uint16_t ap = bound_audio_port_;
    const int ch = cfg_.aoip_channels;
    discovery_.set_aoip_provider([ap, ch]() {
        lv::AoipAnnounce a{};
        a.enabled = true;
        a.hostname = lv::local_host_name();
        a.audio_driver = "LanVid Virtual Audio";
        a.driver_connected = true;
        a.channels = uint16_t(ch);
        a.rx_port = ap;
        a.tx_port = ap;
        a.duplex = true;
        return a;
    });
    if (!discovery_.start(bound_audio_port_, studio_name_buf_, discovery_port)) {
        monitor_srv_->stop();
        monitor_srv_.reset();
        audio_sock_.close();
        return false;
    }
    bound_disc_port_ = discovery_port;

    // 4. Advertise via mDNS (_lanvid._udp) with role=audio
    mdns_.start(studio_name_buf_, monitor_srv_->bound_port(), {{"role", "audio"}});

    services_running_.store(true, std::memory_order_release);
    return true;
}

void LanVidAudioStudioApp::stop_services() {
    services_running_.store(false, std::memory_order_release);
    mdns_.stop();
    discovery_.stop();
    if (monitor_srv_) {
        monitor_srv_->stop();
        monitor_srv_.reset();
    }
    if (grid_node_) {
        grid_node_.reset();
    }
    if (dsp_server_) {
        dsp_server_.reset();
    }
    audio_sock_.close();
}

void LanVidAudioStudioApp::refresh_nics() {
    nics_ = lv::enumerate_network_interfaces(false, false);
}

void LanVidAudioStudioApp::apply_nics() {
    discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    if (monitor_srv_) {
        monitor_srv_->set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    }
    save_app_config(cfg_, config_path_);
}

void LanVidAudioStudioApp::start_lan_scan() {
    if (scanning_.exchange(true)) return;
    if (scan_thread_.joinable()) scan_thread_.join();
    const std::string pri = cfg_.primary_nic_ip;
    const std::string sec = cfg_.secondary_nic_ip;
    scan_thread_ = std::thread([this, pri, sec]() {
        auto found = lv::discovery_scan_roles(320, true, false, true, pri, sec);
        {
            std::lock_guard<std::mutex> lk(scan_mu_);
            peers_ = std::move(found);
        }
        scanning_.store(false, std::memory_order_release);
    });
}

void LanVidAudioStudioApp::render_frame() {
    anim_time_ += ImGui::GetIO().DeltaTime;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidAudioRoot", nullptr, flags);
    draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##AudioSidebar", ImVec2(215.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##AudioMain", ImVec2(0.0f, avail_h), false);
    if (active_tab_ == 0) draw_connect_tab();
    else if (active_tab_ == 1) draw_patchbay_tab();
    else if (active_tab_ == 2) draw_monitor_tab();
    else draw_devices_grid_tab();
    ImGui::EndChild();

    ImGui::End();
}

void LanVidAudioStudioApp::draw_top_bar() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.accent_purp, "LanVid Audio");
    ImGui::SameLine();
    draw_status_pill("[AUDIO]", p.accent_node, true);
    ImGui::SameLine();
    const bool running = services_running();
    draw_status_pill(running ? "ONLINE" : "STOPPED", running ? p.green : p.red, true);
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "Audio :%u  |  Monitor :%u  |  Discovery :%u",
                       unsigned(bound_audio_port()), unsigned(bound_monitor_port()),
                       unsigned(bound_discovery_port()));

    const float right_x = ImGui::GetWindowWidth() - 250.0f;
    if (right_x > ImGui::GetCursorPosX()) ImGui::SameLine(right_x);
    const char* mode_lbl = (cfg_.network_engine_mode == 1)
                               ? "Grid L2 (256ch)"
                               : (cfg_.network_engine_mode == 2 ? "Hybrid L2+L3" : "Classic UDP/Wi-Fi");
    draw_status_pill(mode_lbl, p.accent_teal, false);
    ImGui::SameLine();
    if (ImGui::SmallButton(cfg_.dark_theme ? "Світла тема" : "Темна тема")) {
        cfg_.dark_theme = !cfg_.dark_theme;
        apply_studio_theme(cfg_.dark_theme);
        save_app_config(cfg_, config_path_);
    }
}

void LanVidAudioStudioApp::draw_sidebar() {
    const auto& p = studio_palette();
    const char* tabs[] = {
        "З'єднання (Connect)",
        "Маршрути (Patchbay)",
        "Моніторинг (Monitor)",
        "Пристрої та Grid"
    };
    for (int i = 0; i < 4; ++i) {
        const bool sel = (active_tab_ == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, p.accent_node);
        else ImGui::PushStyleColor(ImGuiCol_Button, p.card_soft);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        else ImGui::PushStyleColor(ImGuiCol_Text, p.text_main);
        if (ImGui::Button(tabs[i], ImVec2(-1.0f, 38.0f))) active_tab_ = i;
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(p.text_muted, "Конфігурація:");
    ImGui::TextWrapped("lanvid_audio_config.json");
    ImGui::Spacing();
    if (ImGui::Button(services_running() ? "Перезапустити служби" : "Запустити служби", ImVec2(-1.0f, 32.0f))) {
        start_services(uint16_t(cfg_.listen_port), lv::kAudioMonitorPort, lv::kAudioDiscoveryPort);
    }
}

void LanVidAudioStudioApp::draw_connect_tab() {
    const auto& p = studio_palette();

    // Engine Mode Switcher: Classic (UDP/Wi-Fi) | Grid (L2 · 256ch) | Hybrid
    ImGui::BeginChild("##EngineModeCard", ImVec2(0.0f, 76.0f), true);
    ImGui::TextColored(p.text_main, "Режим мережевого рушія:");
    const char* modes[] = {"Classic (UDP / Wi-Fi)", "Grid (Layer 2 · 256ch)", "Hybrid (Grid L2 + Wi-Fi)"};
    for (int m = 0; m < 3; ++m) {
        if (m > 0) ImGui::SameLine();
        const bool active = (cfg_.network_engine_mode == m);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, p.accent_purp);
        if (ImGui::Button(modes[m], ImVec2(220.0f, 30.0f))) {
            cfg_.network_engine_mode = m;
            save_app_config(cfg_, config_path_);
        }
        if (active) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // Telemetry Gauges
    const int cli_count = monitor_srv_ ? int(monitor_srv_->clients().size()) : 0;
    char buf_ch[32], buf_lat[32], buf_cli[32];
    std::snprintf(buf_ch, sizeof(buf_ch), "%d ch", cfg_.network_engine_mode == 1 ? cfg_.grid_channels : cfg_.aoip_channels);
    std::snprintf(buf_lat, sizeof(buf_lat), "%d ms", cfg_.aoip_latency_ms);
    std::snprintf(buf_cli, sizeof(buf_cli), "%d", cli_count);

    draw_telemetry_ring_gauge("КАНАЛИ", buf_ch, "48 kHz", float(cfg_.aoip_channels) / 64.0f);
    ImGui::SameLine();
    draw_telemetry_ring_gauge("ЗАТРИМКА", buf_lat, cfg_.adaptive_jitter ? "Adaptive" : "Fixed", 0.25f, 136.0f, 112.0f, false, true);
    ImGui::SameLine();
    draw_telemetry_ring_gauge("СМАРТФОНИ", buf_cli, "Monitor :5020", float(cli_count) / 8.0f);

    // Topology Canvas + Discovered [AUDIO] Nodes
    ImGui::BeginChild("##AudioTopology", ImVec2(ImGui::GetContentRegionAvail().x * 0.60f, 260.0f), true);
    ImGui::TextColored(p.text_main, "Топологія аудіовузлів у LAN");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const ImVec2 n_local(base.x + 20.0f, base.y + 45.0f);
    const ImVec2 n_remote(base.x + 310.0f, base.y + 45.0f);
    dl->AddRectFilled(n_local, ImVec2(n_local.x + 165.0f, n_local.y + 68.0f), ImGui::GetColorU32(p.card_soft), 12.0f);
    dl->AddRect(n_local, ImVec2(n_local.x + 165.0f, n_local.y + 68.0f), ImGui::GetColorU32(p.accent_node), 12.0f, 0, 2.0f);
    dl->AddText(ImVec2(n_local.x + 12.0f, n_local.y + 14.0f), ImGui::GetColorU32(p.text_main), studio_name_buf_);
    dl->AddText(ImVec2(n_local.x + 12.0f, n_local.y + 38.0f), ImGui::GetColorU32(p.text_muted), "UDP :5004 / :5020");

    dl->AddRectFilled(n_remote, ImVec2(n_remote.x + 185.0f, n_remote.y + 68.0f), ImGui::GetColorU32(p.card_soft), 12.0f);
    dl->AddRect(n_remote, ImVec2(n_remote.x + 185.0f, n_remote.y + 68.0f), ImGui::GetColorU32(p.green), 12.0f, 0, 2.0f);
    dl->AddText(ImVec2(n_remote.x + 12.0f, n_remote.y + 14.0f), ImGui::GetColorU32(p.text_main), "LAN Audio Peer / Monitor");
    dl->AddText(ImVec2(n_remote.x + 12.0f, n_remote.y + 38.0f), ImGui::GetColorU32(p.text_muted), peer_ip_buf_);

    draw_studio_bezier_cable(dl, ImVec2(n_local.x + 165.0f, n_local.y + 34.0f),
                             ImVec2(n_remote.x, n_remote.y + 34.0f),
                             ImGui::GetColorU32(p.accent_node), 3.0f, services_running(), anim_time_);
    if (cfg_.dual_nic_redundancy_enabled && !cfg_.secondary_nic_ip.empty()) {
        draw_studio_dashed_bezier_cable(dl, ImVec2(n_local.x + 165.0f, n_local.y + 46.0f),
                                        ImVec2(n_remote.x, n_remote.y + 46.0f),
                                        ImGui::GetColorU32(p.accent_teal), 2.4f, anim_time_);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##AudioDiscovered", ImVec2(0.0f, 260.0f), true);
    ImGui::TextColored(p.text_main, "Знайдені вузли [AUDIO]");
    ImGui::SameLine();
    if (ImGui::SmallButton(scanning_.load() ? "Пошук..." : "Оновити")) start_lan_scan();
    ImGui::Separator();
    std::vector<lv::DiscoveredReceiver> copy;
    {
        std::lock_guard<std::mutex> lk(scan_mu_);
        copy = peers_;
    }
    draw_discovered_nodes_list("audio_peers", copy, "audio", [this](const lv::DiscoveredReceiver& r) {
        std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", r.ip.c_str());
        cfg_.dest_ip = r.ip;
        cfg_.dest_port = r.stream_port;
    });
    ImGui::EndChild();
}

void LanVidAudioStudioApp::draw_patchbay_tab() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.text_main, "Маршрутизація аудіо (Patchbay та Матриця 64×64)");
    ImGui::SameLine();
    if (ImGui::Button(patchbay_matrix_mode_ ? "Перемкнути у Patchbay" : "Перемкнути у Матрицю 64×64")) {
        patchbay_matrix_mode_ = !patchbay_matrix_mode_;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Feedback Loop Guard", &cfg_.matrix_feedback_guard)) {
        save_app_config(cfg_, config_path_);
    }
    ImGui::Separator();

    if (!patchbay_matrix_mode_) {
        // Visual Patchbay: Sources -> Receivers with Drag-and-Drop Bezier cables
        ImGui::BeginChild("##PatchbayCanvas", ImVec2(0.0f, 0.0f), true);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        std::vector<ImVec2> src_pins, dst_pins;

        ImGui::Columns(2, "pb_cols", false);
        ImGui::TextColored(p.accent_node, "Джерела (Inputs)");
        for (size_t i = 0; i < cfg_.matrix_slots.size(); ++i) {
            const auto& s = cfg_.matrix_slots[i];
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            char lbl[128];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch)##src%zu", s.name.c_str(), s.channels, i);
            if (ImGui::Button(lbl, ImVec2(240.0f, 36.0f))) drag_src_slot_ = s.id;
            src_pins.push_back(ImVec2(pos.x + 244.0f, pos.y + 18.0f));
        }

        ImGui::NextColumn();
        ImGui::TextColored(p.green, "Приймачі (Outputs)");
        for (size_t i = 0; i < cfg_.matrix_slots.size(); ++i) {
            const auto& d = cfg_.matrix_slots[i];
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            char lbl[128];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch)##dst%zu", d.name.c_str(), d.channels, i);
            if (ImGui::Button(lbl, ImVec2(240.0f, 36.0f))) {
                if (drag_src_slot_ >= 0 && (!cfg_.matrix_feedback_guard || drag_src_slot_ != d.id)) {
                    MatrixCrosspoint cp{};
                    cp.src_slot = drag_src_slot_;
                    cp.dst_slot = d.id;
                    cfg_.matrix_crosspoints.push_back(cp);
                    save_app_config(cfg_, config_path_);
                    drag_src_slot_ = -1;
                }
            }
            dst_pins.push_back(ImVec2(pos.x - 4.0f, pos.y + 18.0f));
        }
        ImGui::Columns(1);

        for (const auto& cp : cfg_.matrix_crosspoints) {
            if (cp.src_slot >= 0 && size_t(cp.src_slot) < src_pins.size() &&
                cp.dst_slot >= 0 && size_t(cp.dst_slot) < dst_pins.size()) {
                draw_studio_bezier_cable(dl, src_pins[size_t(cp.src_slot)], dst_pins[size_t(cp.dst_slot)],
                                         ImGui::GetColorU32(p.accent_node), 3.0f, true, anim_time_);
            }
        }
        (void)origin;
        ImGui::EndChild();
    } else {
        // Compact 16x16 / 64x64 scrollable Crosspoint Matrix
        ImGui::BeginChild("##Matrix64", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextColored(p.text_muted, "Матриця кроспоінтів 64×64 (активних маршрутів: %zu)",
                           cfg_.matrix_crosspoints.size());
        constexpr int kShown = 16;
        if (ImGui::BeginTable("m64", kShown + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("In \\ Out", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            for (int c = 0; c < kShown; ++c) {
                char hdr[16];
                std::snprintf(hdr, sizeof(hdr), "Out %d", c + 1);
                ImGui::TableSetupColumn(hdr, ImGuiTableColumnFlags_WidthFixed, 44.0f);
            }
            ImGui::TableHeadersRow();
            for (int r = 0; r < kShown; ++r) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("In %d", r + 1);
                for (int c = 0; c < kShown; ++c) {
                    ImGui::TableSetColumnIndex(c + 1);
                    auto it = std::find_if(cfg_.matrix_crosspoints.begin(), cfg_.matrix_crosspoints.end(),
                                           [r, c](const MatrixCrosspoint& x) {
                                               return x.src_ch == r && x.dst_ch == c && !x.mute;
                                           });
                    const bool on = (it != cfg_.matrix_crosspoints.end());
                    char bid[32];
                    std::snprintf(bid, sizeof(bid), "%s##m_%d_%d", on ? "●" : "·", r, c);
                    if (ImGui::SmallButton(bid)) {
                        if (on) {
                            cfg_.matrix_crosspoints.erase(it);
                        } else if (!cfg_.matrix_feedback_guard || r != c) {
                            MatrixCrosspoint cp{};
                            cp.src_slot = 0; cp.dst_slot = 1; cp.src_ch = r; cp.dst_ch = c;
                            cfg_.matrix_crosspoints.push_back(cp);
                        }
                        save_app_config(cfg_, config_path_);
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
}

void LanVidAudioStudioApp::draw_monitor_tab() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.text_main, "Персональний моніторинг (Dante Wi-Fi Flow · UDP :%u)",
                       unsigned(bound_monitor_port()));
    ImGui::Separator();

    if (ImGui::Checkbox("Wi-Fi Redundancy 1+1 Duplicate (+1.5 мс, без NACK)", &cfg_.wifi_redundancy)) {
        if (monitor_srv_) monitor_srv_->set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
        save_app_config(cfg_, config_path_);
    }

    if (monitor_srv_) {
        const auto clients = monitor_srv_->clients();
        ImGui::TextColored(p.text_muted, "Підключені смартфони (%zu / 8)  |  Flow TX: %llu  |  1+1 Dup TX: %llu",
                           clients.size(),
                           static_cast<unsigned long long>(monitor_srv_->flow_packets_tx()),
                           static_cast<unsigned long long>(monitor_srv_->dup_packets_tx()));
        ImGui::Spacing();
        for (const auto& c : clients) {
            ImGui::BeginChild(c.name.c_str(), ImVec2(0.0f, 64.0f), true);
            draw_status_pill(c.connected ? "ONLINE" : "IDLE", c.connected ? p.green : p.amber, true);
            ImGui::SameLine();
            ImGui::Text("%s (%s) — %u kbps · %u ms · Mask 0x%llX",
                        c.name.c_str(), c.address.c_str(), unsigned(c.bitrate_kbps), unsigned(c.latency_ms),
                        static_cast<unsigned long long>(c.active_channel_mask));
            ImGui::SameLine();
            bool m = c.muted;
            if (ImGui::Checkbox("Mute", &m)) monitor_srv_->set_client_mute(c.slot, m);
            ImGui::EndChild();
        }
        if (clients.empty()) {
            ImGui::TextColored(p.text_muted,
                               "Відкрийте LanVid Monitor на iOS або Android — студія [AUDIO] з'явиться автоматично.");
        }
    }
}

void LanVidAudioStudioApp::draw_devices_grid_tab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##AudioDevicesGrid", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(p.text_main, "1. Віртуальний аудіопристрій (LanVid Virtual ASIO / CoreAudio HAL)");
    ImGui::TextColored(p.text_muted, "64 In / 64 Out @ 48 kHz · Zero-Copy Shared Memory Bus");
    ImGui::InputText("Назва аудіостудії", studio_name_buf_, sizeof(studio_name_buf_));
    ImGui::Separator();

    const uint64_t fo = monitor_srv_ ? monitor_srv_->failover_packets() : 0;
    if (draw_nic_selector_panel("audio_nic", nics_, cfg_.primary_nic_id, cfg_.primary_nic_ip,
                                cfg_.secondary_nic_id, cfg_.secondary_nic_ip,
                                cfg_.dual_nic_redundancy_enabled, fo, [this]() { refresh_nics(); })) {
        apply_nics();
    }

    ImGui::Separator();
    ImGui::TextColored(p.text_main, "2. LanVid Grid Layer 2 (256ch) та DSP Server");
    ImGui::SliderInt("Канали Grid L2", &cfg_.grid_channels, 64, 256);
    ImGui::Checkbox("IEEE 802.1Q VLAN / 802.1p QoS (PCP=6)", &cfg_.grid_vlan_enabled);
    ImGui::Checkbox("Clock Master (L2 Sync)", &cfg_.grid_clock_master);
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "3. MIDI Sync (16-event Redundancy Ring)");
    ImGui::Checkbox("MIDI Only (Ultra-Low Latency ~1.5 ms)", &cfg_.midi_only);

    ImGui::EndChild();
}

}  // namespace lv::gui
