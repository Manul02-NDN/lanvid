#include "intercom_service.hpp"

#include <algorithm>
#include <cstdio>

namespace lv::apps {

LanVidIntercomService::LanVidIntercomService(std::string config_path_override)
    : config_path_(config_path_override.empty() ? lv::ui::config_file_path_for("lanvid_intercom_config.json")
                                                : std::move(config_path_override)) {
    lv_init();
    for (uint32_t i = 0; i < kMaxIntercomChannels; ++i) {
        slots_[i].camera_id = i + 1;
        slots_[i].label = "OP " + std::to_string(i + 1) + " (CAM " + std::to_string(i + 1) + ")";
        slots_[i].group = (i < 8) ? PartylineGroup::kGroupA : PartylineGroup::kGroupB;
    }
}

LanVidIntercomService::~LanVidIntercomService() {
    stop_services();
    save_config();
    lv_shutdown();
}

void LanVidIntercomService::on_config_sync(const LV_VideoConfigSync* cfg, void* user_data) {
    auto* self = static_cast<LanVidIntercomService*>(user_data);
    if (!self || !cfg || cfg->camera_id == 0) return;
    std::lock_guard<std::mutex> lk(self->slots_mu_);
    for (auto& s : self->slots_) {
        if (s.camera_id == cfg->camera_id) {
            s.online = true;
            if (cfg->assigned_cam_tag[0] != '\0') s.label = cfg->assigned_cam_tag;
            break;
        }
    }
}

bool LanVidIntercomService::load_config() {
    lv::ui::JsonConfig j;
    if (!j.load(config_path_)) return false;
    j.get_string("desk_name", cfg_.desk_name);
    j.get_int("intercom_port", cfg_.intercom_port);
    j.get_int("discovery_port", cfg_.discovery_port);
    j.get_int("mic_index", cfg_.mic_index);
    j.get_int("headphones_index", cfg_.headphones_index);
    j.get_float("ifb_volume", cfg_.ifb_volume);
    j.get_float("sidetone_gain", cfg_.sidetone_gain);
    j.get_float("auto_ducking_db", cfg_.auto_ducking_db);
    j.get_bool("listen_muted", cfg_.listen_muted);
    j.get_bool("dark_theme", cfg_.dark_theme);
    j.get_int("active_group_filter", cfg_.active_group_filter);
    j.get_string_list("slot_labels", cfg_.slot_labels);
    j.get_int_list("slot_groups", cfg_.slot_groups);

    std::lock_guard<std::mutex> lk(slots_mu_);
    for (uint32_t i = 0; i < kMaxIntercomChannels; ++i) {
        if (i < cfg_.slot_labels.size() && !cfg_.slot_labels[i].empty()) slots_[i].label = cfg_.slot_labels[i];
        if (i < cfg_.slot_groups.size()) {
            slots_[i].group = static_cast<PartylineGroup>(std::clamp(cfg_.slot_groups[i], 0, 2));
        }
    }
    return true;
}

bool LanVidIntercomService::save_config() const {
    lv::ui::JsonConfig j;
    j.set_string("desk_name", cfg_.desk_name);
    j.set_int("intercom_port", cfg_.intercom_port);
    j.set_int("discovery_port", cfg_.discovery_port);
    j.set_int("mic_index", cfg_.mic_index);
    j.set_int("headphones_index", cfg_.headphones_index);
    j.set_float("ifb_volume", cfg_.ifb_volume);
    j.set_float("sidetone_gain", cfg_.sidetone_gain);
    j.set_float("auto_ducking_db", cfg_.auto_ducking_db);
    j.set_bool("listen_muted", cfg_.listen_muted);
    j.set_bool("dark_theme", cfg_.dark_theme);
    j.set_int("active_group_filter", cfg_.active_group_filter);

    std::vector<std::string> lbls(kMaxIntercomChannels);
    std::vector<int> grps(kMaxIntercomChannels);
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        for (uint32_t i = 0; i < kMaxIntercomChannels; ++i) {
            lbls[i] = slots_[i].label;
            grps[i] = int(slots_[i].group);
        }
    }
    j.set_string_list("slot_labels", lbls);
    j.set_int_list("slot_groups", grps);
    return j.save(config_path_);
}

bool LanVidIntercomService::start_services(uint16_t intercom_port, uint16_t discovery_port) {
    stop_services();
    bound_disc_port_ = discovery_port;
    handle_ = lv_tally_intercom_create_studio(intercom_port);
    if (!handle_) return false;
    lv_tally_intercom_set_video_config_sync_callback(handle_, &LanVidIntercomService::on_config_sync, this);

    discovery_.set_role("intercom");
    if (!discovery_.start(bound_intercom_port(), cfg_.desk_name, discovery_port)) {
        lv_tally_intercom_destroy(handle_);
        handle_ = nullptr;
        return false;
    }

    mdns_.start(cfg_.desk_name, bound_intercom_port(), {{"role", "intercom"}, {"ver", "2.7.0"}});
    services_running_.store(true, std::memory_order_release);
    return true;
}

void LanVidIntercomService::stop_services() {
    services_running_.store(false, std::memory_order_release);
    mdns_.stop();
    discovery_.stop();
    if (handle_) {
        lv_tally_intercom_destroy(handle_);
        handle_ = nullptr;
    }
}

void LanVidIntercomService::poll() {
    if (handle_) lv_tally_intercom_poll(handle_);
}

void LanVidIntercomService::set_director_state(bool ptt_or_latch, bool listen_muted, float ifb_gain,
                                                 float sidetone_gain) {
    director_talk_ = ptt_or_latch;
    cfg_.listen_muted = listen_muted;
    cfg_.ifb_volume = std::clamp(ifb_gain, 0.0f, 2.0f);
    cfg_.sidetone_gain = std::clamp(sidetone_gain, 0.0f, 1.0f);
    if (handle_) {
        const bool any_talk = director_talk_ || all_call_.load(std::memory_order_relaxed) ||
                              talk_group_a_.load(std::memory_order_relaxed) ||
                              talk_group_b_.load(std::memory_order_relaxed);
        lv_intercom_set_state(handle_, any_talk ? 1 : 0, listen_muted ? 1 : 0, cfg_.ifb_volume, cfg_.sidetone_gain);
    }
}

void LanVidIntercomService::set_all_call(bool enabled) {
    all_call_.store(enabled, std::memory_order_relaxed);
    if (handle_) {
        lv_tally_intercom_set_camera_intercom(handle_, 0, enabled ? 1 : 0, 1, enabled ? 1 : 0);
        set_director_state(director_talk_, cfg_.listen_muted, cfg_.ifb_volume, cfg_.sidetone_gain);
    }
}

void LanVidIntercomService::set_partyline_talk(PartylineGroup group, bool enabled) {
    if (group == PartylineGroup::kGroupA) talk_group_a_.store(enabled, std::memory_order_relaxed);
    else if (group == PartylineGroup::kGroupB) talk_group_b_.store(enabled, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(slots_mu_);
    for (auto& s : slots_) {
        if (s.group == group) {
            s.talk_to_op = enabled;
            if (handle_) {
                lv_tally_intercom_set_camera_intercom(handle_, s.camera_id, enabled ? 1 : 0,
                                                      s.listen_from_op ? 1 : 0,
                                                      all_call_.load(std::memory_order_relaxed) ? 1 : 0);
            }
        }
    }
    set_director_state(director_talk_, cfg_.listen_muted, cfg_.ifb_volume, cfg_.sidetone_gain);
}

bool LanVidIntercomService::partyline_talk(PartylineGroup group) const {
    if (group == PartylineGroup::kGroupA) return talk_group_a_.load(std::memory_order_relaxed);
    if (group == PartylineGroup::kGroupB) return talk_group_b_.load(std::memory_order_relaxed);
    return false;
}

void LanVidIntercomService::set_operator_talk(uint32_t slot, bool talk, bool listen) {
    if (slot >= kMaxIntercomChannels) return;
    uint32_t cam_id = 0;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        slots_[slot].talk_to_op = talk;
        slots_[slot].listen_from_op = listen;
        cam_id = slots_[slot].camera_id;
    }
    if (handle_) {
        lv_tally_intercom_set_camera_intercom(handle_, cam_id, talk ? 1 : 0, listen ? 1 : 0,
                                              all_call_.load(std::memory_order_relaxed) ? 1 : 0);
    }
}

void LanVidIntercomService::set_operator_group(uint32_t slot, PartylineGroup group) {
    if (slot >= kMaxIntercomChannels) return;
    std::lock_guard<std::mutex> lk(slots_mu_);
    slots_[slot].group = group;
}

void LanVidIntercomService::set_operator_tally(uint32_t slot, uint8_t tally_state, bool remote_rec) {
    if (slot >= kMaxIntercomChannels) return;
    uint32_t cam_id = 0;
    std::string lbl;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        slots_[slot].tally_state = tally_state;
        slots_[slot].remote_rec = remote_rec;
        cam_id = slots_[slot].camera_id;
        lbl = slots_[slot].label;
    }
    if (handle_) {
        lv_tally_intercom_set_camera_tally(handle_, cam_id, tally_state, lbl.c_str(), remote_rec ? 1 : 0);
    }
}

int LanVidIntercomService::push_director_mic_pcm(const float* samples, int frames) {
    return handle_ ? lv_intercom_push_mic_pcm(handle_, samples, frames) : LV_ERR_NOT_RUNNING;
}

int LanVidIntercomService::pull_director_rx_pcm(float* out_samples, int frames) {
    return handle_ ? lv_intercom_pull_rx_pcm(handle_, out_samples, frames) : 0;
}

bool LanVidIntercomService::is_director_speaking() const {
    return handle_ && lv_tally_intercom_is_director_speaking(handle_) != 0;
}

float LanVidIntercomService::ducking_gain() const {
    return handle_ ? lv_tally_intercom_ducking_gain(handle_) : 1.0f;
}

std::array<IntercomOperatorSlot, kMaxIntercomChannels> LanVidIntercomService::slots() const {
    std::lock_guard<std::mutex> lk(slots_mu_);
    return slots_;
}

std::vector<LvNetInterfaceInfo> LanVidIntercomService::network_interfaces() const {
    std::vector<LvNetInterfaceInfo> out(32);
    const int n = lv_net_list_interfaces(out.data(), 32);
    out.resize(size_t(std::max(0, n)));
    return out;
}

std::vector<lv::SystemAudioDeviceEntry> LanVidIntercomService::microphones() const {
    return lv::list_system_audio_devices(false, true);
}

std::vector<lv::SystemAudioDeviceEntry> LanVidIntercomService::headphones() const {
    return lv::list_system_audio_devices(true, true);
}

}  // namespace lv::apps
