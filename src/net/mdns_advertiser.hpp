#pragma once
// DNS-SD / mDNS service advertisement so LanVid Monitor (Android NsdManager / iOS NWBrowser) finds the
// studio in one tap: service type "_lanvid._udp", instance "LanVid Studio (<host>)", TXT records
// ("role=monitor", "ver=...", "ch=<n>").
//
//   macOS / iOS : Bonjour (dns_sd.h, DNSServiceRegister) - always available.
//   Windows     : DnsServiceRegister (windns.h, Windows 10 1809+) when the SDK declares it.
//   Linux / Android : not advertised (start() returns false) - phones connect by IP.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lv {

constexpr const char* kLanVidMdnsService = "_lanvid._udp";

class MdnsAdvertiser {
public:
    MdnsAdvertiser();
    ~MdnsAdvertiser();
    MdnsAdvertiser(const MdnsAdvertiser&) = delete;
    MdnsAdvertiser& operator=(const MdnsAdvertiser&) = delete;

    // Registers `instance_name` for `service_type` on `port`. Returns false if this platform has no
    // advertiser or registration failed immediately. Re-calling replaces the previous registration.
    bool start(const std::string& instance_name, uint16_t port,
               const std::vector<std::pair<std::string, std::string>>& txt = {},
               const std::string& service_type = kLanVidMdnsService);
    void stop();
    bool running() const;
    static bool supported();  // compile-time platform support

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lv
