#include "gui/app_config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace lv::gui {

namespace {

std::string join_bar(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += '|';
        for (char c : v[i]) out += (c == '|') ? '/' : c;
    }
    return out;
}

std::string json_esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

bool find_json_raw(const std::string& text, const char* key, std::string& val) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
        ++pos;
    if (pos >= text.size()) return false;
    if (text[pos] == '"') {
        ++pos;
        std::string s;
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\' && pos + 1 < text.size()) {
                ++pos;
                if (text[pos] == 'n') s.push_back('\n');
                else if (text[pos] == 'r') s.push_back('\r');
                else s.push_back(text[pos]);
            } else {
                s.push_back(text[pos]);
            }
            ++pos;
        }
        val = s;
        return true;
    }
    size_t end = pos;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n' && text[end] != '\r')
        ++end;
    val = text.substr(pos, end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
    return !val.empty();
}

void read_int(const std::string& j, const char* k, int& dst) {
    std::string v;
    if (find_json_raw(j, k, v)) dst = std::atoi(v.c_str());
}
void read_dbl(const std::string& j, const char* k, double& dst) {
    std::string v;
    if (find_json_raw(j, k, v)) dst = std::atof(v.c_str());
}
void read_flt(const std::string& j, const char* k, float& dst) {
    std::string v;
    if (find_json_raw(j, k, v)) dst = float(std::atof(v.c_str()));
}
void read_bool(const std::string& j, const char* k, bool& dst) {
    std::string v;
    if (find_json_raw(j, k, v)) dst = (v == "true" || v == "1");
}
void read_str(const std::string& j, const char* k, std::string& dst) {
    std::string v;
    if (find_json_raw(j, k, v)) dst = v;
}

std::string sanitize_pipe(const std::string& s) {
    std::string out = s;
    for (char& c : out) if (c == '|' || c == ';' || c == '\n' || c == '\r') c = ' ';
    return out;
}

std::vector<std::string> split_char(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string serialize_matrix_slots(const std::vector<MatrixSlotConfig>& slots) {
    std::ostringstream os;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i) os << ";";
        const auto& s = slots[i];
        os << s.id << "|" << int(s.type) << "|" << int(s.role) << "|" << s.channels << "|"
           << s.buffer_size << "|" << (s.active ? 1 : 0) << "|" << s.device_index << "|"
           << sanitize_pipe(s.name) << "|" << sanitize_pipe(s.device_id);
    }
    return os.str();
}

void deserialize_matrix_slots(const std::string& raw, std::vector<MatrixSlotConfig>& out) {
    out.clear();
    if (raw.empty()) return;
    for (const auto& entry : split_char(raw, ';')) {
        if (entry.empty()) continue;
        const auto parts = split_char(entry, '|');
        if (parts.size() < 9) continue;
        MatrixSlotConfig s{};
        s.id = std::clamp(std::atoi(parts[0].c_str()), 0, kMatrixMaxSlots - 1);
        s.type = static_cast<MatrixSlotType>(std::clamp(std::atoi(parts[1].c_str()), 0, 3));
        s.role = static_cast<MatrixSlotRole>(std::clamp(std::atoi(parts[2].c_str()), 0, 2));
        s.channels = std::clamp(std::atoi(parts[3].c_str()), 1, kMatrixMaxChannels);
        s.buffer_size = std::clamp(std::atoi(parts[4].c_str()), 64, 1024);
        s.active = std::atoi(parts[5].c_str()) != 0;
        s.device_index = std::atoi(parts[6].c_str());
        s.name = parts[7];
        s.device_id = parts[8];
        out.push_back(std::move(s));
    }
}

std::string serialize_matrix_crosspoints(const std::vector<MatrixCrosspoint>& cps) {
    std::ostringstream os;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (i) os << ";";
        const auto& c = cps[i];
        os << c.src_slot << "|" << c.src_ch << "|" << c.dst_slot << "|" << c.dst_ch << "|"
           << c.gain_db << "|" << (c.mute ? 1 : 0);
    }
    return os.str();
}

void deserialize_matrix_crosspoints(const std::string& raw, std::vector<MatrixCrosspoint>& out) {
    out.clear();
    if (raw.empty()) return;
    for (const auto& entry : split_char(raw, ';')) {
        if (entry.empty()) continue;
        const auto parts = split_char(entry, '|');
        if (parts.size() < 6) continue;
        MatrixCrosspoint c{};
        c.src_slot = std::clamp(std::atoi(parts[0].c_str()), 0, kMatrixMaxSlots - 1);
        c.src_ch = std::clamp(std::atoi(parts[1].c_str()), 0, kMatrixMaxChannels - 1);
        c.dst_slot = std::clamp(std::atoi(parts[2].c_str()), 0, kMatrixMaxSlots - 1);
        c.dst_ch = std::clamp(std::atoi(parts[3].c_str()), 0, kMatrixMaxChannels - 1);
        c.gain_db = std::clamp(float(std::atof(parts[4].c_str())), -96.0f, 12.0f);
        c.mute = std::atoi(parts[5].c_str()) != 0;
        out.push_back(c);
    }
}

std::string serialize_midi_slots(const std::vector<MidiSlotConfig>& slots) {
    std::ostringstream os;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i) os << ";";
        const auto& s = slots[i];
        os << s.id << "|" << int(s.type) << "|" << int(s.port_index) << "|" << (s.active ? 1 : 0) << "|"
           << s.device_index << "|" << sanitize_pipe(s.name) << "|" << sanitize_pipe(s.device_name);
    }
    return os.str();
}

void deserialize_midi_slots(const std::string& raw, std::vector<MidiSlotConfig>& out) {
    out.clear();
    if (raw.empty()) return;
    for (const auto& entry : split_char(raw, ';')) {
        if (entry.empty()) continue;
        const auto parts = split_char(entry, '|');
        if (parts.size() < 7) continue;
        MidiSlotConfig s{};
        s.id = std::clamp(std::atoi(parts[0].c_str()), 0, kMatrixMaxMidiSlots - 1);
        s.type = static_cast<MidiSlotType>(std::clamp(std::atoi(parts[1].c_str()), 0, 4));
        s.port_index = uint8_t(std::clamp(std::atoi(parts[2].c_str()), 0, 15));
        s.active = std::atoi(parts[3].c_str()) != 0;
        s.device_index = std::atoi(parts[4].c_str());
        s.name = parts[5];
        s.device_name = parts[6];
        out.push_back(std::move(s));
    }
}

std::string serialize_midi_crosspoints(const std::vector<MidiCrosspoint>& cps) {
    std::ostringstream os;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (i) os << ";";
        const auto& c = cps[i];
        os << c.src_slot << "|" << c.dst_slot << "|" << c.channel_filter << "|" << (c.mute ? 1 : 0);
    }
    return os.str();
}

void deserialize_midi_crosspoints(const std::string& raw, std::vector<MidiCrosspoint>& out) {
    out.clear();
    if (raw.empty()) return;
    for (const auto& entry : split_char(raw, ';')) {
        if (entry.empty()) continue;
        const auto parts = split_char(entry, '|');
        if (parts.size() < 4) continue;
        MidiCrosspoint c{};
        c.src_slot = std::clamp(std::atoi(parts[0].c_str()), 0, kMatrixMaxMidiSlots - 1);
        c.dst_slot = std::clamp(std::atoi(parts[1].c_str()), 0, kMatrixMaxMidiSlots - 1);
        c.channel_filter = std::clamp(std::atoi(parts[2].c_str()), 0, 16);
        c.mute = std::atoi(parts[3].c_str()) != 0;
        out.push_back(c);
    }
}

}  // namespace

std::string config_file_path_for(const std::string& filename) {
    namespace fs = std::filesystem;
    const std::string name = filename.empty() ? "config.json" : filename;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    fs::path base = appdata && *appdata ? fs::path(appdata) : fs::current_path();
    return (base / "LanVid" / name).string();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) {
        base = fs::path(xdg);
    } else {
        const char* home = std::getenv("HOME");
        base = home && *home ? (fs::path(home) / ".config") : fs::current_path();
    }
    return (base / "lanvid" / name).string();
#endif
}

std::string config_file_path() {
    return config_file_path_for("config.json");
}

bool load_app_config(AppConfig& cfg) {
    return load_app_config(cfg, config_file_path());
}

bool load_app_config(AppConfig& cfg, const std::string& custom_path) {
    std::ifstream in(custom_path.empty() ? config_file_path() : custom_path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string j = ss.str();
    if (j.empty()) return false;

    read_str(j, "dest_ip", cfg.dest_ip);
    read_int(j, "dest_port", cfg.dest_port);
    read_int(j, "listen_port", cfg.listen_port);
    read_int(j, "source_type", cfg.source_type);
    read_int(j, "monitor_index", cfg.monitor_index);
    read_int(j, "capture_backend", cfg.capture_backend);
    read_int(j, "camera_index", cfg.camera_index);
    read_str(j, "file_path", cfg.file_path);
    read_int(j, "pattern_width", cfg.pattern_width);
    read_int(j, "pattern_height", cfg.pattern_height);
    read_int(j, "audio_mode", cfg.audio_mode);
    read_int(j, "audio_device_index", cfg.audio_device_index);
    read_dbl(j, "audio_chunk_ms", cfg.audio_chunk_ms);
    read_int(j, "preset", cfg.preset);
    read_int(j, "codec", cfg.codec);
    read_int(j, "bitrate_mbps", cfg.bitrate_mbps);
    read_int(j, "keyframe_interval", cfg.keyframe_interval);
    read_int(j, "quality", cfg.quality);
    read_int(j, "fps", cfg.fps);
    read_int(j, "fec_group", cfg.fec_group);
    read_bool(j, "adaptive_quality", cfg.adaptive_quality);
    read_int(j, "q_min", cfg.q_min);
    read_int(j, "q_max", cfg.q_max);
    read_flt(j, "volume", cfg.volume);
    read_int(j, "conceal_mode", cfg.conceal_mode);
    read_dbl(j, "audio_target_ms", cfg.audio_target_ms);
    read_bool(j, "audio_only", cfg.audio_only);
    read_bool(j, "midi_only", cfg.midi_only);
    read_int(j, "audio_codec", cfg.audio_codec);
    read_int(j, "audio_bitrate_kbps", cfg.audio_bitrate_kbps);
    read_int(j, "aoip_channels", cfg.aoip_channels);
    read_int(j, "aoip_latency_ms", cfg.aoip_latency_ms);
    {
        bool b = cfg.adaptive_jitter != 0;  // accepts 0/1 as well as true/false
        read_bool(j, "adaptive_jitter", b);
        cfg.adaptive_jitter = b ? 1 : 0;
    }
    read_bool(j, "aoip_tone", cfg.aoip_tone);
    read_int(j, "aoip_source", cfg.aoip_source);
    read_int(j, "aoip_output", cfg.aoip_output);
    read_str(j, "dpx_peer_host", cfg.dpx_peer_host);
    read_int(j, "dpx_peer_port", cfg.dpx_peer_port);
    read_int(j, "dpx_listen_port", cfg.dpx_listen_port);
    read_bool(j, "show_hud", cfg.show_hud);
    read_str(j, "preview_backend", cfg.preview_backend);
    read_int(j, "active_tab", cfg.active_tab);
    read_int(j, "connect_role", cfg.connect_role);
    read_bool(j, "dark_theme", cfg.dark_theme);
    read_bool(j, "matrix_stereo_pairs", cfg.matrix_stereo_pairs);
    read_bool(j, "sync_settings", cfg.sync_settings);
    read_bool(j, "sync_matrix_routing", cfg.sync_matrix_routing);
    read_bool(j, "wifi_redundancy", cfg.wifi_redundancy);
    read_str(j, "primary_nic_id", cfg.primary_nic_id);
    read_str(j, "primary_nic_ip", cfg.primary_nic_ip);
    read_str(j, "secondary_nic_id", cfg.secondary_nic_id);
    read_str(j, "secondary_nic_ip", cfg.secondary_nic_ip);
    read_bool(j, "dual_nic_redundancy_enabled", cfg.dual_nic_redundancy_enabled);
    read_int(j, "network_engine_mode", cfg.network_engine_mode);
    cfg.network_engine_mode = std::clamp(cfg.network_engine_mode, 0, 2);
    read_str(j, "grid_nic_name", cfg.grid_nic_name);
    read_bool(j, "grid_vlan_enabled", cfg.grid_vlan_enabled);
    read_int(j, "grid_vlan_id", cfg.grid_vlan_id);
    read_int(j, "grid_vlan_pcp", cfg.grid_vlan_pcp);
    read_int(j, "grid_channels", cfg.grid_channels);
    read_bool(j, "grid_clock_master", cfg.grid_clock_master);
    read_int(j, "matrix_buffer_size", cfg.matrix_buffer_size);
    read_bool(j, "matrix_feedback_guard", cfg.matrix_feedback_guard);
    {
        std::string raw_slots, raw_cps, raw_mslots, raw_mcps;
        read_str(j, "matrix_slots", raw_slots);
        read_str(j, "matrix_crosspoints", raw_cps);
        read_str(j, "midi_slots", raw_mslots);
        read_str(j, "midi_crosspoints", raw_mcps);
        deserialize_matrix_slots(raw_slots, cfg.matrix_slots);
        deserialize_matrix_crosspoints(raw_cps, cfg.matrix_crosspoints);
        deserialize_midi_slots(raw_mslots, cfg.midi_slots);
        deserialize_midi_crosspoints(raw_mcps, cfg.midi_crosspoints);
    }

    cfg.dest_port = std::clamp(cfg.dest_port, 1024, 65535);
    cfg.listen_port = std::clamp(cfg.listen_port, 1024, 65535);
    cfg.codec = std::clamp(cfg.codec, 0, 2);
    cfg.bitrate_mbps = std::clamp(cfg.bitrate_mbps, 1, 50);
    cfg.keyframe_interval = std::clamp(cfg.keyframe_interval, 15, 300);
    cfg.quality = std::clamp(cfg.quality, 1, 100);
    cfg.fps = std::clamp(cfg.fps, 5, 240);
    cfg.volume = std::clamp(cfg.volume, 0.0f, 1.5f);
    cfg.aoip_channels = std::clamp(cfg.aoip_channels, 1, 64);
    cfg.aoip_latency_ms = std::clamp(cfg.aoip_latency_ms, 2, 2000);
    cfg.adaptive_jitter = cfg.adaptive_jitter != 0 ? 1 : 0;
    cfg.audio_codec = std::clamp(cfg.audio_codec, 0, 8);
    if (cfg.audio_bitrate_kbps != 0 && cfg.audio_bitrate_kbps != 256 && cfg.audio_bitrate_kbps != 192 &&
        cfg.audio_bitrate_kbps != 128 && cfg.audio_bitrate_kbps != 96 && cfg.audio_bitrate_kbps != 64) {
        cfg.audio_bitrate_kbps = 0;
    }
    cfg.aoip_source = std::clamp(cfg.aoip_source, 0, 1);
    cfg.aoip_output = std::clamp(cfg.aoip_output, 0, 1);
    cfg.dpx_peer_port = std::clamp(cfg.dpx_peer_port, 1024, 65535);
    cfg.dpx_listen_port = std::clamp(cfg.dpx_listen_port, 1024, 65535);
    cfg.active_tab = std::clamp(cfg.active_tab, 0, 3);
    cfg.connect_role = std::clamp(cfg.connect_role, 0, 2);
    cfg.matrix_buffer_size = (cfg.matrix_buffer_size <= 128) ? 128 : (cfg.matrix_buffer_size <= 256 ? 256 : 512);
    read_int(j, "video_lip_sync_ms", cfg.video_lip_sync_ms);
    cfg.video_lip_sync_ms = std::clamp(cfg.video_lip_sync_ms, -200, 200);
    read_bool(j, "video_obs_output_enabled", cfg.video_obs_output_enabled);
    read_flt(j, "intercom_ducking_db", cfg.intercom_ducking_db);
    cfg.intercom_ducking_db = std::clamp(cfg.intercom_ducking_db, -36.0f, 0.0f);
    read_bool(j, "video_dscp_af41", cfg.video_dscp_af41);
    read_int(j, "video_max_slots", cfg.video_max_slots);
    cfg.video_max_slots = std::clamp(cfg.video_max_slots, 1, 16);
    read_int(j, "video_port_mode", cfg.video_port_mode);
    cfg.video_port_mode = std::clamp(cfg.video_port_mode, 0, 1);
    read_int(j, "video_multiview_layout", cfg.video_multiview_layout);
    cfg.video_multiview_layout = std::clamp(cfg.video_multiview_layout, 1, 3);
    {
        auto split = [](const std::string& s) {
            std::vector<std::string> out;
            if (s.empty()) return out;
            size_t a = 0;
            for (;;) {
                const size_t b = s.find('|', a);
                out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            return out;
        };
        std::string labels, ips;
        read_str(j, "video_slot_labels", labels);
        read_str(j, "video_slot_ips", ips);
        if (!labels.empty()) cfg.video_slot_labels = split(labels);
        if (!ips.empty()) cfg.video_slot_ips = split(ips);
    }
    read_str(j, "iso_output_dir", cfg.iso_output_dir);
    read_str(j, "iso_session_name", cfg.iso_session_name);
    read_int(j, "iso_container", cfg.iso_container);
    cfg.iso_container = std::clamp(cfg.iso_container, 0, 2);
    read_bool(j, "iso_record_program", cfg.iso_record_program);
    read_int(j, "iso_slot_mask", cfg.iso_slot_mask);
    read_int(j, "vcam_count", cfg.vcam_count);
    cfg.vcam_count = std::clamp(cfg.vcam_count, 0, 8);
    read_bool(j, "vcam_placeholder", cfg.vcam_placeholder);
    read_bool(j, "video_hw_decode", cfg.video_hw_decode);
    read_int(j, "video_audio_route", cfg.video_audio_route);
    cfg.video_audio_route = std::clamp(cfg.video_audio_route, 0, 2);
    read_int(j, "intercom_mic_index", cfg.intercom_mic_index);
    read_int(j, "intercom_out_index", cfg.intercom_out_index);
    read_flt(j, "intercom_ifb_volume", cfg.intercom_ifb_volume);
    cfg.intercom_ifb_volume = std::clamp(cfg.intercom_ifb_volume, 0.0f, 2.0f);
    read_bool(j, "tally_dscp_ef", cfg.tally_dscp_ef);
    read_str(j, "studio_name", cfg.studio_name);
    {
        std::string subtabs;
        read_str(j, "active_subtab", subtabs);
        if (!subtabs.empty()) {
            size_t a = 0;
            for (int i = 0; i < 4 && a <= subtabs.size(); ++i) {
                const size_t b = subtabs.find('|', a);
                const std::string tok = subtabs.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (!tok.empty()) cfg.active_subtab[i] = std::clamp(std::atoi(tok.c_str()), 0, 5);
                if (b == std::string::npos) break;
                a = b + 1;
            }
        }
    }
    read_int(j, "audio_sample_rate", cfg.audio_sample_rate);
    if (cfg.audio_sample_rate != 96000) cfg.audio_sample_rate = 48000;
    read_int(j, "audio_monitor_port", cfg.audio_monitor_port);
    read_int(j, "audio_discovery_port", cfg.audio_discovery_port);
    cfg.audio_monitor_port = std::clamp(cfg.audio_monitor_port, 1024, 65535);
    cfg.audio_discovery_port = std::clamp(cfg.audio_discovery_port, 1024, 65535);
    read_int(j, "grid_block_samples", cfg.grid_block_samples);
    if (cfg.grid_block_samples != 16 && cfg.grid_block_samples != 48) cfg.grid_block_samples = 32;
    read_str(j, "grid_nic_mac", cfg.grid_nic_mac);
    read_str(j, "grid_dsp_primary", cfg.grid_dsp_primary);
    read_str(j, "grid_dsp_backup", cfg.grid_dsp_backup);
    {
        auto split_bar = [](const std::string& s) {
            std::vector<std::string> out;
            size_t a = 0;
            while (a <= s.size()) {
                const size_t b = s.find('|', a);
                out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            return out;
        };
        std::string names, favs, mask;
        read_str(j, "monitor_channel_names", names);
        read_str(j, "favorite_peers", favs);
        read_str(j, "monitor_channel_mask", mask);
        if (!names.empty()) cfg.monitor_channel_names = split_bar(names);
        if (!favs.empty()) cfg.favorite_peers = split_bar(favs);
        if (!mask.empty()) cfg.monitor_channel_mask = std::strtoull(mask.c_str(), nullptr, 16);
    }
    read_bool(j, "autostart_session", cfg.autostart_session);
    read_bool(j, "monitor_dsp_hpf_enabled", cfg.monitor_dsp_hpf_enabled);
    read_flt(j, "monitor_dsp_hpf_hz", cfg.monitor_dsp_hpf_hz);
    cfg.monitor_dsp_hpf_hz = std::clamp(cfg.monitor_dsp_hpf_hz, 20.0f, 250.0f);
    read_bool(j, "monitor_dsp_eq_enabled", cfg.monitor_dsp_eq_enabled);
    read_flt(j, "monitor_dsp_eq_low_db", cfg.monitor_dsp_eq_low_db);
    read_flt(j, "monitor_dsp_eq_mid_hz", cfg.monitor_dsp_eq_mid_hz);
    read_flt(j, "monitor_dsp_eq_mid_db", cfg.monitor_dsp_eq_mid_db);
    read_flt(j, "monitor_dsp_eq_high_db", cfg.monitor_dsp_eq_high_db);
    read_bool(j, "monitor_dsp_comp_enabled", cfg.monitor_dsp_comp_enabled);
    read_flt(j, "monitor_dsp_comp_thresh_db", cfg.monitor_dsp_comp_thresh_db);
    read_flt(j, "monitor_dsp_comp_ratio", cfg.monitor_dsp_comp_ratio);
    read_flt(j, "monitor_dsp_comp_attack_ms", cfg.monitor_dsp_comp_attack_ms);
    read_flt(j, "monitor_dsp_comp_release_ms", cfg.monitor_dsp_comp_release_ms);
    read_flt(j, "monitor_dsp_comp_makeup_db", cfg.monitor_dsp_comp_makeup_db);
    read_flt(j, "monitor_dsp_master_db", cfg.monitor_dsp_master_db);
    read_bool(j, "monitor_dsp_master_mute", cfg.monitor_dsp_master_mute);
    return true;
}

bool save_app_config(const AppConfig& cfg) {
    return save_app_config(cfg, config_file_path());
}

bool save_app_config(const AppConfig& cfg, const std::string& custom_path) {
    namespace fs = std::filesystem;
    const fs::path p(custom_path.empty() ? config_file_path() : custom_path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n"
        << "  \"dest_ip\": \"" << json_esc(cfg.dest_ip) << "\",\n"
        << "  \"dest_port\": " << cfg.dest_port << ",\n"
        << "  \"listen_port\": " << cfg.listen_port << ",\n"
        << "  \"source_type\": " << cfg.source_type << ",\n"
        << "  \"monitor_index\": " << cfg.monitor_index << ",\n"
        << "  \"capture_backend\": " << cfg.capture_backend << ",\n"
        << "  \"camera_index\": " << cfg.camera_index << ",\n"
        << "  \"file_path\": \"" << json_esc(cfg.file_path) << "\",\n"
        << "  \"pattern_width\": " << cfg.pattern_width << ",\n"
        << "  \"pattern_height\": " << cfg.pattern_height << ",\n"
        << "  \"audio_mode\": " << cfg.audio_mode << ",\n"
        << "  \"audio_device_index\": " << cfg.audio_device_index << ",\n"
        << "  \"audio_chunk_ms\": " << cfg.audio_chunk_ms << ",\n"
        << "  \"preset\": " << cfg.preset << ",\n"
        << "  \"codec\": " << cfg.codec << ",\n"
        << "  \"bitrate_mbps\": " << cfg.bitrate_mbps << ",\n"
        << "  \"keyframe_interval\": " << cfg.keyframe_interval << ",\n"
        << "  \"quality\": " << cfg.quality << ",\n"
        << "  \"fps\": " << cfg.fps << ",\n"
        << "  \"fec_group\": " << cfg.fec_group << ",\n"
        << "  \"adaptive_quality\": " << (cfg.adaptive_quality ? "true" : "false") << ",\n"
        << "  \"q_min\": " << cfg.q_min << ",\n"
        << "  \"q_max\": " << cfg.q_max << ",\n"
        << "  \"volume\": " << cfg.volume << ",\n"
        << "  \"conceal_mode\": " << cfg.conceal_mode << ",\n"
        << "  \"audio_target_ms\": " << cfg.audio_target_ms << ",\n"
        << "  \"audio_only\": " << (cfg.audio_only ? "true" : "false") << ",\n"
        << "  \"midi_only\": " << (cfg.midi_only ? "true" : "false") << ",\n"
        << "  \"audio_codec\": " << cfg.audio_codec << ",\n"
        << "  \"audio_bitrate_kbps\": " << cfg.audio_bitrate_kbps << ",\n"
        << "  \"aoip_channels\": " << cfg.aoip_channels << ",\n"
        << "  \"aoip_latency_ms\": " << cfg.aoip_latency_ms << ",\n"
        << "  \"adaptive_jitter\": " << cfg.adaptive_jitter << ",\n"
        << "  \"aoip_tone\": " << (cfg.aoip_tone ? "true" : "false") << ",\n"
        << "  \"aoip_source\": " << cfg.aoip_source << ",\n"
        << "  \"aoip_output\": " << cfg.aoip_output << ",\n"
        << "  \"dpx_peer_host\": \"" << json_esc(cfg.dpx_peer_host) << "\",\n"
        << "  \"dpx_peer_port\": " << cfg.dpx_peer_port << ",\n"
        << "  \"dpx_listen_port\": " << cfg.dpx_listen_port << ",\n"
        << "  \"show_hud\": " << (cfg.show_hud ? "true" : "false") << ",\n"
        << "  \"preview_backend\": \"" << json_esc(cfg.preview_backend) << "\",\n"
        << "  \"active_tab\": " << cfg.active_tab << ",\n"
        << "  \"connect_role\": " << cfg.connect_role << ",\n"
        << "  \"dark_theme\": " << (cfg.dark_theme ? "true" : "false") << ",\n"
        << "  \"matrix_stereo_pairs\": " << (cfg.matrix_stereo_pairs ? "true" : "false") << ",\n"
        << "  \"sync_settings\": " << (cfg.sync_settings ? "true" : "false") << ",\n"
        << "  \"sync_matrix_routing\": " << (cfg.sync_matrix_routing ? "true" : "false") << ",\n"
        << "  \"wifi_redundancy\": " << (cfg.wifi_redundancy ? "true" : "false") << ",\n"
        << "  \"primary_nic_id\": \"" << json_esc(cfg.primary_nic_id) << "\",\n"
        << "  \"primary_nic_ip\": \"" << json_esc(cfg.primary_nic_ip) << "\",\n"
        << "  \"secondary_nic_id\": \"" << json_esc(cfg.secondary_nic_id) << "\",\n"
        << "  \"secondary_nic_ip\": \"" << json_esc(cfg.secondary_nic_ip) << "\",\n"
        << "  \"dual_nic_redundancy_enabled\": " << (cfg.dual_nic_redundancy_enabled ? "true" : "false") << ",\n"
        << "  \"network_engine_mode\": " << cfg.network_engine_mode << ",\n"
        << "  \"grid_nic_name\": \"" << json_esc(cfg.grid_nic_name) << "\",\n"
        << "  \"grid_vlan_enabled\": " << (cfg.grid_vlan_enabled ? "true" : "false") << ",\n"
        << "  \"grid_vlan_id\": " << cfg.grid_vlan_id << ",\n"
        << "  \"grid_vlan_pcp\": " << cfg.grid_vlan_pcp << ",\n"
        << "  \"grid_channels\": " << cfg.grid_channels << ",\n"
        << "  \"grid_clock_master\": " << (cfg.grid_clock_master ? "true" : "false") << ",\n"
        << "  \"matrix_buffer_size\": " << cfg.matrix_buffer_size << ",\n"
        << "  \"matrix_feedback_guard\": " << (cfg.matrix_feedback_guard ? "true" : "false") << ",\n"
        << "  \"matrix_slots\": \"" << json_esc(serialize_matrix_slots(cfg.matrix_slots)) << "\",\n"
        << "  \"matrix_crosspoints\": \"" << json_esc(serialize_matrix_crosspoints(cfg.matrix_crosspoints)) << "\",\n"
        << "  \"midi_slots\": \"" << json_esc(serialize_midi_slots(cfg.midi_slots)) << "\",\n"
        << "  \"midi_crosspoints\": \"" << json_esc(serialize_midi_crosspoints(cfg.midi_crosspoints)) << "\",\n"
        << "  \"video_lip_sync_ms\": " << cfg.video_lip_sync_ms << ",\n"
        << "  \"video_obs_output_enabled\": " << (cfg.video_obs_output_enabled ? "true" : "false") << ",\n"
        << "  \"intercom_ducking_db\": " << cfg.intercom_ducking_db << ",\n"
        << "  \"video_dscp_af41\": " << (cfg.video_dscp_af41 ? "true" : "false") << ",\n"
        << "  \"tally_dscp_ef\": " << (cfg.tally_dscp_ef ? "true" : "false") << ",\n"
        << "  \"video_max_slots\": " << cfg.video_max_slots << ",\n"
        << "  \"video_port_mode\": " << cfg.video_port_mode << ",\n"
        << "  \"video_multiview_layout\": " << cfg.video_multiview_layout << ",\n"
        << "  \"video_slot_labels\": \"" << json_esc(join_bar(cfg.video_slot_labels)) << "\",\n"
        << "  \"video_slot_ips\": \"" << json_esc(join_bar(cfg.video_slot_ips)) << "\",\n"
        << "  \"iso_output_dir\": \"" << json_esc(cfg.iso_output_dir) << "\",\n"
        << "  \"iso_session_name\": \"" << json_esc(cfg.iso_session_name) << "\",\n"
        << "  \"iso_container\": " << cfg.iso_container << ",\n"
        << "  \"iso_record_program\": " << (cfg.iso_record_program ? "true" : "false") << ",\n"
        << "  \"iso_slot_mask\": " << cfg.iso_slot_mask << ",\n"
        << "  \"vcam_count\": " << cfg.vcam_count << ",\n"
        << "  \"vcam_placeholder\": " << (cfg.vcam_placeholder ? "true" : "false") << ",\n"
        << "  \"video_hw_decode\": " << (cfg.video_hw_decode ? "true" : "false") << ",\n"
        << "  \"video_audio_route\": " << cfg.video_audio_route << ",\n"
        << "  \"intercom_mic_index\": " << cfg.intercom_mic_index << ",\n"
        << "  \"intercom_out_index\": " << cfg.intercom_out_index << ",\n"
        << "  \"intercom_ifb_volume\": " << cfg.intercom_ifb_volume << ",\n"
        << "  \"studio_name\": \"" << json_esc(cfg.studio_name) << "\",\n"
        << "  \"active_subtab\": \"" << cfg.active_subtab[0] << "|" << cfg.active_subtab[1] << "|"
        << cfg.active_subtab[2] << "|" << cfg.active_subtab[3] << "\",\n"
        << "  \"audio_sample_rate\": " << cfg.audio_sample_rate << ",\n"
        << "  \"audio_monitor_port\": " << cfg.audio_monitor_port << ",\n"
        << "  \"audio_discovery_port\": " << cfg.audio_discovery_port << ",\n"
        << "  \"grid_block_samples\": " << cfg.grid_block_samples << ",\n"
        << "  \"grid_nic_mac\": \"" << json_esc(cfg.grid_nic_mac) << "\",\n"
        << "  \"grid_dsp_primary\": \"" << json_esc(cfg.grid_dsp_primary) << "\",\n"
        << "  \"grid_dsp_backup\": \"" << json_esc(cfg.grid_dsp_backup) << "\",\n"
        << "  \"monitor_channel_names\": \"" << json_esc(join_bar(cfg.monitor_channel_names)) << "\",\n"
        << "  \"monitor_channel_mask\": \"" << std::hex << cfg.monitor_channel_mask << std::dec << "\",\n"
        << "  \"favorite_peers\": \"" << json_esc(join_bar(cfg.favorite_peers)) << "\",\n"
        << "  \"autostart_session\": " << (cfg.autostart_session ? "true" : "false") << ",\n"
        << "  \"monitor_dsp_hpf_enabled\": " << (cfg.monitor_dsp_hpf_enabled ? "true" : "false") << ",\n"
        << "  \"monitor_dsp_hpf_hz\": " << cfg.monitor_dsp_hpf_hz << ",\n"
        << "  \"monitor_dsp_eq_enabled\": " << (cfg.monitor_dsp_eq_enabled ? "true" : "false") << ",\n"
        << "  \"monitor_dsp_eq_low_db\": " << cfg.monitor_dsp_eq_low_db << ",\n"
        << "  \"monitor_dsp_eq_mid_hz\": " << cfg.monitor_dsp_eq_mid_hz << ",\n"
        << "  \"monitor_dsp_eq_mid_db\": " << cfg.monitor_dsp_eq_mid_db << ",\n"
        << "  \"monitor_dsp_eq_high_db\": " << cfg.monitor_dsp_eq_high_db << ",\n"
        << "  \"monitor_dsp_comp_enabled\": " << (cfg.monitor_dsp_comp_enabled ? "true" : "false") << ",\n"
        << "  \"monitor_dsp_comp_thresh_db\": " << cfg.monitor_dsp_comp_thresh_db << ",\n"
        << "  \"monitor_dsp_comp_ratio\": " << cfg.monitor_dsp_comp_ratio << ",\n"
        << "  \"monitor_dsp_comp_attack_ms\": " << cfg.monitor_dsp_comp_attack_ms << ",\n"
        << "  \"monitor_dsp_comp_release_ms\": " << cfg.monitor_dsp_comp_release_ms << ",\n"
        << "  \"monitor_dsp_comp_makeup_db\": " << cfg.monitor_dsp_comp_makeup_db << ",\n"
        << "  \"monitor_dsp_master_db\": " << cfg.monitor_dsp_master_db << ",\n"
        << "  \"monitor_dsp_master_mute\": " << (cfg.monitor_dsp_master_mute ? "true" : "false") << "\n"
        << "}\n";
    return out.good();
}

}  // namespace lv::gui
