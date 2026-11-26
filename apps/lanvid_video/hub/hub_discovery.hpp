#pragma once
// HubDiscovery — Non-blocking LAN/mDNS scanner + persistent device_uid -> CAM N slot binder.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net/discovery.hpp"
#include "net/mdns_advertiser.hpp"
#include "hub/camera_slot.hpp"

namespace lv::hub {

struct DiscoveredPhonePeer {
    std::string device_uid;
    std::string ip;
    uint16_t stream_port = 5006;
    std::string name;
    std::string os;
    std::string role;
    uint32_t rtt_ms = 0;
    int assigned_slot = -1;  // 0..15 or -1 if unassigned
    bool online = true;
};

class HubDiscovery {
public:
    HubDiscovery() = default;
    ~HubDiscovery();

    HubDiscovery(const HubDiscovery&) = delete;
    HubDiscovery& operator=(const HubDiscovery&) = delete;

    bool start(const std::string& hub_name, uint16_t stream_port, uint16_t tally_port, uint16_t discovery_port,
               const std::string& primary_nic_ip = "", const std::string& secondary_nic_ip = "");
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    void set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip = "");

    // Persistent UID / IP -> slot binding (reconnecting phone returns to same CAM N).
    void load_assignments(const std::vector<std::string>& slot_uids, const std::vector<std::string>& slot_ips);
    void export_assignments(std::vector<std::string>& out_uids, std::vector<std::string>& out_ips,
                            uint32_t max_slots) const;

    void pin_slot(uint32_t slot, const std::string& device_uid, const std::string& ip);
    void unpin_slot(uint32_t slot);
    int resolve_or_assign_slot(const std::string& device_uid, const std::string& ip, uint32_t max_slots,
                               bool allow_assign = true);
    int assigned_slot_for(const std::string& device_uid, const std::string& ip, uint32_t max_slots) const;

    // Non-blocking UI snapshot + manual scan trigger
    void trigger_scan();
    std::vector<DiscoveredPhonePeer> discovered_peers() const;
    void merge_tally_peers(const std::vector<DiscoveredPhonePeer>& tally_peers);

    static std::string build_qr_uri(const std::string& host_ip, uint16_t video_port, uint16_t tally_port,
                                    const std::string& studio_name);

private:
    void scan_loop();
    void run_scan_once();

    lv::DiscoveryResponder responder_;
    lv::MdnsAdvertiser mdns_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> scan_now_{false};
    std::thread scan_thread_;

    mutable std::mutex mu_;
    std::string hub_name_ = "LanVid Video Hub";
    uint16_t stream_port_ = lv::kVideoStreamPort;
    uint16_t tally_port_ = lv::kVideoTallyPort;
    uint16_t disc_port_ = lv::kVideoDiscoveryPort;
    std::string primary_ip_;
    std::string secondary_ip_;

    std::array<std::string, kMaxHubSlots> slot_uids_{};
    std::array<std::string, kMaxHubSlots> slot_ips_{};
    std::unordered_map<std::string, uint32_t> uid_to_slot_;
    std::unordered_map<std::string, uint32_t> ip_to_slot_;
    std::vector<DiscoveredPhonePeer> cached_lan_peers_;
    std::vector<DiscoveredPhonePeer> cached_tally_peers_;
};

}  // namespace lv::hub
