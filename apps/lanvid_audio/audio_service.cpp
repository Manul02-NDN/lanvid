#include "audio_service.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lv::apps {

LanVidAudioService::LanVidAudioService(std::string config_path_override)
    : config_path_(config_path_override.empty() ? lv::ui::config_file_path_for("lanvid_audio_config.json")
                                                : std::move(config_path_override)) {
    lv_init();
    for (int i = 0; i < 64; ++i) {
        tx_routing[size_t(i)] = int8_t(i);
        rx_routing[size_t(i)] = int8_t(i);
    }
    duplex_ = lv_duplex_create();
    monitor_srv_ = lv_monitor_server_create(48000);
    cached_adapters_ = lv::grid::list_l2_adapters();
}

LanVidAudioService::~LanVidAudioService() {
    stop_services();
    save_config();
    if (monitor_srv_) {
        lv_monitor_server_destroy(monitor_srv_);
        monitor_srv_ = nullptr;
    }
    if (duplex_) {
        lv_duplex_destroy(duplex_);
        duplex_ = nullptr;
    }
    lv_shutdown();
}

bool LanVidAudioService::load_config() {
    lv::ui::JsonConfig j;
    if (!j.load(config_path_)) return false;
    j.get_string("studio_name", cfg_.studio_name);
    j.get_string("peer_ip", cfg_.peer_ip);
    j.get_int("peer_port", cfg_.peer_port);
    j.get_int("listen_port", cfg_.listen_port);
    j.get_int("monitor_port", cfg_.monitor_port);
    j.get_int("discovery_port", cfg_.discovery_port);
    j.get_int("channels", cfg_.channels);
    j.get_int("latency_ms", cfg_.latency_ms);
    j.get_int("audio_codec", cfg_.audio_codec);
    j.get_int("bitrate_kbps", cfg_.bitrate_kbps);
    j.get_bool("adaptive_jitter", cfg_.adaptive_jitter);
    j.get_bool("wifi_redundancy", cfg_.wifi_redundancy);
    j.get_bool("midi_only", cfg_.midi_only);
    j.get_bool("monitor_flow_enabled", cfg_.monitor_flow_enabled);
    j.get_bool("dark_theme", cfg_.dark_theme);
    j.get_int("active_page", cfg_.active_page);
    j.get_int("active_subtab", cfg_.active_subtab);
    j.get_int("network_engine_mode", cfg_.network_engine_mode);
    j.get_string("grid_nic_name", cfg_.grid_nic_name);
    j.get_int("grid_channels", cfg_.grid_channels);
    j.get_bool("grid_clock_master", cfg_.grid_clock_master);
    j.get_string("primary_nic_ip", cfg_.primary_nic_ip);
    j.get_string("secondary_nic_ip", cfg_.secondary_nic_ip);
    j.get_bool("dual_nic_enabled", cfg_.dual_nic_enabled);
    j.get_string_list("monitor_channel_names", cfg_.monitor_channel_names);

    cfg_.channels = std::clamp(cfg_.channels, 2, 64);
    cfg_.latency_ms = std::clamp(cfg_.latency_ms, 2, 2000);
    cfg_.listen_port = std::clamp(cfg_.listen_port, 1024, 65535);
    cfg_.monitor_port = std::clamp(cfg_.monitor_port, 1024, 65535);
    cfg_.discovery_port = std::clamp(cfg_.discovery_port, 1024, 65535);
    return true;
}

bool LanVidAudioService::save_config() const {
    lv::ui::JsonConfig j;
    j.set_string("studio_name", cfg_.studio_name);
    j.set_string("peer_ip", cfg_.peer_ip);
    j.set_int("peer_port", cfg_.peer_port);
    j.set_int("listen_port", cfg_.listen_port);
    j.set_int("monitor_port", cfg_.monitor_port);
    j.set_int("discovery_port", cfg_.discovery_port);
    j.set_int("channels", cfg_.channels);
    j.set_int("latency_ms", cfg_.latency_ms);
    j.set_int("audio_codec", cfg_.audio_codec);
    j.set_int("bitrate_kbps", cfg_.bitrate_kbps);
    j.set_bool("adaptive_jitter", cfg_.adaptive_jitter);
    j.set_bool("wifi_redundancy", cfg_.wifi_redundancy);
    j.set_bool("midi_only", cfg_.midi_only);
    j.set_bool("monitor_flow_enabled", cfg_.monitor_flow_enabled);
    j.set_bool("dark_theme", cfg_.dark_theme);
    j.set_int("active_page", cfg_.active_page);
    j.set_int("active_subtab", cfg_.active_subtab);
    j.set_int("network_engine_mode", cfg_.network_engine_mode);
    j.set_string("grid_nic_name", cfg_.grid_nic_name);
    j.set_int("grid_channels", cfg_.grid_channels);
    j.set_bool("grid_clock_master", cfg_.grid_clock_master);
    j.set_string("primary_nic_ip", cfg_.primary_nic_ip);
    j.set_string("secondary_nic_ip", cfg_.secondary_nic_ip);
    j.set_bool("dual_nic_enabled", cfg_.dual_nic_enabled);
    j.set_string_list("monitor_channel_names", cfg_.monitor_channel_names);
    return j.save(config_path_);
}

bool LanVidAudioService::start_services(uint16_t audio_port, uint16_t monitor_port, uint16_t discovery_port) {
    stop_services();
    bound_audio_port_ = audio_port;
    bound_disc_port_ = discovery_port;

    if (monitor_srv_) {
        if (lv_monitor_server_start(monitor_srv_, monitor_port) != LV_OK) {
            return false;
        }
        lv_monitor_server_set_wifi_redundancy(monitor_srv_, cfg_.wifi_redundancy ? 1 : 0, 1500);
        lv_monitor_server_set_multichannel_flow(monitor_srv_, cfg_.monitor_flow_enabled ? 1 : 0, 16);
        for (size_t i = 0; i < cfg_.monitor_channel_names.size() && i < 64; ++i) {
            lv_monitor_server_set_channel_name(monitor_srv_, int(i), cfg_.monitor_channel_names[i].c_str());
        }
    }

    discovery_.set_role("audio");
    discovery_.set_network_interfaces(cfg_.primary_nic_ip,
                                      cfg_.dual_nic_enabled ? cfg_.secondary_nic_ip : "");
    const uint16_t ap = bound_audio_port_;
    const int ch = cfg_.channels;
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
    if (!discovery_.start(bound_audio_port_, cfg_.studio_name, discovery_port)) {
        if (monitor_srv_) lv_monitor_server_stop(monitor_srv_);
        return false;
    }

    mdns_.start(cfg_.studio_name, bound_monitor_port(), {{"role", "audio"}, {"ver", "2.7.0"}});
    if (!matrix_.is_running()) matrix_.start();
    if (cfg_.network_engine_mode != 0) toggle_grid_l2(cfg_.network_engine_mode);

    services_running_.store(true, std::memory_order_release);
    return true;
}

void LanVidAudioService::stop_services() {
    services_running_.store(false, std::memory_order_release);
    stop_duplex_stream();
    if (grid_node_) {
        grid_node_.reset();
    }
    matrix_.stop();
    mdns_.stop();
    discovery_.stop();
    if (monitor_srv_) lv_monitor_server_stop(monitor_srv_);
}

bool LanVidAudioService::start_duplex_stream() {
    stop_duplex_stream();
    if (!duplex_) return false;
    LV_DuplexConfig dc{};
    lv_duplex_config_default(&dc);
    std::snprintf(dc.peer_host, sizeof(dc.peer_host), "%s", cfg_.peer_ip.c_str());
    dc.peer_port = uint16_t(std::clamp(cfg_.peer_port, 1024, 65535));
    dc.listen_port = bound_audio_port_;
    dc.discovery_port = bound_disc_port_;
    dc.latency_ms = uint32_t(std::clamp(cfg_.latency_ms, 2, 2000));
    dc.num_channels = std::clamp(cfg_.channels, 1, 64);
    dc.audio_codec = std::clamp(cfg_.audio_codec, 0, 8);
    dc.adaptive_jitter = cfg_.adaptive_jitter ? 1 : 0;
    std::snprintf(dc.device_name, sizeof(dc.device_name), "%s", cfg_.studio_name.c_str());
    if (lv_duplex_start(duplex_, &dc) != LV_OK) return false;

    duplex_running_.store(true, std::memory_order_release);
    apply_live_settings();
    return true;
}

void LanVidAudioService::stop_duplex_stream() {
    duplex_running_.store(false, std::memory_order_release);
    if (duplex_) lv_duplex_stop(duplex_);
}

void LanVidAudioService::apply_live_settings() {
    if (duplex_ && duplex_running_.load(std::memory_order_acquire)) {
        lv_duplex_set_latency_ms(duplex_, uint32_t(std::clamp(cfg_.latency_ms, 2, 2000)));
        lv_duplex_set_adaptive_jitter(duplex_, cfg_.adaptive_jitter ? 1 : 0);
        lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        lv_duplex_set_audio_bitrate(duplex_, cfg_.bitrate_kbps);
        lv_duplex_set_wifi_redundancy(duplex_, cfg_.wifi_redundancy ? 1 : 0, 1500);
        lv_duplex_set_midi_only(duplex_, cfg_.midi_only ? 1 : 0);
        lv_duplex_set_network_interfaces(duplex_, cfg_.primary_nic_ip.c_str(),
                                         cfg_.dual_nic_enabled ? cfg_.secondary_nic_ip.c_str() : "");
        lv_audio_set_tx_routing(duplex_, tx_routing.data());
        lv_audio_set_rx_routing(duplex_, rx_routing.data());
    }
    if (monitor_srv_) {
        lv_monitor_server_set_wifi_redundancy(monitor_srv_, cfg_.wifi_redundancy ? 1 : 0, 1500);
        lv_monitor_server_set_multichannel_flow(monitor_srv_, cfg_.monitor_flow_enabled ? 1 : 0, 16);
    }
}

void LanVidAudioService::set_channel_routing(bool is_tx, int src_ch, int dst_ch) {
    if (src_ch < 0 || src_ch >= 64) return;
    auto& arr = is_tx ? tx_routing : rx_routing;
    arr[size_t(src_ch)] = int8_t(dst_ch);
    if (duplex_ && duplex_running_.load(std::memory_order_acquire)) {
        if (is_tx) lv_audio_set_tx_routing(duplex_, arr.data());
        else lv_audio_set_rx_routing(duplex_, arr.data());
    }
}

void LanVidAudioService::reset_routing_identity() {
    for (int i = 0; i < 64; ++i) {
        tx_routing[size_t(i)] = int8_t(i);
        rx_routing[size_t(i)] = int8_t(i);
    }
    if (duplex_ && duplex_running_.load(std::memory_order_acquire)) {
        lv_audio_set_tx_routing(duplex_, nullptr);
        lv_audio_set_rx_routing(duplex_, nullptr);
    }
}

void LanVidAudioService::set_channel_mute_solo(bool is_tx, uint16_t ch, int mute, int solo) {
    if (ch >= 64) return;
    if (mute >= 0) ch_mute[ch] = (mute != 0);
    if (solo >= 0) ch_solo[ch] = (solo != 0);
    if (duplex_ && duplex_running_.load(std::memory_order_acquire)) {
        lv_audio_set_channel_mute_solo(duplex_, is_tx ? LV_AUDIO_DIR_TX : LV_AUDIO_DIR_RX, ch, mute, solo);
    }
}

void LanVidAudioService::send_test_midi_note(uint8_t note, bool on) {
    if (duplex_ && duplex_running_.load(std::memory_order_acquire)) {
        lv_duplex_send_midi(duplex_, on ? 0x90 : 0x80, note, on ? 100 : 0, 0, 0);
    }
}

void LanVidAudioService::toggle_grid_l2(int mode) {
    cfg_.network_engine_mode = mode;
    if (grid_node_) {
        grid_node_.reset();
    }
    if (mode == 0) {
        grid_status_ = "Classic L3 UDP";
        return;
    }
    cached_adapters_ = lv::grid::list_l2_adapters();
    if (cached_adapters_.empty()) {
        grid_status_ = "No Ethernet L2 adapter / BPF permission";
        return;
    }
    lv::grid::L2AdapterInfo chosen{};
    bool found = false;
    if (!cfg_.grid_nic_name.empty()) {
        for (const auto& ad : cached_adapters_) {
            if (ad.device_name == cfg_.grid_nic_name || ad.friendly_name == cfg_.grid_nic_name) {
                chosen = ad;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        found = lv::grid::pick_default_l2_adapter(cached_adapters_, chosen);
    }
    if (!found) chosen = cached_adapters_[0];

    std::string l2_err;
    auto transport = lv::grid::create_native_l2_transport(chosen, &l2_err);
    if (!transport || !transport->is_open()) {
        grid_status_ = "Grid L2 open failed: " + (l2_err.empty() ? chosen.device_name : l2_err);
        return;
    }

    lv::grid::GridNodeConfig gc{};
    gc.node_name = cfg_.studio_name;
    gc.role = lv::grid::GridNodeRole::kStudioHost;
    gc.max_channels = uint16_t(std::clamp(cfg_.grid_channels, 64, 256));
    gc.clock_master = cfg_.grid_clock_master;
    grid_node_ = std::make_unique<lv::grid::GridNode>(std::move(transport), gc);
    if (grid_node_->is_open()) {
        grid_node_->announce_self();
        grid_status_ = "Grid L2 ACTIVE (" + chosen.friendly_name + ")";
    } else {
        grid_status_ = "Grid L2 transport unavailable (" + chosen.device_name + ")";
        grid_node_.reset();
    }
}

void LanVidAudioService::scan_nodes() {
    LV_AudioNode buf[32]{};
    const int n = lv_discovery_scan_audio_nodes(250, bound_disc_port_, buf, 32);
    std::lock_guard<std::mutex> lk(scan_mu_);
    cached_nodes_.clear();
    for (int i = 0; i < n; ++i) cached_nodes_.push_back(buf[i]);
}

std::vector<LV_AudioNode> LanVidAudioService::discovered_nodes() const {
    std::lock_guard<std::mutex> lk(scan_mu_);
    return cached_nodes_;
}

std::vector<LV_MonitorClientInfo> LanVidAudioService::monitor_clients() const {
    std::vector<LV_MonitorClientInfo> out(8);
    if (!monitor_srv_) return {};
    const int n = lv_monitor_server_get_clients(monitor_srv_, out.data(), 8);
    out.resize(size_t(std::max(0, n)));
    return out;
}

std::vector<LvNetInterfaceInfo> LanVidAudioService::network_interfaces() const {
    std::vector<LvNetInterfaceInfo> out(32);
    const int n = lv_net_list_interfaces(out.data(), 32);
    out.resize(size_t(std::max(0, n)));
    return out;
}

LV_DuplexStats LanVidAudioService::duplex_stats() const {
    LV_DuplexStats st{};
    if (duplex_) lv_duplex_get_stats(duplex_, &st);
    return st;
}

std::vector<lv::SystemAudioDeviceEntry> LanVidAudioService::system_outputs() const {
    return lv::list_system_audio_devices(true, true);
}

std::vector<lv::SystemAudioDeviceEntry> LanVidAudioService::system_inputs() const {
    return lv::list_system_audio_devices(false, true);
}

std::vector<lv::grid::L2AdapterInfo> LanVidAudioService::grid_adapters() const {
    return cached_adapters_;
}

}  // namespace lv::apps
