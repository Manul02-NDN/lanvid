#pragma once
// AIMD (Additive Increase, Multiplicative Decrease) Adaptive Quality controller.
// Reacts to receiver FeedbackPayload telemetry (loss permille + FEC recovery permille).

#include <cstdint>

#include "net/packet.hpp"

namespace lv {

struct AdaptiveQualityConfig {
    bool enabled = false;
    int q_min = 35;
    int q_max = 85;
    int initial_q = 75;
    uint16_t loss_threshold_permille = 5;    // > 0.5% packet loss triggers multiplicative decrease
    uint16_t fec_threshold_permille = 50;    // > 5.0% FEC recovery also indicates channel stress
    double decrease_factor = 0.82;
    int increase_step = 2;
    uint64_t decrease_cooldown_us = 200000;  // 200 ms between drops
    uint64_t stable_hold_us = 500000;        // 500 ms clean before ramping up
    uint64_t increase_interval_us = 200000;  // +2 Q every 200 ms while clean
};

class AdaptiveQualityController {
public:
    explicit AdaptiveQualityController(const AdaptiveQualityConfig& cfg = {});

    void reset(const AdaptiveQualityConfig& cfg);

    // Feeds a telemetry feedback packet. Returns true if the active quality changed.
    bool on_feedback(const FeedbackPayload& fb, uint64_t now_us);

    bool enabled() const { return cfg_.enabled; }
    int current_quality() const { return q_; }
    bool has_feedback() const { return has_fb_; }
    FeedbackPayload last_feedback() const { return last_fb_; }
    uint64_t feedback_count() const { return fb_count_; }
    uint64_t decrease_count() const { return decreases_; }
    uint64_t increase_count() const { return increases_; }

private:
    AdaptiveQualityConfig cfg_;
    int q_ = 75;
    bool has_fb_ = false;
    FeedbackPayload last_fb_{};
    uint64_t fb_count_ = 0;
    uint64_t decreases_ = 0;
    uint64_t increases_ = 0;
    uint64_t last_decrease_us_ = 0;
    uint64_t clean_since_us_ = 0;
    uint64_t last_increase_us_ = 0;
};

}  // namespace lv
