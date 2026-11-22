#include "net/adaptive_quality.hpp"

#include <algorithm>
#include <cmath>

namespace lv {

AdaptiveQualityController::AdaptiveQualityController(const AdaptiveQualityConfig& cfg) { reset(cfg); }

void AdaptiveQualityController::reset(const AdaptiveQualityConfig& cfg) {
    cfg_ = cfg;
    cfg_.q_min = std::clamp(cfg_.q_min, 1, 100);
    cfg_.q_max = std::clamp(cfg_.q_max, cfg_.q_min, 100);
    q_ = std::clamp(cfg_.initial_q, cfg_.q_min, cfg_.q_max);
    has_fb_ = false;
    last_fb_ = {};
    fb_count_ = 0;
    decreases_ = 0;
    increases_ = 0;
    last_decrease_us_ = 0;
    clean_since_us_ = 0;
    last_increase_us_ = 0;
}

bool AdaptiveQualityController::on_feedback(const FeedbackPayload& fb, uint64_t now_us) {
    has_fb_ = true;
    last_fb_ = fb;
    ++fb_count_;
    if (!cfg_.enabled) return false;

    const bool congested = (fb.packet_loss_permille > cfg_.loss_threshold_permille) ||
                           (fb.fec_recovered_permille > cfg_.fec_threshold_permille);

    if (congested) {
        clean_since_us_ = 0;
        if (last_decrease_us_ == 0 || (now_us >= last_decrease_us_ &&
                                       (now_us - last_decrease_us_) >= cfg_.decrease_cooldown_us)) {
            int next_q = int(std::floor(double(q_) * cfg_.decrease_factor));
            if (next_q >= q_ && q_ > cfg_.q_min) next_q = q_ - 1;
            next_q = std::clamp(next_q, cfg_.q_min, cfg_.q_max);
            last_decrease_us_ = now_us;
            if (next_q != q_) {
                q_ = next_q;
                ++decreases_;
                return true;
            }
        }
        return false;
    }

    // Channel is clean (0% loss and low FEC activity): hold 500 ms then additive increase.
    if (fb.packet_loss_permille == 0 && fb.fec_recovered_permille <= cfg_.fec_threshold_permille) {
        if (clean_since_us_ == 0) {
            clean_since_us_ = now_us;
            last_increase_us_ = now_us;
            return false;
        }
        if (now_us >= clean_since_us_ && (now_us - clean_since_us_) >= cfg_.stable_hold_us &&
            now_us >= last_increase_us_ && (now_us - last_increase_us_) >= cfg_.increase_interval_us) {
            last_increase_us_ = now_us;
            const int next_q = std::min(cfg_.q_max, q_ + cfg_.increase_step);
            if (next_q != q_) {
                q_ = next_q;
                ++increases_;
                return true;
            }
        }
    } else {
        // Minor loss in (0, 0.5%]: hold current Q steady without ramping up.
        clean_since_us_ = 0;
    }
    return false;
}

}  // namespace lv
