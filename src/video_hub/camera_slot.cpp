#include "video_hub/camera_slot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/clock.hpp"
#include "net/udp_socket.hpp"

namespace lv::hub {

namespace {

std::string strip_ep_port(const std::string& ep) {
    const size_t colon = ep.rfind(':');
    return (colon == std::string::npos) ? ep : ep.substr(0, colon);
}

const char* codec_display_name(lv::VideoCodecId cid) {
    switch (cid) {
        case lv::VideoCodecId::kH264: return "H.264";
        case lv::VideoCodecId::kH265: return "H.265";
        case lv::VideoCodecId::kLanVidDct:
        default: return "DCT";
    }
}

}  // namespace

CameraSlotEngine::CameraSlotEngine(uint32_t slot_index) : slot_index_(slot_index) {
    snap_.slot_index = slot_index;
    snap_.label = "CAM " + std::to_string(slot_index + 1);
    snap_.settings.assigned_cam_number = uint8_t(std::min<uint32_t>(slot_index + 1, kMaxHubSlots));
    snap_.settings.assigned_cam_tag = "CAM " + std::to_string(snap_.settings.assigned_cam_number);

    lv::VideoPlayoutConfig pc{};
    pc.fixed_delay_mode = true;
    pc.target_delay_ms = 33;
    pc.adaptive_max_ms = 200;
    pc.idr_rate_limit_ms = 500;
    pc.max_queue_frames = 16;
    playout_.set_config(pc);
    playout_.set_idr_request_callback([this]() {
        if (demux_rx_) demux_rx_->request_keyframe();
        IdrCallback cb;
        uint32_t cam_id = 0;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            cam_id = snap_.camera_id;
        }
        {
            std::lock_guard<std::mutex> clk(cb_mu_);
            cb = idr_cb_;
        }
        if (cb) cb(slot_index_, cam_id);
    });
}

CameraSlotEngine::~CameraSlotEngine() {
    stop();
}

void CameraSlotEngine::on_sdk_video(const LV_VideoFrameView* frame, void* user_data) {
    auto* self = static_cast<CameraSlotEngine*>(user_data);
    if (!self || !frame || !frame->data || frame->width <= 0 || frame->height <= 0 ||
        !self->running_.load(std::memory_order_acquire)) {
        return;
    }

    lv::Image img;
    const lv::PixelOrder ord = (frame->pixel_format == LV_PIXEL_FORMAT_RGBA8888) ? lv::PixelOrder::kRGBA
                                                                                   : lv::PixelOrder::kBGRA;
    img.allocate(frame->width, frame->height, ord);
    const size_t row_bytes = size_t(frame->width) * 4u;
    const size_t src_stride = frame->stride > 0 ? size_t(frame->stride) : row_bytes;
    for (int y = 0; y < frame->height; ++y) {
        std::memcpy(img.data.data() + size_t(y) * img.stride, frame->data + size_t(y) * src_stride, row_bytes);
    }
    const bool corrupt = (frame->slices_total > 0 && frame->slices_ok < frame->slices_total &&
                          frame->codec != LV_CODEC_LANVID_DCT);
    const char* cname = frame->codec == LV_CODEC_H264 ? "H.264" : (frame->codec == LV_CODEC_H265 ? "H.265" : "DCT");
    self->push_decoded_frame(std::move(img), frame->frame_id, frame->timestamp_us, lv::now_us(),
                             frame->is_keyframe != 0, corrupt, 0.0, cname);
}

void CameraSlotEngine::on_sdk_audio(const LV_AudioChunkView* audio, void* user_data) {
    auto* self = static_cast<CameraSlotEngine*>(user_data);
    if (!self || !audio || !audio->data || audio->num_frames <= 0 ||
        !self->running_.load(std::memory_order_acquire)) {
        return;
    }

    lv::AudioHeader ah{};
    ah.sample_rate = uint32_t(audio->sample_rate > 0 ? audio->sample_rate : 48000);
    ah.channels = uint16_t(std::max(1, audio->channels));
    ah.sample_format = (audio->audio_format == LV_AUDIO_FORMAT_F32) ? lv::AudioSampleFormat::kFloat32
                                                                    : lv::AudioSampleFormat::kPcm16;
    ah.num_frames = uint16_t(audio->num_frames);

    float peak = 0.0f;
    const size_t count = size_t(audio->num_frames) * size_t(ah.channels);
    if (ah.sample_format == lv::AudioSampleFormat::kPcm16) {
        const auto* p = static_cast<const int16_t*>(audio->data);
        for (size_t i = 0; i < count; ++i) {
            const float v = std::fabs(float(p[i]) / 32768.0f);
            if (v > peak) peak = v;
        }
    } else {
        const auto* p = static_cast<const float*>(audio->data);
        for (size_t i = 0; i < count; ++i) {
            const float v = std::fabs(p[i]);
            if (v > peak) peak = v;
        }
    }
    const float db = peak > 1e-6f ? (20.0f * std::log10(peak)) : -120.0f;
    self->set_audio_peak(db);

    AudioCallback cb;
    {
        std::lock_guard<std::mutex> clk(self->cb_mu_);
        cb = self->audio_cb_;
    }
    if (cb) {
        cb(self->slot_index_, ah, static_cast<const uint8_t*>(audio->data));
    }
}

bool CameraSlotEngine::start_dedicated_port(const std::string& bind_ip, uint16_t listen_port,
                                            const std::string& hub_name) {
    stop();
    sdk_mode_ = true;
    sdk_rx_ = lv_receiver_create();
    if (!sdk_rx_) return false;

    LV_ReceiverConfig rcfg{};
    lv_sdk_receiver_config_default(&rcfg);
    std::snprintf(rcfg.bind_ip, sizeof(rcfg.bind_ip), "%s", bind_ip.empty() ? "0.0.0.0" : bind_ip.c_str());
    rcfg.bind_port = listen_port;
    rcfg.output_format = LV_PIXEL_FORMAT_BGRA8888;
    rcfg.decode_threads = 2;
    rcfg.conceal_mode = LV_CONCEALMENT_PREV_FRAME;
    rcfg.enable_os_audio = 0;
    rcfg.enable_discovery = 0;
    std::snprintf(rcfg.device_name, sizeof(rcfg.device_name), "%s CAM %u", hub_name.c_str(), slot_index_ + 1);

    lv_receiver_set_video_callback(sdk_rx_, &CameraSlotEngine::on_sdk_video, this);
    lv_receiver_set_audio_callback(sdk_rx_, &CameraSlotEngine::on_sdk_audio, this);

    if (lv_sdk_receiver_start(sdk_rx_, &rcfg) != LV_OK) {
        lv_receiver_destroy(sdk_rx_);
        sdk_rx_ = nullptr;
        return false;
    }

    bound_port_ = lv_receiver_get_local_port(sdk_rx_);
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        snap_.listen_port = bound_port_;
        snap_.bound = true;
        snap_.streaming = false;
        last_rate_sample_us_ = lv::now_us();
    }
    playout_.reset();
    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&CameraSlotEngine::worker_loop, this);
    return true;
}

bool CameraSlotEngine::start_demux_receiver(lv::UdpSocket* shared_sock, uint16_t shared_port) {
    stop();
    if (!shared_sock) return false;
    sdk_mode_ = false;
    bound_port_ = shared_port;

    lv::ReceiverConfig rcfg{};
    rcfg.decode_threads = 2;
    rcfg.assembler.output_order = lv::PixelOrder::kBGRA;
    rcfg.assembler.conceal = lv::ConcealMode::kPrev;
    rcfg.enable_feedback = true;
    rcfg.audio_latency_ms = 12;
    rcfg.audio_adaptive_jitter = false;

    demux_rx_ = std::make_unique<lv::UdpReceiver>(rcfg, [this](const lv::CompletedFrame& cf) {
        if (!cf.image || cf.image->width <= 0 || !running_.load(std::memory_order_acquire)) return;
        const uint64_t now = lv::now_us();
        const bool corrupt = (cf.slices_corrupt > 0 || cf.slices_lost > 0) &&
                             cf.codec != lv::VideoCodecId::kLanVidDct;
        if (demux_rx_) {
            lv::Endpoint ep{};
            if (demux_rx_->sender_endpoint(ep)) {
                const std::string ip = strip_ep_port(ep.to_string());
                std::lock_guard<std::mutex> lk(state_mu_);
                if (!ip.empty() && ip != "0.0.0.0") snap_.sender_ip = ip;
                snap_.sender_port = ep.port;
                snap_.decoder_backend = demux_rx_->decoder_backend_name();
                snap_.hardware_decode = demux_rx_->decoder_is_hardware();
            }
        }
        push_decoded_frame(*cf.image, cf.frame_id, cf.sender_timestamp_us, now, cf.is_keyframe, corrupt, 0.0,
                           codec_display_name(cf.codec));
    });

    demux_rx_->set_video_bitstream_callback([this](lv::VideoCodecId cid, const uint8_t* data, size_t size,
                                                   bool is_keyframe, uint32_t /*frame_id*/, uint64_t arr_us) {
        BitstreamCallback cb;
        {
            std::lock_guard<std::mutex> clk(cb_mu_);
            cb = bitstream_cb_;
        }
        if (cb && data && size > 0) {
            cb(slot_index_, int(cid), data, size, is_keyframe, arr_us > 0 ? arr_us : lv::now_us());
        }
    });

    demux_rx_->set_audio_callback([this](uint32_t, uint32_t, const lv::AudioHeader& ah, const uint8_t* pcm) {
        if (!pcm || ah.num_frames == 0 || !running_.load(std::memory_order_acquire)) return;
        float peak = 0.0f;
        const size_t count = size_t(ah.num_frames) * std::max<size_t>(1, ah.channels);
        if (ah.sample_format == lv::AudioSampleFormat::kPcm16) {
            const auto* p = reinterpret_cast<const int16_t*>(pcm);
            for (size_t i = 0; i < count; ++i) {
                const float v = std::fabs(float(p[i]) / 32768.0f);
                if (v > peak) peak = v;
            }
        }
        set_audio_peak(peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f);

        AudioCallback cb;
        {
            std::lock_guard<std::mutex> clk(cb_mu_);
            cb = audio_cb_;
        }
        if (cb) cb(slot_index_, ah, pcm);
    });

    if (!demux_rx_->init_external(shared_sock)) {
        demux_rx_.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(state_mu_);
        snap_.listen_port = shared_port;
        snap_.bound = true;
        snap_.streaming = false;
        last_rate_sample_us_ = lv::now_us();
    }
    playout_.reset();
    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&CameraSlotEngine::worker_loop, this);
    return true;
}

void CameraSlotEngine::stop() {
    running_.store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    dgram_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (sdk_rx_) {
        lv_receiver_stop(sdk_rx_);
        lv_receiver_destroy(sdk_rx_);
        sdk_rx_ = nullptr;
    }
    demux_rx_.reset();
    {
        std::lock_guard<std::mutex> lk(dgram_mu_);
        dgram_q_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        snap_.bound = false;
        snap_.streaming = false;
    }
}

void CameraSlotEngine::set_bitstream_callback(BitstreamCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    bitstream_cb_ = std::move(cb);
}

void CameraSlotEngine::set_audio_callback(AudioCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    audio_cb_ = std::move(cb);
}

void CameraSlotEngine::set_idr_callback(IdrCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    idr_cb_ = std::move(cb);
}

void CameraSlotEngine::set_playout_callback(PlayoutFrameCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    playout_cb_ = std::move(cb);
}

void CameraSlotEngine::enqueue_datagram(const uint8_t* data, size_t len, const lv::Endpoint& src, uint64_t now_us) {
    if (!data || len == 0 || !running_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(dgram_mu_);
        if (dgram_q_.size() >= 1024) {
            dgram_q_.pop_front();
        }
        QueuedDatagram q{};
        q.data.assign(data, data + len);
        q.src = src;
        q.arrival_us = now_us;
        dgram_q_.push_back(std::move(q));
    }
    dgram_cv_.notify_one();
}

void CameraSlotEngine::bind_sender(const std::string& ip, uint16_t port, const std::string& uid) {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (!ip.empty()) snap_.sender_ip = ip;
    if (port != 0) snap_.sender_port = port;
    if (!uid.empty()) snap_.device_uid = uid;
}

void CameraSlotEngine::release_sender() {
    playout_.reset();
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.sender_ip.clear();
    snap_.sender_port = 0;
    snap_.streaming = false;
    snap_.tally_connected = false;
    snap_.config_synced = false;
    snap_.width = 0;
    snap_.height = 0;
    snap_.fps = 0.0;
    snap_.bitrate_mbps = 0.0;
}

void CameraSlotEngine::push_decoded_frame(lv::Image img, uint32_t frame_id, uint32_t media_ts_us32,
                                          uint64_t arrival_us, bool is_keyframe, bool corrupt, double fps_hint,
                                          const std::string& codec) {
    if (img.width <= 0 || img.height <= 0) return;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        snap_.width = uint32_t(img.width);
        snap_.height = uint32_t(img.height);
        if (!codec.empty()) snap_.codec_name = codec;
        if (!corrupt) {
            ++snap_.frames_rx;
            snap_.streaming = true;
            last_rx_frame_us_ = arrival_us;
            if (fps_hint > 0.1) snap_.fps = fps_hint;
        }
    }
    playout_.push_frame(std::move(img), frame_id, media_ts_us32, arrival_us, is_keyframe, corrupt);
}

bool CameraSlotEngine::poll_playout_frame(uint64_t now_us, lv::Image& out, uint64_t& inout_seq) {
    return playout_.poll_frame(now_us, out, inout_seq);
}

void CameraSlotEngine::set_label(const std::string& label) {
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.label = label.empty() ? ("CAM " + std::to_string(slot_index_ + 1)) : label;
}

void CameraSlotEngine::set_pinned_ip(const std::string& ip) {
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.pinned_ip = ip;
}

void CameraSlotEngine::set_device_uid(const std::string& uid) {
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.device_uid = uid;
}

void CameraSlotEngine::set_playout_delay(uint32_t delay_ms, bool fixed_mode) {
    const uint32_t clamped = std::clamp<uint32_t>(delay_ms, 5, 250);
    playout_.set_delay(clamped, fixed_mode);
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.settings.video_delay_ms = uint16_t(clamped);
    snap_.settings.fixed_delay_mode = fixed_mode;
}

void CameraSlotEngine::force_idr() {
    if (demux_rx_) demux_rx_->request_keyframe();
    IdrCallback cb;
    uint32_t cam_id = 0;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        cam_id = snap_.camera_id;
    }
    {
        std::lock_guard<std::mutex> clk(cb_mu_);
        cb = idr_cb_;
    }
    if (cb) cb(slot_index_, cam_id);
}

void CameraSlotEngine::set_remote_settings(const CameraRemoteSettings& s) {
    playout_.set_delay(std::clamp<uint16_t>(s.video_delay_ms, 5, 250), s.fixed_delay_mode);
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.settings = s;
    snap_.settings.assigned_cam_number = std::clamp<uint8_t>(s.assigned_cam_number, 1, 16);
    if (snap_.settings.assigned_cam_tag.empty()) {
        snap_.settings.assigned_cam_tag = "CAM " + std::to_string(snap_.settings.assigned_cam_number);
    }
    snap_.settings.video_delay_ms = std::clamp<uint16_t>(s.video_delay_ms, 5, 250);
}

CameraRemoteSettings CameraSlotEngine::remote_settings() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return snap_.settings;
}

lv::VideoConfigSyncPayload CameraSlotEngine::build_config_sync_payload(uint32_t origin_id) {
    std::lock_guard<std::mutex> lk(state_mu_);
    lv::VideoConfigSyncPayload p{};
    p.config_revision = ++local_config_rev_;
    p.origin_id = origin_id;
    p.camera_id = snap_.camera_id;
    p.assigned_cam_number = snap_.settings.assigned_cam_number;
    p.resolution_preset = snap_.settings.resolution_preset;
    p.target_fps = snap_.settings.target_fps;
    p.codec = snap_.settings.codec;
    p.bitrate_kbps = snap_.settings.bitrate_kbps;
    p.video_delay_ms = snap_.settings.video_delay_ms;
    p.fixed_delay_mode = snap_.settings.fixed_delay_mode ? 1 : 0;
    p.camera_lens = snap_.settings.camera_lens;
    p.flags = lv::kVideoConfigFlagHasBadgeColor;
    std::memcpy(p.badge_accent_rgba, snap_.settings.badge_accent_rgba, 4);
    std::snprintf(p.assigned_cam_tag, sizeof(p.assigned_cam_tag), "%s", snap_.settings.assigned_cam_tag.c_str());
    return p;
}

bool CameraSlotEngine::apply_incoming_config_sync(const lv::VideoConfigSyncPayload& p, uint32_t local_origin_id) {
    if (local_origin_id != 0 && p.origin_id == local_origin_id) {
        return false;  // Echo Guard
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    if (p.config_revision != 0 && p.config_revision == last_remote_rev_) {
        return false;  // Deduplicate repeated redundant UDP copy
    }
    last_remote_rev_ = p.config_revision;
    if (p.assigned_cam_number >= 1 && p.assigned_cam_number <= 16) {
        snap_.settings.assigned_cam_number = p.assigned_cam_number;
    }
    if (p.assigned_cam_tag[0] != '\0') {
        snap_.settings.assigned_cam_tag = p.assigned_cam_tag;
    }
    if ((p.flags & lv::kVideoConfigFlagHasBadgeColor) != 0) {
        std::memcpy(snap_.settings.badge_accent_rgba, p.badge_accent_rgba, 4);
    }
    if (p.resolution_preset <= lv::kQuality4K) snap_.settings.resolution_preset = p.resolution_preset;
    if (p.target_fps >= 15 && p.target_fps <= 120) snap_.settings.target_fps = p.target_fps;
    if (p.codec <= 2) snap_.settings.codec = p.codec;
    if (p.bitrate_kbps >= 1000 && p.bitrate_kbps <= 150000) snap_.settings.bitrate_kbps = p.bitrate_kbps;
    if (p.camera_lens <= lv::kLensFront) snap_.settings.camera_lens = p.camera_lens;
    if (p.video_delay_ms >= 5 && p.video_delay_ms <= 250) {
        snap_.settings.video_delay_ms = p.video_delay_ms;
        snap_.settings.fixed_delay_mode = p.fixed_delay_mode != 0;
        playout_.set_delay(p.video_delay_ms, p.fixed_delay_mode != 0);
    }
    snap_.config_synced = true;
    return true;
}

void CameraSlotEngine::merge_tally_telemetry(uint32_t camera_id, const std::string& device_name,
                                             const std::string& ip, bool connected, uint8_t battery_pct,
                                             int8_t temp_c, uint8_t tally, bool rec_trigger, bool op_speaking) {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (camera_id != 0) snap_.camera_id = camera_id;
    if (!device_name.empty()) snap_.source_name = device_name;
    if (!ip.empty()) snap_.sender_ip = ip;
    snap_.tally_connected = connected;
    snap_.battery_pct = battery_pct;
    snap_.temp_c = temp_c;
    snap_.tally = tally;
    snap_.rec_trigger_active = rec_trigger;
    snap_.operator_speaking = op_speaking;
}

void CameraSlotEngine::set_intercom_routing(bool talk_to_cam, bool listen_from_cam) {
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.talk_to_cam = talk_to_cam;
    snap_.listen_from_cam = listen_from_cam;
}

void CameraSlotEngine::set_audio_peak(float peak_dbfs) {
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.audio_peak_dbfs = peak_dbfs;
}

void CameraSlotEngine::mark_config_synced(bool synced) {
    std::lock_guard<std::mutex> lk(state_mu_);
    snap_.config_synced = synced;
}

uint16_t CameraSlotEngine::listen_port() const {
    return bound_port_;
}

CameraSlotSnapshot CameraSlotEngine::snapshot() const {
    CameraSlotSnapshot out;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        out = snap_;
    }
    const auto pst = playout_.stats();
    out.playout_delay_ms = pst.effective_delay_ms;
    out.playout_jitter_ms = pst.jitter_ms;
    out.playout_held_frames = pst.frames_held;
    out.playout_dropped_frames = pst.frames_dropped_late + pst.frames_dropped_corrupt;
    out.playout_idr_requests = pst.idr_requests;
    return out;
}

void CameraSlotEngine::push_history_locked(float jitter_ms, float loss_pct) {
    if (snap_.history_count < kTelemetryHistoryLen) {
        snap_.jitter_history[snap_.history_count] = jitter_ms;
        snap_.loss_history[snap_.history_count] = loss_pct;
        ++snap_.history_count;
    } else {
        std::move(snap_.jitter_history.begin() + 1, snap_.jitter_history.end(), snap_.jitter_history.begin());
        std::move(snap_.loss_history.begin() + 1, snap_.loss_history.end(), snap_.loss_history.begin());
        snap_.jitter_history.back() = jitter_ms;
        snap_.loss_history.back() = loss_pct;
    }
}

void CameraSlotEngine::update_telemetry_and_history(uint64_t now_us) {
    const auto pst = playout_.stats();
    if (sdk_mode_ && sdk_rx_) {
        LV_StreamStats st{};
        if (lv_receiver_get_stream_stats(sdk_rx_, &st) == LV_OK) {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (st.width > 0 && st.height > 0) {
                snap_.width = uint32_t(st.width);
                snap_.height = uint32_t(st.height);
            }
            if (st.fps > 0.1) snap_.fps = st.fps;
            snap_.bitrate_mbps = st.bitrate_mbps;
            snap_.loss_pct = st.packet_loss_pct;
            snap_.decoder_backend = st.codec_backend;
            snap_.hardware_decode = st.codec_is_hardware != 0;
            snap_.streaming = (last_rx_frame_us_ > 0 && (now_us - last_rx_frame_us_) < 2'000'000ULL);
            if (now_us - last_rate_sample_us_ >= 250'000ULL) {
                last_rate_sample_us_ = now_us;
                push_history_locked(float(pst.jitter_ms), float(snap_.loss_pct));
            }
        }
        return;
    }

    if (demux_rx_) {
        const auto& ast = demux_rx_->assembler_stats();
        const double loss = demux_rx_->seq_tracker().loss_percent();
        std::lock_guard<std::mutex> lk(state_mu_);
        snap_.loss_pct = loss;
        snap_.streaming = (last_rx_frame_us_ > 0 && (now_us - last_rx_frame_us_) < 2'000'000ULL);
        const uint64_t dt_us = now_us - last_rate_sample_us_;
        if (dt_us >= 250'000ULL) {
            const double sec = double(dt_us) / 1e6;
            const uint64_t df = snap_.frames_rx - prev_frames_rx_;
            const uint64_t db = ast.bytes_total - prev_bytes_rx_;
            prev_frames_rx_ = snap_.frames_rx;
            prev_bytes_rx_ = ast.bytes_total;
            last_rate_sample_us_ = now_us;
            if (df > 0) snap_.fps = double(df) / sec;
            else if (!snap_.streaming) snap_.fps = 0.0;
            snap_.bitrate_mbps = (double(db) * 8.0) / (sec * 1e6);
            push_history_locked(float(pst.jitter_ms), float(snap_.loss_pct));
        }
    }
}

void CameraSlotEngine::worker_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        if (!sdk_mode_ && demux_rx_) {
            std::deque<QueuedDatagram> batch;
            {
                std::unique_lock<std::mutex> lk(dgram_mu_);
                if (dgram_q_.empty()) {
                    dgram_cv_.wait_for(lk, std::chrono::milliseconds(4),
                                       [this]() { return !dgram_q_.empty() || stop_.load(std::memory_order_relaxed); });
                }
                batch.swap(dgram_q_);
            }
            for (const auto& q : batch) {
                demux_rx_->ingest_datagram(q.data.data(), q.data.size(), q.src, q.arrival_us);
            }
            demux_rx_->service(lv::now_us());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }

        const uint64_t now = lv::now_us();
        lv::Image out;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> plk(playout_pub_mu_);
            seq = playout_pub_seq_;
            if (playout_.poll_frame(now, out, seq)) {
                playout_pub_seq_ = seq;
            } else {
                out.width = 0;
            }
        }
        if (out.width > 0) {
            PlayoutFrameCallback cb;
            double fps = 30.0;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                fps = snap_.fps > 0.1 ? snap_.fps : double(snap_.settings.target_fps);
            }
            {
                std::lock_guard<std::mutex> clk(cb_mu_);
                cb = playout_cb_;
            }
            if (cb) cb(slot_index_, out, fps, now);
        }

        update_telemetry_and_history(now);
    }
}

}  // namespace lv::hub
