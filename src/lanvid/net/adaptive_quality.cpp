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
    cfg_.bitrate_min_kbps = std::clamp<uint32_t>(cfg_.bitrate_min_kbps, 250u, 200000u);
    cfg_.bitrate_max_kbps = std::clamp<uint32_t>(cfg_.bitrate_max_kbps, cfg_.bitrate_min_kbps, 200000u);
    bitrate_kbps_ = std::clamp<uint32_t>(cfg_.initial_bitrate_kbps, cfg_.bitrate_min_kbps,
                                         cfg_.bitrate_max_kbps);
    has_fb_ = false;
    last_fb_ = {};
    fb_count_ = 0;
    decreases_ = 0;
    increases_ = 0;
    last_decrease_us_ = 0;
    clean_since_us_ = 0;
    last_increase_us_ = 0;
}

void AdaptiveQualityController::set_target_bitrate_kbps(uint32_t kbps) {
    bitrate_kbps_ = std::clamp<uint32_t>(kbps, cfg_.bitrate_min_kbps, cfg_.bitrate_max_kbps);
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

            uint32_t next_br = uint32_t(std::floor(double(bitrate_kbps_) * cfg_.decrease_factor));
            if (next_br >= bitrate_kbps_ && bitrate_kbps_ > cfg_.bitrate_min_kbps) {
                next_br = bitrate_kbps_ - std::min<uint32_t>(250u, bitrate_kbps_ - cfg_.bitrate_min_kbps);
            }
            next_br = std::clamp<uint32_t>(next_br, cfg_.bitrate_min_kbps, cfg_.bitrate_max_kbps);

            last_decrease_us_ = now_us;
            const bool changed = (next_q != q_) || (next_br != bitrate_kbps_);
            if (changed) {
                q_ = next_q;
                bitrate_kbps_ = next_br;
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
            const uint32_t next_br =
                std::min<uint32_t>(cfg_.bitrate_max_kbps, bitrate_kbps_ + cfg_.bitrate_increase_step_kbps);
            const bool changed = (next_q != q_) || (next_br != bitrate_kbps_);
            if (changed) {
                q_ = next_q;
                bitrate_kbps_ = next_br;
                ++increases_;
                return true;
            }
        }
    } else {
        // Minor loss in (0, 0.5%]: hold current Q/bitrate steady without ramping up.
        clean_since_us_ = 0;
    }
    return false;
}

}  // namespace lv
