#include "video_service.hpp"

#include <algorithm>

namespace lv::apps {

LanVidVideoService::LanVidVideoService(std::string config_path_override)
    : config_path_(config_path_override.empty() ? lv::ui::config_file_path_for("lanvid_video_config.json")
                                                : std::move(config_path_override)) {
    lv_init();
    cfg_.iso_output_dir = lv::hub::IsoMultiRecorder::default_output_dir();
}

LanVidVideoService::~LanVidVideoService() {
    stop_services();
    save_config();
    lv_shutdown();
}

bool LanVidVideoService::load_config() {
    lv::ui::JsonConfig j;
    if (!j.load(config_path_)) return false;
    j.get_string("hub_name", cfg_.hub_name);
    j.get_int("video_port", cfg_.video_port);
    j.get_int("tally_port", cfg_.tally_port);
    j.get_int("discovery_port", cfg_.discovery_port);
    j.get_int("port_mode", cfg_.port_mode);
    j.get_int("max_slots", cfg_.max_slots);
    j.get_int("multiview_layout", cfg_.multiview_layout);
    j.get_int("default_delay_ms", cfg_.default_delay_ms);
    j.get_bool("default_fixed_delay", cfg_.default_fixed_delay);
    j.get_int("vcam_count", cfg_.vcam_count);
    j.get_bool("vcam_placeholder", cfg_.vcam_placeholder);
    j.get_int("lip_sync_ms", cfg_.lip_sync_ms);
    j.get_string("iso_output_dir", cfg_.iso_output_dir);
    j.get_string("iso_session_name", cfg_.iso_session_name);
    j.get_int("iso_container", cfg_.iso_container);
    j.get_bool("iso_record_program", cfg_.iso_record_program);
    j.get_int("iso_slot_mask", cfg_.iso_slot_mask);
    j.get_bool("sync_remote_rec", cfg_.sync_remote_rec);
    j.get_bool("dark_theme", cfg_.dark_theme);
    j.get_int("active_page", cfg_.active_page);
    j.get_string("primary_nic_ip", cfg_.primary_nic_ip);
    j.get_string("secondary_nic_ip", cfg_.secondary_nic_ip);
    j.get_bool("dual_nic_enabled", cfg_.dual_nic_enabled);
    j.get_string_list("slot_labels", cfg_.slot_labels);
    j.get_string_list("slot_ips", cfg_.slot_ips);
    j.get_string_list("slot_uids", cfg_.slot_uids);
    j.get_int_list("slot_cam_numbers", cfg_.slot_cam_numbers);
    j.get_string_list("slot_cam_tags", cfg_.slot_cam_tags);
    j.get_int_list("slot_delays_ms", cfg_.slot_delays_ms);
    j.get_int_list("slot_fixed_modes", cfg_.slot_fixed_modes);

    cfg_.max_slots = std::clamp(cfg_.max_slots, 1, 16);
    cfg_.multiview_layout = std::clamp(cfg_.multiview_layout, 1, 4);
    cfg_.default_delay_ms = std::clamp(cfg_.default_delay_ms, 5, 250);
    cfg_.vcam_count = std::clamp(cfg_.vcam_count, 0, 8);
    if (cfg_.iso_output_dir.empty()) cfg_.iso_output_dir = lv::hub::IsoMultiRecorder::default_output_dir();
    return true;
}

bool LanVidVideoService::save_config() const {
    lv::ui::JsonConfig j;
    j.set_string("hub_name", cfg_.hub_name);
    j.set_int("video_port", cfg_.video_port);
    j.set_int("tally_port", cfg_.tally_port);
    j.set_int("discovery_port", cfg_.discovery_port);
    j.set_int("port_mode", cfg_.port_mode);
    j.set_int("max_slots", cfg_.max_slots);
    j.set_int("multiview_layout", cfg_.multiview_layout);
    j.set_int("default_delay_ms", cfg_.default_delay_ms);
    j.set_bool("default_fixed_delay", cfg_.default_fixed_delay);
    j.set_int("vcam_count", cfg_.vcam_count);
    j.set_bool("vcam_placeholder", cfg_.vcam_placeholder);
    j.set_int("lip_sync_ms", cfg_.lip_sync_ms);
    j.set_string("iso_output_dir", cfg_.iso_output_dir);
    j.set_string("iso_session_name", cfg_.iso_session_name);
    j.set_int("iso_container", cfg_.iso_container);
    j.set_bool("iso_record_program", cfg_.iso_record_program);
    j.set_int("iso_slot_mask", cfg_.iso_slot_mask);
    j.set_bool("sync_remote_rec", cfg_.sync_remote_rec);
    j.set_bool("dark_theme", cfg_.dark_theme);
    j.set_int("active_page", cfg_.active_page);
    j.set_string("primary_nic_ip", cfg_.primary_nic_ip);
    j.set_string("secondary_nic_ip", cfg_.secondary_nic_ip);
    j.set_bool("dual_nic_enabled", cfg_.dual_nic_enabled);
    j.set_string_list("slot_labels", cfg_.slot_labels);
    j.set_string_list("slot_ips", cfg_.slot_ips);
    j.set_string_list("slot_uids", cfg_.slot_uids);
    j.set_int_list("slot_cam_numbers", cfg_.slot_cam_numbers);
    j.set_string_list("slot_cam_tags", cfg_.slot_cam_tags);
    j.set_int_list("slot_delays_ms", cfg_.slot_delays_ms);
    j.set_int_list("slot_fixed_modes", cfg_.slot_fixed_modes);
    return j.save(config_path_);
}

bool LanVidVideoService::start_services(uint16_t video_port, uint16_t tally_port, uint16_t discovery_port) {
    stop_services();
    disc_port_ = discovery_port;
    lv::hub::VideoHubConfig hc{};
    hc.hub_name = cfg_.hub_name;
    hc.video_port = video_port;
    hc.tally_port = tally_port;
    hc.discovery_port = discovery_port;
    hc.port_mode = cfg_.port_mode;
    hc.max_slots = uint32_t(std::clamp(cfg_.max_slots, 1, 16));
    hc.publish_vcam_bus = true;
    hc.vcam_count = uint32_t(std::clamp(cfg_.vcam_count, 0, 8));
    hc.vcam_placeholder = cfg_.vcam_placeholder;
    hc.primary_nic_ip = cfg_.primary_nic_ip;
    hc.secondary_nic_ip = cfg_.dual_nic_enabled ? cfg_.secondary_nic_ip : "";
    hc.default_delay_ms = uint16_t(std::clamp(cfg_.default_delay_ms, 5, 250));
    hc.default_fixed_delay = cfg_.default_fixed_delay;
    hc.sync_remote_rec = cfg_.sync_remote_rec;

    for (uint32_t i = 0; i < lv::hub::kMaxHubSlots; ++i) {
        hc.slot_labels[i] = i < cfg_.slot_labels.size() && !cfg_.slot_labels[i].empty()
                                ? cfg_.slot_labels[i]
                                : ("CAM " + std::to_string(i + 1));
        hc.slot_ips[i] = i < cfg_.slot_ips.size() ? cfg_.slot_ips[i] : "";
        hc.slot_uids[i] = i < cfg_.slot_uids.size() ? cfg_.slot_uids[i] : "";
        hc.slot_cam_numbers[i] = (i < cfg_.slot_cam_numbers.size() && cfg_.slot_cam_numbers[i] > 0)
                                     ? uint8_t(cfg_.slot_cam_numbers[i])
                                     : uint8_t(i + 1);
        hc.slot_cam_tags[i] = i < cfg_.slot_cam_tags.size() ? cfg_.slot_cam_tags[i] : "";
        hc.slot_delays_ms[i] = (i < cfg_.slot_delays_ms.size() && cfg_.slot_delays_ms[i] >= 5)
                                   ? uint16_t(cfg_.slot_delays_ms[i])
                                   : hc.default_delay_ms;
        hc.slot_fixed_modes[i] =
            i < cfg_.slot_fixed_modes.size() ? (cfg_.slot_fixed_modes[i] != 0) : hc.default_fixed_delay;
    }

    if (!hub_.start(hc)) {
        services_running_.store(false, std::memory_order_release);
        return false;
    }
    hub_.vcam().set_lip_sync_ms(cfg_.lip_sync_ms);
    services_running_.store(true, std::memory_order_release);
    return true;
}

void LanVidVideoService::stop_services() {
    services_running_.store(false, std::memory_order_release);
    hub_.stop();
}

void LanVidVideoService::poll() {
    if (services_running_.load(std::memory_order_acquire)) hub_.poll();
}

void LanVidVideoService::sync_hub_to_config() {
    const uint32_t n = std::clamp<uint32_t>(uint32_t(cfg_.max_slots), 1, lv::hub::kMaxHubSlots);
    cfg_.slot_labels.resize(n);
    cfg_.slot_ips.resize(n);
    cfg_.slot_uids.resize(n);
    cfg_.slot_cam_numbers.resize(n);
    cfg_.slot_cam_tags.resize(n);
    cfg_.slot_delays_ms.resize(n);
    cfg_.slot_fixed_modes.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const auto s = hub_.slot_snapshot(i);
        cfg_.slot_labels[i] = s.label;
        cfg_.slot_ips[i] = s.pinned_ip;
        cfg_.slot_uids[i] = s.device_uid;
        cfg_.slot_cam_numbers[i] = s.settings.assigned_cam_number;
        cfg_.slot_cam_tags[i] = s.settings.assigned_cam_tag;
        cfg_.slot_delays_ms[i] = s.settings.video_delay_ms;
        cfg_.slot_fixed_modes[i] = s.settings.fixed_delay_mode ? 1 : 0;
    }
}

std::vector<LvNetInterfaceInfo> LanVidVideoService::network_interfaces() const {
    std::vector<LvNetInterfaceInfo> out(32);
    const int n = lv_net_list_interfaces(out.data(), 32);
    out.resize(size_t(std::max(0, n)));
    return out;
}

}  // namespace lv::apps
