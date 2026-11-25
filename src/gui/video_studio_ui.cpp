#include "gui/video_studio_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"

namespace lv::gui {

LanVidVideoStudioApp::LanVidVideoStudioApp(std::string config_path_override)
    : config_path_(config_path_override.empty() ? config_file_path_for("lanvid_video_config.json")
                                                : std::move(config_path_override)) {
    cfg_.listen_port = lv::kVideoStreamPort;
    cfg_.dest_port = lv::kVideoStreamPort;
    cfg_.audio_only = false;
}

LanVidVideoStudioApp::~LanVidVideoStudioApp() {
    shutdown();
}

void LanVidVideoStudioApp::init() {
    load_app_config(cfg_, config_path_);
    if (cfg_.listen_port == 5000) cfg_.listen_port = lv::kVideoStreamPort;
    if (cfg_.dest_port == 5000) cfg_.dest_port = lv::kVideoStreamPort;

    apply_studio_theme(cfg_.dark_theme);
    refresh_nics();

    if (host_.create_texture) {
        preview_tex_ = host_.create_texture(host_.ctx);
    }

    start_services(uint16_t(cfg_.listen_port), lv::kVideoTallyPort, lv::kVideoDiscoveryPort);
    start_lan_scan();
}

void LanVidVideoStudioApp::shutdown() {
    stop_services();
    if (scan_thread_.joinable()) scan_thread_.join();
    if (preview_tex_ && host_.destroy_texture) {
        host_.destroy_texture(host_.ctx, preview_tex_);
        preview_tex_ = nullptr;
    }
    save_app_config(cfg_, config_path_);
}

bool LanVidVideoStudioApp::start_services(uint16_t video_port, uint16_t tally_port, uint16_t discovery_port) {
    stop_services();

    // 1. Start Video Receiver on dedicated Video UDP port (default 5006)
    rx_ = lv_receiver_create();
    if (!rx_) return false;
    LvReceiverConfig rc{};
    lv_receiver_config_default(&rc);
    rc.bind_port = video_port;
    rc.enable_discovery = 0;  // Managed explicitly on Video Discovery port 5023 with role="video"
    rc.enable_audio = 1;
    rc.audio_target_ms = std::clamp(cfg_.audio_target_ms + double(cfg_.video_lip_sync_ms), 2.0, 500.0);
    std::snprintf(rc.device_name, sizeof(rc.device_name), "%s", studio_name_buf_);
    if (!lv_receiver_start(rx_, &rc)) {
        lv_receiver_destroy(rx_);
        rx_ = nullptr;
        return false;
    }
    bound_video_port_ = video_port;

    // 2. Start Studio Tally & Full-Duplex Intercom Server on UDP port 5022
    const std::string bind_ip = cfg_.primary_nic_ip.empty() ? "0.0.0.0" : cfg_.primary_nic_ip;
    if (!intercom_.start_studio(tally_port, bind_ip)) {
        if (!intercom_.start_studio(tally_port, "0.0.0.0")) {
            lv_receiver_stop(rx_);
            lv_receiver_destroy(rx_);
            rx_ = nullptr;
            return false;
        }
    }
    intercom_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);

    // 3. Start DiscoveryResponder on Video Discovery port (default 5023) advertising role="video"
    discovery_.set_role("video");
    discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    if (!discovery_.start(bound_video_port_, studio_name_buf_, discovery_port)) {
        intercom_.stop();
        lv_receiver_stop(rx_);
        lv_receiver_destroy(rx_);
        rx_ = nullptr;
        return false;
    }
    bound_disc_port_ = discovery_port;

    // 3b. Best-effort legacy discovery responder on :5099 (non-fatal if unified Studio holds it)
    legacy_discovery_.set_role("video");
    legacy_discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    (void)legacy_discovery_.start(bound_video_port_, studio_name_buf_, lv::kDiscoveryPort);

    // 4. Advertise via mDNS (_lanvid._udp) with role=video
    mdns_.start(studio_name_buf_, bound_video_port_, {{"role", "video"}});

    services_running_.store(true, std::memory_order_release);
    return true;
}

void LanVidVideoStudioApp::stop_services() {
    services_running_.store(false, std::memory_order_release);
    mdns_.stop();
    legacy_discovery_.stop();
    discovery_.stop();
    intercom_.stop();
    if (rx_) {
        lv_receiver_stop(rx_);
        lv_receiver_destroy(rx_);
        rx_ = nullptr;
    }
}

void LanVidVideoStudioApp::refresh_nics() {
    nics_ = lv::enumerate_network_interfaces(false, false);
}

void LanVidVideoStudioApp::apply_nics() {
    discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    save_app_config(cfg_, config_path_);
}

void LanVidVideoStudioApp::start_lan_scan() {
    if (scanning_.exchange(true)) return;
    if (scan_thread_.joinable()) scan_thread_.join();
    const std::string pri = cfg_.primary_nic_ip;
    const std::string sec = cfg_.secondary_nic_ip;
    scan_thread_ = std::thread([this, pri, sec]() {
        auto found = lv::discovery_scan_roles(320, false, true, true, pri, sec);
        {
            std::lock_guard<std::mutex> lk(scan_mu_);
            peers_ = std::move(found);
        }
        scanning_.store(false, std::memory_order_release);
    });
}

void LanVidVideoStudioApp::poll_video_and_tally() {
    intercom_.poll();
    if (!rx_) return;
    lv_receiver_get_stats(rx_, &rx_stats_);
    if (preview_tex_) {
        int w = 0, h = 0;
        const int want_rgba = preview_tex_->wants_rgba() ? 1 : 0;
        int rc = lv_receiver_get_latest_frame(rx_, frame_buf_.data(), frame_buf_.size(),
                                              &w, &h, &last_frame_seq_, want_rgba);
        if (rc == -1 && w > 0 && h > 0) {
            frame_buf_.resize(size_t(w) * size_t(h) * 4u);
            rc = lv_receiver_get_latest_frame(rx_, frame_buf_.data(), frame_buf_.size(),
                                              &w, &h, &last_frame_seq_, want_rgba);
        }
        if (rc == 1 && w > 0 && h > 0) {
            preview_tex_->update(frame_buf_.data(), w, h);
        }
    }
}

void LanVidVideoStudioApp::render_frame() {
    anim_time_ += ImGui::GetIO().DeltaTime;
    poll_video_and_tally();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidVideoRoot", nullptr, flags);
    draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##VideoSidebar", ImVec2(220.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##VideoMain", ImVec2(0.0f, avail_h), false);
    if (active_tab_ == 0) draw_cameras_tally_tab();
    else if (active_tab_ == 1) draw_intercom_tab();
    else if (active_tab_ == 2) draw_outputs_obs_tab();
    else draw_settings_tab();
    ImGui::EndChild();

    ImGui::End();
}

void LanVidVideoStudioApp::draw_top_bar() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.accent_blue, "LanVid Video");
    ImGui::SameLine();
    draw_status_pill("[VIDEO]", p.accent_blue, true);
    ImGui::SameLine();
    const bool live = rx_stats_.fps > 0.5;
    draw_status_pill(live ? "ON AIR" : "STANDBY", live ? p.red : p.green, true);
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "Video :%u  |  Tally/IFB :%u  |  Discovery :%u",
                       unsigned(bound_video_port()), unsigned(bound_tally_port()),
                       unsigned(bound_discovery_port()));
    if (!nics_.empty()) {
        ImGui::SameLine();
        const std::string show_ip = cfg_.primary_nic_ip.empty() ? nics_[0].ipv4_address : cfg_.primary_nic_ip;
        char ip_pill[96];
        std::snprintf(ip_pill, sizeof(ip_pill), "IP ПК: %s", show_ip.c_str());
        draw_status_pill(ip_pill, p.accent_teal, false);
    }

    const float right_x = ImGui::GetWindowWidth() - 180.0f;
    if (right_x > ImGui::GetCursorPosX()) ImGui::SameLine(right_x);
    if (ImGui::SmallButton(cfg_.dark_theme ? "Світла тема" : "Темна тема")) {
        cfg_.dark_theme = !cfg_.dark_theme;
        apply_studio_theme(cfg_.dark_theme);
        save_app_config(cfg_, config_path_);
    }
}

void LanVidVideoStudioApp::draw_sidebar() {
    const auto& p = studio_palette();
    const char* tabs[] = {
        "Камери та Ефір (Tally)",
        "Інтерком (Intercom/IFB)",
        "Вивід та OBS (Outputs)",
        "Мережа та Налаштування"
    };
    for (int i = 0; i < 4; ++i) {
        const bool sel = (active_tab_ == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, p.accent_blue);
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
    ImGui::TextWrapped("lanvid_video_config.json");
    ImGui::Spacing();
    if (ImGui::Button(services_running() ? "Перезапустити відео" : "Запустити відео", ImVec2(-1.0f, 32.0f))) {
        start_services(uint16_t(cfg_.listen_port), lv::kVideoTallyPort, lv::kVideoDiscoveryPort);
    }
}

void LanVidVideoStudioApp::draw_cameras_tally_tab() {
    const auto& p = studio_palette();

    // Top Live Preview + Telemetry
    ImGui::BeginChild("##LivePreviewPane", ImVec2(ImGui::GetContentRegionAvail().x * 0.62f, 310.0f), true);
    ImGui::TextColored(p.text_main, "Живе прев'ю ефіру (PGM · %dx%d @ %.1f fps · %.1f Mbps)",
                       rx_stats_.width, rx_stats_.height, rx_stats_.fps, rx_stats_.rx_mbps);
    if (preview_tex_ && preview_tex_->width() > 0 && preview_tex_->height() > 0) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float aspect = float(preview_tex_->width()) / float(std::max(1, preview_tex_->height()));
        float dw = avail.x;
        float dh = dw / aspect;
        if (dh > avail.y) { dh = avail.y; dw = dh * aspect; }
        ImGui::Image(preview_tex_->imgui_id(), ImVec2(dw, dh));
    } else {
        ImGui::TextColored(p.text_muted,
                           "Очікування відеопотоку на UDP :%u — запустіть LanVid Camera на смартфоні.",
                           unsigned(bound_video_port()));
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Connected Cameras & Tally Control (STBY / PVW / PGM)
    ImGui::BeginChild("##CamerasTallyGrid", ImVec2(0.0f, 310.0f), true);
    ImGui::TextColored(p.text_main, "Підключені камери та Tally (STBY | PVW | PGM)");
    ImGui::Separator();
    auto cams = intercom_.studio_cameras();
    if (cams.empty()) {
        // Show virtual CAM 1 / CAM 2 tally preview slots ready for connection
        for (int cid = 1; cid <= 2; ++cid) {
            ImGui::PushID(cid);
            ImGui::Text("CAM %d (очікування підключення :%u)", cid, unsigned(bound_tally_port()));
            const bool is_pgm = (selected_pgm_cam_ == cid);
            const bool is_pvw = (selected_pvw_cam_ == cid);
            if (ImGui::SmallButton("STBY")) {
                if (is_pgm) selected_pgm_cam_ = 0;
                if (is_pvw) selected_pvw_cam_ = 0;
                intercom_.studio_set_camera_tally(uint32_t(cid), lv::kTallyStandby, "CAM " + std::to_string(cid), false);
            }
            ImGui::SameLine();
            if (is_pvw) ImGui::PushStyleColor(ImGuiCol_Button, p.green);
            if (ImGui::SmallButton("PVW")) {
                selected_pvw_cam_ = cid;
                intercom_.studio_set_camera_tally(uint32_t(cid), lv::kTallyPreview, "CAM " + std::to_string(cid), false);
            }
            if (is_pvw) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (is_pgm) ImGui::PushStyleColor(ImGuiCol_Button, p.red);
            if (ImGui::SmallButton("PGM ON AIR")) {
                selected_pgm_cam_ = cid;
                intercom_.studio_set_camera_tally(uint32_t(cid), lv::kTallyProgram, "CAM " + std::to_string(cid), true);
            }
            if (is_pgm) ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::PopID();
        }
    } else {
        for (const auto& cam : cams) {
            ImGui::PushID(int(cam.camera_id));
            const ImVec4 tcol = (cam.tally_state == lv::kTallyProgram)
                                    ? p.red
                                    : (cam.tally_state == lv::kTallyPreview ? p.green : p.text_muted);
            const char* tlbl = (cam.tally_state == lv::kTallyProgram)
                                   ? "PGM ON AIR"
                                   : (cam.tally_state == lv::kTallyPreview ? "PVW" : "STBY");
            draw_status_pill(tlbl, tcol, true);
            ImGui::SameLine();
            ImGui::Text("%s (%s) · %u%% · %d°C", cam.label.c_str(), cam.address.c_str(),
                        unsigned(cam.battery_pct), int(cam.temp_c));
            if (ImGui::SmallButton("STBY")) {
                intercom_.studio_set_camera_tally(cam.camera_id, lv::kTallyStandby, cam.label, false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("PVW")) {
                intercom_.studio_set_camera_tally(cam.camera_id, lv::kTallyPreview, cam.label, false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("PGM")) {
                intercom_.studio_set_camera_tally(cam.camera_id, lv::kTallyProgram, cam.label, true);
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Discovered [VIDEO] nodes in LAN
    ImGui::BeginChild("##VideoNodesCard", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.text_main, "Відеовузли у LAN ([VIDEO])");
    ImGui::SameLine();
    if (ImGui::SmallButton(scanning_.load() ? "Пошук..." : "Оновити")) start_lan_scan();
    std::vector<lv::DiscoveredReceiver> copy;
    {
        std::lock_guard<std::mutex> lk(scan_mu_);
        copy = peers_;
    }
    draw_discovered_nodes_list("video_peers", copy, "video", nullptr);
    ImGui::EndChild();
}

void LanVidVideoStudioApp::draw_intercom_tab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##IntercomCard", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.text_main, "Режисерський дуплексний інтерком та IFB (UDP :%u)",
                       unsigned(bound_tally_port()));
    ImGui::Separator();

    const bool talking = director_ptt_ || director_latch_;
    if (talking) ImGui::PushStyleColor(ImGuiCol_Button, p.red);
    ImGui::Button(talking ? "DIRECTOR VOICE: ON AIR (PTT)" : "УТРИМУЙТЕ ДЛЯ РОЗМОВИ (TALK PTT)",
                  ImVec2(320.0f, 44.0f));
    director_ptt_ = ImGui::IsItemActive();
    if (talking) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Checkbox("LATCH (Фіксація мікрофона)", &director_latch_)) {}
    ImGui::SameLine();
    if (ImGui::Checkbox("All Call (Усі камери)", &all_call_)) {}

    intercom_.set_intercom_state(director_ptt_ || director_latch_, false, 1.0f, 0.0f);

    if (ImGui::SliderFloat("Автоматичний Ducking програмного звуку (дБ)", &cfg_.intercom_ducking_db, -24.0f, 0.0f, "%.1f dB")) {
        save_app_config(cfg_, config_path_);
    }

    ImGui::TextColored(p.text_muted, "Intercom TX: %llu pkts  |  RX: %llu pkts  |  1+1 Dedup: %llu",
                       static_cast<unsigned long long>(intercom_.intercom_tx_packets()),
                       static_cast<unsigned long long>(intercom_.intercom_rx_packets()),
                       static_cast<unsigned long long>(intercom_.intercom_duplicates_dropped()));
    ImGui::EndChild();
}

void LanVidVideoStudioApp::draw_outputs_obs_tab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##ObsOutputsCard", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.text_main, "Вивід у OBS Studio (obs-lanvid) та Декодування");
    ImGui::Separator();

    if (ImGui::Checkbox("Маршрутизувати активну камеру / PGM у OBS Studio (obs-lanvid)", &cfg_.video_obs_output_enabled)) {
        save_app_config(cfg_, config_path_);
    }

    const char* codecs[] = {"LanVid DCT (Ultra-Low Latency)", "H.264 Hardware", "H.265 / HEVC Hardware"};
    if (ImGui::Combo("Декодер відео", &cfg_.codec, codecs, 3)) {
        save_app_config(cfg_, config_path_);
    }

    if (ImGui::SliderInt("Корекція A/V Lip-Sync затримки (мс)", &cfg_.video_lip_sync_ms, -200, 200, "%d ms")) {
        save_app_config(cfg_, config_path_);
    }

    ImGui::Spacing();
    ImGui::TextColored(p.text_muted,
                       "Вбудований звук камер: %.0f kbps  |  Буфер: %.1f ms  |  E2E P50: %.1f ms",
                       rx_stats_.audio_kbps, rx_stats_.audio_buffer_ms, rx_stats_.latency_p50_ms);
    ImGui::EndChild();
}

void LanVidVideoStudioApp::draw_settings_tab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##VideoSettingsCard", ImVec2(0.0f, 0.0f), true);
    ImGui::InputText("Назва відеостудії", studio_name_buf_, sizeof(studio_name_buf_));
    ImGui::Separator();

    if (draw_nic_selector_panel("video_nic", nics_, cfg_.primary_nic_id, cfg_.primary_nic_ip,
                                cfg_.secondary_nic_id, cfg_.secondary_nic_ip,
                                cfg_.dual_nic_redundancy_enabled, 0, [this]() { refresh_nics(); })) {
        apply_nics();
    }

    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Пріоритезація трафіку (QoS / DSCP)");
    if (ImGui::Checkbox("DSCP AF41 (34) для відеопотоків (H.264/HEVC/DCT)", &cfg_.video_dscp_af41)) {
        save_app_config(cfg_, config_path_);
    }
    if (ImGui::Checkbox("DSCP CS7 / EF (46) для Tally та Intercom/IFB", &cfg_.tally_dscp_ef)) {
        save_app_config(cfg_, config_path_);
    }
    if (ImGui::Checkbox("Wi-Fi Redundancy 1+1 Duplicate для Tally/Intercom", &cfg_.wifi_redundancy)) {
        intercom_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
        save_app_config(cfg_, config_path_);
    }
    ImGui::EndChild();
}

}  // namespace lv::gui
