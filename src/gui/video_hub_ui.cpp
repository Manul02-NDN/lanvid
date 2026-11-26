#include "gui/video_hub_ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/clock.hpp"
#include "gui/qr_code.hpp"
#include "imgui.h"
#include "video/video_bus.hpp"

namespace lv::gui {

namespace {

constexpr const char* kSidebarTitles[] = {
    "Хаб камер",
    "Інспектор камери",
    "Запис та Ефір",
    "Вивід, OBS та Інтерком",
};

constexpr const char* kCameraHubSubtabs[] = {"Сітка камер (Multiview)", "Пошук та QR-підключення"};
constexpr const char* kRecordSubtabs[] = {"Багатоканальний ISO-запис", "Ефірний пульт (PVW / PGM)"};
constexpr const char* kOutputSubtabs[] = {"Віртуальні камери та OBS", "Пульт інтеркому (Intercom)"};

constexpr int kPlayoutPresets[] = {16, 33, 50, 80};
constexpr const char* kResLabels[] = {"360p Proxy", "720p HD", "1080p FHD", "4K UHD"};
constexpr const int kFpsChoices[] = {24, 25, 30, 50, 60};
constexpr const char* kFpsLabels[] = {"24 fps", "25 fps", "30 fps", "50 fps", "60 fps"};
constexpr const char* kCodecLabels[] = {"LanVid DCT", "H.264 (AVC)", "H.265 (HEVC)"};
constexpr const char* kLensLabels[] = {"Wide (1x)", "Ultra-Wide (0.5x)", "Tele (2x/3x)", "Front"};

void draw_vu_meter(float dbfs, float w, float h) {
    const auto& p = studio_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float norm = std::clamp((dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(p.card_soft), 4.0f);
    if (norm > 0.01f) {
        const ImVec4 col = (norm > 0.88f) ? p.red : (norm > 0.65f ? p.amber : p.accent_node);
        dl->AddRectFilled(pos, ImVec2(pos.x + w * norm, pos.y + h), ImGui::GetColorU32(col), 4.0f);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void draw_qr_box(const std::string& text, float box_px) {
    const QrCode qr = qr_encode(text);
    if (qr.size <= 0) {
        ImGui::TextDisabled("QR недоступний");
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float quiet = 8.0f;
    const float cell = (box_px - 2.0f * quiet) / float(qr.size);
    dl->AddRectFilled(pos, ImVec2(pos.x + box_px, pos.y + box_px), IM_COL32(255, 255, 255, 255), 10.0f);
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.dark(x, y)) continue;
            const ImVec2 a(pos.x + quiet + float(x) * cell, pos.y + quiet + float(y) * cell);
            const ImVec2 b(a.x + cell + 0.3f, a.y + cell + 0.3f);
            dl->AddRectFilled(a, b, IM_COL32(30, 27, 75, 255));
        }
    }
    ImGui::Dummy(ImVec2(box_px, box_px));
}

void open_folder_in_os(const std::string& path) {
    if (path.empty()) return;
#if defined(_WIN32)
    std::string cmd = "explorer \"" + path + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + path + "\" &";
#else
    std::string cmd = "xdg-open \"" + path + "\" &";
#endif
    (void)std::system(cmd.c_str());
}

}  // namespace

LanVidVideoHubApp::LanVidVideoHubApp(std::string config_path_override)
    : config_path_(config_path_override.empty() ? config_file_path_for("lanvid_video_config.json")
                                                : std::move(config_path_override)) {
    cfg_.listen_port = lv::kVideoStreamPort;
    cfg_.dest_port = lv::kVideoStreamPort;
    cfg_.studio_name = "LanVid Video Hub";
    std::snprintf(studio_name_buf_, sizeof(studio_name_buf_), "%s", cfg_.studio_name.c_str());
    std::snprintf(manual_ip_buf_, sizeof(manual_ip_buf_), "192.168.1.50");
    std::snprintf(iso_dir_buf_, sizeof(iso_dir_buf_), "%s", lv::hub::IsoMultiRecorder::default_output_dir().c_str());
    std::snprintf(iso_sess_buf_, sizeof(iso_sess_buf_), "%s", cfg_.iso_session_name.c_str());
}

LanVidVideoHubApp::~LanVidVideoHubApp() {
    shutdown();
}

void LanVidVideoHubApp::init() {
    load_app_config(cfg_, config_path_);
    if (cfg_.listen_port == 5000 || cfg_.listen_port == 5004) cfg_.listen_port = lv::kVideoStreamPort;
    if (cfg_.studio_name == "LanVid Audio" || cfg_.studio_name.empty()) cfg_.studio_name = "LanVid Video Hub";
    std::snprintf(studio_name_buf_, sizeof(studio_name_buf_), "%s", cfg_.studio_name.c_str());
    std::snprintf(iso_dir_buf_, sizeof(iso_dir_buf_), "%s",
                  cfg_.iso_output_dir.empty() ? lv::hub::IsoMultiRecorder::default_output_dir().c_str()
                                              : cfg_.iso_output_dir.c_str());
    std::snprintf(iso_sess_buf_, sizeof(iso_sess_buf_), "%s", cfg_.iso_session_name.c_str());

    apply_studio_theme(cfg_.dark_theme);
    refresh_nics();
    refresh_audio_devices();
    start_services(uint16_t(cfg_.listen_port), lv::kVideoTallyPort, lv::kVideoDiscoveryPort);
}

void LanVidVideoHubApp::shutdown() {
    stop_services();
    sync_hub_to_config();
    save_app_config(cfg_, config_path_);
    if (host_.destroy_texture) {
        for (uint32_t i = 0; i < lv::hub::kMaxHubSlots; ++i) {
            if (slot_tex_[i]) {
                host_.destroy_texture(host_.ctx, slot_tex_[i]);
                slot_tex_[i] = nullptr;
            }
        }
    }
}

bool LanVidVideoHubApp::start_services(uint16_t video_port, uint16_t tally_port, uint16_t discovery_port) {
    stop_services();
    disc_port_ = discovery_port;
    lv::hub::VideoHubConfig hcfg{};
    hcfg.hub_name = studio_name_buf_;
    hcfg.video_port = video_port;
    hcfg.tally_port = tally_port;
    hcfg.discovery_port = discovery_port;
    hcfg.port_mode = cfg_.video_port_mode;
    hcfg.max_slots = uint32_t(std::clamp(cfg_.video_max_slots, 1, 16));
    hcfg.publish_vcam_bus = true;
    hcfg.vcam_count = uint32_t(std::clamp(cfg_.vcam_count, 0, 8));
    hcfg.vcam_placeholder = cfg_.vcam_placeholder;
    hcfg.primary_nic_ip = cfg_.primary_nic_ip;
    hcfg.secondary_nic_ip = cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : "";
    hcfg.default_delay_ms = uint16_t(std::clamp(cfg_.video_default_delay_ms, 5, 250));
    hcfg.default_fixed_delay = cfg_.video_default_fixed_delay;
    hcfg.sync_remote_rec = sync_remote_rec_;

    for (uint32_t i = 0; i < lv::hub::kMaxHubSlots; ++i) {
        hcfg.slot_labels[i] = i < cfg_.video_slot_labels.size() && !cfg_.video_slot_labels[i].empty()
                                  ? cfg_.video_slot_labels[i]
                                  : ("CAM " + std::to_string(i + 1));
        hcfg.slot_ips[i] = i < cfg_.video_slot_ips.size() ? cfg_.video_slot_ips[i] : "";
        hcfg.slot_uids[i] = i < cfg_.video_slot_device_uids.size() ? cfg_.video_slot_device_uids[i] : "";
        hcfg.slot_cam_numbers[i] = (i < cfg_.video_slot_cam_numbers.size() && cfg_.video_slot_cam_numbers[i] > 0)
                                       ? uint8_t(cfg_.video_slot_cam_numbers[i])
                                       : uint8_t(i + 1);
        hcfg.slot_cam_tags[i] = i < cfg_.video_slot_cam_tags.size() ? cfg_.video_slot_cam_tags[i] : "";
        hcfg.slot_delays_ms[i] = (i < cfg_.video_slot_delay_ms.size() && cfg_.video_slot_delay_ms[i] >= 5)
                                     ? uint16_t(cfg_.video_slot_delay_ms[i])
                                     : hcfg.default_delay_ms;
        hcfg.slot_fixed_modes[i] =
            i < cfg_.video_slot_fixed_delay.size() ? (cfg_.video_slot_fixed_delay[i] != 0) : hcfg.default_fixed_delay;
    }

    if (!hub_.start(hcfg)) {
        services_running_.store(false, std::memory_order_release);
        return false;
    }
    hub_.tally_intercom().set_ducking_db(cfg_.intercom_ducking_db);
    hub_.tally_intercom().set_audio_devices(cfg_.intercom_mic_index, cfg_.intercom_out_index);
    hub_.vcam().set_lip_sync_ms(cfg_.video_lip_sync_ms);
    services_running_.store(true, std::memory_order_release);
    return true;
}

void LanVidVideoHubApp::stop_services() {
    services_running_.store(false, std::memory_order_release);
    hub_.stop();
}

void LanVidVideoHubApp::sync_config_to_hub() {
    hub_.set_vcam_config(uint32_t(std::clamp(cfg_.vcam_count, 0, 8)), cfg_.vcam_placeholder, cfg_.video_lip_sync_ms);
    hub_.set_network_interfaces(cfg_.primary_nic_ip,
                                cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : "");
    hub_.tally_intercom().set_ducking_db(cfg_.intercom_ducking_db);
    hub_.tally_intercom().set_audio_devices(cfg_.intercom_mic_index, cfg_.intercom_out_index);
}

void LanVidVideoHubApp::sync_hub_to_config() {
    const uint32_t n = std::clamp<uint32_t>(uint32_t(cfg_.video_max_slots), 1, lv::hub::kMaxHubSlots);
    cfg_.video_slot_labels.resize(n);
    cfg_.video_slot_ips.resize(n);
    cfg_.video_slot_device_uids.resize(n);
    cfg_.video_slot_delay_ms.resize(n);
    cfg_.video_slot_fixed_delay.resize(n);
    cfg_.video_slot_cam_numbers.resize(n);
    cfg_.video_slot_cam_tags.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const auto s = hub_.slot_snapshot(i);
        cfg_.video_slot_labels[i] = s.label;
        cfg_.video_slot_ips[i] = s.pinned_ip;
        cfg_.video_slot_device_uids[i] = s.device_uid;
        cfg_.video_slot_delay_ms[i] = int(s.settings.video_delay_ms);
        cfg_.video_slot_fixed_delay[i] = s.settings.fixed_delay_mode ? 1 : 0;
        cfg_.video_slot_cam_numbers[i] = int(s.settings.assigned_cam_number);
        cfg_.video_slot_cam_tags[i] = s.settings.assigned_cam_tag;
    }
    cfg_.iso_output_dir = iso_dir_buf_;
    cfg_.iso_session_name = iso_sess_buf_;
    cfg_.studio_name = studio_name_buf_;
}

void LanVidVideoHubApp::refresh_nics() {
    nics_ = lv::enumerate_network_interfaces(false, false);
}

void LanVidVideoHubApp::refresh_audio_devices() {
    sys_mics_ = lv::list_system_audio_devices(false, true);
    sys_outs_ = lv::list_system_audio_devices(true, true);
}

void LanVidVideoHubApp::update_slot_texture(uint32_t slot) {
    if (!host_.create_texture || slot >= lv::hub::kMaxHubSlots) return;
    lv::Image frame;
    if (!hub_.poll_slot_frame(slot, frame, slot_tex_seq_[slot])) return;
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) return;

    if (!slot_tex_[slot]) {
        slot_tex_[slot] = host_.create_texture(host_.ctx);
    }
    if (!slot_tex_[slot]) return;

    if (slot_tex_[slot]->wants_rgba() && frame.order == lv::PixelOrder::kBGRA) {
        const size_t px = size_t(frame.width) * size_t(frame.height);
        rgba_scratch_.resize(px * 4u);
        for (size_t i = 0; i < px; ++i) {
            rgba_scratch_[i * 4 + 0] = frame.data[i * 4 + 2];
            rgba_scratch_[i * 4 + 1] = frame.data[i * 4 + 1];
            rgba_scratch_[i * 4 + 2] = frame.data[i * 4 + 0];
            rgba_scratch_[i * 4 + 3] = frame.data[i * 4 + 3];
        }
        slot_tex_[slot]->update(rgba_scratch_.data(), frame.width, frame.height);
    } else {
        slot_tex_[slot]->update(frame.data.data(), frame.width, frame.height);
    }
}

void LanVidVideoHubApp::draw_slot_preview_box(uint32_t slot, float w, float h, const char* empty_hint) {
    const auto& p = studio_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + w, pos.y + h);

    if (slot < lv::hub::kMaxHubSlots && slot_tex_[slot] && slot_tex_[slot]->width() > 0) {
        const float tw = float(slot_tex_[slot]->width());
        const float th = float(slot_tex_[slot]->height());
        const float aspect = tw / std::max(1.0f, th);
        float dw = w;
        float dh = dw / aspect;
        if (dh > h) {
            dh = h;
            dw = dh * aspect;
        }
        const float ox = pos.x + (w - dw) * 0.5f;
        const float oy = pos.y + (h - dh) * 0.5f;
        dl->AddRectFilled(pos, end, IM_COL32(15, 23, 42, 235), 8.0f);
        dl->AddImage(slot_tex_[slot]->imgui_id(), ImVec2(ox, oy), ImVec2(ox + dw, oy + dh));
    } else {
        dl->AddRectFilled(pos, end, ImGui::GetColorU32(p.card_soft), 8.0f);
        dl->AddRect(pos, end, ImGui::GetColorU32(p.border), 8.0f);
        const ImVec2 tsz = ImGui::CalcTextSize(empty_hint);
        dl->AddText(ImVec2(pos.x + (w - tsz.x) * 0.5f, pos.y + (h - tsz.y) * 0.5f),
                    ImGui::GetColorU32(p.text_muted), empty_hint);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void LanVidVideoHubApp::render_frame() {
    const uint64_t now = lv::now_us();
    hub_.poll();

    if (now - last_disk_check_us_ > 2'000'000ULL) {
        last_disk_check_us_ = now;
        cached_disk_free_gb_ = double(lv::hub::IsoMultiRecorder::disk_free_bytes(iso_dir_buf_)) / (1024.0 * 1024.0 * 1024.0);
    }

    if (config_dirty_ && now - config_save_us_ > 700'000ULL) {
        config_dirty_ = false;
        config_save_us_ = now;
        sync_hub_to_config();
        save_app_config(cfg_, config_path_);
    }

    for (uint32_t i = 0; i < uint32_t(std::clamp(cfg_.video_max_slots, 1, 16)); ++i) {
        update_slot_texture(i);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidVideoHubRoot", nullptr, wf);
    draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##VideoHubSidebar", ImVec2(226.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##VideoHubMain", ImVec2(0.0f, avail_h), false);
    if (cfg_.active_tab == 0) draw_cameras_hub_page();
    else if (cfg_.active_tab == 1) draw_inspector_page();
    else if (cfg_.active_tab == 2) draw_record_switcher_page();
    else draw_outputs_intercom_page();
    ImGui::EndChild();

    ImGui::End();
}

void LanVidVideoHubApp::draw_top_bar() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.accent_purp, "LanVid Video Hub v2.7.0");
    ImGui::SameLine();

    const bool running = services_running();
    draw_status_pill(running ? "HUB LIVE" : "HUB OFF", running ? p.green : p.red, true);

    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "NIC: %s",
                       cfg_.primary_nic_ip.empty() ? "Auto" : cfg_.primary_nic_ip.c_str());

    ImGui::SameLine();
    const std::string tc = hub_.iso().session_timecode();
    draw_status_pill(("TC " + tc).c_str(), hub_.iso().is_recording_any() ? p.red : p.accent_ind, false);

    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "SSD: %.1f GB", cached_disk_free_gb_);

    const float right_x = std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - 340.0f);
    ImGui::SameLine(right_x);

    const bool rec_all = hub_.iso().is_recording_any();
    ImGui::PushStyleColor(ImGuiCol_Button, rec_all ? p.red : p.accent_node);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::SmallButton(rec_all ? "STOP REC ALL" : "REC ALL")) {
        if (rec_all) {
            hub_.rec_stop_all(sync_remote_rec_);
        } else {
            lv::hub::IsoMultiSessionConfig sc{};
            sc.output_dir = iso_dir_buf_;
            sc.session_name = iso_sess_buf_;
            sc.container = static_cast<lv::IsoContainer>(std::clamp(cfg_.iso_container, 0, 2));
            sc.record_program = cfg_.iso_record_program;
            sc.slot_mask = uint32_t(cfg_.iso_slot_mask);
            sc.slot_count = uint32_t(std::clamp(cfg_.video_max_slots, 1, 16));
            hub_.rec_start_all(sc, sync_remote_rec_);
        }
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    const bool all_call = hub_.tally_intercom().all_call();
    ImGui::PushStyleColor(ImGuiCol_Button, all_call ? p.amber : p.card_soft);
    ImGui::PushStyleColor(ImGuiCol_Text, all_call ? ImVec4(1, 1, 1, 1) : p.text_main);
    if (ImGui::SmallButton("ALL CALL")) {
        hub_.tally_intercom().set_all_call(!all_call);
        hub_.tally_intercom().set_director_mic_state(!all_call || director_ptt_ || director_latch_,
                                                     director_listen_mute_, cfg_.intercom_ifb_volume, 0.0f);
    }
    ImGui::PopStyleColor(2);
}

void LanVidVideoHubApp::draw_sidebar() {
    const auto& p = studio_palette();
    for (int i = 0; i < 4; ++i) {
        const bool sel = (cfg_.active_tab == i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        if (ImGui::Button(kSidebarTitles[i], ImVec2(-1.0f, 40.0f))) {
            cfg_.active_tab = i;
            config_dirty_ = true;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    int live_cams = 0;
    const uint32_t max_s = uint32_t(std::clamp(cfg_.video_max_slots, 1, 16));
    for (uint32_t i = 0; i < max_s; ++i) {
        if (hub_.slot_snapshot(i).streaming) ++live_cams;
    }

    ImGui::TextColored(p.text_muted, "Статус відеохабу");
    ImGui::Text("Камери: %d / %u активні", live_cams, unsigned(max_s));
    ImGui::Text("UDP Відео :%u | Tally :%u", unsigned(bound_video_port()), unsigned(bound_tally_port()));
    draw_status_pill(hub_.vcam().is_open() ? "VideoBus SHM: OK" : "VideoBus SHM: OFF",
                     hub_.vcam().is_open() ? p.green : p.amber, false);

    if (hub_.iso().is_recording_any()) {
        draw_status_pill("ISO REC ACTIVE", p.red, true);
        ImGui::TextColored(p.red, "Швидкість: %.1f MB/s", hub_.iso().total_write_mbps() / 8.0);
    }

    ImGui::Spacing();
    if (ImGui::SmallButton(cfg_.dark_theme ? "Світла Лаванда" : "Темна Студія")) {
        cfg_.dark_theme = !cfg_.dark_theme;
        apply_studio_theme(cfg_.dark_theme);
        config_dirty_ = true;
    }
}

void LanVidVideoHubApp::draw_subtab_pills(int page_idx, const char* const* labels, int count) {
    const auto& p = studio_palette();
    int& cur = cfg_.active_subtab[size_t(page_idx)];
    cur = std::clamp(cur, 0, count - 1);
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool sel = (cur == i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        char bid[96];
        std::snprintf(bid, sizeof(bid), "%s##vhsub_%d_%d", labels[i], page_idx, i);
        if (ImGui::Button(bid, ImVec2(0.0f, 30.0f))) {
            cur = i;
            config_dirty_ = true;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::Separator();
}

void LanVidVideoHubApp::draw_cameras_hub_page() {
    draw_subtab_pills(0, kCameraHubSubtabs, 2);
    if (cfg_.active_subtab[0] == 0) draw_multiview_subtab();
    else draw_discovery_qr_subtab();
}

void LanVidVideoHubApp::draw_multiview_subtab() {
    const auto& p = studio_palette();
    ImGui::Text("Сітка прев'ю:");
    for (int g = 1; g <= 4; ++g) {
        ImGui::SameLine();
        const bool sel = (cfg_.video_multiview_layout == g);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%dx%d##grid%d", g, g, g);
        if (ImGui::SmallButton(lbl)) {
            cfg_.video_multiview_layout = g;
            config_dirty_ = true;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderInt("Слотів", &cfg_.video_max_slots, 1, 16)) {
        config_dirty_ = true;
    }

    const int cols = std::clamp(cfg_.video_multiview_layout, 1, 4);
    const int total = std::clamp(cfg_.video_max_slots, 1, 16);
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float card_w = std::max(210.0f, (avail_w - float(cols - 1) * 10.0f) / float(cols));
    const float preview_h = std::max(105.0f, (card_w - 16.0f) * 9.0f / 16.0f);

    for (int i = 0; i < total; ++i) {
        if (i % cols != 0) ImGui::SameLine();
        const auto s = hub_.slot_snapshot(uint32_t(i));
        const bool is_pgm = (hub_.program_slot() == i);
        const bool is_pvw = (hub_.preview_slot() == i);
        const bool is_sel = (selected_slot_ == i);

        const ImVec4 border_col = is_pgm ? p.red : (is_pvw ? p.green : (is_sel ? p.accent_node : p.border));
        ImGui::PushStyleColor(ImGuiCol_Border, border_col);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, (is_pgm || is_pvw || is_sel) ? 2.2f : 1.0f);
        char cid[32];
        std::snprintf(cid, sizeof(cid), "##camcard_%d", i);
        ImGui::BeginChild(cid, ImVec2(card_w, preview_h + 106.0f), true);

        // Card header: badge selector + label + Tally status
        ImGui::SetNextItemWidth(74.0f);
        int cam_num = s.settings.assigned_cam_number;
        char cnum_lbl[32];
        std::snprintf(cnum_lbl, sizeof(cnum_lbl), "##cbadge_%d", i);
        if (ImGui::BeginCombo(cnum_lbl, ("CAM " + std::to_string(cam_num)).c_str())) {
            for (int n = 1; n <= 16; ++n) {
                char opt[16];
                std::snprintf(opt, sizeof(opt), "CAM %d", n);
                if (ImGui::Selectable(opt, n == cam_num)) {
                    hub_.assign_slot_badge(uint32_t(i), uint8_t(n), "", true);
                    config_dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextColored(p.text_main, "%s", s.label.c_str());
        ImGui::SameLine();
        draw_status_pill(is_pgm ? "PGM" : (is_pvw ? "PVW" : (s.streaming ? "LIVE" : "STBY")),
                         is_pgm ? p.red : (is_pvw ? p.green : (s.streaming ? p.accent_teal : p.text_muted)),
                         is_pgm || is_pvw);

        draw_slot_preview_box(uint32_t(i), card_w - 16.0f, preview_h, "Очікування камери");
        if (ImGui::IsItemClicked()) {
            selected_slot_ = i;
        }

        // Quick action row: PVW / PGM / REC / TALK / Inspect
        ImGui::PushStyleColor(ImGuiCol_Button, is_pvw ? p.green : p.card_soft);
        if (ImGui::SmallButton("PVW")) hub_.set_preview_slot(i);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, is_pgm ? p.red : p.card_soft);
        if (ImGui::SmallButton("PGM")) hub_.set_program_slot(i);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        const bool rec = hub_.iso().is_slot_recording(uint32_t(i));
        ImGui::PushStyleColor(ImGuiCol_Button, rec ? p.red : p.card_soft);
        if (ImGui::SmallButton("REC")) hub_.rec_slot(uint32_t(i), !rec, sync_remote_rec_);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, s.talk_to_cam ? p.amber : p.card_soft);
        if (ImGui::SmallButton("TALK")) {
            const bool next_talk = !s.talk_to_cam;
            hub_.slot_engine(uint32_t(i))->set_intercom_routing(next_talk, s.listen_from_cam);
            hub_.tally_intercom().set_camera_intercom(s.camera_id, next_talk, s.listen_from_cam,
                                                      hub_.tally_intercom().all_call());
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Огляд")) {
            selected_slot_ = i;
            cfg_.active_tab = 1;
        }

        ImGui::TextColored(p.text_muted, "%ux%u • %.0ffps • J:%.1fms • Bat:%u%% %d°C", s.width, s.height, s.fps,
                           s.playout_jitter_ms, unsigned(s.battery_pct), int(s.temp_c));

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

void LanVidVideoHubApp::draw_discovery_qr_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##DiscListCol", ImVec2(ImGui::GetContentRegionAvail().x * 0.62f, 0.0f), true);
    ImGui::TextColored(p.accent_node, "Знайдені камери у локальній мережі");
    ImGui::SameLine();
    if (ImGui::SmallButton("Сканувати мережу")) {
        hub_.discovery().trigger_scan();
    }

    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("Прив'язати до слоту", ("CAM " + std::to_string(connect_target_slot_ + 1)).c_str())) {
        for (int i = 0; i < std::clamp(cfg_.video_max_slots, 1, 16); ++i) {
            char opt[24];
            std::snprintf(opt, sizeof(opt), "CAM %d", i + 1);
            if (ImGui::Selectable(opt, i == connect_target_slot_)) connect_target_slot_ = i;
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();

    const auto peers = hub_.discovery().discovered_peers();
    if (peers.empty()) {
        ImGui::TextColored(p.text_muted, "Поки не знайдено смартфонів. Перевірте Wi-Fi або відскануйте QR-код праворуч.");
    } else {
        for (size_t i = 0; i < peers.size(); ++i) {
            const auto& pr = peers[i];
            ImGui::PushID(int(i));
            draw_status_pill(pr.online ? "ONLINE" : "OFFLINE", pr.online ? p.green : p.text_muted, false);
            ImGui::SameLine();
            ImGui::Text("%s (%s:%u)", pr.name.c_str(), pr.ip.c_str(), unsigned(pr.stream_port));
            ImGui::SameLine();
            if (pr.assigned_slot >= 0) {
                ImGui::TextColored(p.accent_teal, "[CAM %d]", pr.assigned_slot + 1);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Підключити в 1 клік")) {
                hub_.connect_peer_to_slot(uint32_t(connect_target_slot_), pr.ip, pr.stream_port, pr.device_uid, pr.name);
                selected_slot_ = connect_target_slot_;
                config_dirty_ = true;
            }
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Ручне підключення за IP:");
    ImGui::SetNextItemWidth(170.0f);
    ImGui::InputText("IP адреса", manual_ip_buf_, sizeof(manual_ip_buf_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(85.0f);
    ImGui::InputInt("Порт", &manual_port_);
    ImGui::SameLine();
    if (ImGui::Button("Прив'язати слот")) {
        hub_.connect_peer_to_slot(uint32_t(connect_target_slot_), manual_ip_buf_, uint16_t(manual_port_), "", "");
        config_dirty_ = true;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##QrCol", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.accent_purp, "QR-код швидкого підключення");
    const std::string host_ip = !cfg_.primary_nic_ip.empty()
                                    ? cfg_.primary_nic_ip
                                    : (nics_.empty() ? "127.0.0.1" : nics_[0].ipv4_address);
    const std::string uri = lv::hub::HubDiscovery::build_qr_uri(host_ip, bound_video_port(), bound_tally_port(),
                                                                studio_name_buf_);
    draw_qr_box(uri, 210.0f);
    ImGui::TextWrapped("%s", uri.c_str());
    ImGui::EndChild();
}

void LanVidVideoHubApp::draw_inspector_page() {
    const auto& p = studio_palette();
    const int max_s = std::clamp(cfg_.video_max_slots, 1, 16);
    selected_slot_ = std::clamp(selected_slot_, 0, max_s - 1);

    ImGui::Text("Вибрана камера:");
    for (int i = 0; i < max_s; ++i) {
        ImGui::SameLine();
        const bool sel = (i == selected_slot_);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        char b[24];
        std::snprintf(b, sizeof(b), "CAM %d##insp%d", i + 1, i);
        if (ImGui::SmallButton(b)) selected_slot_ = i;
        ImGui::PopStyleColor(2);
    }
    ImGui::Separator();

    const auto s = hub_.slot_snapshot(uint32_t(selected_slot_));
    if (slot_tag_edit_idx_ != selected_slot_) {
        slot_tag_edit_idx_ = selected_slot_;
        std::snprintf(slot_label_buf_, sizeof(slot_label_buf_), "%s", s.label.c_str());
        std::snprintf(slot_tag_buf_, sizeof(slot_tag_buf_), "%s", s.settings.assigned_cam_tag.c_str());
    }

    const float col_w = ImGui::GetContentRegionAvail().x * 0.48f;
    ImGui::BeginChild("##InspLeft", ImVec2(col_w, 0.0f), true);
    draw_slot_preview_box(uint32_t(selected_slot_), col_w - 20.0f, (col_w - 20.0f) * 9.0f / 16.0f,
                          "Немає відеосигналу");

    ImGui::Text("Тег бейджа на телефоні: %s  •  Статус: %s", s.settings.assigned_cam_tag.c_str(),
                s.config_synced ? "Synced with Phone" : "Pending Sync");
    ImGui::PlotLines("Джитер Wi-Fi (ms)", s.jitter_history.data(), int(s.history_count), 0, nullptr, 0.0f, 80.0f,
                     ImVec2(-1.0f, 70.0f));
    ImGui::PlotLines("Втрати пакетів (%)", s.loss_history.data(), int(s.history_count), 0, nullptr, 0.0f, 15.0f,
                     ImVec2(-1.0f, 60.0f));
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##InspRight", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.accent_node, "Значок та ідентифікація камери");
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputText("Назва слоту", slot_label_buf_, sizeof(slot_label_buf_))) {
        hub_.set_slot_label(uint32_t(selected_slot_), slot_label_buf_);
        config_dirty_ = true;
    }
    int badge_n = s.settings.assigned_cam_number;
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::SliderInt("Номер значка", &badge_n, 1, 16)) {
        hub_.assign_slot_badge(uint32_t(selected_slot_), uint8_t(badge_n), "", true);
        slot_tag_edit_idx_ = -1;
        config_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputText("Тег", slot_tag_buf_, sizeof(slot_tag_buf_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Застосувати тег")) {
        hub_.assign_slot_badge(uint32_t(selected_slot_), uint8_t(badge_n), slot_tag_buf_, true);
        config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(p.accent_node, "Відео-джитер буфер (VideoPlayoutBuffer)");
    bool fixed = s.settings.fixed_delay_mode;
    int delay_ms = s.settings.video_delay_ms;
    if (ImGui::RadioButton("Fixed Delay (Без фризів)", fixed)) {
        hub_.set_slot_playout_delay(uint32_t(selected_slot_), uint32_t(delay_ms), true, true);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Adaptive Wi-Fi", !fixed)) {
        hub_.set_slot_playout_delay(uint32_t(selected_slot_), uint32_t(delay_ms), false, true);
        config_dirty_ = true;
    }
    for (int pms : kPlayoutPresets) {
        char pl[24];
        std::snprintf(pl, sizeof(pl), "%d ms", pms);
        if (ImGui::SmallButton(pl)) {
            hub_.set_slot_playout_delay(uint32_t(selected_slot_), uint32_t(pms), fixed, true);
            config_dirty_ = true;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::SliderInt("Затримка буфера (ms)", &delay_ms, 5, 250)) {
        hub_.set_slot_playout_delay(uint32_t(selected_slot_), uint32_t(delay_ms), fixed, true);
        config_dirty_ = true;
    }
    ImGui::TextColored(p.text_muted, "Buffer: %.1f ms • Jitter: %.2f ms • Hold: %llu • Drops: %llu • IDR: %llu",
                       s.playout_delay_ms, s.playout_jitter_ms,
                       static_cast<unsigned long long>(s.playout_held_frames),
                       static_cast<unsigned long long>(s.playout_dropped_frames),
                       static_cast<unsigned long long>(s.playout_idr_requests));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(p.accent_node, "Двостороннє керування камерою телефону (kVideoConfigSync)");
    auto rs = s.settings;
    bool changed = false;
    int res_idx = rs.resolution_preset;
    if (ImGui::Combo("Роздільність", &res_idx, kResLabels, 4)) {
        rs.resolution_preset = uint8_t(res_idx);
        changed = true;
    }
    int fps_idx = 4;
    for (int f = 0; f < 5; ++f)
        if (kFpsChoices[f] == rs.target_fps) fps_idx = f;
    if (ImGui::Combo("Частота (FPS)", &fps_idx, kFpsLabels, 5)) {
        rs.target_fps = uint8_t(kFpsChoices[fps_idx]);
        changed = true;
    }
    int codec_idx = rs.codec;
    if (ImGui::Combo("Кодек", &codec_idx, kCodecLabels, 3)) {
        rs.codec = uint8_t(codec_idx);
        changed = true;
    }
    int mbps = int(rs.bitrate_kbps / 1000u);
    if (ImGui::SliderInt("Бітрейт (Mbps)", &mbps, 5, 150)) {
        rs.bitrate_kbps = uint32_t(mbps) * 1000u;
        changed = true;
    }
    int lens_idx = rs.camera_lens;
    if (ImGui::Combo("Об'єктив", &lens_idx, kLensLabels, 4)) {
        rs.camera_lens = uint8_t(lens_idx);
        changed = true;
    }
    if (changed) {
        hub_.apply_slot_remote_settings(uint32_t(selected_slot_), rs, true);
        config_dirty_ = true;
    }
    if (ImGui::Button("Force IDR (Ключовий кадр)")) {
        hub_.force_slot_idr(uint32_t(selected_slot_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Відв'язати камеру")) {
        hub_.release_slot(uint32_t(selected_slot_));
    }
    ImGui::EndChild();
}

void LanVidVideoHubApp::draw_record_switcher_page() {
    draw_subtab_pills(2, kRecordSubtabs, 2);
    if (cfg_.active_subtab[2] == 0) draw_iso_record_subtab();
    else draw_live_switcher_subtab();
}

void LanVidVideoHubApp::draw_iso_record_subtab() {
    const auto& p = studio_palette();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Назва сесії", iso_sess_buf_, sizeof(iso_sess_buf_));
    ImGui::SameLine();
    const char* conts[] = {"MP4", "MKV", "MOV"};
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::Combo("Контейнер", &cfg_.iso_container, conts, 3)) config_dirty_ = true;

    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("Папка ISO-записів", iso_dir_buf_, sizeof(iso_dir_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Відкрити папку")) {
        const std::string dir = hub_.iso().session_dir().empty() ? std::string(iso_dir_buf_) : hub_.iso().session_dir();
        open_folder_in_os(dir);
    }

    ImGui::Checkbox("Писати PGM_Master файл", &cfg_.iso_record_program);
    ImGui::SameLine();
    if (ImGui::Checkbox("Синхронний запис на телефонах (Remote Phone REC)", &sync_remote_rec_)) {
        hub_.set_sync_remote_rec(sync_remote_rec_);
    }

    ImGui::Separator();
    const int n = std::clamp(cfg_.video_max_slots, 1, 16);
    if (ImGui::BeginTable("##IsoTable", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Вкл", ImGuiTableColumnFlags_WidthFixed, 42.0f);
        ImGui::TableSetupColumn("Камера", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Стан", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Кадри", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Розмір (MB)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Швидкість / Файл");
        ImGui::TableHeadersRow();

        for (int i = 0; i < n; ++i) {
            const auto st = hub_.iso().slot_stats(uint32_t(i));
            const auto sl = hub_.slot_snapshot(uint32_t(i));
            bool inc = (cfg_.iso_slot_mask & (1 << i)) != 0;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char cb[16];
            std::snprintf(cb, sizeof(cb), "##inc%d", i);
            if (ImGui::Checkbox(cb, &inc)) {
                if (inc) cfg_.iso_slot_mask |= (1 << i);
                else cfg_.iso_slot_mask &= ~(1 << i);
                config_dirty_ = true;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s (%s)", sl.settings.assigned_cam_tag.c_str(), sl.label.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(st.recording ? p.red : p.text_muted, st.recording ? "REC" : "IDLE");
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(st.frames));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f MB", double(st.bytes) / (1024.0 * 1024.0));
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.1f Mbps • %s", st.write_mbps, st.path.c_str());
        }
        ImGui::EndTable();
    }
}

void LanVidVideoHubApp::draw_live_switcher_subtab() {
    const auto& p = studio_palette();
    const float w = (ImGui::GetContentRegionAvail().x - 16.0f) * 0.5f;
    const float h = w * 9.0f / 16.0f;

    ImGui::BeginChild("##SwPvw", ImVec2(w, h + 46.0f), true);
    ImGui::TextColored(p.green, "PREVIEW — CAM %d", hub_.preview_slot() + 1);
    draw_slot_preview_box(uint32_t(std::max(0, hub_.preview_slot())), w - 16.0f, h, "PREVIEW: Очікування");
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##SwPgm", ImVec2(w, h + 46.0f), true);
    ImGui::TextColored(p.red, "PROGRAM ON AIR — CAM %d", hub_.program_slot() + 1);
    draw_slot_preview_box(uint32_t(std::max(0, hub_.program_slot())), w - 16.0f, h, "PROGRAM: Очікування");
    ImGui::EndChild();

    ImGui::PushStyleColor(ImGuiCol_Button, p.red);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("CUT (PVW -> PGM)", ImVec2(220.0f, 38.0f))) {
        hub_.cut_preview_to_program();
    }
    ImGui::PopStyleColor(2);

    ImGui::Separator();
    const int n = std::clamp(cfg_.video_max_slots, 1, 16);
    ImGui::Text("Швидкий вибір PROGRAM:");
    for (int i = 0; i < n; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool pgm = (hub_.program_slot() == i);
        ImGui::PushStyleColor(ImGuiCol_Button, pgm ? p.red : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, pgm ? ImVec4(1, 1, 1, 1) : p.text_main);
        char b[24];
        std::snprintf(b, sizeof(b), "CAM %d##pgmb%d", i + 1, i);
        if (ImGui::Button(b, ImVec2(72.0f, 34.0f))) hub_.set_program_slot(i);
        ImGui::PopStyleColor(2);
    }
}

void LanVidVideoHubApp::draw_outputs_intercom_page() {
    draw_subtab_pills(3, kOutputSubtabs, 2);
    if (cfg_.active_subtab[3] == 0) draw_vcam_obs_subtab();
    else draw_intercom_settings_subtab();
}

void LanVidVideoHubApp::draw_vcam_obs_subtab() {
    const auto& p = studio_palette();
    ImGui::TextColored(hub_.vcam().is_open() ? p.green : p.red, "Шина LanVidVideoBus_v1: %s",
                       hub_.vcam().is_open() ? "Активна (Нульова затримка для OBS і браузерів)" : "Вимкнена");
    if (ImGui::SliderInt("Кількість віртуальних камер", &cfg_.vcam_count, 0, 8)) {
        sync_config_to_hub();
        config_dirty_ = true;
    }
    if (ImGui::Checkbox("Лавандова заглушка Standby без сигналу", &cfg_.vcam_placeholder)) {
        sync_config_to_hub();
        config_dirty_ = true;
    }
    if (ImGui::SliderInt("A/V Lip-Sync корекція (ms)", &cfg_.video_lip_sync_ms, -200, 200)) {
        sync_config_to_hub();
        config_dirty_ = true;
    }

    ImGui::Separator();
    for (uint32_t i = 0; i < lv::kVideoBusSlots; ++i) {
        draw_status_pill(i < uint32_t(cfg_.vcam_count) || i == lv::kVideoBusProgramSlot ? "ACTIVE" : "IDLE",
                         i == lv::kVideoBusProgramSlot ? p.red : p.accent_node, false);
        ImGui::SameLine();
        ImGui::Text("%s", lv::video_bus_device_name(i));
    }
}

void LanVidVideoHubApp::draw_intercom_settings_subtab() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.accent_node, "Пульт інтеркому режисера (Duplex IFB)");
    if (ImGui::SmallButton("Оновити аудіопристрої")) refresh_audio_devices();

    const char* cur_mic = (cfg_.intercom_mic_index >= 0 && cfg_.intercom_mic_index < int(sys_mics_.size()))
                              ? sys_mics_[size_t(cfg_.intercom_mic_index)].name.c_str()
                              : "Системний за замовчуванням";
    if (ImGui::BeginCombo("Мікрофон режисера", cur_mic)) {
        if (ImGui::Selectable("Системний за замовчуванням", cfg_.intercom_mic_index < 0)) {
            cfg_.intercom_mic_index = -1;
            sync_config_to_hub();
        }
        for (size_t i = 0; i < sys_mics_.size(); ++i) {
            if (ImGui::Selectable(sys_mics_[i].name.c_str(), cfg_.intercom_mic_index == int(i))) {
                cfg_.intercom_mic_index = int(i);
                sync_config_to_hub();
            }
        }
        ImGui::EndCombo();
    }

    const char* cur_out = (cfg_.intercom_out_index >= 0 && cfg_.intercom_out_index < int(sys_outs_.size()))
                              ? sys_outs_[size_t(cfg_.intercom_out_index)].name.c_str()
                              : "Системні навушники за замовчуванням";
    if (ImGui::BeginCombo("Навушники режисера", cur_out)) {
        if (ImGui::Selectable("Системні навушники за замовчуванням", cfg_.intercom_out_index < 0)) {
            cfg_.intercom_out_index = -1;
            sync_config_to_hub();
        }
        for (size_t i = 0; i < sys_outs_.size(); ++i) {
            if (ImGui::Selectable(sys_outs_[i].name.c_str(), cfg_.intercom_out_index == int(i))) {
                cfg_.intercom_out_index = int(i);
                sync_config_to_hub();
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::SliderFloat("Гучність IFB (в навушники операторів)", &cfg_.intercom_ifb_volume, 0.0f, 2.0f, "%.2fx")) {
        config_dirty_ = true;
    }
    if (ImGui::SliderFloat("Auto-Ducking програми (dB)", &cfg_.intercom_ducking_db, -24.0f, 0.0f, "%.1f dB")) {
        sync_config_to_hub();
        config_dirty_ = true;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, director_ptt_ ? p.red : p.accent_node);
    ImGui::Button("ТРИМАТИ ДЛЯ PTT", ImVec2(180.0f, 34.0f));
    director_ptt_ = ImGui::IsItemActive();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("LATCH (Постійний ефір)", &director_latch_);
    ImGui::SameLine();
    ImGui::Checkbox("Mute Прийомного каналу", &director_listen_mute_);

    hub_.tally_intercom().set_director_mic_state(director_ptt_ || director_latch_ || hub_.tally_intercom().all_call(),
                                                 director_listen_mute_, cfg_.intercom_ifb_volume, 0.0f);

    ImGui::Text("Рівень TX (Директор):");
    ImGui::SameLine();
    draw_vu_meter(hub_.tally_intercom().tx_peak_dbfs(), 160.0f, 14.0f);
    ImGui::Text("Рівень RX (Оператори):");
    ImGui::SameLine();
    draw_vu_meter(hub_.tally_intercom().rx_peak_dbfs(), 160.0f, 14.0f);

    ImGui::Separator();
    draw_nic_selector_panel(
        "vh_nic", nics_, cfg_.primary_nic_id, cfg_.primary_nic_ip, cfg_.secondary_nic_id, cfg_.secondary_nic_ip,
        cfg_.dual_nic_redundancy_enabled, 0, [this]() { refresh_nics(); });
}

}  // namespace lv::gui
