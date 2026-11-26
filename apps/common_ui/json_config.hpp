#pragma once
// Pure C++20 JSON configuration helper (Zero GUI dependencies) used by all LanVid apps and tests.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lv::ui {

class JsonConfig {
public:
    void clear();
    bool empty() const { return raw_.empty(); }

    void set_string(const std::string& key, const std::string& val);
    void set_int(const std::string& key, int val);
    void set_u64(const std::string& key, uint64_t val);
    void set_float(const std::string& key, float val);
    void set_bool(const std::string& key, bool val);
    void set_string_list(const std::string& key, const std::vector<std::string>& list);
    void set_int_list(const std::string& key, const std::vector<int>& list);

    bool get_string(const std::string& key, std::string& out) const;
    bool get_int(const std::string& key, int& out) const;
    bool get_u64(const std::string& key, uint64_t& out) const;
    bool get_float(const std::string& key, float& out) const;
    bool get_bool(const std::string& key, bool& out) const;
    bool get_string_list(const std::string& key, std::vector<std::string>& out) const;
    bool get_int_list(const std::string& key, std::vector<int>& out) const;

    bool load(const std::string& path);
    bool save(const std::string& path) const;

private:
    std::map<std::string, std::string> raw_;  // stored as JSON literal values (quoted string or raw token)
};

// Returns platform config path (%APPDATA%\LanVid\<filename> or ~/.config/lanvid/<filename>).
std::string config_file_path_for(const std::string& filename);
std::string config_file_path();

}  // namespace lv::ui
