#include "net/mdns_advertiser.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <arpa/inet.h>
#include <dns_sd.h>
#include <sys/select.h>
#define LV_MDNS_BONJOUR 1
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windns.h>
#if defined(DNS_QUERY_REQUEST_VERSION1) && defined(NTDDI_WIN10_RS5) && (NTDDI_VERSION >= NTDDI_WIN10_RS5)
#define LV_MDNS_WINDNS 1
#if defined(_MSC_VER)
#pragma comment(lib, "dnsapi.lib")
#endif
#endif
#endif

namespace lv {

#if defined(LV_MDNS_BONJOUR)
struct MdnsAdvertiser::Impl {
    DNSServiceRef ref = nullptr;
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<bool> ok{false};

    static void DNSSD_API on_reg(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType err, const char*, const char*,
                                 const char*, void* ctx) {
        static_cast<Impl*>(ctx)->ok.store(err == kDNSServiceErr_NoError, std::memory_order_release);
    }

    bool start(const std::string& name, uint16_t port, const std::vector<std::pair<std::string, std::string>>& txt,
               const std::string& type) {
        TXTRecordRef rec;
        TXTRecordCreate(&rec, 0, nullptr);
        for (const auto& kv : txt)
            TXTRecordSetValue(&rec, kv.first.c_str(), uint8_t(std::min<size_t>(kv.second.size(), 255)), kv.second.data());
        const DNSServiceErrorType e =
            DNSServiceRegister(&ref, 0, 0, name.c_str(), type.c_str(), nullptr, nullptr, htons(port),
                               TXTRecordGetLength(&rec), TXTRecordGetBytesPtr(&rec), &Impl::on_reg, this);
        TXTRecordDeallocate(&rec);
        if (e != kDNSServiceErr_NoError || !ref) {
            ref = nullptr;
            return false;
        }
        ok.store(true, std::memory_order_release);  // accepted by mDNSResponder; callback confirms / revokes
        stop.store(false);
        th = std::thread([this] {
            const int fd = DNSServiceRefSockFD(ref);
            while (!stop.load(std::memory_order_acquire)) {
                fd_set rs;
                FD_ZERO(&rs);
                FD_SET(fd, &rs);
                timeval tv{0, 200000};
                if (select(fd + 1, &rs, nullptr, nullptr, &tv) > 0 && FD_ISSET(fd, &rs)) {
                    if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) break;
                }
            }
        });
        return true;
    }
    void stop_all() {
        stop.store(true, std::memory_order_release);
        if (th.joinable()) th.join();
        if (ref) DNSServiceRefDeallocate(ref);  // withdraws the announcement (goodbye packet)
        ref = nullptr;
        ok.store(false);
    }
};
bool MdnsAdvertiser::supported() { return true; }

#elif defined(LV_MDNS_WINDNS)
struct MdnsAdvertiser::Impl {
    DNS_SERVICE_REGISTER_REQUEST req{};
    PDNS_SERVICE_INSTANCE inst = nullptr;
    std::atomic<bool> ok{false};
    std::wstring w_inst, w_host;

    static VOID WINAPI on_reg(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE instance) {
        static_cast<Impl*>(ctx)->ok.store(status == ERROR_SUCCESS, std::memory_order_release);
        if (instance) DnsServiceFreeInstance(instance);
    }
    static std::wstring widen(const std::string& s) {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
        return w;
    }

    bool start(const std::string& name, uint16_t port, const std::vector<std::pair<std::string, std::string>>& txt,
               const std::string& type) {
        wchar_t host[256] = {};
        DWORD hlen = 255;
        GetComputerNameW(host, &hlen);
        w_inst = widen(name) + L"." + widen(type) + L".local";
        w_host = std::wstring(host) + L".local";
        std::vector<std::wstring> keys, vals;
        for (const auto& kv : txt) {
            keys.push_back(widen(kv.first));
            vals.push_back(widen(kv.second));
        }
        std::vector<PCWSTR> kp, vp;
        for (size_t i = 0; i < keys.size(); ++i) {
            kp.push_back(keys[i].c_str());
            vp.push_back(vals[i].c_str());
        }
        inst = DnsServiceConstructInstance(w_inst.c_str(), w_host.c_str(), nullptr, nullptr, port, 0, 0,
                                           DWORD(kp.size()), kp.empty() ? nullptr : kp.data(),
                                           vp.empty() ? nullptr : vp.data());
        if (!inst) return false;
        req = {};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex = 0;
        req.pServiceInstance = inst;
        req.pRegisterCompletionCallback = &Impl::on_reg;
        req.pQueryContext = this;
        req.unicastEnabled = FALSE;
        const DWORD r = DnsServiceRegister(&req, nullptr);
        if (r != DNS_REQUEST_PENDING) {
            DnsServiceFreeInstance(inst);
            inst = nullptr;
            return false;
        }
        ok.store(true, std::memory_order_release);
        return true;
    }
    void stop_all() {
        if (inst) {
            DnsServiceDeRegister(&req, nullptr);
            DnsServiceFreeInstance(inst);
            inst = nullptr;
        }
        ok.store(false);
    }
};
bool MdnsAdvertiser::supported() { return true; }

#else
struct MdnsAdvertiser::Impl {
    bool start(const std::string&, uint16_t, const std::vector<std::pair<std::string, std::string>>&,
               const std::string&) {
        return false;
    }
    void stop_all() {}
    std::atomic<bool> ok{false};
};
bool MdnsAdvertiser::supported() { return false; }
#endif

MdnsAdvertiser::MdnsAdvertiser() : impl_(std::make_unique<Impl>()) {}
MdnsAdvertiser::~MdnsAdvertiser() { stop(); }

bool MdnsAdvertiser::start(const std::string& instance_name, uint16_t port,
                           const std::vector<std::pair<std::string, std::string>>& txt,
                           const std::string& service_type) {
    stop();
    return impl_->start(instance_name, port, txt, service_type);
}
void MdnsAdvertiser::stop() { impl_->stop_all(); }
bool MdnsAdvertiser::running() const { return impl_->ok.load(std::memory_order_acquire); }

}  // namespace lv
