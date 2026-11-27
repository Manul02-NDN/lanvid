#include "net/assembler.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "codec/dct.hpp"
#include "common/clock.hpp"

namespace lv {

// ---------------------------------------------------------------- SeqLossTracker

bool SeqLossTracker::get_bit(uint16_t seq) const {
    const uint32_t idx = seq & (kWindow - 1);
    return (bitmap_[idx >> 6] & (uint64_t(1) << (idx & 63))) != 0;
}

void SeqLossTracker::set_bit(uint16_t seq, bool v) {
    const uint32_t idx = seq & (kWindow - 1);
    const uint64_t mask = uint64_t(1) << (idx & 63);
    if (v) bitmap_[idx >> 6] |= mask;
    else bitmap_[idx >> 6] &= ~mask;
}

void SeqLossTracker::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    initialized_ = false;
    max_seq_ = 0;
    span_ = 0;
    received_ = 0;
    duplicates_ = 0;
    reordered_ = 0;
    evicted_lost_ = 0;
    std::memset(bitmap_, 0, sizeof(bitmap_));
}

void SeqLossTracker::observe(uint16_t seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!initialized_) {
        initialized_ = true;
        max_seq_ = seq;
        span_ = 1;
        set_bit(seq, true);
        ++received_;
        return;
    }

    const int16_t d = seq_diff(seq, max_seq_);
    if (d > 0) {
        if (d >= kWindow) {
            const uint64_t tracked = std::min<uint64_t>(span_, kWindow);
            for (uint64_t i = 0; i < tracked; ++i) {
                const uint16_t s = uint16_t(max_seq_ - i);
                if (!get_bit(s)) ++evicted_lost_;
            }
            evicted_lost_ += uint64_t(d - kWindow);
            std::memset(bitmap_, 0, sizeof(bitmap_));
            max_seq_ = seq;
            span_ = std::min<uint64_t>(span_ + uint64_t(d), kWindow);
            set_bit(seq, true);
        } else {
            for (int step = 1; step <= d; ++step) {
                const uint16_t new_s = uint16_t(max_seq_ + step);
                if (span_ >= uint64_t(kWindow)) {
                    if (!get_bit(new_s)) ++evicted_lost_;
                } else {
                    ++span_;
                }
                set_bit(new_s, false);
            }
            max_seq_ = seq;
            set_bit(seq, true);
        }
        ++received_;
    } else if (-d < kWindow) {
        if (get_bit(seq)) {
            ++duplicates_;
        } else {
            set_bit(seq, true);
            ++received_;
            if (d < 0) ++reordered_;
        }
    } else {
        ++duplicates_;
    }
}

uint64_t SeqLossTracker::lost_locked(int reorder_margin) const {
    if (!initialized_) return 0;
    const uint64_t tracked = std::min<uint64_t>(span_, kWindow);
    const uint64_t start = uint64_t(std::max(0, reorder_margin));
    uint64_t in_win_lost = 0;
    for (uint64_t i = start; i < tracked; ++i) {
        const uint16_t s = uint16_t(max_seq_ - i);
        if (!get_bit(s)) ++in_win_lost;
    }
    return evicted_lost_ + in_win_lost;
}

uint64_t SeqLossTracker::lost(int reorder_margin) const {
    std::lock_guard<std::mutex> lk(mu_);
    return lost_locked(reorder_margin);
}

double SeqLossTracker::loss_percent(int reorder_margin) const {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t l = lost_locked(reorder_margin);
    const uint64_t expected = received_ + l;
    if (expected == 0) return 0.0;
    return 100.0 * double(l) / double(expected);
}

// ---------------------------------------------------------------- FrameAssembler

FrameAssembler::FrameAssembler(const AssemblerConfig& cfg, FrameCallback on_frame,
                               ThreadPool* decode_pool)
    : cfg_(cfg),
      conceal_mode_(uint8_t(cfg.conceal)),
      on_frame_(std::move(on_frame)),
      pool_(decode_pool),
      decoder_backend_(std::string("LanVid DCT (") + dct_kernels_best().name + ")") {
    const uint32_t n = pool_ ? pool_->slot_count() : 1u;
    scratches_.resize(n);
}

FrameAssembler::~FrameAssembler() {
    stop_decoder_worker();
    for (auto& slot : slots_) {
        while (slot.in_flight.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
    }
}

std::vector<size_t> FrameAssembler::active_slots_sorted() const {
    std::vector<size_t> idx;
    idx.reserve(kNumSlots);
    for (size_t i = 0; i < kNumSlots; ++i) {
        if (slots_[i].active) idx.push_back(i);
    }
    if (idx.size() > 1) {
        const uint32_t base = has_finalized_ ? last_finalized_id_ : slots_[idx[0]].frame_id;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            return frame_diff(slots_[a].frame_id, base) < frame_diff(slots_[b].frame_id, base);
        });
    }
    return idx;
}

void FrameAssembler::init_slot(FrameSlot& slot, const PacketHeader& h, uint64_t now_us) {
    slot.active = true;
    slot.frame_id = h.frame_id;
    slot.sender_ts_us = h.timestamp_us;
    slot.first_arrival_us = now_us;
    slot.total_slices = h.total_slices;
    slot.codec = packet_codec_id(h.flags);
    slot.is_keyframe = (h.flags & kFlagKeyFrame) != 0;
    slot.has_codec_config = (h.flags & kFlagCodecConfig) != 0;
    slot.has_dims = false;
    slot.width = last_known_w_;
    slot.height = last_known_h_;
    slot.nominal_slice_h = last_known_slice_h_;
    slot.quality = last_known_quality_;
    slot.slices_dispatched = 0;
    slot.slices_done.store(0, std::memory_order_relaxed);
    slot.in_flight.store(0, std::memory_order_relaxed);
    slot.decode_us_sum.store(0, std::memory_order_relaxed);
    slot.row_claimed.clear();

    if (slot.slices_cap < h.total_slices) {
        slot.slices = std::make_unique<SliceSlot[]>(h.total_slices);
        slot.slices_cap = h.total_slices;
    }
    for (uint16_t i = 0; i < h.total_slices; ++i) slot.slices[i].reset();
}

void FrameAssembler::ensure_slice_initialized(SliceSlot& ss, uint16_t frag_count) {
    if (SliceState(ss.state.load(std::memory_order_acquire)) != SliceState::kEmpty) return;
    ss.state.store(uint8_t(SliceState::kPartial), std::memory_order_relaxed);
    ss.frag_count = frag_count;
    ss.frags_received = 0;
    ss.chunk_size = 0;
    ss.last_frag_size = 0;
    ss.has_deferred_last = false;
    ss.frag_bitmap.assign((size_t(frag_count) + 7) / 8, 0);
    ss.frag_lens.assign(frag_count, 0);
}

void FrameAssembler::conceal_strip(Image& dst, int y0, int y1) const {
    y0 = std::max(0, y0);
    y1 = std::min(dst.height, y1);
    if (y0 >= y1) return;

    const bool can_copy_prev =
        conceal_mode() == ConcealMode::kPrev && prev_output_.width == dst.width &&
        prev_output_.height == dst.height;

    if (can_copy_prev) {
        for (int y = y0; y < y1; ++y) {
            std::memcpy(dst.row(y), prev_output_.row(y), dst.stride);
        }
    } else {
        for (int y = y0; y < y1; ++y) {
            uint8_t* p = dst.row(y);
            for (int x = 0; x < dst.width; ++x) {
                p[x * 4 + 0] = 128;
                p[x * 4 + 1] = 128;
                p[x * 4 + 2] = 128;
                p[x * 4 + 3] = 255;
            }
        }
    }
}

void FrameAssembler::trigger_pli(uint32_t failed_frame_id, uint64_t now_us, bool immediate) {
    constexpr uint64_t kPliMinIntervalUs = 35000;  // 35 ms cooldown for repeated PLIs
    if (!immediate && last_pli_us_ != 0 && now_us >= last_pli_us_ &&
        (now_us - last_pli_us_) < kPliMinIntervalUs) {
        return;
    }
    last_pli_us_ = now_us;
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.pli_requests;
    }
    if (on_pli_) on_pli_(failed_frame_id);
}

void FrameAssembler::set_backend(const std::string& name, bool hw) {
    {
        std::lock_guard<std::mutex> lk(backend_mu_);
        decoder_backend_ = name;
    }
    decoder_is_hw_.store(hw, std::memory_order_relaxed);
}

void FrameAssembler::finalize_inter_frame_slot(FrameSlot& slot, uint64_t now_us) {
    if (!has_finalized_ || frame_newer(slot.frame_id, last_finalized_id_)) {
        has_finalized_ = true;
        last_finalized_id_ = slot.frame_id;
    }
    if (!cfg_.async_inter_decode) {
        decode_inter_frame_sync(slot, now_us);
        return;
    }

    // ---- async mode: gate on the ingest thread, decode on the worker ----
    fold_vdec_feedback(now_us);
    if (active_codec() != slot.codec) {
        active_codec_.store(uint8_t(slot.codec), std::memory_order_release);
        has_submitted_inter_ = false;
        awaiting_idr_.store(!slot.is_keyframe, std::memory_order_release);
    }

    const bool complete =
        slot.total_slices >= 1 &&
        SliceState(slot.slices[0].state.load(std::memory_order_acquire)) == SliceState::kComplete;
    if (!complete) {
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.frames_concealed;
            ++stats_.slices_lost;
        }
        const bool first_loss = !awaiting_idr_.exchange(true, std::memory_order_acq_rel);
        trigger_pli(slot.frame_id, now_us, first_loss);
        slot.active = false;
        return;
    }

    const bool frame_gap = has_submitted_inter_ && (frame_diff(slot.frame_id, last_submitted_fid_) != 1);
    const bool need_idr_first = !has_submitted_inter_ && !slot.is_keyframe;
    if (!slot.is_keyframe && (awaiting_idr() || frame_gap || need_idr_first)) {
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.frames_dropped_awaiting_idr;
        }
        const bool first_loss = !awaiting_idr_.exchange(true, std::memory_order_acq_rel);
        trigger_pli(slot.frame_id, now_us, first_loss);
        slot.active = false;
        return;
    }

    if (on_bitstream_) {
        const SliceSlot& bs = slot.slices[0];
        on_bitstream_(slot.codec, bs.buf.data(), bs.total_size, slot.is_keyframe, slot.frame_id,
                      slot.first_arrival_us);
    }
    InterJob job;
    job.frame_id = slot.frame_id;
    job.sender_ts_us = slot.sender_ts_us;
    job.first_arrival_us = slot.first_arrival_us;
    job.codec = slot.codec;
    job.is_keyframe = slot.is_keyframe;
    const SliceSlot& ss = slot.slices[0];
    job.bits.assign(ss.buf.data(), ss.buf.data() + ss.total_size);
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(vdec_mu_);
        if (!vdec_thread_.joinable() && !vdec_stop_) vdec_thread_ = std::thread([this] { vdec_loop(); });
        if (vdec_q_.size() < kMaxInterQueue) {
            vdec_q_.push_back(std::move(job));
            queued = true;
        } else if (slot.is_keyframe) {  // backlog full: an IDR supersedes the frames still waiting
            vdec_skipped_.fetch_add(vdec_q_.size(), std::memory_order_relaxed);
            vdec_q_.clear();
            vdec_q_.push_back(std::move(job));
            queued = true;
        }
    }
    if (!queued) {  // decoder cannot keep up: skip ahead to the next IDR instead of growing latency
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.frames_dropped_awaiting_idr;
        }
        awaiting_idr_.store(true, std::memory_order_release);
        has_submitted_inter_ = false;
        trigger_pli(slot.frame_id, now_us, true);
        slot.active = false;
        return;
    }
    vdec_cv_.notify_one();
    if (slot.is_keyframe) awaiting_idr_.store(false, std::memory_order_release);
    has_submitted_inter_ = true;
    last_submitted_fid_ = slot.frame_id;
    slot.active = false;
}

void FrameAssembler::fold_vdec_feedback(uint64_t now_us) {
    if (!cfg_.async_inter_decode) return;
    const uint64_t ok = vdec_ok_.load(std::memory_order_acquire);
    const uint64_t fail = vdec_fail_.load(std::memory_order_acquire);
    const uint64_t skipped = vdec_skipped_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.frames_complete += ok - vdec_ok_seen_;
        stats_.slices_ok += ok - vdec_ok_seen_;
        stats_.slices_corrupt += fail - vdec_fail_seen_;
        stats_.frames_concealed += fail - vdec_fail_seen_;
        stats_.frames_dropped_awaiting_idr += skipped - vdec_skipped_seen_;
    }
    vdec_ok_seen_ = ok;
    vdec_fail_seen_ = fail;
    vdec_skipped_seen_ = skipped;
    if (const int w = vdec_w_.load(std::memory_order_relaxed); w > 0) {
        last_known_w_ = w;
        last_known_h_ = vdec_h_.load(std::memory_order_relaxed);
    }
    if (vdec_error_.exchange(false, std::memory_order_acq_rel)) {
        awaiting_idr_.store(true, std::memory_order_release);
        has_submitted_inter_ = false;
        trigger_pli(vdec_error_fid_.load(std::memory_order_relaxed), now_us, true);
    }
}

void FrameAssembler::vdec_loop() {
    for (;;) {
        InterJob job;
        {
            std::unique_lock<std::mutex> lk(vdec_mu_);
            vdec_cv_.wait(lk, [&] { return vdec_stop_ || !vdec_q_.empty(); });
            if (vdec_q_.empty()) break;  // stop requested and nothing left
            job = std::move(vdec_q_.front());
            vdec_q_.pop_front();
            vdec_busy_ = true;
        }
        // The decoder is created, used and destroyed on this thread only (Media Foundation / COM
        // apartment and VideoToolbox session stay on one thread).
        if (!video_decoder_ || video_decoder_->codec_id() != job.codec) {
            video_decoder_ = create_video_decoder(job.codec, cfg_.output_order);
            set_backend(video_decoder_ ? video_decoder_->backend_name() : "Unavailable",
                        video_decoder_ ? video_decoder_->is_hardware() : false);
            vdec_need_key_ = true;
        }
        if (!video_decoder_) {
            vdec_fail_.fetch_add(1, std::memory_order_relaxed);
        } else if (vdec_need_key_ && !job.is_keyframe) {
            vdec_skipped_.fetch_add(1, std::memory_order_relaxed);  // waiting for the IDR the PLI asked for
        } else {
            const uint64_t t0 = lv::now_us();
            const bool decoded = video_decoder_->decode(job.bits.data(), job.bits.size(), cfg_.output_order,
                                                        vdec_frame_) &&
                                 vdec_frame_.width > 0 && vdec_frame_.height > 0;
            const uint64_t dt = lv::now_us() - t0;
            if (!decoded) {
                video_decoder_->reset();
                vdec_need_key_ = true;
                vdec_fail_.fetch_add(1, std::memory_order_relaxed);
                vdec_error_fid_.store(job.frame_id, std::memory_order_relaxed);
                vdec_error_.store(true, std::memory_order_release);
            } else {
                vdec_need_key_ = false;
                vdec_w_.store(vdec_frame_.width, std::memory_order_relaxed);
                vdec_h_.store(vdec_frame_.height, std::memory_order_relaxed);
                vdec_ok_.fetch_add(1, std::memory_order_release);
                if (on_frame_) {
                    CompletedFrame cf;
                    cf.frame_id = job.frame_id;
                    cf.sender_timestamp_us = job.sender_ts_us;
                    cf.first_arrival_us = job.first_arrival_us;
                    cf.complete_time_us = lv::now_us();
                    cf.decode_time_us = uint32_t(dt);
                    cf.total_slices = 1;
                    cf.slices_ok = 1;
                    cf.codec = job.codec;
                    cf.is_keyframe = job.is_keyframe;
                    cf.image = &vdec_frame_;
                    on_frame_(cf);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(vdec_mu_);
            vdec_busy_ = false;
            if (vdec_q_.empty()) vdec_idle_cv_.notify_all();
        }
    }
    video_decoder_.reset();
}

void FrameAssembler::vdec_drain() {
    std::unique_lock<std::mutex> lk(vdec_mu_);
    if (!vdec_thread_.joinable()) return;
    vdec_idle_cv_.wait_for(lk, std::chrono::seconds(2), [&] { return vdec_q_.empty() && !vdec_busy_; });
}

void FrameAssembler::stop_decoder_worker() {
    {
        std::lock_guard<std::mutex> lk(vdec_mu_);
        vdec_stop_ = true;
        vdec_q_.clear();
    }
    vdec_cv_.notify_all();
    if (vdec_thread_.joinable()) vdec_thread_.join();
}

void FrameAssembler::decode_inter_frame_sync(FrameSlot& slot, uint64_t now_us) {
    if (!video_decoder_ || video_decoder_->codec_id() != slot.codec) {
        video_decoder_ = create_video_decoder(slot.codec, cfg_.output_order);
        active_codec_.store(uint8_t(slot.codec), std::memory_order_release);
        set_backend(video_decoder_ ? video_decoder_->backend_name() : "Unavailable",
                    video_decoder_ ? video_decoder_->is_hardware() : false);
        has_decoded_inter_frame_ = false;
        awaiting_idr_.store(!slot.is_keyframe, std::memory_order_release);
    }

    const bool complete =
        slot.total_slices >= 1 &&
        SliceState(slot.slices[0].state.load(std::memory_order_acquire)) == SliceState::kComplete;

    if (!complete) {
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.frames_concealed;
            ++stats_.slices_lost;
        }
        const bool first_loss = !awaiting_idr_.exchange(true, std::memory_order_acq_rel);
        trigger_pli(slot.frame_id, now_us, first_loss);
        slot.active = false;
        return;
    }

    const bool frame_gap =
        has_decoded_inter_frame_ && (frame_diff(slot.frame_id, last_decoded_inter_fid_) != 1);
    const bool need_idr_first = !has_decoded_inter_frame_ && !slot.is_keyframe;

    if (!slot.is_keyframe && (awaiting_idr() || frame_gap || need_idr_first)) {
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.frames_dropped_awaiting_idr;
        }
        const bool first_loss = !awaiting_idr_.exchange(true, std::memory_order_acq_rel);
        trigger_pli(slot.frame_id, now_us, first_loss);
        slot.active = false;
        return;
    }

    if (!video_decoder_) {
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.slices_corrupt;
            ++stats_.frames_concealed;
        }
        slot.active = false;
        return;
    }

    if (slot.is_keyframe) {
        awaiting_idr_.store(false, std::memory_order_release);
    }

    const SliceSlot& ss = slot.slices[0];
    if (on_bitstream_) {
        on_bitstream_(slot.codec, ss.buf.data(), ss.total_size, slot.is_keyframe, slot.frame_id,
                      slot.first_arrival_us);
    }
    const uint64_t t0 = lv::now_us();
    const bool decoded = video_decoder_->decode(ss.buf.data(), ss.total_size, cfg_.output_order, slot.frame);
    const uint64_t dt = lv::now_us() - t0;

    if (!decoded || slot.frame.width <= 0 || slot.frame.height <= 0) {
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.slices_corrupt;
            ++stats_.frames_concealed;
        }
        awaiting_idr_.store(true, std::memory_order_release);
        video_decoder_->reset();
        trigger_pli(slot.frame_id, now_us, true);
        slot.active = false;
        return;
    }

    has_decoded_inter_frame_ = true;
    last_decoded_inter_fid_ = slot.frame_id;
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.frames_complete;
        ++stats_.slices_ok;
    }
    last_known_w_ = slot.frame.width;
    last_known_h_ = slot.frame.height;

    if (on_frame_) {
        CompletedFrame cf;
        cf.frame_id = slot.frame_id;
        cf.sender_timestamp_us = slot.sender_ts_us;
        cf.first_arrival_us = slot.first_arrival_us;
        cf.complete_time_us = lv::now_us();
        cf.decode_time_us = uint32_t(dt);
        cf.total_slices = 1;
        cf.slices_ok = 1;
        cf.slices_corrupt = 0;
        cf.slices_lost = 0;
        cf.quality = 0;
        cf.codec = slot.codec;
        cf.is_keyframe = slot.is_keyframe;
        cf.image = &slot.frame;
        on_frame_(cf);
    }

    slot.active = false;
}

void FrameAssembler::finalize_slot(FrameSlot& slot, uint64_t now_us) {
    if (!slot.active) return;

    if (slot.codec != VideoCodecId::kLanVidDct) {
        finalize_inter_frame_slot(slot, now_us);
        return;
    }

    if (active_codec() != VideoCodecId::kLanVidDct) {
        active_codec_.store(uint8_t(VideoCodecId::kLanVidDct), std::memory_order_release);
        set_backend(std::string("LanVid DCT (") + dct_kernels_best().name + ")", false);
        awaiting_idr_.store(false, std::memory_order_release);
        has_decoded_inter_frame_ = false;
        has_submitted_inter_ = false;
    }

    bool waited = false;
    while (slot.in_flight.load(std::memory_order_acquire) > 0) {
        waited = true;
        std::this_thread::yield();
    }
    if (waited && pool_) now_us = lv::now_us();

    const int w = slot.has_dims ? slot.width : last_known_w_;
    const int h = slot.has_dims ? slot.height : last_known_h_;
    const int sh = slot.nominal_slice_h > 0 ? slot.nominal_slice_h : last_known_slice_h_;

    uint16_t ok_cnt = 0;
    uint16_t corrupt_cnt = 0;
    uint16_t lost_cnt = 0;

    if (w > 0 && h > 0) {
        if (slot.frame.width != w || slot.frame.height != h) {
            slot.frame.allocate(w, h, cfg_.output_order);
        }
        for (uint16_t sid = 0; sid < slot.total_slices; ++sid) {
            const auto st = SliceState(slot.slices[sid].state.load(std::memory_order_acquire));
            if (st == SliceState::kDecoded) {
                ++ok_cnt;
            } else {
                if (st == SliceState::kCorrupt) ++corrupt_cnt;
                else ++lost_cnt;
                const int y0 = int(sid) * sh;
                const int y1 = std::min(h, y0 + sh);
                conceal_strip(slot.frame, y0, y1);
            }
        }
    } else {
        for (uint16_t sid = 0; sid < slot.total_slices; ++sid) {
            const auto st = SliceState(slot.slices[sid].state.load(std::memory_order_acquire));
            if (st == SliceState::kCorrupt) ++corrupt_cnt;
            else ++lost_cnt;
        }
    }

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.slices_ok += ok_cnt;
        stats_.slices_corrupt += corrupt_cnt;
        stats_.slices_lost += lost_cnt;
        if (corrupt_cnt == 0 && lost_cnt == 0) ++stats_.frames_complete;
        else ++stats_.frames_concealed;
    }

    if (!has_finalized_ || frame_newer(slot.frame_id, last_finalized_id_)) {
        has_finalized_ = true;
        last_finalized_id_ = slot.frame_id;
    }

    if (w > 0 && h > 0) {
        last_known_w_ = w;
        last_known_h_ = h;
        last_known_slice_h_ = sh;
        if (slot.quality > 0) last_known_quality_ = slot.quality;
        if (prev_output_.width != w || prev_output_.height != h) {
            prev_output_.allocate(w, h, cfg_.output_order);
        }
        std::memcpy(prev_output_.data.data(), slot.frame.data.data(), slot.frame.data.size());
    }

    if (on_frame_ && w > 0 && h > 0) {
        const uint64_t total_slice_dec_us = slot.decode_us_sum.load(std::memory_order_relaxed);
        const uint32_t threads = pool_ ? std::max(1u, pool_->num_threads()) : 1u;

        CompletedFrame cf;
        cf.frame_id = slot.frame_id;
        cf.sender_timestamp_us = slot.sender_ts_us;
        cf.first_arrival_us = slot.first_arrival_us;
        cf.complete_time_us = now_us;
        cf.decode_time_us = uint32_t(total_slice_dec_us / threads);
        cf.total_slices = slot.total_slices;
        cf.slices_ok = ok_cnt;
        cf.slices_corrupt = corrupt_cnt;
        cf.slices_lost = lost_cnt;
        cf.quality = slot.quality ? slot.quality : last_known_quality_;
        cf.codec = VideoCodecId::kLanVidDct;
        cf.is_keyframe = true;
        cf.image = &slot.frame;
        on_frame_(cf);
    }

    slot.active = false;
}

void FrameAssembler::dispatch_slice_decode(FrameSlot& slot, uint16_t slice_id) {
    SliceSlot& ss = slot.slices[slice_id];
    SliceHeader sh;
    if (!parse_slice_header(ss.buf.data(), ss.total_size, sh)) {
        ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
        slot.slices_done.fetch_add(1, std::memory_order_release);
        return;
    }
    ss.hdr = sh;
    slot.quality = sh.quality;

    if (!slot.has_dims) {
        slot.has_dims = true;
        slot.width = sh.width;
        slot.height = sh.height;
        if (slot.frame.width != sh.width || slot.frame.height != sh.height) {
            slot.frame.allocate(sh.width, sh.height, cfg_.output_order);
        }
        slot.row_claimed.assign(size_t(sh.height), 0);
        if (slice_id + 1 < slot.total_slices) {
            slot.nominal_slice_h = sh.slice_h;
        } else if (slice_id > 0 && sh.slice_y0 % slice_id == 0) {
            slot.nominal_slice_h = sh.slice_y0 / slice_id;
        } else {
            slot.nominal_slice_h = sh.slice_h;
        }
    } else if (slice_id + 1 < slot.total_slices) {
        slot.nominal_slice_h = sh.slice_h;
    }

    if (sh.width != slot.width || sh.height != slot.height) {
        ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
        slot.slices_done.fetch_add(1, std::memory_order_release);
        return;
    }
    // Reject overlapping slice row ranges so parallel ThreadPool workers never race on shared output rows.
    const int y0 = sh.slice_y0;
    const int y1 = y0 + sh.slice_h;
    if (y0 < 0 || y1 > slot.height || size_t(y1) > slot.row_claimed.size()) {
        ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
        slot.slices_done.fetch_add(1, std::memory_order_release);
        return;
    }
    for (int y = y0; y < y1; ++y) {
        if (slot.row_claimed[size_t(y)]) {
            ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
            slot.slices_done.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    std::memset(slot.row_claimed.data() + y0, 1, size_t(sh.slice_h));

    auto run_decode = [this, slot_ptr = &slot, slice_id](uint32_t worker_slot) {
        SliceSlot& s = slot_ptr->slices[slice_id];
        const uint64_t t0 = lv::now_us();
        const DecodeStatus st =
            decode_slice(s.buf.data(), s.total_size, scratches_[worker_slot].scratch,
                         slot_ptr->frame.data.data(), slot_ptr->frame.stride, slot_ptr->frame.width,
                         slot_ptr->frame.height, nullptr, cfg_.output_order);
        const uint64_t dt = lv::now_us() - t0;
        slot_ptr->decode_us_sum.fetch_add(dt, std::memory_order_relaxed);
        s.state.store(uint8_t(st == DecodeStatus::kOk ? SliceState::kDecoded : SliceState::kCorrupt),
                      std::memory_order_release);
        slot_ptr->slices_done.fetch_add(1, std::memory_order_release);
    };

    if (pool_) {
        slot.in_flight.fetch_add(1, std::memory_order_acq_rel);
        pool_->enqueue([slot_ptr = &slot, run_decode](uint32_t worker_slot) {
            run_decode(worker_slot);
            slot_ptr->in_flight.fetch_sub(1, std::memory_order_acq_rel);
        });
    } else {
        run_decode(0);
    }
}

void FrameAssembler::insert_data_fragment(FrameSlot& slot, SliceSlot& ss, uint16_t slice_id, uint16_t fi,
                                          const uint8_t* payload, uint16_t payload_size, uint64_t now_us,
                                          bool run_fec) {
    constexpr size_t kMaxSliceBytes = 16u * 1024u * 1024u;
    if (SliceState(ss.state.load(std::memory_order_acquire)) != SliceState::kPartial) return;
    if (fi >= ss.frag_count) return;
    const uint8_t bit = uint8_t(1u << (fi & 7));
    if (ss.frag_bitmap[fi >> 3] & bit) return;  // duplicate

    ss.frag_bitmap[fi >> 3] |= bit;
    ss.frag_lens[fi] = payload_size;
    ++ss.frags_received;

    if (ss.frag_count == 1) {
        ss.buf.resize(payload_size);
        std::memcpy(ss.buf.data(), payload, payload_size);
        ss.total_size = payload_size;
    } else if (fi + 1 < ss.frag_count) {
        if (ss.chunk_size == 0) {
            const size_t need_bytes = size_t(ss.frag_count) * size_t(payload_size);
            if (payload_size == 0 || need_bytes > kMaxSliceBytes) {
                ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
                ++slot.slices_dispatched;
                slot.slices_done.fetch_add(1, std::memory_order_release);
                return;
            }
            ss.chunk_size = payload_size;
            ss.buf.resize(need_bytes);
            if (ss.has_deferred_last) {
                if (ss.last_frag_size > ss.chunk_size) {
                    ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
                    ++slot.slices_dispatched;
                    slot.slices_done.fetch_add(1, std::memory_order_release);
                    return;
                }
                std::memcpy(ss.buf.data() + size_t(ss.frag_count - 1) * ss.chunk_size,
                            ss.last_frag_tmp.data(), ss.last_frag_size);
                ss.has_deferred_last = false;
            }
        } else if (payload_size != ss.chunk_size) {
            ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
            ++slot.slices_dispatched;
            slot.slices_done.fetch_add(1, std::memory_order_release);
            return;
        }
        std::memcpy(ss.buf.data() + size_t(fi) * ss.chunk_size, payload, payload_size);
    } else {
        ss.last_frag_size = payload_size;
        if (ss.chunk_size == 0) {
            ss.last_frag_tmp.assign(payload, payload + payload_size);
            ss.has_deferred_last = true;
        } else {
            if (payload_size > ss.chunk_size) {
                ss.state.store(uint8_t(SliceState::kCorrupt), std::memory_order_release);
                ++slot.slices_dispatched;
                slot.slices_done.fetch_add(1, std::memory_order_release);
                return;
            }
            std::memcpy(ss.buf.data() + size_t(fi) * ss.chunk_size, payload, payload_size);
        }
    }

    if (ss.frags_received == ss.frag_count) {
        if (ss.frag_count > 1) {
            ss.total_size = size_t(ss.frag_count - 1) * ss.chunk_size + ss.last_frag_size;
        }
        ss.state.store(uint8_t(SliceState::kComplete), std::memory_order_release);
        ++slot.slices_dispatched;
        if (slot.codec == VideoCodecId::kLanVidDct) {
            dispatch_slice_decode(slot, slice_id);
        } else {
            slot.slices_done.fetch_add(1, std::memory_order_release);
        }
    } else if (run_fec) {
        // Attempt XOR FEC recovery in case a parity packet is waiting for this fragment.
        try_fec_recover(slot, ss, slice_id, now_us);
    }
}

void FrameAssembler::try_fec_recover(FrameSlot& slot, SliceSlot& ss, uint16_t slice_id, uint64_t now_us) {
    if (SliceState(ss.state.load(std::memory_order_acquire)) != SliceState::kPartial) return;
    if (ss.fec_parities.empty()) return;

    // Iterative recovery loop (no mutual recursion -> immune to crafted FEC chain stack overflow).
    bool progress = true;
    while (progress && SliceState(ss.state.load(std::memory_order_acquire)) == SliceState::kPartial) {
        progress = false;
        for (auto& fp : ss.fec_parities) {
            if (fp.used) continue;
            const uint16_t start = fp.hdr.group_start_frag;
            const uint16_t cnt = fp.hdr.group_data_count;
            if (uint32_t(start) + cnt > ss.frag_count) {
                fp.used = true;
                continue;
            }

            int missing_count = 0;
            uint16_t miss_fi = 0;
            for (uint16_t fi = start; fi < start + cnt; ++fi) {
                if ((ss.frag_bitmap[fi >> 3] & uint8_t(1u << (fi & 7))) == 0) {
                    ++missing_count;
                    miss_fi = fi;
                    if (missing_count > 1) break;
                }
            }
            if (missing_count == 0) {
                fp.used = true;
                continue;
            }
            if (missing_count != 1) continue;

            // Recover exact length and payload of `miss_fi` via XOR!
            uint16_t rec_len = fp.hdr.xor_payload_len;
            for (uint16_t fi = start; fi < start + cnt; ++fi) {
                if (fi != miss_fi) rec_len ^= ss.frag_lens[fi];
            }
            if (rec_len == 0 || rec_len > fp.parity.size()) {
                fp.used = true;
                continue;
            }

            std::vector<uint8_t> recovered(fp.parity.begin(), fp.parity.begin() + ptrdiff_t(rec_len));
            bool ok = true;
            for (uint16_t fi = start; fi < start + cnt; ++fi) {
                if (fi == miss_fi) continue;
                const uint8_t* ptr = nullptr;
                const uint16_t flen = ss.frag_lens[fi];
                if (ss.frag_count == 1) {
                    ptr = ss.buf.data();
                } else if (fi + 1 == ss.frag_count && ss.has_deferred_last) {
                    ptr = ss.last_frag_tmp.data();
                } else if (ss.chunk_size > 0) {
                    ptr = ss.buf.data() + size_t(fi) * ss.chunk_size;
                } else {
                    ok = false;
                    break;
                }
                const size_t xlen = std::min<size_t>(rec_len, flen);
                for (size_t i = 0; i < xlen; ++i) recovered[i] ^= ptr[i];
            }
            fp.used = true;
            if (!ok) continue;

            {
                std::lock_guard<std::mutex> lk(stats_mu_);
                ++stats_.fec_recovered_packets;
            }
            insert_data_fragment(slot, ss, slice_id, miss_fi, recovered.data(), rec_len, now_us, false);
            progress = true;
            if (SliceState(ss.state.load(std::memory_order_acquire)) != SliceState::kPartial) break;
        }
    }
}

void FrameAssembler::try_finalize_ready(uint64_t now_us) {
    for (size_t idx : active_slots_sorted()) {
        FrameSlot& s = slots_[idx];
        if (!s.active) continue;
        const bool all_dispatched = s.slices_dispatched >= s.total_slices;
        const bool all_done = s.slices_done.load(std::memory_order_acquire) >= s.total_slices &&
                              s.in_flight.load(std::memory_order_acquire) == 0;
        const bool timed_out =
            cfg_.frame_timeout_us > 0 && now_us >= s.first_arrival_us &&
            (now_us - s.first_arrival_us) >= cfg_.frame_timeout_us;
        if (all_dispatched || all_done || timed_out) {
            finalize_slot(s, now_us);
        } else {
            break;
        }
    }
}

void FrameAssembler::record_bad_packet(PacketParseStatus st, size_t dgram_len) {
    std::lock_guard<std::mutex> lk(stats_mu_);
    ++stats_.datagrams_total;
    stats_.bytes_total += dgram_len;
    ++stats_.bad_packets;
    if (st == PacketParseStatus::kBadCrc) ++stats_.crc_errors;
}

PacketParseStatus FrameAssembler::ingest_datagram(const uint8_t* dgram, size_t len, uint64_t now_us) {
    PacketHeader h;
    const uint8_t* payload = nullptr;
    const PacketParseStatus st = parse_packet(dgram, len, h, &payload);
    if (st != PacketParseStatus::kOk) {
        record_bad_packet(st, len);
        return st;
    }
    if (h.type == PacketType::kVideoSlice) {
        ingest_video_packet(h, payload, len, now_us);
    } else {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.datagrams_total;
        stats_.bytes_total += len;
    }
    return PacketParseStatus::kOk;
}

void FrameAssembler::ingest_video_packet(const PacketHeader& h, const uint8_t* payload, size_t dgram_len,
                                         uint64_t now_us) {
    constexpr uint16_t kMaxSlicesPerFrame = 2048;
    constexpr uint16_t kMaxFragsPerSlice = 8192;
    constexpr size_t kMaxFecParitiesPerSlice = 256;

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.datagrams_total;
        stats_.bytes_total += dgram_len;
    }
    seq_.observe(h.seq);

    if (h.total_slices == 0 || h.total_slices > kMaxSlicesPerFrame || h.slice_id >= h.total_slices ||
        h.frag_count == 0 || h.frag_count > kMaxFragsPerSlice) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.bad_packets;
        return;
    }

    if (has_finalized_ && frame_diff(h.frame_id, last_finalized_id_) <= 0) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        if ((h.flags & kFlagFecParity) && h.frame_id == last_finalized_id_) {
            ++stats_.fec_parity_received;
        } else {
            ++stats_.late_packets;
        }
        return;
    }

    const size_t target_idx = size_t(h.frame_id & (kNumSlots - 1));
    for (size_t idx : active_slots_sorted()) {
        FrameSlot& s = slots_[idx];
        const int32_t gap = frame_diff(h.frame_id, s.frame_id);
        if (gap >= 2 || (idx == target_idx && gap > 0)) {
            finalize_slot(s, now_us);
        }
    }

    FrameSlot& slot = slots_[target_idx];
    if (!slot.active) {
        init_slot(slot, h, now_us);
    } else if (slot.frame_id != h.frame_id) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.late_packets;
        return;
    }

    if (h.total_slices != slot.total_slices || h.slice_id >= slot.total_slices) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.bad_packets;
        return;
    }
    if (h.flags & kFlagKeyFrame) slot.is_keyframe = true;
    if (h.flags & kFlagCodecConfig) slot.has_codec_config = true;

    SliceSlot& ss = slot.slices[h.slice_id];
    const auto cur_state = SliceState(ss.state.load(std::memory_order_acquire));
    if (cur_state != SliceState::kEmpty && cur_state != SliceState::kPartial) {
        if (h.flags & kFlagFecParity) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.fec_parity_received;
        }
        return;  // slice already complete / decoded / corrupt
    }

    ensure_slice_initialized(ss, h.frag_count);
    if (h.frag_count != ss.frag_count) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        ++stats_.bad_packets;
        return;
    }

    if (h.flags & kFlagFecParity) {
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.fec_parity_received;
        }
        FecHeader fh;
        if (!parse_fec_header(payload, h.payload_size, fh) ||
            uint32_t(fh.group_start_frag) + fh.group_data_count > ss.frag_count) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.bad_packets;
            return;
        }
        // Ignore duplicate parity for the same group.
        for (const auto& existing : ss.fec_parities) {
            if (existing.hdr.group_start_frag == fh.group_start_frag &&
                existing.hdr.group_data_count == fh.group_data_count) {
                return;
            }
        }
        if (ss.fec_parities.size() >= kMaxFecParitiesPerSlice) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            ++stats_.bad_packets;
            return;
        }
        FecParityEntry entry;
        entry.hdr = fh;
        entry.parity.assign(payload + kFecHeaderSize, payload + h.payload_size);
        ss.fec_parities.push_back(std::move(entry));
        try_fec_recover(slot, ss, h.slice_id, now_us);
    } else {
        insert_data_fragment(slot, ss, h.slice_id, h.frag_index, payload, h.payload_size, now_us);
    }

    try_finalize_ready(now_us);
}

void FrameAssembler::poll(uint64_t now_us) {
    try_finalize_ready(now_us);
    fold_vdec_feedback(now_us);
}

void FrameAssembler::flush(uint64_t now_us) {
    for (size_t idx : active_slots_sorted()) {
        finalize_slot(slots_[idx], now_us);
    }
    vdec_drain();
    fold_vdec_feedback(now_us);
}

}  // namespace lv
