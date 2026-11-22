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
    read_bool(j, "show_hud", cfg.show_hud);
    read_str(j, "preview_backend", cfg.preview_backend);
    read_int(j, "active_tab", cfg.active_tab);

    cfg.dest_port = std::clamp(cfg.dest_port, 1024, 65535);
    cfg.listen_port = std::clamp(cfg.listen_port, 1024, 65535);
    cfg.codec = std::clamp(cfg.codec, 0, 2);
    cfg.bitrate_mbps = std::clamp(cfg.bitrate_mbps, 1, 50);
    cfg.keyframe_interval = std::clamp(cfg.keyframe_interval, 15, 300);
    cfg.quality = std::clamp(cfg.quality, 1, 100);
    cfg.fps = std::clamp(cfg.fps, 5, 240);
    cfg.volume = std::clamp(cfg.volume, 0.0f, 1.5f);
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
        << "  \"show_hud\": " << (cfg.show_hud ? "true" : "false") << ",\n"
        << "  \"preview_backend\": \"" << json_esc(cfg.preview_backend) << "\",\n"
        << "  \"active_tab\": " << cfg.active_tab << "\n"
        << "}\n";
    return out.good();
}

}  // namespace lv::gui
