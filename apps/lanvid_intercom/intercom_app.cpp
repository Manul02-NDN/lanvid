#include "intercom_app.hpp"

#include <algorithm>
#include <cstdio>

#include "common_ui/qr_code.hpp"
#include "common_ui/theme.hpp"
#include "imgui.h"

namespace lv::apps {

LanVidIntercomApp::LanVidIntercomApp(std::string config_path_override) : svc_(std::move(config_path_override)) {}
LanVidIntercomApp::~LanVidIntercomApp() { shutdown(); }

void LanVidIntercomApp::init() {
    svc_.load_config();
    auto& c = svc_.config();
    lv::ui::apply_theme(c.dark_theme);
    nics_ = svc_.network_interfaces();
    mics_ = svc_.microphones();
    hps_ = svc_.headphones();
    if (!svc_.start_services(uint16_t(c.intercom_port), uint16_t(c.discovery_port))) {
        svc_.start_services(0, 0);
    }
}

void LanVidIntercomApp::shutdown() {
    svc_.stop_services();
    svc_.save_config();
}

void LanVidIntercomApp::render_frame() {
    svc_.poll();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidIntercomRoot", nullptr, wf);
    lv::ui::draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##IcmSide", ImVec2(260.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##IcmMain", ImVec2(0.0f, avail_h), false);
    draw_operator_matrix();
    ImGui::EndChild();

    ImGui::End();
}

void LanVidIntercomApp::draw_top_bar() {
    const auto& p = lv::ui::theme_palette();
    ImGui::TextColored(p.accent_purp, "LanVid Intercom v2.7.0");
    ImGui::SameLine();
    lv::ui::draw_status_pill(svc_.services_running() ? "DESK LIVE" : "OFF",
                             svc_.services_running() ? p.green : p.red, true);
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "Intercom :%u  |  Discovery :%u",
                       unsigned(svc_.bound_intercom_port()), unsigned(svc_.bound_discovery_port()));

    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - 320.0f));
    const bool all_on = svc_.all_call();
    ImGui::PushStyleColor(ImGuiCol_Button, all_on ? p.red : p.accent_node);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::SmallButton(all_on ? "ALL CALL: ON AIR" : "ALL CALL")) svc_.set_all_call(!all_on);
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    const bool ga = svc_.partyline_talk(PartylineGroup::kGroupA);
    ImGui::PushStyleColor(ImGuiCol_Button, ga ? p.amber : p.card_soft);
    if (ImGui::SmallButton("Group A")) svc_.set_partyline_talk(PartylineGroup::kGroupA, !ga);
    ImGui::PopStyleColor();

    ImGui::SameLine();
    const bool gb = svc_.partyline_talk(PartylineGroup::kGroupB);
    ImGui::PushStyleColor(ImGuiCol_Button, gb ? p.amber : p.card_soft);
    if (ImGui::SmallButton("Group B")) svc_.set_partyline_talk(PartylineGroup::kGroupB, !gb);
    ImGui::PopStyleColor();
}

void LanVidIntercomApp::draw_sidebar() {
    const auto& p = lv::ui::theme_palette();
    auto& c = svc_.config();

    ImGui::PushStyleColor(ImGuiCol_Button, ptt_active_ ? p.red : p.accent_node);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::Button("TALK (PTT — тримати)", ImVec2(-1.0f, 52.0f));
    ptt_active_ = ImGui::IsItemActive();
    ImGui::PopStyleColor(2);

    ImGui::Checkbox("LATCH (Постійний голос)", &latch_active_);
    if (ImGui::Checkbox("Вимкнути звук прийому (Mute)", &c.listen_muted)) {}
    ImGui::SliderFloat("Гучність IFB", &c.ifb_volume, 0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Auto-Ducking (dB)", &c.auto_ducking_db, -24.0f, 0.0f, "%.1f dB");
    svc_.set_director_state(ptt_active_ || latch_active_, c.listen_muted, c.ifb_volume, c.sidetone_gain);

    ImGui::Separator();
    const char* mic_n = (c.mic_index >= 0 && c.mic_index < int(mics_.size()))
                            ? mics_[size_t(c.mic_index)].name.c_str()
                            : "Системний мікрофон";
    if (ImGui::BeginCombo("Мікрофон", mic_n)) {
        if (ImGui::Selectable("Системний мікрофон", c.mic_index < 0)) c.mic_index = -1;
        for (size_t i = 0; i < mics_.size(); ++i)
            if (ImGui::Selectable(mics_[i].name.c_str(), c.mic_index == int(i))) c.mic_index = int(i);
        ImGui::EndCombo();
    }
    const char* hp_n = (c.headphones_index >= 0 && c.headphones_index < int(hps_.size()))
                           ? hps_[size_t(c.headphones_index)].name.c_str()
                           : "Системні навушники";
    if (ImGui::BeginCombo("Навушники", hp_n)) {
        if (ImGui::Selectable("Системні навушники", c.headphones_index < 0)) c.headphones_index = -1;
        for (size_t i = 0; i < hps_.size(); ++i)
            if (ImGui::Selectable(hps_[i].name.c_str(), c.headphones_index == int(i))) c.headphones_index = int(i);
        ImGui::EndCombo();
    }

    ImGui::Separator();
    const std::string ip = nics_.empty() ? "127.0.0.1" : nics_[0].ipv4_address;
    const std::string qr = "lanvid://intercom?host=" + ip + "&port=" + std::to_string(svc_.bound_intercom_port());
    ImGui::TextColored(p.accent_purp, "QR для Beltpack:");
    lv::ui::draw_qr_box(qr, 165.0f);
    if (ImGui::SmallButton(c.dark_theme ? "Світла Лаванда" : "Темна Студія")) {
        c.dark_theme = !c.dark_theme;
        lv::ui::apply_theme(c.dark_theme);
    }
}

void LanVidIntercomApp::draw_operator_matrix() {
    const auto& p = lv::ui::theme_palette();
    const auto& slots = svc_.slots();
    const float cw = std::max(210.0f, (ImGui::GetContentRegionAvail().x - 30.0f) / 4.0f);

    for (uint32_t i = 0; i < kMaxIntercomChannels; ++i) {
        if (i % 4 != 0) ImGui::SameLine();
        const auto& s = slots[i];
        const ImVec4 bc = (s.tally_state == 2) ? p.red : (s.tally_state == 1 ? p.green : p.border);
        ImGui::PushStyleColor(ImGuiCol_Border, bc);
        ImGui::BeginChild(("##op" + std::to_string(i)).c_str(), ImVec2(cw, 132.0f), true);

        ImGui::Text("%s", s.label.c_str());
        lv::ui::draw_status_pill(s.tally_state == 2 ? "PGM" : (s.tally_state == 1 ? "PVW" : "STBY"),
                                 s.tally_state == 2 ? p.red : (s.tally_state == 1 ? p.green : p.text_muted),
                                 s.tally_state != 0);
        ImGui::SameLine();
        ImGui::TextColored(p.text_muted, s.group == PartylineGroup::kGroupA ? "Grp A" :
                                         (s.group == PartylineGroup::kGroupB ? "Grp B" : "No Grp"));

        ImGui::PushStyleColor(ImGuiCol_Button, s.talk_to_op ? p.amber : p.card_soft);
        if (ImGui::SmallButton("TALK")) svc_.set_operator_talk(i, !s.talk_to_op, s.listen_from_op);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(s.listen_from_op ? "RX:ON" : "RX:MUTE"))
            svc_.set_operator_talk(i, s.talk_to_op, !s.listen_from_op);

        if (ImGui::SmallButton("STBY")) svc_.set_operator_tally(i, 0, s.remote_rec);
        ImGui::SameLine();
        if (ImGui::SmallButton("PVW")) svc_.set_operator_tally(i, 1, s.remote_rec);
        ImGui::SameLine();
        if (ImGui::SmallButton("PGM")) svc_.set_operator_tally(i, 2, s.remote_rec);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

}  // namespace lv::apps
