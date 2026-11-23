#include "gui/app_config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace lv::gui {

namespace {

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

std::string config_file_path() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    fs::path base = appdata && *appdata ? fs::path(appdata) : fs::current_path();
    return (base / "LanVid" / "config.json").string();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) {
        base = fs::path(xdg);
    } else {
        const char* home = std::getenv("HOME");
        base = home && *home ? (fs::path(home) / ".config") : fs::current_path();
    }
    return (base / "lanvid" / "config.json").string();
#endif
}

bool load_app_config(AppConfig& cfg) {
    std::ifstream in(config_file_path(), std::ios::binary);
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
    return true;
}

bool save_app_config(const AppConfig& cfg) {
    namespace fs = std::filesystem;
    const fs::path p(config_file_path());
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
        << "  \"matrix_buffer_size\": " << cfg.matrix_buffer_size << ",\n"
        << "  \"matrix_feedback_guard\": " << (cfg.matrix_feedback_guard ? "true" : "false") << ",\n"
        << "  \"matrix_slots\": \"" << json_esc(serialize_matrix_slots(cfg.matrix_slots)) << "\",\n"
        << "  \"matrix_crosspoints\": \"" << json_esc(serialize_matrix_crosspoints(cfg.matrix_crosspoints)) << "\",\n"
        << "  \"midi_slots\": \"" << json_esc(serialize_midi_slots(cfg.midi_slots)) << "\",\n"
        << "  \"midi_crosspoints\": \"" << json_esc(serialize_midi_crosspoints(cfg.midi_crosspoints)) << "\"\n"
        << "}\n";
    return out.good();
}

}  // namespace lv::gui
