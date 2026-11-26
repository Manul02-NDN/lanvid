#include "video_hub/hub_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace lv::hub {

HubDiscovery::~HubDiscovery() {
    stop();
}

bool HubDiscovery::start(const std::string& hub_name, uint16_t stream_port, uint16_t tally_port,
                         uint16_t discovery_port, const std::string& primary_nic_ip,
                         const std::string& secondary_nic_ip) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        hub_name_ = hub_name.empty() ? "LanVid Video Hub" : hub_name;
        stream_port_ = stream_port;
        tally_port_ = tally_port;
        disc_port_ = discovery_port;
        primary_ip_ = primary_nic_ip;
        secondary_ip_ = secondary_nic_ip;
    }

    responder_.set_role("video");
    responder_.set_network_interfaces(primary_nic_ip, secondary_nic_ip);
    if (!responder_.start(stream_port, hub_name_, discovery_port)) {
        return false;
    }

    mdns_.start(hub_name_, stream_port,
                {{"role", "video"}, {"tally", std::to_string(tally_port)}, {"ver", "2.7.0"}});

    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    scan_now_.store(true, std::memory_order_release);
    scan_thread_ = std::thread(&HubDiscovery::scan_loop, this);
    return true;
}

void HubDiscovery::stop() {
    running_.store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    responder_.stop();
    mdns_.stop();
}

void HubDiscovery::set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        primary_ip_ = primary_ip;
        secondary_ip_ = secondary_ip;
    }
    responder_.set_network_interfaces(primary_ip, secondary_ip);
    scan_now_.store(true, std::memory_order_release);
}

void HubDiscovery::load_assignments(const std::vector<std::string>& slot_uids,
                                    const std::vector<std::string>& slot_ips) {
    std::lock_guard<std::mutex> lk(mu_);
    uid_to_slot_.clear();
    ip_to_slot_.clear();
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        slot_uids_[i] = i < slot_uids.size() ? slot_uids[i] : "";
        slot_ips_[i] = i < slot_ips.size() ? slot_ips[i] : "";
        if (!slot_uids_[i].empty()) uid_to_slot_[slot_uids_[i]] = i;
        if (!slot_ips_[i].empty()) ip_to_slot_[slot_ips_[i]] = i;
    }
}

void HubDiscovery::export_assignments(std::vector<std::string>& out_uids, std::vector<std::string>& out_ips,
                                      uint32_t max_slots) const {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t n = std::min(max_slots, kMaxHubSlots);
    out_uids.resize(n);
    out_ips.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        out_uids[i] = slot_uids_[i];
        out_ips[i] = slot_ips_[i];
    }
}

void HubDiscovery::pin_slot(uint32_t slot, const std::string& device_uid, const std::string& ip) {
    if (slot >= kMaxHubSlots) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!slot_uids_[slot].empty()) uid_to_slot_.erase(slot_uids_[slot]);
    if (!slot_ips_[slot].empty()) ip_to_slot_.erase(slot_ips_[slot]);

    if (!device_uid.empty()) {
        auto it = uid_to_slot_.find(device_uid);
        if (it != uid_to_slot_.end() && it->second != slot) {
            slot_uids_[it->second].clear();
        }
        slot_uids_[slot] = device_uid;
        uid_to_slot_[device_uid] = slot;
    } else {
        slot_uids_[slot].clear();
    }

    if (!ip.empty()) {
        auto it = ip_to_slot_.find(ip);
        if (it != ip_to_slot_.end() && it->second != slot) {
            slot_ips_[it->second].clear();
        }
        slot_ips_[slot] = ip;
        ip_to_slot_[ip] = slot;
    } else {
        slot_ips_[slot].clear();
    }
}

void HubDiscovery::unpin_slot(uint32_t slot) {
    if (slot >= kMaxHubSlots) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!slot_uids_[slot].empty()) uid_to_slot_.erase(slot_uids_[slot]);
    if (!slot_ips_[slot].empty()) ip_to_slot_.erase(slot_ips_[slot]);
    slot_uids_[slot].clear();
    slot_ips_[slot].clear();
}

int HubDiscovery::assigned_slot_for(const std::string& device_uid, const std::string& ip,
                                    uint32_t max_slots) const {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t n = std::min(max_slots, kMaxHubSlots);
    if (!device_uid.empty()) {
        const auto it = uid_to_slot_.find(device_uid);
        if (it != uid_to_slot_.end() && it->second < n) return int(it->second);
    }
    if (!ip.empty()) {
        const auto it = ip_to_slot_.find(ip);
        if (it != ip_to_slot_.end() && it->second < n) return int(it->second);
    }
    return -1;
}

int HubDiscovery::resolve_or_assign_slot(const std::string& device_uid, const std::string& ip,
                                         uint32_t max_slots, bool allow_assign) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t n = std::min(max_slots, kMaxHubSlots);
    if (!device_uid.empty()) {
        const auto it = uid_to_slot_.find(device_uid);
        if (it != uid_to_slot_.end() && it->second < n) {
            if (!ip.empty()) {
                slot_ips_[it->second] = ip;
                ip_to_slot_[ip] = it->second;
            }
            return int(it->second);
        }
    }
    if (!ip.empty()) {
        const auto it = ip_to_slot_.find(ip);
        if (it != ip_to_slot_.end() && it->second < n) {
            if (!device_uid.empty() && slot_uids_[it->second].empty()) {
                slot_uids_[it->second] = device_uid;
                uid_to_slot_[device_uid] = it->second;
            }
            return int(it->second);
        }
    }
    if (!allow_assign) return -1;

    for (uint32_t i = 0; i < n; ++i) {
        if (slot_uids_[i].empty() && slot_ips_[i].empty()) {
            if (!device_uid.empty()) {
                slot_uids_[i] = device_uid;
                uid_to_slot_[device_uid] = i;
            }
            if (!ip.empty()) {
                slot_ips_[i] = ip;
                ip_to_slot_[ip] = i;
            }
            return int(i);
        }
    }
    return -1;
}

void HubDiscovery::trigger_scan() {
    scan_now_.store(true, std::memory_order_release);
}

void HubDiscovery::merge_tally_peers(const std::vector<DiscoveredPhonePeer>& tally_peers) {
    std::lock_guard<std::mutex> lk(mu_);
    cached_tally_peers_ = tally_peers;
}

std::vector<DiscoveredPhonePeer> HubDiscovery::discovered_peers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DiscoveredPhonePeer> merged = cached_tally_peers_;
    for (const auto& lp : cached_lan_peers_) {
        bool dup = false;
        for (auto& existing : merged) {
            if (!lp.ip.empty() && existing.ip == lp.ip) {
                if (existing.rtt_ms == 0) existing.rtt_ms = lp.rtt_ms;
                dup = true;
                break;
            }
        }
        if (!dup) merged.push_back(lp);
    }
    return merged;
}

std::string HubDiscovery::build_qr_uri(const std::string& host_ip, uint16_t video_port, uint16_t tally_port,
                                       const std::string& studio_name) {
    std::ostringstream os;
    os << "lanvid://connect?host=" << host_ip << "&port=" << video_port << "&tally=" << tally_port
       << "&name=";
    for (char ch : studio_name) {
        if (ch == ' ') os << "%20";
        else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                 ch == '-' || ch == '_' || ch == '.') {
            os << ch;
        }
    }
    return os.str();
}

void HubDiscovery::run_scan_once() {
    std::string p_ip, s_ip;
    uint16_t d_port = lv::kVideoDiscoveryPort;
    {
        std::lock_guard<std::mutex> lk(mu_);
        p_ip = primary_ip_;
        s_ip = secondary_ip_;
        d_port = disc_port_;
    }

    auto found = lv::discovery_scan_roles(220, true, true, true, p_ip, s_ip);
    if (d_port != lv::kVideoDiscoveryPort && d_port != lv::kDiscoveryPort) {
        auto custom = lv::discovery_scan(160, d_port, p_ip, s_ip);
        found.insert(found.end(), custom.begin(), custom.end());
    }

    std::vector<DiscoveredPhonePeer> out;
    out.reserve(found.size());
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : found) {
        if (r.stream_port == stream_port_ && (r.ip == "127.0.0.1" || r.ip == p_ip)) continue;
        DiscoveredPhonePeer peer{};
        peer.device_uid = !r.hostname.empty() ? (r.hostname + ":" + r.ip) : r.ip;
        peer.ip = r.ip;
        peer.stream_port = r.stream_port;
        peer.name = r.device_name.empty() ? r.ip : r.device_name;
        peer.os = r.os;
        peer.role = r.role;
        peer.rtt_ms = r.rtt_ms;
        peer.online = true;
        auto it_ip = ip_to_slot_.find(peer.ip);
        if (it_ip != ip_to_slot_.end()) peer.assigned_slot = int(it_ip->second);
        out.push_back(std::move(peer));
    }
    cached_lan_peers_ = std::move(out);
}

void HubDiscovery::scan_loop() {
    int ticks = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        if (scan_now_.exchange(false, std::memory_order_acq_rel) || ticks >= 25) {
            ticks = 0;
            run_scan_once();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++ticks;
    }
}

}  // namespace lv::hub
