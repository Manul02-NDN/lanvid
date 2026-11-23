#include "midi/midi_device_manager.hpp"

#include <cstring>

#include "common/clock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#endif

namespace lv {

#if defined(_WIN32)
namespace {
std::string wide_to_utf8_midi(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}
}  // namespace

std::vector<MidiDeviceInfo> list_midi_inputs() {
    std::vector<MidiDeviceInfo> out;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            out.push_back({int(i), wide_to_utf8_midi(caps.szPname), true, false});
        }
    }
    return out;
}

std::vector<MidiDeviceInfo> list_midi_outputs() {
    std::vector<MidiDeviceInfo> out;
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            out.push_back({int(i), wide_to_utf8_midi(caps.szPname), false, false});
        }
    }
    return out;
}

struct MidiInputPort::Impl {
    HMIDIIN handle = nullptr;
    Callback cb;

    static void CALLBACK midi_in_proc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto* self = reinterpret_cast<Impl*>(instance);
        if (!self || !self->cb) return;
        const uint64_t ts = now_us();  // high-resolution QPC timestamp at arrival
        const uint32_t raw = uint32_t(param1);
        const uint8_t st = uint8_t(raw & 0xFFu);
        const uint8_t d1 = uint8_t((raw >> 8) & 0xFFu);
        const uint8_t d2 = uint8_t((raw >> 16) & 0xFFu);
        self->cb(st, d1, d2, ts);
    }
};

MidiInputPort::MidiInputPort() : impl_(std::make_unique<Impl>()) {}
MidiInputPort::~MidiInputPort() { close(); }

bool MidiInputPort::open(int device_index, const std::string& device_name, Callback cb) {
    close();
    const auto devs = list_midi_inputs();
    int idx = device_index;
    if (!device_name.empty()) {
        for (const auto& d : devs) {
            if (d.name == device_name) { idx = d.index; break; }
        }
    }
    if (idx < 0 || idx >= int(devs.size())) return false;
    impl_->cb = std::move(cb);
    if (midiInOpen(&impl_->handle, UINT(idx), reinterpret_cast<DWORD_PTR>(&Impl::midi_in_proc),
                   reinterpret_cast<DWORD_PTR>(impl_.get()), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        impl_->handle = nullptr;
        return false;
    }
    if (midiInStart(impl_->handle) != MMSYSERR_NOERROR) {
        midiInClose(impl_->handle);
        impl_->handle = nullptr;
        return false;
    }
    return true;
}

void MidiInputPort::close() {
    if (impl_ && impl_->handle) {
        midiInStop(impl_->handle);
        midiInReset(impl_->handle);
        midiInClose(impl_->handle);
        impl_->handle = nullptr;
    }
}

bool MidiInputPort::is_open() const { return impl_ && impl_->handle != nullptr; }

struct MidiOutputPort::Impl {
    HMIDIOUT handle = nullptr;
};

MidiOutputPort::MidiOutputPort() : impl_(std::make_unique<Impl>()) {}
MidiOutputPort::~MidiOutputPort() { close(); }

bool MidiOutputPort::open(int device_index, const std::string& device_name) {
    close();
    const auto devs = list_midi_outputs();
    int idx = device_index;
    if (!device_name.empty()) {
        for (const auto& d : devs) {
            if (d.name == device_name) { idx = d.index; break; }
        }
    }
    if (idx < 0 || idx >= int(devs.size())) return false;
    if (midiOutOpen(&impl_->handle, UINT(idx), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        impl_->handle = nullptr;
        return false;
    }
    return true;
}

void MidiOutputPort::close() {
    if (impl_ && impl_->handle) {
        midiOutReset(impl_->handle);
        midiOutClose(impl_->handle);
        impl_->handle = nullptr;
    }
}

bool MidiOutputPort::is_open() const { return impl_ && impl_->handle != nullptr; }

bool MidiOutputPort::send_short(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!impl_ || !impl_->handle) return false;
    const DWORD msg = DWORD(status) | (DWORD(d1) << 8) | (DWORD(d2) << 16);
    return midiOutShortMsg(impl_->handle, msg) == MMSYSERR_NOERROR;
}

#elif defined(__APPLE__)
namespace {
std::string cf_to_utf8(CFStringRef s) {
    if (!s) return {};
    char buf[256] = {};
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return {};
}
std::string endpoint_name(MIDIEndpointRef ep) {
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) != noErr || !name) {
        MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &name);
    }
    std::string out = cf_to_utf8(name);
    if (name) CFRelease(name);
    return out;
}
}  // namespace

std::vector<MidiDeviceInfo> list_midi_inputs() {
    std::vector<MidiDeviceInfo> out;
    out.push_back({-1, "LanVid Virtual MIDI In", true, true});
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; ++i) {
        out.push_back({int(i), endpoint_name(MIDIGetSource(i)), true, false});
    }
    return out;
}

std::vector<MidiDeviceInfo> list_midi_outputs() {
    std::vector<MidiDeviceInfo> out;
    out.push_back({-1, "LanVid Virtual MIDI Out", false, true});
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        out.push_back({int(i), endpoint_name(MIDIGetDestination(i)), false, false});
    }
    return out;
}

struct MidiInputPort::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef in_port = 0;
    MIDIEndpointRef virt_dest = 0;
    MIDIEndpointRef connected_src = 0;
    Callback cb;

    static void read_proc(const MIDIPacketList* list, void* refcon, void*) {
        auto* self = static_cast<Impl*>(refcon);
        if (!self || !self->cb || !list) return;
        const uint64_t ts = now_us();
        const MIDIPacket* pkt = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            self->parse_packet(pkt->data, pkt->length, ts);
            pkt = MIDIPacketNext(pkt);
        }
    }

    // A MIDIPacket may hold SEVERAL messages (CoreMIDI coalesces events sent close together),
    // with running status, interleaved real-time bytes and SysEx. Deliver every short message.
    static int data_len(uint8_t st) {
        if (st < 0xF0) {
            const uint8_t hi = st & 0xF0;
            return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
        }
        if (st == 0xF1 || st == 0xF3) return 1;  // MTC quarter frame, song select
        if (st == 0xF2) return 2;                // song position
        return 0;                                // F6 tune request, F8..FF real-time
    }
    void parse_packet(const Byte* d, UInt16 len, uint64_t ts) {
        uint8_t running = 0;
        UInt16 i = 0;
        while (i < len) {
            const uint8_t b = d[i];
            if (b >= 0xF8) {  // real-time: single byte, may appear anywhere
                cb(b, 0, 0, ts);
                ++i;
                continue;
            }
            if (b == 0xF0) {  // SysEx: not a short message -> skip to EOX
                while (i < len && d[i] != 0xF7) ++i;
                ++i;
                running = 0;
                continue;
            }
            uint8_t st;
            if (b & 0x80) {
                st = b;
                running = (b < 0xF0) ? b : 0;
                ++i;
            } else if (running) {
                st = running;  // running status: data bytes follow directly
            } else {
                ++i;  // stray data byte
                continue;
            }
            const int n = data_len(st);
            if (i + n > len) break;
            const uint8_t d1 = n >= 1 ? d[i] : 0;
            const uint8_t d2 = n >= 2 ? d[i + 1] : 0;
            cb(st, d1, d2, ts);
            i = UInt16(i + n);
        }
    }
};

MidiInputPort::MidiInputPort() : impl_(std::make_unique<Impl>()) {}
MidiInputPort::~MidiInputPort() { close(); }

bool MidiInputPort::open(int device_index, const std::string& device_name, Callback cb) {
    close();
    impl_->cb = std::move(cb);
    if (MIDIClientCreate(CFSTR("LanVidMIDIIn"), nullptr, nullptr, &impl_->client) != noErr) return false;
    if (device_name == "LanVid Virtual MIDI In" || device_index < 0) {
        if (MIDIDestinationCreate(impl_->client, CFSTR("LanVid Virtual MIDI In"), &Impl::read_proc,
                                  impl_.get(), &impl_->virt_dest) != noErr) {
            close();
            return false;
        }
        return true;
    }
    if (MIDIInputPortCreate(impl_->client, CFSTR("LanVidInPort"), &Impl::read_proc, impl_.get(),
                            &impl_->in_port) != noErr) {
        close();
        return false;
    }
    const ItemCount n = MIDIGetNumberOfSources();
    int idx = device_index;
    if (!device_name.empty() &&
        (idx < 0 || ItemCount(idx) >= n || endpoint_name(MIDIGetSource(ItemCount(idx))) != device_name)) {
        for (ItemCount i = 0; i < n; ++i) {
            if (endpoint_name(MIDIGetSource(i)) == device_name) { idx = int(i); break; }
        }
    }
    if (idx < 0 || ItemCount(idx) >= n) { close(); return false; }
    impl_->connected_src = MIDIGetSource(ItemCount(idx));
    if (MIDIPortConnectSource(impl_->in_port, impl_->connected_src, nullptr) != noErr) { close(); return false; }
    return true;
}

void MidiInputPort::close() {
    if (!impl_) return;
    if (impl_->in_port && impl_->connected_src) MIDIPortDisconnectSource(impl_->in_port, impl_->connected_src);
    if (impl_->in_port) { MIDIPortDispose(impl_->in_port); impl_->in_port = 0; }
    if (impl_->virt_dest) { MIDIEndpointDispose(impl_->virt_dest); impl_->virt_dest = 0; }
    if (impl_->client) { MIDIClientDispose(impl_->client); impl_->client = 0; }
    impl_->connected_src = 0;
}

bool MidiInputPort::is_open() const { return impl_ && (impl_->in_port != 0 || impl_->virt_dest != 0); }

struct MidiOutputPort::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef out_port = 0;
    MIDIEndpointRef dest = 0;
    MIDIEndpointRef virt_src = 0;
};

MidiOutputPort::MidiOutputPort() : impl_(std::make_unique<Impl>()) {}
MidiOutputPort::~MidiOutputPort() { close(); }

bool MidiOutputPort::open(int device_index, const std::string& device_name) {
    close();
    if (MIDIClientCreate(CFSTR("LanVidMIDIOut"), nullptr, nullptr, &impl_->client) != noErr) return false;
    if (device_name == "LanVid Virtual MIDI Out" || device_index < 0) {
        if (MIDISourceCreate(impl_->client, CFSTR("LanVid Virtual MIDI Out"), &impl_->virt_src) != noErr) {
            close();
            return false;
        }
        return true;
    }
    if (MIDIOutputPortCreate(impl_->client, CFSTR("LanVidOutPort"), &impl_->out_port) != noErr) {
        close();
        return false;
    }
    const ItemCount n = MIDIGetNumberOfDestinations();
    int idx = device_index;
    if (!device_name.empty() &&
        (idx < 0 || ItemCount(idx) >= n || endpoint_name(MIDIGetDestination(ItemCount(idx))) != device_name)) {
        for (ItemCount i = 0; i < n; ++i) {
            if (endpoint_name(MIDIGetDestination(i)) == device_name) { idx = int(i); break; }
        }
    }
    if (idx < 0 || ItemCount(idx) >= n) { close(); return false; }
    impl_->dest = MIDIGetDestination(ItemCount(idx));
    return true;
}

void MidiOutputPort::close() {
    if (!impl_) return;
    if (impl_->out_port) { MIDIPortDispose(impl_->out_port); impl_->out_port = 0; }
    if (impl_->virt_src) { MIDIEndpointDispose(impl_->virt_src); impl_->virt_src = 0; }
    if (impl_->client) { MIDIClientDispose(impl_->client); impl_->client = 0; }
    impl_->dest = 0;
}

bool MidiOutputPort::is_open() const { return impl_ && (impl_->out_port != 0 || impl_->virt_src != 0); }

bool MidiOutputPort::send_short(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!is_open()) return false;
    Byte buf[64];
    auto* list = reinterpret_cast<MIDIPacketList*>(buf);
    MIDIPacket* pkt = MIDIPacketListInit(list);
    Byte data[3] = {status, d1, d2};
    const ByteCount len = ((status & 0xF0u) == 0xC0u || (status & 0xF0u) == 0xD0u) ? 2 : 3;
    MIDIPacketListAdd(list, sizeof(buf), pkt, 0, len, data);
    if (impl_->virt_src) return MIDIReceived(impl_->virt_src, list) == noErr;
    return MIDISend(impl_->out_port, impl_->dest, list) == noErr;
}

#else
std::vector<MidiDeviceInfo> list_midi_inputs() { return {}; }
std::vector<MidiDeviceInfo> list_midi_outputs() { return {}; }
struct MidiInputPort::Impl {};
MidiInputPort::MidiInputPort() : impl_(std::make_unique<Impl>()) {}
MidiInputPort::~MidiInputPort() = default;
bool MidiInputPort::open(int, const std::string&, Callback) { return false; }
void MidiInputPort::close() {}
bool MidiInputPort::is_open() const { return false; }
struct MidiOutputPort::Impl {};
MidiOutputPort::MidiOutputPort() : impl_(std::make_unique<Impl>()) {}
MidiOutputPort::~MidiOutputPort() = default;
bool MidiOutputPort::open(int, const std::string&) { return false; }
void MidiOutputPort::close() {}
bool MidiOutputPort::is_open() const { return false; }
bool MidiOutputPort::send_short(uint8_t, uint8_t, uint8_t) { return false; }
#endif

void MidiOutputPort::send_panic() {
    for (uint8_t ch = 0; ch < 16; ++ch) {
        send_short(uint8_t(0xB0u | ch), 123, 0);  // CC 123: All Notes Off
        send_short(uint8_t(0xB0u | ch), 120, 0);  // CC 120: All Sound Off
    }
}

}  // namespace lv
