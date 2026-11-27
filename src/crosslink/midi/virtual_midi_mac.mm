#include "midi/virtual_midi.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace cl {
namespace {

uint64_t now_us() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Stable deterministic UniqueIDs so DAWs (Logic, REAPER, Ableton) preserve track routing across restarts.
constexpr SInt32 kCrossLinkMidiSourceUniqueId = 0x4C564D01;       // 'LVM\1'
constexpr SInt32 kCrossLinkMidiDestinationUniqueId = 0x4C564D02;  // 'LVM\2'

void set_endpoint_properties(MIDIEndpointRef ep, CFStringRef name, SInt32 unique_id) {
    if (!ep) return;
    MIDIObjectSetStringProperty(ep, kMIDIPropertyName, name);
    MIDIObjectSetStringProperty(ep, kMIDIPropertyDisplayName, name);
    MIDIObjectSetStringProperty(ep, kMIDIPropertyManufacturer, CFSTR("CrossLink"));
    MIDIObjectSetStringProperty(ep, kMIDIPropertyModel, CFSTR("CrossLink Network MIDI"));
    MIDIObjectSetIntegerProperty(ep, kMIDIPropertyUniqueID, unique_id);
    MIDIObjectSetIntegerProperty(ep, kMIDIPropertyOffline, 0);
}

// Decodes MIDI 1.0 Channel Voice / System Common UMP words from a CoreMIDI MIDIEventList.
void dispatch_event_list(const MIDIEventList* evtlist, const VirtualMidiPort::Callback& cb) {
    if (!evtlist || !cb) return;
    const uint64_t ts = now_us();
    const MIDIEventPacket* pkt = &evtlist->packet[0];
    for (UInt32 p = 0; p < evtlist->numPackets; ++p) {
        UInt32 w = 0;
        while (w < pkt->wordCount) {
            const uint32_t word = pkt->words[w];
            const uint8_t mt = uint8_t((word >> 28) & 0x0Fu);
            if (mt == 0x2u) {
                // MIDI 1.0 Channel Voice message (32-bit UMP): [MT=2][Group][Status][Data1][Data2]
                const uint8_t status = uint8_t((word >> 16) & 0xFFu);
                const uint8_t d1 = uint8_t((word >> 8) & 0xFFu);
                const uint8_t d2 = uint8_t(word & 0xFFu);
                cb(status, d1, d2, ts);
                w += 1;
            } else if (mt == 0x1u) {
                // System Common / Real-Time (32-bit UMP)
                const uint8_t status = uint8_t((word >> 16) & 0xFFu);
                const uint8_t d1 = uint8_t((word >> 8) & 0xFFu);
                const uint8_t d2 = uint8_t(word & 0xFFu);
                cb(status, d1, d2, ts);
                w += 1;
            } else if (mt == 0x4u) {
                // MIDI 2.0 Channel Voice (64-bit UMP) -> downsample to MIDI 1.0 if received
                if (w + 1 < pkt->wordCount) {
                    const uint8_t status = uint8_t((word >> 16) & 0xFFu);
                    const uint8_t hi = status & 0xF0u;
                    const uint32_t word2 = pkt->words[w + 1];
                    if (hi == 0x90u || hi == 0x80u) {
                        const uint8_t note = uint8_t((word >> 8) & 0x7Fu);
                        uint8_t vel = uint8_t((word2 >> 25) & 0x7Fu);
                        if (hi == 0x90u && (word2 >> 16) != 0 && vel == 0) vel = 1;  // preserve quiet Note On
                        cb(status, note, vel, ts);
                    } else if (hi == 0xB0u) {
                        const uint8_t cc = uint8_t((word >> 8) & 0x7Fu);
                        const uint8_t val = uint8_t((word2 >> 25) & 0x7Fu);
                        cb(status, cc, val, ts);
                    } else if (hi == 0xC0u) {
                        const uint8_t prog = uint8_t((word2 >> 24) & 0x7Fu);
                        cb(status, prog, 0, ts);
                    } else if (hi == 0xE0u) {
                        const uint16_t bend14 = uint16_t((word2 >> 18) & 0x3FFFu);
                        cb(status, uint8_t(bend14 & 0x7Fu), uint8_t((bend14 >> 7) & 0x7Fu), ts);
                    }
                }
                w += 2;
            } else if (mt == 0x5u || mt == 0xDu || mt == 0xFu) {
                w += 4;  // 128-bit UMP (Data128 / Flex Data / UMP Stream)
            } else if (mt == 0x3u || mt == 0x8u || mt == 0x9u || mt == 0xAu) {
                w += 2;  // 64-bit UMP
            } else {
                w += 1;  // 32-bit UMP
            }
        }
        pkt = MIDIEventPacketNext(pkt);
    }
}

}  // namespace

struct VirtualMidiPort::Impl {
    struct CallbackState {
        std::mutex mu;
        std::atomic<bool> alive{false};
        Callback cb;
    };
    struct CoreMidiHandles {
        MIDIClientRef client = 0;
        MIDIEndpointRef virt_source = 0;
        MIDIEndpointRef virt_dest = 0;
    };

    mutable std::mutex mu;
    VirtualMidiStatus st{};
    std::shared_ptr<CallbackState> cb_state = std::make_shared<CallbackState>();
    MIDIClientRef client = 0;
    MIDIEndpointRef virt_source = 0;  // DAW MIDI Input ("CrossLink MIDI")
    MIDIEndpointRef virt_dest = 0;    // DAW MIDI Output ("CrossLink MIDI")

    CoreMidiHandles detach_locked() {
        CoreMidiHandles h{client, virt_source, virt_dest};
        client = 0;
        virt_source = 0;
        virt_dest = 0;
        st = VirtualMidiStatus{};
        return h;
    }

    static void dispose_handles(const CoreMidiHandles& h) {
        if (h.virt_source) MIDIEndpointDispose(h.virt_source);
        if (h.virt_dest) MIDIEndpointDispose(h.virt_dest);
        if (h.client) MIDIClientDispose(h.client);
    }
};

VirtualMidiPort::VirtualMidiPort() : impl_(std::make_unique<Impl>()) {}
VirtualMidiPort::~VirtualMidiPort() { close(); }

bool VirtualMidiPort::open(const std::string& daw_port_name, Callback on_from_daw) {
    close();
    const std::string name = daw_port_name.empty() ? "CrossLink MIDI" : daw_port_name;

    auto new_cb_state = std::make_shared<Impl::CallbackState>();
    new_cb_state->cb = std::move(on_from_daw);
    new_cb_state->alive.store(true, std::memory_order_release);

    CFStringRef cf_name = CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
    if (!cf_name) return false;

    Impl::CoreMidiHandles h{};
    OSStatus err = MIDIClientCreateWithBlock(CFSTR("CrossLink MIDI Client"), &h.client,
                                             ^(const MIDINotification*) {});
    if (err != noErr) {
        CFRelease(cf_name);
        Impl::dispose_handles(h);
        return false;
    }

    // Virtual Source: events sent by CrossLink via MIDIReceivedEventList appear as DAW MIDI Input.
    err = MIDISourceCreateWithProtocol(h.client, cf_name, kMIDIProtocol_1_0, &h.virt_source);
    if (err != noErr) {
        CFRelease(cf_name);
        Impl::dispose_handles(h);
        return false;
    }
    set_endpoint_properties(h.virt_source, cf_name, kCrossLinkMidiSourceUniqueId);

    // Virtual Destination: events sent by DAW to "CrossLink MIDI" arrive in our non-blocking callback.
    std::shared_ptr<Impl::CallbackState> cb_ref = new_cb_state;
    err = MIDIDestinationCreateWithProtocol(
        h.client, cf_name, kMIDIProtocol_1_0, &h.virt_dest,
        ^(const MIDIEventList* evtlist, void*) {
            if (!cb_ref || !cb_ref->alive.load(std::memory_order_acquire)) return;
            Callback fn;
            {
                std::lock_guard<std::mutex> clk(cb_ref->mu);
                if (!cb_ref->alive.load(std::memory_order_relaxed) || !cb_ref->cb) return;
                fn = cb_ref->cb;
            }
            dispatch_event_list(evtlist, fn);
        });
    if (err != noErr) {
        CFRelease(cf_name);
        new_cb_state->alive.store(false, std::memory_order_release);
        Impl::dispose_handles(h);
        return false;
    }
    set_endpoint_properties(h.virt_dest, cf_name, kCrossLinkMidiDestinationUniqueId);
    CFRelease(cf_name);

    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->cb_state = std::move(new_cb_state);
    impl_->client = h.client;
    impl_->virt_source = h.virt_source;
    impl_->virt_dest = h.virt_dest;
    impl_->st.open = true;
    impl_->st.backend = VirtualMidiBackend::kCoreMidi;
    impl_->st.daw_port_name = name;
    impl_->st.internal_port_name = name;
    impl_->st.backend_name = "CoreMIDI Virtual";
    impl_->st.status_text = name + " (Активний — оберіть у REAPER / Logic -> MIDI Devices)";
    return true;
}

void VirtualMidiPort::close() {
    if (!impl_) return;
    Impl::CoreMidiHandles h{};
    std::shared_ptr<Impl::CallbackState> old_cb;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        h = impl_->detach_locked();
        old_cb = impl_->cb_state;
    }
    if (old_cb) {
        std::lock_guard<std::mutex> clk(old_cb->mu);
        old_cb->alive.store(false, std::memory_order_release);
        old_cb->cb = nullptr;
    }
    Impl::dispose_handles(h);
}

bool VirtualMidiPort::is_open() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->st.open;
}

bool VirtualMidiPort::send_to_daw(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->st.open || !impl_->virt_source) return false;

    const uint32_t mt = (status >= 0xF0u) ? 0x1u : 0x2u;
    const uint32_t word = (mt << 28) | (uint32_t(status) << 16) | (uint32_t(d1) << 8) | uint32_t(d2);

    MIDIEventList list{};
    MIDIEventPacket* pkt = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    MIDIEventListAdd(&list, sizeof(list), pkt, 0, 1, &word);
    return MIDIReceivedEventList(impl_->virt_source, &list) == noErr;
}

void VirtualMidiPort::send_panic_to_daw() {
    for (uint8_t ch = 0; ch < 16; ++ch) {
        send_to_daw(uint8_t(0xB0u | ch), 123, 0);
        send_to_daw(uint8_t(0xB0u | ch), 120, 0);
    }
}

VirtualMidiStatus VirtualMidiPort::status() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->st;
}

}  // namespace cl
