#include "common_ui/json_config.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace lv::ui {

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20) out.push_back(char(c));
    }
    return out;
}

std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char nx = s[++i];
            if (nx == 'n') out.push_back('\n');
            else if (nx == 'r') out.push_back('\r');
            else if (nx == 't') out.push_back('\t');
            else out.push_back(nx);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string quote_str(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

bool unquote_str(const std::string& token, std::string& out) {
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        out = json_unescape(token.substr(1, token.size() - 2));
        return true;
    }
    return false;
}

std::string encode_pipe_list(const std::vector<std::string>& list) {
    std::string s;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) s.push_back('|');
        for (char c : list[i]) {
            if (c == '%' || c == '|') {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "%%%02X", unsigned(static_cast<unsigned char>(c)));
                s += hex;
            } else {
                s.push_back(c);
            }
        }
    }
    return s;
}

std::vector<std::string> decode_pipe_list(const std::string& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;
    size_t a = 0;
    for (;;) {
        const size_t b = raw.find('|', a);
        const std::string tok = raw.substr(a, b == std::string::npos ? std::string::npos : b - a);
        std::string un;
        un.reserve(tok.size());
        for (size_t i = 0; i < tok.size(); ++i) {
            if (tok[i] == '%' && i + 2 < tok.size() && std::isxdigit(static_cast<unsigned char>(tok[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(tok[i + 2]))) {
                const int v = std::stoi(tok.substr(i + 1, 2), nullptr, 16);
                un.push_back(char(v));
                i += 2;
            } else {
                un.push_back(tok[i]);
            }
        }
        out.push_back(std::move(un));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

}  // namespace

void JsonConfig::clear() {
    raw_.clear();
}

void JsonConfig::set_string(const std::string& key, const std::string& val) {
    raw_[key] = quote_str(val);
}

void JsonConfig::set_int(const std::string& key, int val) {
    raw_[key] = std::to_string(val);
}

void JsonConfig::set_u64(const std::string& key, uint64_t val) {
    raw_[key] = std::to_string(val);
}

void JsonConfig::set_float(const std::string& key, float val) {
    std::ostringstream os;
    os << std::setprecision(7) << val;
    raw_[key] = os.str();
}

void JsonConfig::set_bool(const std::string& key, bool val) {
    raw_[key] = val ? "true" : "false";
}

void JsonConfig::set_string_list(const std::string& key, const std::vector<std::string>& list) {
    set_string(key, encode_pipe_list(list));
}

void JsonConfig::set_int_list(const std::string& key, const std::vector<int>& list) {
    std::string s;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) s.push_back('|');
        s += std::to_string(list[i]);
    }
    set_string(key, s);
}

bool JsonConfig::get_string(const std::string& key, std::string& out) const {
    const auto it = raw_.find(key);
    if (it == raw_.end()) return false;
    return unquote_str(it->second, out);
}

bool JsonConfig::get_int(const std::string& key, int& out) const {
    const auto it = raw_.find(key);
    if (it == raw_.end() || it->second.empty()) return false;
    std::string tmp;
    if (unquote_str(it->second, tmp)) {
        out = std::atoi(tmp.c_str());
        return true;
    }
    out = std::atoi(it->second.c_str());
    return true;
}

bool JsonConfig::get_u64(const std::string& key, uint64_t& out) const {
    const auto it = raw_.find(key);
    if (it == raw_.end() || it->second.empty()) return false;
    std::string tmp;
    const char* s = unquote_str(it->second, tmp) ? tmp.c_str() : it->second.c_str();
    out = std::strtoull(s, nullptr, 10);
    return true;
}

bool JsonConfig::get_float(const std::string& key, float& out) const {
    const auto it = raw_.find(key);
    if (it == raw_.end() || it->second.empty()) return false;
    out = std::strtof(it->second.c_str(), nullptr);
    return true;
}

bool JsonConfig::get_bool(const std::string& key, bool& out) const {
    const auto it = raw_.find(key);
    if (it == raw_.end()) return false;
    if (it->second == "true" || it->second == "1") {
        out = true;
        return true;
    }
    if (it->second == "false" || it->second == "0") {
        out = false;
        return true;
    }
    return false;
}

bool JsonConfig::get_string_list(const std::string& key, std::vector<std::string>& out) const {
    std::string raw;
    if (!get_string(key, raw)) return false;
    out = decode_pipe_list(raw);
    return true;
}

bool JsonConfig::get_int_list(const std::string& key, std::vector<int>& out) const {
    std::string raw;
    if (!get_string(key, raw)) return false;
    out.clear();
    if (raw.empty()) return true;
    size_t a = 0;
    for (;;) {
        const size_t b = raw.find('|', a);
        const std::string tok = raw.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!tok.empty()) out.push_back(std::atoi(tok.c_str()));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return true;
}

bool JsonConfig::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    if (src.empty()) return false;

    raw_.clear();
    size_t i = 0;
    const size_t n = src.size();
    auto skip_ws = [&]() {
        while (i < n && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
    };
    auto parse_quoted = [&](std::string& lit) -> bool {
        if (i >= n || src[i] != '"') return false;
        const size_t start = i++;
        while (i < n) {
            if (src[i] == '\\' && i + 1 < n) {
                i += 2;
            } else if (src[i] == '"') {
                ++i;
                lit = src.substr(start, i - start);
                return true;
            } else {
                ++i;
            }
        }
        return false;
    };

    skip_ws();
    if (i >= n || src[i] != '{') return false;
    ++i;
    while (i < n) {
        skip_ws();
        if (i < n && src[i] == '}') break;
        std::string key_lit;
        if (!parse_quoted(key_lit)) break;
        std::string key;
        unquote_str(key_lit, key);
        skip_ws();
        if (i >= n || src[i] != ':') break;
        ++i;
        skip_ws();
        std::string val_lit;
        if (i < n && src[i] == '"') {
            if (!parse_quoted(val_lit)) break;
        } else {
            const size_t start = i;
            while (i < n && src[i] != ',' && src[i] != '}' && !std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            val_lit = src.substr(start, i - start);
        }
        if (!key.empty()) raw_[key] = val_lit;
        skip_ws();
        if (i < n && src[i] == ',') ++i;
    }
    return true;
}

bool JsonConfig::save(const std::string& path) const {
    const fs::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n";
    size_t idx = 0;
    for (const auto& [k, v] : raw_) {
        out << "  \"" << json_escape(k) << "\": " << v;
        if (++idx < raw_.size()) out << ",";
        out << "\n";
    }
    out << "}\n";
    return out.good();
}

std::string config_file_path_for(const std::string& filename) {
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

}  // namespace lv::ui
