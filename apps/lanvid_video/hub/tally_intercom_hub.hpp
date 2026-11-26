#pragma once
// StudioTallyIntercomHub — Centralized Tally (STBY/PVW/PGM + Remote REC), Duplex Director
// Intercom (PTT / Latch / ALL CALL / Auto-Ducking), and Bidirectional kVideoConfigSync router.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio/intercom_engine.hpp"
#include "net/packet.hpp"

namespace lv::hub {

class StudioTallyIntercomHub {
public:
    using ConfigSyncCallback = std::function<void(const lv::VideoConfigSyncPayload&)>;

    StudioTallyIntercomHub() = default;
    ~StudioTallyIntercomHub();

    StudioTallyIntercomHub(const StudioTallyIntercomHub&) = delete;
    StudioTallyIntercomHub& operator=(const StudioTallyIntercomHub&) = delete;

    bool start(uint16_t bind_port = lv::kDefaultTallyIntercomPort, const std::string& bind_ip = "0.0.0.0");
    void stop();
    bool is_running() const { return engine_.is_running(); }
    uint16_t bound_port() const { return engine_.bound_port(); }
    uint32_t origin_id() const { return engine_.origin_id(); }

    void set_config_sync_callback(ConfigSyncCallback cb);
    void poll();

    // ---- Tally & Remote Camera REC ----
    bool set_camera_tally(uint32_t camera_id, uint8_t tally_state, const std::string& label, bool rec_trigger);
    bool send_video_config_sync(uint32_t camera_id, const lv::VideoConfigSyncPayload& cfg);
    bool request_keyframe(uint32_t camera_id);

    // ---- Duplex Director Intercom (Director <-> Operators) ----
    void set_director_mic_state(bool ptt_or_latch, bool listen_muted, float ifb_gain, float sidetone_gain);
    void set_camera_intercom(uint32_t camera_id, bool talk_to_cam, bool listen_from_cam, bool all_call);
    void set_all_call(bool enabled);
    bool all_call() const { return all_call_.load(std::memory_order_relaxed); }

    void set_ducking_db(float db) { ducking_db_.store(db, std::memory_order_relaxed); }
    float ducking_db() const { return ducking_db_.load(std::memory_order_relaxed); }
    void set_audio_devices(int mic_index, int out_index) {
        mic_index_.store(mic_index, std::memory_order_relaxed);
        out_index_.store(out_index, std::memory_order_relaxed);
    }
    int mic_index() const { return mic_index_.load(std::memory_order_relaxed); }
    int out_index() const { return out_index_.load(std::memory_order_relaxed); }

    void push_director_mic_pcm(const float* mono_48k, int frames);
    int pull_director_rx_pcm(float* out_mono_48k, int frames);

    float tx_peak_dbfs() const { return engine_.tx_peak_dbfs(); }
    float rx_peak_dbfs() const { return engine_.rx_peak_dbfs(); }
    std::vector<lv::StudioCameraInfo> cameras() const { return engine_.studio_cameras(); }

    void set_wifi_redundancy(bool enabled, uint32_t stagger_us = 1500) {
        engine_.set_wifi_redundancy(enabled, stagger_us);
    }

private:
    lv::TallyIntercomEngine engine_;
    std::atomic<bool> all_call_{false};
    std::atomic<float> ducking_db_{-12.0f};
    std::atomic<int> mic_index_{-1};
    std::atomic<int> out_index_{-1};
};

}  // namespace lv::hub
