#include "midi/virtual_midi.hpp"

namespace lv {

struct VirtualMidiPort::Impl {
    VirtualMidiStatus st{};
};

VirtualMidiPort::VirtualMidiPort() : impl_(std::make_unique<Impl>()) {}
VirtualMidiPort::~VirtualMidiPort() = default;

bool VirtualMidiPort::open(const std::string& daw_port_name, Callback) {
    impl_->st = VirtualMidiStatus{};
    impl_->st.daw_port_name = daw_port_name.empty() ? "LanVid MIDI" : daw_port_name;
    impl_->st.backend_name = "Unavailable";
    impl_->st.status_text = "Віртуальний MIDI-порт не підтримується на цій платформі";
    return false;
}

void VirtualMidiPort::close() {
    if (impl_) impl_->st = VirtualMidiStatus{};
}

bool VirtualMidiPort::is_open() const { return false; }

bool VirtualMidiPort::send_to_daw(uint8_t, uint8_t, uint8_t) { return false; }

void VirtualMidiPort::send_panic_to_daw() {}

VirtualMidiStatus VirtualMidiPort::status() const { return impl_ ? impl_->st : VirtualMidiStatus{}; }

}  // namespace lv
