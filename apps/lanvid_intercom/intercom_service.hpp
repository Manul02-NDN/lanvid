#pragma once
// Headless Director Intercom & Tally desk service for LanVid Intercom.
// Default ports: Intercom 5024, Discovery 5025 (role = "intercom"), completely non-conflicting
// with LanVid Audio (5004/5020/5021) and LanVid Video (5006/5022/5023).

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio/multi_device_matrix.hpp"
#include "common_ui/json_config.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/discovery.hpp"
#include "net/mdns_advertiser.hpp"

namespace lv::apps {

constexpr uint16_t kIntercomDefaultPort = 5024;
constexpr uint16_t kIntercomDiscoveryPort = 5025;
constexpr uint32_t kMaxIntercomChannels = 16;

enum class PartylineGroup : int {
    kNone = 0,
    kGroupA = 1,
    kGroupB = 2,
};

struct IntercomOperatorSlot {
    uint32_t camera_id = 1;       // 1..16
    std::string label = "CAM 1";
    uint8_t tally_state = 0;      // 0=STBY, 1=PVW, 2=PGM
    bool talk_to_op = false;
    bool listen_from_op = true;
    PartylineGroup group = PartylineGroup::kGroupA;
    bool remote_rec = false;
    bool speaking = false;
    bool online = false;
};

struct IntercomAppConfig {
    std::string desk_name = "LanVid Intercom";
    int intercom_port = kIntercomDefaultPort;     // 5024
    int discovery_port = kIntercomDiscoveryPort;  // 5025
    int mic_index = -1;
    int headphones_index = -1;
    float ifb_volume = 1.0f;
    float sidetone_gain = 0.0f;
    float auto_ducking_db = -12.0f;
    bool listen_muted = false;
    bool dark_theme = false;
    int active_group_filter = 0;                  // 0=All, 1=Group A, 2=Group B
    std::vector<std::string> slot_labels;
    std::vector<int> slot_groups;
};

class LanVidIntercomService {
public:
    explicit LanVidIntercomService(std::string config_path_override = "");
    ~LanVidIntercomService();

    LanVidIntercomService(const LanVidIntercomService&) = delete;
    LanVidIntercomService& operator=(const LanVidIntercomService&) = delete;

    bool start_services(uint16_t intercom_port = kIntercomDefaultPort,
                        uint16_t discovery_port = kIntercomDiscoveryPort);
    void stop_services();
    bool services_running() const { return services_running_.load(std::memory_order_acquire); }
    void poll();

    // Director Talkback & Partyline routing
    void set_director_state(bool ptt_or_latch, bool listen_muted, float ifb_gain, float sidetone_gain);
    void set_all_call(bool enabled);
    bool all_call() const { return all_call_.load(std::memory_order_relaxed); }

    void set_partyline_talk(PartylineGroup group, bool enabled);
    bool partyline_talk(PartylineGroup group) const;

    void set_operator_talk(uint32_t slot, bool talk, bool listen);
    void set_operator_group(uint32_t slot, PartylineGroup group);
    void set_operator_tally(uint32_t slot, uint8_t tally_state, bool remote_rec);

    // Pure C ABI 48 kHz mono float32 voice I/O
    int push_director_mic_pcm(const float* samples, int frames);
    int pull_director_rx_pcm(float* out_samples, int frames);
    bool is_director_speaking() const;
    float ducking_gain() const;

    uint16_t bound_intercom_port() const { return handle_ ? lv_tally_intercom_port(handle_) : 0; }
    uint16_t bound_discovery_port() const { return bound_disc_port_; }
    std::string config_path() const { return config_path_; }

    std::array<IntercomOperatorSlot, kMaxIntercomChannels> slots() const;
    std::vector<LvNetInterfaceInfo> network_interfaces() const;
    std::vector<lv::SystemAudioDeviceEntry> microphones() const;
    std::vector<lv::SystemAudioDeviceEntry> headphones() const;

    IntercomAppConfig& config() { return cfg_; }
    const IntercomAppConfig& config() const { return cfg_; }
    bool load_config();
    bool save_config() const;

private:
    static void on_config_sync(const LV_VideoConfigSync* cfg, void* user_data);

    std::string config_path_;
    IntercomAppConfig cfg_{};
    LvTallyIntercomHandle* handle_ = nullptr;
    lv::DiscoveryResponder discovery_;
    lv::MdnsAdvertiser mdns_;

    std::atomic<bool> services_running_{false};
    std::atomic<bool> all_call_{false};
    std::atomic<bool> talk_group_a_{false};
    std::atomic<bool> talk_group_b_{false};
    bool director_talk_ = false;
    uint16_t bound_disc_port_ = kIntercomDiscoveryPort;

    mutable std::mutex slots_mu_;
    std::array<IntercomOperatorSlot, kMaxIntercomChannels> slots_{};
};

}  // namespace lv::apps
