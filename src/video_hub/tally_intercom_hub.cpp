#include "video_hub/tally_intercom_hub.hpp"

#include <algorithm>

namespace lv::hub {

StudioTallyIntercomHub::~StudioTallyIntercomHub() {
    stop();
}

bool StudioTallyIntercomHub::start(uint16_t bind_port, const std::string& bind_ip) {
    return engine_.start_studio(bind_port, bind_ip);
}

void StudioTallyIntercomHub::stop() {
    engine_.stop();
}

void StudioTallyIntercomHub::set_config_sync_callback(ConfigSyncCallback cb) {
    engine_.set_video_config_sync_callback(std::move(cb));
}

void StudioTallyIntercomHub::poll() {
    engine_.poll();
}

bool StudioTallyIntercomHub::set_camera_tally(uint32_t camera_id, uint8_t tally_state, const std::string& label,
                                              bool rec_trigger) {
    if (camera_id == 0) return false;
    return engine_.studio_set_camera_tally(camera_id, tally_state, label, rec_trigger);
}

bool StudioTallyIntercomHub::send_video_config_sync(uint32_t camera_id, const lv::VideoConfigSyncPayload& cfg) {
    if (camera_id == 0) return false;
    return engine_.send_video_config_sync(camera_id, cfg);
}

bool StudioTallyIntercomHub::request_keyframe(uint32_t camera_id) {
    if (camera_id == 0) return false;
    return engine_.studio_request_keyframe(camera_id);
}

void StudioTallyIntercomHub::set_director_mic_state(bool ptt_or_latch, bool listen_muted, float ifb_gain,
                                                    float sidetone_gain) {
    engine_.set_intercom_state(ptt_or_latch, listen_muted, ifb_gain, sidetone_gain);
}

void StudioTallyIntercomHub::set_camera_intercom(uint32_t camera_id, bool talk_to_cam, bool listen_from_cam,
                                                  bool all_call) {
    all_call_.store(all_call, std::memory_order_relaxed);
    engine_.studio_set_camera_intercom(camera_id, talk_to_cam, listen_from_cam, all_call);
}

void StudioTallyIntercomHub::set_all_call(bool enabled) {
    all_call_.store(enabled, std::memory_order_relaxed);
    engine_.studio_set_camera_intercom(0, enabled, true, enabled);
}

void StudioTallyIntercomHub::push_director_mic_pcm(const float* mono_48k, int frames) {
    engine_.push_mic_pcm(mono_48k, frames);
}

int StudioTallyIntercomHub::pull_director_rx_pcm(float* out_mono_48k, int frames) {
    return engine_.pull_rx_pcm(out_mono_48k, frames);
}

}  // namespace lv::hub
