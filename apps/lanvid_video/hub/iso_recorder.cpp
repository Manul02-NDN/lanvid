#include "hub/iso_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "common/clock.hpp"
#include "video/video_bus.hpp"

namespace fs = std::filesystem;

namespace lv::hub {

namespace {

std::string sanitize_filename(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(char(c));
        } else if (c == ' ') {
            if (out.empty() || out.back() != '_') out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "Cam" : out;
}

}  // namespace

IsoMultiRecorder::IsoMultiRecorder() {
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        writers_[i] = std::make_unique<SlotWriter>();
    }
}

IsoMultiRecorder::~IsoMultiRecorder() {
    stop_session();
}

std::string IsoMultiRecorder::default_output_dir() {
#if defined(_WIN32)
    const char* up = std::getenv("USERPROFILE");
    fs::path p = up && *up ? (fs::path(up) / "Videos" / "LanVid") : fs::current_path();
#elif defined(__APPLE__)
    const char* h = std::getenv("HOME");
    fs::path p = h && *h ? (fs::path(h) / "Movies" / "LanVid") : fs::current_path();
#else
    const char* h = std::getenv("HOME");
    fs::path p = h && *h ? (fs::path(h) / "Videos" / "LanVid") : fs::current_path();
#endif
    return p.string();
}

uint64_t IsoMultiRecorder::disk_free_bytes(const std::string& dir) {
    std::error_code ec;
    const fs::path p = dir.empty() ? fs::path(default_output_dir()) : fs::path(dir);
    fs::path probe = p;
    while (!probe.empty() && !fs::exists(probe, ec)) {
        probe = probe.parent_path();
    }
    if (probe.empty()) probe = fs::current_path(ec);
    const auto sp = fs::space(probe, ec);
    return ec ? 0 : uint64_t(sp.available);
}

std::string IsoMultiRecorder::build_slot_path(uint32_t slot, const std::string& label, lv::IsoContainer c) const {
    const std::string clean = sanitize_filename(label);
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "CAM%u_", slot + 1);
    return (fs::path(session_dir_) / (std::string(prefix) + clean + "." + lv::iso_container_extension(c))).string();
}

void IsoMultiRecorder::close_writer(SlotWriter& w) {
    std::lock_guard<std::mutex> lk(w.mu);
    w.want_rec = false;
    if (w.rec) {
        w.last_stats = w.rec->stats();
        w.last_path = w.rec->path();
        w.rec->close();
        w.rec.reset();
    }
    w.enc.reset();
}

bool IsoMultiRecorder::start_session(const IsoMultiSessionConfig& cfg,
                                     const std::array<std::string, kMaxHubSlots>& slot_labels) {
    stop_session();

    const std::time_t now_t = std::time(nullptr);
    std::tm tm_now{};
#if defined(_WIN32)
    localtime_s(&tm_now, &now_t);
#else
    localtime_r(&now_t, &tm_now);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d-%02d-%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                  tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    const std::string base_dir = cfg.output_dir.empty() ? default_output_dir() : cfg.output_dir;
    const std::string sess_name = cfg.session_name.empty() ? "LanVid" : sanitize_filename(cfg.session_name);
    const fs::path sdir = fs::path(base_dir) / (sess_name + "_" + stamp);
    std::error_code ec;
    fs::create_directories(sdir, ec);
    if (ec) return false;

    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        cfg_ = cfg;
        cfg_.slot_count = std::clamp<uint32_t>(cfg.slot_count, 1, kMaxHubSlots);
        session_dir_ = sdir.string();
        session_t0_us_ = lv::now_us();
        session_unix_s_ = uint64_t(now_t);
        session_tc_start_frame_ = lv::iso_timecode_now(30);
        session_active_ = true;
    }

    for (uint32_t i = 0; i < cfg_.slot_count; ++i) {
        auto& w = *writers_[i];
        std::lock_guard<std::mutex> lk(w.mu);
        w.label = slot_labels[i].empty() ? ("CAM " + std::to_string(i + 1)) : slot_labels[i];
        w.want_rec = (cfg.slot_mask & (1u << i)) != 0;
        w.rec.reset();
        w.enc.reset();
        w.last_stats = {};
        w.last_path.clear();
    }

    if (cfg.record_program) {
        std::lock_guard<std::mutex> lk(program_writer_.mu);
        program_writer_.want_rec = true;
        program_writer_.rec.reset();
        program_writer_.enc.reset();
        program_writer_.last_stats = {};
        program_writer_.last_path.clear();
    }

    stop_.store(false, std::memory_order_release);
    worker_ = std::thread(&IsoMultiRecorder::worker_loop, this);
    return true;
}

void IsoMultiRecorder::stop_session() {
    stop_.store(true, std::memory_order_release);
    q_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        q_.clear();
    }
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        close_writer(*writers_[i]);
    }
    close_writer(program_writer_);

    std::lock_guard<std::mutex> lk(cfg_mu_);
    session_active_ = false;
}

bool IsoMultiRecorder::set_slot_recording(uint32_t slot, bool on, const std::string& label) {
    if (slot >= kMaxHubSlots) return false;
    {
        std::lock_guard<std::mutex> clk(cfg_mu_);
        if (!session_active_) return false;
    }
    auto& w = *writers_[slot];
    std::lock_guard<std::mutex> lk(w.mu);
    if (!label.empty()) w.label = label;
    w.want_rec = on;
    if (!on && w.rec) {
        w.last_stats = w.rec->stats();
        w.last_path = w.rec->path();
        w.rec->close();
        w.rec.reset();
        w.enc.reset();
    }
    return true;
}

bool IsoMultiRecorder::is_recording_any() const {
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        if (!session_active_) return false;
    }
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        std::lock_guard<std::mutex> lk(writers_[i]->mu);
        if (writers_[i]->want_rec) return true;
    }
    return false;
}

bool IsoMultiRecorder::is_slot_recording(uint32_t slot) const {
    if (slot >= kMaxHubSlots) return false;
    std::lock_guard<std::mutex> lk(writers_[slot]->mu);
    return writers_[slot]->want_rec && writers_[slot]->rec && writers_[slot]->rec->is_open();
}

void IsoMultiRecorder::set_program_slot(int slot) {
    program_slot_.store(slot, std::memory_order_relaxed);
}

void IsoMultiRecorder::feed_encoded_access_unit(uint32_t slot, lv::IsoVideoCodec codec, const uint8_t* annexb,
                                                size_t size, bool is_keyframe, uint64_t arrival_us) {
    if (slot >= kMaxHubSlots || !annexb || size == 0) return;
    lv::IsoContainer cont = lv::IsoContainer::kMp4;
    uint64_t t0 = 0, ux = 0, tc0 = 0;
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        if (!session_active_) return;
        cont = cfg_.container;
        t0 = session_t0_us_;
        ux = session_unix_s_;
        tc0 = session_tc_start_frame_;
    }

    auto& w = *writers_[slot];
    std::lock_guard<std::mutex> lk(w.mu);
    if (!w.want_rec) return;

    if (!w.rec) {
        int cw = 1920, ch = 1080;
        if (codec == lv::IsoVideoCodec::kH264) {
            for (const auto& [nal, nsz] : lv::annexb_split(annexb, size)) {
                if (nsz > 0 && (nal[0] & 0x1F) == 7) {
                    lv::h264_sps_dimensions(nal, nsz, cw, ch);
                    break;
                }
            }
        }
        lv::IsoRecorderConfig rc{};
        rc.path = build_slot_path(slot, w.label, cont);
        rc.container = cont;
        rc.codec = codec;
        rc.width = cw;
        rc.height = ch;
        rc.timecode_fps = 30;
        rc.timecode_start_frame = tc0;
        rc.creation_unix_s = ux;
        rc.audio_sample_rate = 48000;
        rc.audio_channels = 2;
        rc.camera_label = w.label;
        auto rec = std::make_unique<lv::IsoRecorder>();
        if (!rec->open(rc)) return;
        w.rec = std::move(rec);
    }

    const uint64_t pts_us = arrival_us > t0 ? (arrival_us - t0) : 0;
    w.rec->write_video(annexb, size, is_keyframe, pts_us);
}

void IsoMultiRecorder::feed_bgra_frame(uint32_t slot, const lv::Image& bgra, int codec_id, bool is_program,
                                       uint64_t arrival_us) {
    if (slot >= kMaxHubSlots || bgra.width <= 0 || bgra.height <= 0) return;
    bool rec_prog = false;
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        if (!session_active_) return;
        rec_prog = cfg_.record_program && is_program;
    }

    bool slot_needs_encode = false;
    if (codec_id == int(lv::VideoCodecId::kLanVidDct)) {
        std::lock_guard<std::mutex> lk(writers_[slot]->mu);
        slot_needs_encode = writers_[slot]->want_rec;
    }
    if (!slot_needs_encode && !rec_prog) return;

    {
        std::lock_guard<std::mutex> qlk(q_mu_);
        if (q_.size() >= 16) q_.pop_front();
        if (slot_needs_encode) {
            q_.push_back(BgraJob{slot, bgra, arrival_us, false, true});
        }
        if (rec_prog) {
            q_.push_back(BgraJob{slot, bgra, arrival_us, true, true});
        }
    }
    q_cv_.notify_one();
}

void IsoMultiRecorder::feed_audio_pcm16(uint32_t slot, const int16_t* pcm16, uint32_t frames, uint64_t arrival_us) {
    if (slot >= kMaxHubSlots || !pcm16 || frames == 0) return;
    uint64_t t0 = 0;
    bool rec_prog = false;
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        if (!session_active_) return;
        t0 = session_t0_us_;
        rec_prog = cfg_.record_program;
    }
    const uint64_t pts_us = arrival_us > t0 ? (arrival_us - t0) : 0;
    {
        std::lock_guard<std::mutex> lk(writers_[slot]->mu);
        if (writers_[slot]->want_rec && writers_[slot]->rec && writers_[slot]->rec->is_open()) {
            writers_[slot]->rec->write_audio(pcm16, frames, pts_us);
        }
    }
    if (rec_prog && int(slot) == program_slot_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(program_writer_.mu);
        if (program_writer_.rec && program_writer_.rec->is_open()) {
            program_writer_.rec->write_audio(pcm16, frames, pts_us);
        }
    }
}

void IsoMultiRecorder::process_job(const BgraJob& job) {
    lv::IsoContainer cont = lv::IsoContainer::kMp4;
    uint64_t t0 = 0, ux = 0, tc0 = 0;
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        if (!session_active_) return;
        cont = cfg_.container;
        t0 = session_t0_us_;
        ux = session_unix_s_;
        tc0 = session_tc_start_frame_;
    }

    SlotWriter& w = job.is_program ? program_writer_ : *writers_[job.slot];
    std::lock_guard<std::mutex> lk(w.mu);
    if (!w.want_rec) return;

    if (!w.rec) {
        const int ew = job.is_program ? 1920 : (job.image.width & ~1);
        const int eh = job.is_program ? 1080 : (job.image.height & ~1);
        lv::IsoRecorderConfig rc{};
        rc.path = job.is_program
                      ? (fs::path(session_dir_) / (std::string("PGM_Master.") + lv::iso_container_extension(cont))).string()
                      : build_slot_path(job.slot, w.label, cont);
        rc.container = cont;
        rc.codec = lv::IsoVideoCodec::kH264;
        rc.width = std::max(64, ew);
        rc.height = std::max(64, eh);
        rc.timecode_fps = 30;
        rc.timecode_start_frame = tc0;
        rc.creation_unix_s = ux;
        rc.audio_sample_rate = 48000;
        rc.audio_channels = 2;
        rc.camera_label = job.is_program ? "PGM Master" : w.label;
        auto rec = std::make_unique<lv::IsoRecorder>();
        if (!rec->open(rc)) return;

        lv::VideoEncoderConfig ec{};
        ec.width = rc.width;
        ec.height = rc.height;
        ec.fps = 30.0;
        ec.bitrate_kbps = 25000;
        ec.keyframe_interval = 30;
        ec.prefer_hardware = true;
        auto enc = lv::create_video_encoder(lv::VideoCodecId::kH264, ec);
        if (!enc || !enc->init(ec)) {
            rec->close();
            return;
        }
        w.rec = std::move(rec);
        w.enc = std::move(enc);
    }

    if (!w.rec || !w.enc) return;
    lv::Image enc_img = job.image;
    const int tw = w.rec->stats().video_frames >= 0 ? (job.is_program ? 1920 : (job.image.width & ~1)) : job.image.width;
    const int th = job.is_program ? 1080 : (job.image.height & ~1);
    if (enc_img.width != tw || enc_img.height != th) {
        lv::Image resized;
        resized.allocate(tw, th, lv::PixelOrder::kBGRA);
        lv::VideoBus::FrameInfo fi{};
        fi.width = uint32_t(enc_img.width);
        fi.height = uint32_t(enc_img.height);
        fi.stride = uint32_t(enc_img.stride);
        fi.format = lv::VideoBusFormat::kBGRA;
        lv::video_bus_convert_to_bgra(enc_img.data.data(), fi, resized.data.data(), uint32_t(tw), uint32_t(th),
                                      uint32_t(resized.stride));
        enc_img = std::move(resized);
    }

    lv::EncodedVideoPacket pkt;
    if (w.enc->encode_image(enc_img, pkt) && !pkt.data.empty()) {
        const uint64_t pts_us = job.timestamp_us > t0 ? (job.timestamp_us - t0) : 0;
        w.rec->write_video(pkt.data.data(), pkt.data.size(), pkt.is_keyframe, pts_us);
    }
}

void IsoMultiRecorder::worker_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        std::deque<BgraJob> batch;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            if (q_.empty()) {
                q_cv_.wait_for(lk, std::chrono::milliseconds(10),
                               [this]() { return !q_.empty() || stop_.load(std::memory_order_relaxed); });
            }
            batch.swap(q_);
        }
        for (const auto& j : batch) {
            process_job(j);
        }
    }
}

IsoTrackStats IsoMultiRecorder::slot_stats(uint32_t slot) const {
    IsoTrackStats out{};
    if (slot >= kMaxHubSlots) return out;
    auto& w = *writers_[slot];
    std::lock_guard<std::mutex> lk(w.mu);
    if (w.rec && w.rec->is_open()) {
        const auto st = w.rec->stats();
        out.recording = true;
        out.waiting_for_keyframe = st.waiting_for_keyframe;
        out.path = w.rec->path();
        out.bytes = st.bytes_written;
        out.duration_us = st.duration_us;
        out.frames = st.video_frames;
        if (st.duration_us > 100'000ULL) {
            out.write_mbps = (double(st.bytes_written) * 8.0) / double(st.duration_us);
        }
    } else {
        out.recording = false;
        out.waiting_for_keyframe = false;
        out.path = w.last_path;
        out.bytes = w.last_stats.bytes_written;
        out.duration_us = w.last_stats.duration_us;
        out.frames = w.last_stats.video_frames;
    }
    return out;
}

IsoTrackStats IsoMultiRecorder::program_stats() const {
    IsoTrackStats out{};
    auto& w = const_cast<SlotWriter&>(program_writer_);
    std::lock_guard<std::mutex> lk(w.mu);
    if (w.rec && w.rec->is_open()) {
        const auto st = w.rec->stats();
        out.recording = true;
        out.waiting_for_keyframe = st.waiting_for_keyframe;
        out.path = w.rec->path();
        out.bytes = st.bytes_written;
        out.duration_us = st.duration_us;
        out.frames = st.video_frames;
        if (st.duration_us > 100'000ULL) {
            out.write_mbps = (double(st.bytes_written) * 8.0) / double(st.duration_us);
        }
    } else {
        out.path = w.last_path;
        out.bytes = w.last_stats.bytes_written;
        out.duration_us = w.last_stats.duration_us;
        out.frames = w.last_stats.video_frames;
    }
    return out;
}

double IsoMultiRecorder::total_write_mbps() const {
    double sum = 0.0;
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) sum += slot_stats(i).write_mbps;
    sum += program_stats().write_mbps;
    return sum;
}

std::string IsoMultiRecorder::session_dir() const {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    return session_dir_;
}

uint64_t IsoMultiRecorder::session_elapsed_us() const {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (!session_active_) return 0;
    const uint64_t now = lv::now_us();
    return now > session_t0_us_ ? (now - session_t0_us_) : 0;
}

std::string IsoMultiRecorder::session_timecode() const {
    uint64_t elapsed = 0;
    uint64_t start_f = 0;
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        if (!session_active_) return "00:00:00:00";
        start_f = session_tc_start_frame_;
        const uint64_t now = lv::now_us();
        elapsed = now > session_t0_us_ ? (now - session_t0_us_) : 0;
    }
    return lv::iso_timecode_string(start_f + (elapsed * 30ULL) / 1'000'000ULL, 30);
}

}  // namespace lv::hub
