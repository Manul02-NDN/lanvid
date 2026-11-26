#include "video_app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "common/clock.hpp"
#include "common_ui/qr_code.hpp"
#include "common_ui/theme.hpp"
#include "imgui.h"

namespace lv::apps {

namespace {
constexpr const char* kVideoPages[] = {
    "Хаб камер",
    "Інспектор камери",
    "Запис та Ефір",
    "Віртуальні камери / OBS",
};
constexpr const char* kCamSubtabs[] = {"Сітка камер (Multiview)", "Пошук та QR-підключення"};
constexpr const char* kRecSubtabs[] = {"Багатоканальний ISO-запис", "Ефірний пульт (PVW / PGM)"};
constexpr int kPresets[] = {16, 33, 50, 80};
constexpr const char* kResLabels[] = {"360p Proxy", "720p HD", "1080p FHD", "4K UHD"};
constexpr int kFpsVals[] = {24, 25, 30, 50, 60};
constexpr const char* kFpsLabels[] = {"24 fps", "25 fps", "30 fps", "50 fps", "60 fps"};
constexpr const char* kCodecLabels[] = {"LanVid DCT", "H.264", "H.265"};
constexpr const char* kLensLabels[] = {"Wide (1x)", "Ultra-Wide", "Tele", "Front"};

void open_dir(const std::string& p) {
    if (p.empty()) return;
#if defined(_WIN32)
    (void)std::system(("explorer \"" + p + "\"").c_str());
#elif defined(__APPLE__)
    (void)std::system(("open \"" + p + "\" &").c_str());
#else
    (void)std::system(("xdg-open \"" + p + "\" &").c_str());
#endif
}
}  // namespace

LanVidVideoApp::LanVidVideoApp(std::string config_path_override) : svc_(std::move(config_path_override)) {}
LanVidVideoApp::~LanVidVideoApp() { shutdown(); }

void LanVidVideoApp::init() {
    svc_.load_config();
    auto& c = svc_.config();
    std::snprintf(manual_ip_buf_, sizeof(manual_ip_buf_), "192.168.1.50");
    std::snprintf(iso_dir_buf_, sizeof(iso_dir_buf_), "%s", c.iso_output_dir.c_str());
    std::snprintf(iso_sess_buf_, sizeof(iso_sess_buf_), "%s", c.iso_session_name.c_str());
    lv::ui::apply_theme(c.dark_theme);
    nics_ = svc_.network_interfaces();
    svc_.start_services(uint16_t(c.video_port), uint16_t(c.tally_port), uint16_t(c.discovery_port));
}

void LanVidVideoApp::shutdown() {
    svc_.config().iso_output_dir = iso_dir_buf_;
    svc_.config().iso_session_name = iso_sess_buf_;
    svc_.sync_hub_to_config();
    svc_.stop_services();
    svc_.save_config();
    if (host_.destroy_texture) {
        for (uint32_t i = 0; i < lv::hub::kMaxHubSlots; ++i) {
            if (tex_[i]) {
                host_.destroy_texture(host_.ctx, tex_[i]);
                tex_[i] = nullptr;
            }
        }
    }
}

void LanVidVideoApp::update_slot_texture(uint32_t slot) {
    if (!host_.create_texture || slot >= lv::hub::kMaxHubSlots) return;
    lv::Image frame;
    if (!svc_.engine().poll_slot_frame(slot, frame, tex_seq_[slot])) return;
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) return;
    if (!tex_[slot]) tex_[slot] = host_.create_texture(host_.ctx);
    if (!tex_[slot]) return;
    if (tex_[slot]->wants_rgba() && frame.order == lv::PixelOrder::kBGRA) {
        const size_t px = size_t(frame.width) * size_t(frame.height);
        rgba_tmp_.resize(px * 4u);
        for (size_t i = 0; i < px; ++i) {
            rgba_tmp_[i * 4 + 0] = frame.data[i * 4 + 2];
            rgba_tmp_[i * 4 + 1] = frame.data[i * 4 + 1];
            rgba_tmp_[i * 4 + 2] = frame.data[i * 4 + 0];
            rgba_tmp_[i * 4 + 3] = frame.data[i * 4 + 3];
        }
        tex_[slot]->update(rgba_tmp_.data(), frame.width, frame.height);
    } else {
        tex_[slot]->update(frame.data.data(), frame.width, frame.height);
    }
}

void LanVidVideoApp::draw_slot_preview(uint32_t slot, float w, float h, const char* empty_hint) {
    const auto& p = lv::ui::theme_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + w, pos.y + h);
    if (slot < lv::hub::kMaxHubSlots && tex_[slot] && tex_[slot]->width() > 0) {
        const float aspect = float(tex_[slot]->width()) / std::max(1.0f, float(tex_[slot]->height()));
        float dw = w, dh = w / aspect;
        if (dh > h) { dh = h; dw = h * aspect; }
        const float ox = pos.x + (w - dw) * 0.5f, oy = pos.y + (h - dh) * 0.5f;
        dl->AddRectFilled(pos, end, IM_COL32(15, 23, 42, 235), 8.0f);
        dl->AddImage(tex_[slot]->imgui_id(), ImVec2(ox, oy), ImVec2(ox + dw, oy + dh));
    } else {
        dl->AddRectFilled(pos, end, ImGui::GetColorU32(p.card_soft), 8.0f);
        dl->AddRect(pos, end, ImGui::GetColorU32(p.border), 8.0f);
        const ImVec2 tsz = ImGui::CalcTextSize(empty_hint);
        dl->AddText(ImVec2(pos.x + (w - tsz.x) * 0.5f, pos.y + (h - tsz.y) * 0.5f),
                    ImGui::GetColorU32(p.text_muted), empty_hint);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void LanVidVideoApp::render_frame() {
    svc_.poll();
    const uint64_t now = lv::now_us();
    if (now - last_disk_us_ > 2'000'000ULL) {
        last_disk_us_ = now;
        free_gb_ = double(lv::hub::IsoMultiRecorder::disk_free_bytes(iso_dir_buf_)) / (1024.0 * 1024.0 * 1024.0);
    }
    for (uint32_t i = 0; i < uint32_t(svc_.config().max_slots); ++i) update_slot_texture(i);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidVideoRoot", nullptr, wf);
    lv::ui::draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##VideoSide", ImVec2(220.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##VideoMain", ImVec2(0.0f, avail_h), false);
    const int pg = svc_.config().active_page;
    if (pg == 0) draw_cameras_page();
    else if (pg == 1) draw_inspector_page();
    else if (pg == 2) draw_record_switcher_page();
    else draw_vcam_obs_page();
    ImGui::EndChild();

    ImGui::End();
}

void LanVidVideoApp::draw_top_bar() {
    const auto& p = lv::ui::theme_palette();
    auto& c = svc_.config();
    auto& eng = svc_.engine();
    ImGui::TextColored(p.accent_purp, "LanVid Video v2.7.0");
    ImGui::SameLine();
    lv::ui::draw_status_pill(svc_.services_running() ? "HUB LIVE" : "OFF",
                             svc_.services_running() ? p.green : p.red, true);
    ImGui::SameLine();
    lv::ui::draw_status_pill(("TC " + eng.iso().session_timecode()).c_str(),
                             eng.iso().is_recording_any() ? p.red : p.accent_ind, false);
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "Video :%u | Tally :%u | SSD: %.1f GB",
                       unsigned(svc_.bound_video_port()), unsigned(svc_.bound_tally_port()), free_gb_);

    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - 180.0f));
    const bool rec = eng.iso().is_recording_any();
    ImGui::PushStyleColor(ImGuiCol_Button, rec ? p.red : p.accent_node);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::SmallButton(rec ? "STOP REC ALL" : "REC ALL")) {
        if (rec) eng.rec_stop_all(c.sync_remote_rec);
        else {
            lv::hub::IsoMultiSessionConfig sc{};
            sc.output_dir = iso_dir_buf_;
            sc.session_name = iso_sess_buf_;
            sc.container = static_cast<lv::IsoContainer>(std::clamp(c.iso_container, 0, 2));
            sc.record_program = c.iso_record_program;
            sc.slot_mask = uint32_t(c.iso_slot_mask);
            sc.slot_count = uint32_t(c.max_slots);
            eng.rec_start_all(sc, c.sync_remote_rec);
        }
    }
    ImGui::PopStyleColor(2);
}

void LanVidVideoApp::draw_sidebar() {
    const auto& p = lv::ui::theme_palette();
    auto& c = svc_.config();
    for (int i = 0; i < 4; ++i) {
        const bool sel = (c.active_page == i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        if (ImGui::Button(kVideoPages[i], ImVec2(-1.0f, 38.0f))) c.active_page = i;
        ImGui::PopStyleColor(2);
    }
    ImGui::Separator();
    int live = 0;
    for (int i = 0; i < c.max_slots; ++i)
        if (svc_.engine().slot_snapshot(uint32_t(i)).streaming) ++live;
    ImGui::Text("Камери: %d / %d", live, c.max_slots);
    lv::ui::draw_status_pill(svc_.engine().vcam().is_open() ? "VideoBus SHM: OK" : "VideoBus SHM: OFF",
                             svc_.engine().vcam().is_open() ? p.green : p.amber, false);
    if (ImGui::SmallButton(c.dark_theme ? "Світла Лаванда" : "Темна Студія")) {
        c.dark_theme = !c.dark_theme;
        lv::ui::apply_theme(c.dark_theme);
    }
}

void LanVidVideoApp::draw_cameras_page() {
    auto& c = svc_.config();
    auto& eng = svc_.engine();
    const auto& p = lv::ui::theme_palette();
    lv::ui::draw_subtab_pills(c.active_subtab_cam, kCamSubtabs, 2, "vc");
    if (c.active_subtab_cam == 0) {
        for (int g = 1; g <= 4; ++g) {
            if (g > 1) ImGui::SameLine();
            if (ImGui::SmallButton((std::to_string(g) + "x" + std::to_string(g)).c_str())) c.multiview_layout = g;
        }
        const float cw = std::max(210.0f, (ImGui::GetContentRegionAvail().x - float(c.multiview_layout - 1) * 10.0f) /
                                              float(c.multiview_layout));
        const float ph = std::max(100.0f, (cw - 16.0f) * 9.0f / 16.0f);
        for (int i = 0; i < c.max_slots; ++i) {
            if (i % c.multiview_layout != 0) ImGui::SameLine();
            const auto s = eng.slot_snapshot(uint32_t(i));
            const bool pgm = (eng.program_slot() == i), pvw = (eng.preview_slot() == i);
            ImGui::PushStyleColor(ImGuiCol_Border, pgm ? p.red : (pvw ? p.green : p.border));
            ImGui::BeginChild(("##vc" + std::to_string(i)).c_str(), ImVec2(cw, ph + 96.0f), true);
            ImGui::Text("%s • %s", s.settings.assigned_cam_tag.c_str(), s.label.c_str());
            draw_slot_preview(uint32_t(i), cw - 16.0f, ph, "Очікування камери");
            if (ImGui::SmallButton("PVW")) eng.set_preview_slot(i);
            ImGui::SameLine();
            if (ImGui::SmallButton("PGM")) eng.set_program_slot(i);
            ImGui::SameLine();
            const bool rec = eng.iso().is_slot_recording(uint32_t(i));
            if (ImGui::SmallButton(rec ? "STOP" : "REC")) eng.rec_slot(uint32_t(i), !rec, c.sync_remote_rec);
            ImGui::SameLine();
            if (ImGui::SmallButton("Огляд")) { selected_slot_ = i; c.active_page = 1; }
            ImGui::TextColored(p.text_muted, "%ux%u • %.0ffps • J:%.1fms", s.width, s.height, s.fps,
                               s.playout_jitter_ms);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::BeginChild("##DiscL", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0.0f), true);
        if (ImGui::SmallButton("Сканувати LAN")) eng.discovery().trigger_scan();
        for (const auto& pr : eng.discovery().discovered_peers()) {
            ImGui::Text("%s (%s:%u)", pr.name.c_str(), pr.ip.c_str(), unsigned(pr.stream_port));
            ImGui::SameLine();
            if (ImGui::SmallButton(("Підключити##" + pr.ip).c_str())) {
                eng.connect_peer_to_slot(uint32_t(target_slot_), pr.ip, pr.stream_port, pr.device_uid, pr.name);
            }
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("IP камери", manual_ip_buf_, sizeof(manual_ip_buf_));
        ImGui::SameLine();
        if (ImGui::SmallButton("Прив'язати")) {
            eng.connect_peer_to_slot(uint32_t(target_slot_), manual_ip_buf_, uint16_t(manual_port_), "", "");
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##DiscR", ImVec2(0.0f, 0.0f), true);
        const std::string ip = nics_.empty() ? "127.0.0.1" : nics_[0].ipv4_address;
        const std::string uri =
            lv::hub::HubDiscovery::build_qr_uri(ip, svc_.bound_video_port(), svc_.bound_tally_port(), c.hub_name);
        lv::ui::draw_qr_box(uri, 200.0f);
        ImGui::TextWrapped("%s", uri.c_str());
        ImGui::EndChild();
    }
}

void LanVidVideoApp::draw_inspector_page() {
    auto& eng = svc_.engine();
    auto& c = svc_.config();
    selected_slot_ = std::clamp(selected_slot_, 0, c.max_slots - 1);
    for (int i = 0; i < c.max_slots; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(("CAM " + std::to_string(i + 1)).c_str())) selected_slot_ = i;
    }
    ImGui::Separator();
    const auto s = eng.slot_snapshot(uint32_t(selected_slot_));
    if (tag_edit_idx_ != selected_slot_) {
        tag_edit_idx_ = selected_slot_;
        std::snprintf(label_buf_, sizeof(label_buf_), "%s", s.label.c_str());
        std::snprintf(tag_buf_, sizeof(tag_buf_), "%s", s.settings.assigned_cam_tag.c_str());
    }
    auto rs = s.settings;
    int bn = rs.assigned_cam_number;
    if (ImGui::SliderInt("Значок номера (1..16)", &bn, 1, 16)) {
        eng.assign_slot_badge(uint32_t(selected_slot_), uint8_t(bn), "", true);
    }
    int delay = rs.video_delay_ms;
    bool fixed = rs.fixed_delay_mode;
    if (ImGui::RadioButton("Fixed Delay", fixed)) eng.set_slot_playout_delay(uint32_t(selected_slot_), delay, true);
    ImGui::SameLine();
    if (ImGui::RadioButton("Adaptive Wi-Fi", !fixed)) eng.set_slot_playout_delay(uint32_t(selected_slot_), delay, false);
    for (int pms : kPresets) {
        if (ImGui::SmallButton((std::to_string(pms) + " ms").c_str()))
            eng.set_slot_playout_delay(uint32_t(selected_slot_), uint32_t(pms), fixed);
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::SliderInt("Затримка (ms)", &delay, 5, 250)) {
        eng.set_slot_playout_delay(uint32_t(selected_slot_), uint32_t(delay), fixed);
    }
    bool chg = false;
    int res = rs.resolution_preset, cod = rs.codec, lens = rs.camera_lens;
    int fps_i = 4;
    for (int f = 0; f < 5; ++f) if (kFpsVals[f] == rs.target_fps) fps_i = f;
    int mbps = int(rs.bitrate_kbps / 1000u);
    if (ImGui::Combo("Роздільність", &res, kResLabels, 4)) { rs.resolution_preset = uint8_t(res); chg = true; }
    if (ImGui::Combo("FPS", &fps_i, kFpsLabels, 5)) { rs.target_fps = uint8_t(kFpsVals[fps_i]); chg = true; }
    if (ImGui::Combo("Кодек", &cod, kCodecLabels, 3)) { rs.codec = uint8_t(cod); chg = true; }
    if (ImGui::SliderInt("Бітрейт (Mbps)", &mbps, 5, 150)) { rs.bitrate_kbps = uint32_t(mbps) * 1000u; chg = true; }
    if (ImGui::Combo("Об'єктив", &lens, kLensLabels, 4)) { rs.camera_lens = uint8_t(lens); chg = true; }
    if (chg) eng.apply_slot_remote_settings(uint32_t(selected_slot_), rs, true);
    if (ImGui::Button("Force IDR")) eng.force_slot_idr(uint32_t(selected_slot_));
}

void LanVidVideoApp::draw_record_switcher_page() {
    auto& c = svc_.config();
    auto& eng = svc_.engine();
    lv::ui::draw_subtab_pills(c.active_subtab_rec, kRecSubtabs, 2, "vr");
    if (c.active_subtab_rec == 0) {
        ImGui::InputText("Сесія", iso_sess_buf_, sizeof(iso_sess_buf_));
        ImGui::InputText("Папка", iso_dir_buf_, sizeof(iso_dir_buf_));
        ImGui::SameLine();
        if (ImGui::Button("Відкрити")) open_dir(iso_dir_buf_);
        ImGui::Checkbox("Писати PGM Master", &c.iso_record_program);
        ImGui::SameLine();
        if (ImGui::Checkbox("Remote Phone REC", &c.sync_remote_rec)) eng.set_sync_remote_rec(c.sync_remote_rec);
        for (int i = 0; i < c.max_slots; ++i) {
            const auto st = eng.iso().slot_stats(uint32_t(i));
            ImGui::Text("CAM %d: %s • %llu кадрів • %.2f MB", i + 1, st.recording ? "REC" : "IDLE",
                        static_cast<unsigned long long>(st.frames), double(st.bytes) / (1024.0 * 1024.0));
        }
    } else {
        const float w = (ImGui::GetContentRegionAvail().x - 16.0f) * 0.5f;
        const float h = w * 9.0f / 16.0f;
        ImGui::BeginChild("##Pvw", ImVec2(w, h + 40.0f), true);
        ImGui::Text("PREVIEW CAM %d", eng.preview_slot() + 1);
        draw_slot_preview(uint32_t(std::max(0, eng.preview_slot())), w - 16.0f, h, "PREVIEW");
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##Pgm", ImVec2(w, h + 40.0f), true);
        ImGui::Text("PROGRAM CAM %d", eng.program_slot() + 1);
        draw_slot_preview(uint32_t(std::max(0, eng.program_slot())), w - 16.0f, h, "PROGRAM");
        ImGui::EndChild();
        if (ImGui::Button("CUT (PVW -> PGM)", ImVec2(200.0f, 36.0f))) eng.cut_preview_to_program();
    }
}

void LanVidVideoApp::draw_vcam_obs_page() {
    auto& c = svc_.config();
    auto& eng = svc_.engine();
    if (ImGui::SliderInt("Кількість віртуальних камер (LanVid Cam 1..8)", &c.vcam_count, 0, 8))
        eng.set_vcam_config(uint32_t(c.vcam_count), c.vcam_placeholder, c.lip_sync_ms);
    if (ImGui::Checkbox("Лавандова заглушка Standby", &c.vcam_placeholder))
        eng.set_vcam_config(uint32_t(c.vcam_count), c.vcam_placeholder, c.lip_sync_ms);
    if (ImGui::SliderInt("Lip-Sync (ms)", &c.lip_sync_ms, -200, 200))
        eng.set_vcam_config(uint32_t(c.vcam_count), c.vcam_placeholder, c.lip_sync_ms);
    for (uint32_t i = 0; i < lv::kVideoBusSlots; ++i) {
        ImGui::Text("%s", lv::video_bus_device_name(i));
    }
}

}  // namespace lv::apps
