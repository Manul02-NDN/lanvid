#include "obs_lanvid_core.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "common/clock.hpp"
#include "net/discovery.hpp"
#include "net/network_interfaces.hpp"
#include "net/receiver.hpp"

namespace lv::obs {

namespace {

std::atomic<uint64_t> g_next_source_uid{1};

uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t now_us32() {
    return static_cast<uint32_t>(lv::now_us() & 0xFFFFFFFFull);
}

std::string strip_port(const std::string& addr) {
    const auto pos = addr.rfind(':');
    return pos == std::string::npos ? addr : addr.substr(0, pos);
}

}  // namespace

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
ObsTallyDecision evaluate_auto_tally(const ObsTallyInputs& in) {
    ObsTallyDecision d{};
    switch (in.mode) {
        case TallyControlMode::kStandby:
            d.tally_state = lv::kTallyStandby;
            break;
        case TallyControlMode::kPreview:
            d.tally_state = lv::kTallyPreview;
            break;
        case TallyControlMode::kProgram:
            d.tally_state = lv::kTallyProgram;
            break;
        case TallyControlMode::kAuto:
        default: {
            const bool is_live = in.obs_streaming || in.obs_recording;
            if (in.in_program) {
                d.tally_state = (in.only_on_air_when_live && !is_live) ? lv::kTallyPreview : lv::kTallyProgram;
            } else if (in.studio_mode && in.in_preview) {
                d.tally_state = lv::kTallyPreview;
            } else {
                d.tally_state = lv::kTallyStandby;
            }
            break;
        }
    }
    d.rec_trigger = in.sync_remote_rec && in.obs_recording;
    return d;
}

uint32_t jitter_preset_to_ms(JitterPreset preset) {
    switch (preset) {
        case JitterPreset::kFixed16ms: return 16;
        case JitterPreset::kFixed33ms: return 33;
        case JitterPreset::kFixed50ms: return 50;
        case JitterPreset::kFixed80ms: return 80;
        case JitterPreset::kAdaptiveWifi: return 33;
        case JitterPreset::kCustomFixed:
        default: return 33;
    }
}

LV_ControlSyncState make_quality_preset_control_sync(CameraQualityPreset preset, uint64_t revision, uint32_t origin_id) {
    LV_ControlSyncState st{};
    st.revision = revision;
    st.origin_id = origin_id;
    st.audio_channels = 2;
    st.audio_bitrate_kbps = 0;
    st.audio_only = 0;
    st.flags = 0;
    st.master_gain_cb = 0;
    st.audio_codec = 0;
    switch (preset) {
        case CameraQualityPreset::kProxy360p:
            st.active_preset_id = 0;
            st.target_buffer_ms = 20;
            st.latency_mode = 1;
            break;
        case CameraQualityPreset::kHd720p:
        case CameraQualityPreset::kStudio1080p:
            st.active_preset_id = 1;
            st.target_buffer_ms = 15;
            st.latency_mode = 1;
            break;
        case CameraQualityPreset::kMaster4K60:
        default:
            st.active_preset_id = 2;
            st.target_buffer_ms = 10;
            st.latency_mode = 0;
            break;
    }
    return st;
}

// -----------------------------------------------------------------------------
// LanVidObsSourceEngine
// -----------------------------------------------------------------------------
std::shared_ptr<LanVidObsSourceEngine> LanVidObsSourceEngine::create() {
    return std::make_shared<LanVidObsSourceEngine>();
}

LanVidObsSourceEngine::LanVidObsSourceEngine() : uid_(g_next_source_uid.fetch_add(1, std::memory_order_relaxed)) {}

LanVidObsSourceEngine::~LanVidObsSourceEngine() {
    stop();
}

bool LanVidObsSourceEngine::start(const SourceConfig& cfg, VideoCallback on_video, AudioCallback on_audio) {
    stop();
    {
        std::lock_guard<std::mutex> cb_lk(cb_mu_);
        on_video_ = std::move(on_video);
        on_audio_ = std::move(on_audio);
    }

    SourceConfig effective_cfg = cfg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (effective_cfg.video_delay_ms < 5 || effective_cfg.video_delay_ms > 250) {
            effective_cfg.video_delay_ms = jitter_preset_to_ms(effective_cfg.jitter_preset);
        }
        if (effective_cfg.assigned_cam_tag.empty() && effective_cfg.assigned_cam_number > 0) {
            effective_cfg.assigned_cam_tag = "CAM " + std::to_string(effective_cfg.assigned_cam_number);
        }
        cfg_ = effective_cfg;
        last_w_ = 0;
        last_h_ = 0;
        frames_rx_ = 0;
        dropped_frames_ = 0;
        measured_fps_ = 0.0;
        measured_mbps_ = 0.0;
        measured_loss_pct_ = 0.0;
        detected_peer_ip_.clear();
        last_rx_frame_us_ = 0;
        last_placeholder_us_ = 0;
        playout_seq_ = 0;
        tally_mode_ = effective_cfg.auto_tally ? TallyControlMode::kAuto : TallyControlMode::kStandby;
    }

    VideoPlayoutConfig pc{};
    pc.fixed_delay_mode = effective_cfg.fixed_delay_mode && (effective_cfg.jitter_preset != JitterPreset::kAdaptiveWifi);
    pc.target_delay_ms = std::clamp<uint32_t>(effective_cfg.video_delay_ms, 5, 250);
    playout_.set_config(pc);
    playout_.reset();
    std::weak_ptr<LanVidObsSourceEngine> weak_self = weak_from_this();
    playout_.set_idr_request_callback([weak_self]() {
        if (auto self = weak_self.lock()) self->request_idr_keyframe();
    });

    if (effective_cfg.shm_slot >= 0) {
        const char* bname = effective_cfg.shm_bus_name.empty() ? nullptr : effective_cfg.shm_bus_name.c_str();
        shm_reader_.open(bname);
    } else {
        ReceiverConfig rcfg{};
        rcfg.bind_ip = effective_cfg.bind_ip.empty() ? "0.0.0.0" : effective_cfg.bind_ip;
        rcfg.bind_port = effective_cfg.listen_port;
        rcfg.decode_threads = 2;
        rcfg.assembler.output_order = PixelOrder::kBGRA;
        rcfg.assembler.conceal = ConcealMode::kPrev;
        rcfg.audio_target_ms = double(effective_cfg.audio_latency_ms > 0 ? effective_cfg.audio_latency_ms : 20);
        rcfg.audio_latency_ms = std::clamp<uint32_t>(effective_cfg.audio_latency_ms, 2, 2000);
        rcfg.audio_adaptive_jitter = true;

        rx_ = std::make_unique<lv::UdpReceiver>(
            rcfg, [this](const CompletedFrame& cf) {
                if (!running_.load(std::memory_order_acquire) || !cf.image || cf.image->width <= 0) return;
                const uint64_t now = lv::now_us();
                const bool corrupt = (cf.slices_corrupt > 0 || cf.slices_lost > 0) &&
                                     cf.codec != VideoCodecId::kLanVidDct;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    last_rx_frame_us_ = now;
                    codec_name_ = video_codec_name(cf.codec);
                    if (rx_) {
                        Endpoint ep{};
                        if (rx_->sender_endpoint(ep)) detected_peer_ip_ = strip_port(ep.to_string());
                    }
                }
                playout_.push_frame(*cf.image, cf.frame_id, cf.sender_timestamp_us, now, cf.is_keyframe, corrupt);
            });

        rx_->set_audio_callback([this](uint32_t, uint32_t ts_us, const AudioHeader& ah, const uint8_t* pcm) {
            if (!running_.load(std::memory_order_acquire) || !pcm || ah.num_frames == 0) return;
            AudioRouteMode route;
            {
                std::lock_guard<std::mutex> lk(mu_);
                route = cfg_.audio_route;
            }
            if (route == AudioRouteMode::kVideoOnly || cfg_.audio_channels > 2) return;
            const size_t samples = size_t(ah.num_frames) * ah.channels;
            std::vector<float> tmp(samples);
            if (ah.sample_format == AudioSampleFormat::kPcm16) {
                const auto* p16 = reinterpret_cast<const int16_t*>(pcm);
                for (size_t i = 0; i < samples; ++i) tmp[i] = float(p16[i]) / 32768.0f;
            } else {
                const auto* pf = reinterpret_cast<const float*>(pcm);
                std::memcpy(tmp.data(), pf, samples * sizeof(float));
            }
            route_multichannel_audio(tmp.data(), ah.channels, ah.num_frames, ah.sample_rate,
                                     uint64_t(ts_us) * 1000ull);
        });

        if (!rx_->init()) {
            rx_.reset();
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        last_rx_frame_us_ = lv::now_us();
        last_placeholder_us_ = 0;
    }
    stop_worker_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    if (auto sp = weak_from_this().lock()) {
        LanVidObsRuntime::instance().register_source(sp);
    }
    worker_ = std::thread(&LanVidObsSourceEngine::worker_loop, this);
    return true;
}

void LanVidObsSourceEngine::stop() {
    running_.store(false, std::memory_order_release);
    stop_worker_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    LanVidObsRuntime::instance().unregister_source(uid_);
    rx_.reset();
    shm_reader_.close();
    {
        std::lock_guard<std::mutex> cb_lk(cb_mu_);
        on_video_ = nullptr;
        on_audio_ = nullptr;
    }
}

void LanVidObsSourceEngine::update_config(const SourceConfig& cfg) {
    bool needs_restart = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        needs_restart = (cfg.listen_port != cfg_.listen_port) || (cfg.bind_ip != cfg_.bind_ip) ||
                        (cfg.shm_slot != cfg_.shm_slot) || (cfg.shm_bus_name != cfg_.shm_bus_name);
        cfg_ = cfg;
        const bool fixed = cfg.fixed_delay_mode && (cfg.jitter_preset != JitterPreset::kAdaptiveWifi);
        const uint32_t delay_ms = (cfg.video_delay_ms >= 5 && cfg.video_delay_ms <= 250)
                                      ? cfg.video_delay_ms
                                      : jitter_preset_to_ms(cfg.jitter_preset);
        playout_.set_delay(delay_ms, fixed);
        if (rx_ && !needs_restart) {
            rx_->set_audio_latency_ms(std::clamp<uint32_t>(cfg.audio_latency_ms, 2, 2000));
            rx_->set_audio_adaptive_jitter(!fixed);
        }
    }
    if (needs_restart && running_.load(std::memory_order_acquire)) {
        VideoCallback vcb;
        AudioCallback acb;
        {
            std::lock_guard<std::mutex> cb_lk(cb_mu_);
            vcb = on_video_;
            acb = on_audio_;
        }
        start(cfg, std::move(vcb), std::move(acb));
    }
    LanVidObsRuntime::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::request_idr_keyframe() {
    uint32_t cam_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cam_id = cfg_.camera_id;
    }
    if (rx_) rx_->request_keyframe();
    if (cam_id > 0) {
        LanVidObsRuntime::instance().tally_engine().studio_request_keyframe(cam_id);
    }
}

void LanVidObsSourceEngine::reset_buffers() {
    reset_requested_.store(true, std::memory_order_release);
    playout_.reset();
}

bool LanVidObsSourceEngine::assign_camera_number(uint8_t cam_number, const std::string& tag) {
    VideoConfigSyncPayload vc{};
    uint32_t cam_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_.assigned_cam_number = std::clamp<uint8_t>(cam_number, 1, 16);
        cfg_.assigned_cam_tag = tag.empty() ? ("CAM " + std::to_string(cfg_.assigned_cam_number)) : tag.substr(0, 15);
        cam_id = cfg_.camera_id;
        vc.config_revision = ++config_sync_rev_;
        vc.camera_id = cam_id;
        vc.assigned_cam_number = cfg_.assigned_cam_number;
        std::snprintf(vc.assigned_cam_tag, sizeof(vc.assigned_cam_tag), "%s", cfg_.assigned_cam_tag.c_str());
        vc.video_delay_ms = uint16_t(std::clamp<uint32_t>(cfg_.video_delay_ms, 5, 250));
        vc.fixed_delay_mode = cfg_.fixed_delay_mode ? 1 : 0;
        vc.bitrate_kbps = 0;
    }
    const bool ok = LanVidObsRuntime::instance().tally_engine().send_video_config_sync(cam_id, vc);
    if (ok) {
        std::lock_guard<std::mutex> lk(mu_);
        config_synced_ = true;
    }
    LanVidObsRuntime::instance().sync_all_tallies();
    return ok;
}

bool LanVidObsSourceEngine::send_quality_preset(CameraQualityPreset preset) {
    RemoteCameraControl ctl{};
    ctl.quality = preset;
    switch (preset) {
        case CameraQualityPreset::kProxy360p:
            ctl.fps = 30;
            ctl.bitrate_kbps = 4000;
            break;
        case CameraQualityPreset::kHd720p:
            ctl.fps = 60;
            ctl.bitrate_kbps = 12000;
            break;
        case CameraQualityPreset::kStudio1080p:
            ctl.fps = 60;
            ctl.bitrate_kbps = 25000;
            break;
        case CameraQualityPreset::kMaster4K60:
        default:
            ctl.fps = 60;
            ctl.bitrate_kbps = 85000;
            break;
    }
    return send_remote_camera_control(ctl);
}

bool LanVidObsSourceEngine::send_remote_camera_control(const RemoteCameraControl& ctl) {
    uint32_t cam_id = 0;
    VideoConfigSyncPayload vc{};
    {
        std::lock_guard<std::mutex> lk(mu_);
        cam_id = cfg_.camera_id;
        vc.config_revision = ++config_sync_rev_;
        vc.camera_id = cam_id;
        vc.assigned_cam_number = cfg_.assigned_cam_number;
        std::snprintf(vc.assigned_cam_tag, sizeof(vc.assigned_cam_tag), "%s", cfg_.assigned_cam_tag.c_str());
        vc.resolution_preset = uint8_t(ctl.quality);
        vc.target_fps = ctl.fps ? ctl.fps : 60;
        vc.codec = ctl.codec;
        vc.bitrate_kbps = ctl.bitrate_kbps;
        vc.video_delay_ms = std::clamp<uint16_t>(ctl.video_delay_ms, 5, 250);
        vc.fixed_delay_mode = ctl.fixed_delay_mode ? 1 : 0;
        vc.camera_lens = ctl.lens;
    }
    auto& ti = LanVidObsRuntime::instance().tally_engine();
    const bool ok = ti.send_video_config_sync(cam_id, vc);
    CameraControlPayload legacy{};
    legacy.camera_id = cam_id;
    legacy.lens = ctl.lens;
    legacy.quality = uint8_t(ctl.quality);
    legacy.focus_auto = 1;
    ti.studio_set_camera_control(cam_id, legacy);
    if (ok) {
        std::lock_guard<std::mutex> lk(mu_);
        config_synced_ = true;
    }
    return ok;
}

void LanVidObsSourceEngine::set_obs_visibility(bool in_program, bool in_preview) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        in_program_ = in_program;
        in_preview_ = in_preview;
    }
    LanVidObsRuntime::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::set_tally_mode(TallyControlMode mode) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tally_mode_ = mode;
    }
    LanVidObsRuntime::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::set_manual_rec(bool rec_active) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        manual_rec_ = rec_active;
    }
    LanVidObsRuntime::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::emit_video_image(const Image& img, uint64_t ts_ns, ColorSpace cs) {
    if (img.width <= 0 || img.height <= 0 || img.data.empty()) return;
    VideoCallback cb;
    {
        std::lock_guard<std::mutex> cb_lk(cb_mu_);
        cb = on_video_;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_w_ = uint32_t(img.width);
        last_h_ = uint32_t(img.height);
        ++frames_rx_;
    }
    if (cb) {
        VideoFrameView fv{};
        fv.data[0] = img.data.data();
        fv.linesize[0] = img.stride > 0 ? uint32_t(img.stride) : uint32_t(img.width) * 4u;
        fv.width = uint32_t(img.width);
        fv.height = uint32_t(img.height);
        fv.timestamp_ns = ts_ns > 0 ? ts_ns : now_ns();
        fv.format = VideoPixelFormat::kBGRA;
        fv.color_space = cs;
        fv.full_range = true;
        cb(fv);
    }
}

void LanVidObsSourceEngine::ingest_video_frame_bgra(const uint8_t* bgra, uint32_t width, uint32_t height,
                                                    uint32_t stride, uint64_t timestamp_ns, ColorSpace cs,
                                                    bool is_keyframe, uint32_t media_ts_us32) {
    if (!bgra || width == 0 || height == 0) return;
    const uint32_t src_stride = stride > 0 ? stride : width * 4u;
    Image img;
    img.allocate(int(width), int(height), PixelOrder::kBGRA);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(img.data.data() + size_t(y) * size_t(img.stride), bgra + size_t(y) * src_stride, size_t(width) * 4u);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_rx_frame_us_ = lv::now_us();
    }
    if (media_ts_us32 > 0) {
        playout_.push_frame(std::move(img), uint32_t(frames_rx_ + 1), media_ts_us32, lv::now_us(), is_keyframe, false);
        Image out;
        if (playout_.poll_frame(lv::now_us(), out, playout_seq_)) {
            emit_video_image(out, timestamp_ns, cs);
        }
    } else {
        emit_video_image(img, timestamp_ns, cs);
    }
}

void LanVidObsSourceEngine::route_multichannel_audio(const float* interleaved, uint16_t num_channels,
                                                     uint32_t num_frames, uint32_t sample_rate,
                                                     uint64_t timestamp_ns) {
    if (!interleaved || num_channels == 0 || num_frames == 0) return;
    AudioRouteMode route;
    int pair_idx = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        route = cfg_.audio_route;
        pair_idx = cfg_.audio_pair_index;
    }
    AudioCallback cb;
    {
        std::lock_guard<std::mutex> cb_lk(cb_mu_);
        cb = on_audio_;
    }
    if (route == AudioRouteMode::kVideoOnly || !cb) return;

    audio_l_.resize(num_frames);
    audio_r_.resize(num_frames);

    if (route == AudioRouteMode::kMonoSum) {
        const float inv = 1.0f / static_cast<float>(num_channels);
        for (uint32_t f = 0; f < num_frames; ++f) {
            float sum = 0.0f;
            const float* row = interleaved + size_t(f) * num_channels;
            for (uint16_t c = 0; c < num_channels; ++c) sum += row[c];
            const float s = sum * inv;
            audio_l_[f] = s;
            audio_r_[f] = s;
        }
    } else {
        const uint16_t ch_l = (route == AudioRouteMode::kStereoPair)
                                  ? static_cast<uint16_t>(std::min<int>(pair_idx * 2, num_channels - 1))
                                  : 0;
        const uint16_t ch_r = (route == AudioRouteMode::kStereoPair)
                                  ? static_cast<uint16_t>(std::min<int>(pair_idx * 2 + 1, num_channels - 1))
                                  : static_cast<uint16_t>(std::min<int>(1, num_channels - 1));
        for (uint32_t f = 0; f < num_frames; ++f) {
            const float* row = interleaved + size_t(f) * num_channels;
            audio_l_[f] = row[ch_l];
            audio_r_[f] = row[ch_r];
        }
    }

    AudioBlockView av{};
    av.planes[0] = audio_l_.data();
    av.planes[1] = audio_r_.data();
    av.frames = num_frames;
    av.sample_rate = sample_rate > 0 ? sample_rate : 48000;
    av.channels = 2;
    av.timestamp_ns = timestamp_ns > 0 ? timestamp_ns : now_ns();
    cb(av);
}

uint16_t LanVidObsSourceEngine::bound_port() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rx_ ? rx_->local_port() : cfg_.listen_port;
}

SourceConfig LanVidObsSourceEngine::config() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

ObsTallyDecision LanVidObsSourceEngine::compute_current_tally(bool studio_mode, bool streaming, bool recording) const {
    std::lock_guard<std::mutex> lk(mu_);
    ObsTallyInputs in{};
    in.in_program = in_program_;
    in.in_preview = in_preview_;
    in.studio_mode = studio_mode;
    in.obs_streaming = streaming;
    in.obs_recording = recording;
    in.only_on_air_when_live = cfg_.only_on_air_when_live;
    in.sync_remote_rec = cfg_.sync_remote_rec;
    in.mode = tally_mode_;
    auto d = evaluate_auto_tally(in);
    if (manual_rec_) d.rec_trigger = true;
    return d;
}

SourceTelemetry LanVidObsSourceEngine::telemetry() const {
    const auto ps = playout_.stats();
    SourceTelemetry t{};
    std::lock_guard<std::mutex> lk(mu_);
    t.uid = uid_;
    t.camera_id = cfg_.camera_id;
    t.assigned_cam_number = cfg_.assigned_cam_number;
    t.assigned_cam_tag = cfg_.assigned_cam_tag;
    t.name = cfg_.source_name;
    t.peer_ip = !cfg_.peer_ip.empty() ? cfg_.peer_ip : detected_peer_ip_;
    t.listen_port = rx_ ? rx_->local_port() : cfg_.listen_port;
    t.shm_slot = cfg_.shm_slot;
    t.width = last_w_;
    t.height = last_h_;
    t.fps = measured_fps_;
    t.codec_name = codec_name_;
    t.mbps = measured_mbps_;
    t.loss_pct = measured_loss_pct_;
    t.playout_delay_ms = ps.effective_delay_ms;
    t.playout_jitter_ms = ps.jitter_ms;
    t.frames_rx = frames_rx_;
    t.dropped_frames = dropped_frames_ + ps.frames_dropped_late + ps.frames_dropped_corrupt;
    t.effective_tally = last_effective_tally_;
    t.tally_mode = tally_mode_;
    t.remote_rec_active = manual_rec_;
    t.connected = (last_rx_frame_us_ != 0 && (lv::now_us() - last_rx_frame_us_) < 1'500'000ull);
    t.config_synced = config_synced_;
    return t;
}

void LanVidObsSourceEngine::emit_placeholder_if_needed(uint64_t now_us_val) {
    bool show_ph = false;
    std::string line1, line2;
    {
        std::lock_guard<std::mutex> lk(mu_);
        show_ph = cfg_.show_placeholder;
        if (!show_ph) return;
        if (last_rx_frame_us_ != 0 && now_us_val - last_rx_frame_us_ < 900'000ull) return;
        if (now_us_val - last_placeholder_us_ < 200'000ull) return;  // 5 fps placeholder card
        last_placeholder_us_ = now_us_val;
        line1 = cfg_.assigned_cam_tag.empty() ? cfg_.source_name : cfg_.assigned_cam_tag;
        line2 = cfg_.shm_slot >= 0 ? "LanVid Video SHM • Standby" : "No Signal / Waiting for Camera";
    }
    constexpr uint32_t kW = 1280, kH = 720;
    placeholder_buf_.resize(size_t(kW) * kH * 4);
    lv::video_bus_render_placeholder_bgra(placeholder_buf_.data(), kW, kH, line1.c_str(), line2.c_str(),
                                          now_us_val / 1000ull);
    VideoCallback cb;
    {
        std::lock_guard<std::mutex> cb_lk(cb_mu_);
        cb = on_video_;
    }
    if (cb) {
        VideoFrameView fv{};
        fv.data[0] = placeholder_buf_.data();
        fv.linesize[0] = kW * 4u;
        fv.width = kW;
        fv.height = kH;
        fv.timestamp_ns = now_ns();
        fv.format = VideoPixelFormat::kBGRA;
        fv.color_space = ColorSpace::kRec709;
        fv.full_range = true;
        cb(fv);
    }
}

void LanVidObsSourceEngine::worker_loop() {
    constexpr uint32_t kAudioBlock = 480;
    uint64_t last_stats_us = 0;
    uint64_t prev_bytes = 0, prev_bytes_us = 0;
    uint64_t prev_frames = 0;

    while (!stop_worker_.load(std::memory_order_acquire)) {
        const uint64_t now = lv::now_us();

        if (reset_requested_.exchange(false, std::memory_order_acq_rel)) {
            playout_.reset();
            if (rx_) {
                uint32_t alat = 20;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    alat = cfg_.audio_latency_ms;
                }
                rx_->set_audio_latency_ms(std::clamp<uint32_t>(alat, 2, 2000));
            }
        }

        int shm_slot = -1;
        std::string bus_name;
        {
            std::lock_guard<std::mutex> lk(mu_);
            shm_slot = cfg_.shm_slot;
            bus_name = cfg_.shm_bus_name;
        }

        if (shm_slot >= 0) {
            // Shared Memory LanVidVideoBus_v1 source (zero-latency mirror from LanVid Video)
            if (!shm_reader_.is_open()) {
                shm_reader_.open(bus_name.empty() ? nullptr : bus_name.c_str());
            }
            if (shm_reader_.is_open()) {
                if (shm_buf_.empty()) shm_buf_.resize(lv::kVideoBusFrameBytes);
                lv::VideoBus::FrameInfo fi{};
                if (shm_reader_.read_latest(uint32_t(shm_slot), shm_buf_.data(), shm_buf_.size(), fi) &&
                    fi.width > 0 && fi.height > 0) {
                    const bool new_seq = (fi.frame_seq != playout_seq_);
                    if (new_seq) {
                        playout_seq_ = fi.frame_seq;
                        Image bgra;
                        bgra.allocate(int(fi.width), int(fi.height), PixelOrder::kBGRA);
                        lv::video_bus_convert_to_bgra(shm_buf_.data(), fi, bgra.data.data(), fi.width, fi.height,
                                                      uint32_t(bgra.stride));
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            last_rx_frame_us_ = now;
                            codec_name_ = "SHM";
                        }
                        emit_video_image(bgra, now_ns(), ColorSpace::kRec709);
                    }
                }
            }
            emit_placeholder_if_needed(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        if (!rx_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 1. Step UDP receiver (handles video decode, audio2, clock sync, feedback)
        rx_->step_once();

        // 2. Poll VideoPlayoutBuffer on schedule (absorbs Wi-Fi jitter without blocking UI)
        Image due_frame;
        if (playout_.poll_frame(now, due_frame, playout_seq_)) {
            emit_video_image(due_frame, now_ns(), ColorSpace::kRec709);
        } else {
            emit_placeholder_if_needed(now);
        }

        // 3. Pull 64ch AoIP audio
        AudioRouteMode route;
        uint32_t num_ch = 2;
        {
            std::lock_guard<std::mutex> lk(mu_);
            route = cfg_.audio_route;
            num_ch = cfg_.audio_channels;
        }
        if (route != AudioRouteMode::kVideoOnly && num_ch > 2) {
            audio_interleaved_.resize(size_t(kAudioBlock) * num_ch);
            rx_->pop_audio_multichannel_interleaved(audio_interleaved_.data(), num_ch, kAudioBlock);
            route_multichannel_audio(audio_interleaved_.data(), static_cast<uint16_t>(num_ch), kAudioBlock, 48000,
                                     now_ns());
        }

        // 4. Update live stats at ~4 Hz
        if (now - last_stats_us >= 250'000ull) {
            last_stats_us = now;
            const auto as = rx_->assembler_stats();
            const double loss = rx_->seq_tracker().loss_percent();
            const uint64_t b = as.bytes_total;
            double mbps = 0.0;
            double fps = 0.0;
            if (prev_bytes_us != 0 && now > prev_bytes_us) {
                const double dt_s = double(now - prev_bytes_us) / 1e6;
                mbps = double(b >= prev_bytes ? b - prev_bytes : 0) * 8.0 / (dt_s * 1e6);
                std::lock_guard<std::mutex> lk(mu_);
                fps = double(frames_rx_ >= prev_frames ? frames_rx_ - prev_frames : 0) / dt_s;
                prev_frames = frames_rx_;
            }
            prev_bytes = b;
            prev_bytes_us = now;
            std::lock_guard<std::mutex> lk(mu_);
            measured_fps_ = fps;
            measured_mbps_ = mbps;
            measured_loss_pct_ = loss;
            dropped_frames_ = as.frames_concealed;
        }
    }
}

// -----------------------------------------------------------------------------
// LanVidObsOutputEngine
// -----------------------------------------------------------------------------
LanVidObsOutputEngine::LanVidObsOutputEngine() = default;

LanVidObsOutputEngine::~LanVidObsOutputEngine() {
    stop();
}

bool LanVidObsOutputEngine::start(const OutputConfig& cfg) {
    stop();
    tx_ = lv_sender_create();
    if (!tx_) return false;

    LV_SenderConfig scfg{};
    lv_sdk_sender_config_default(&scfg);
    std::snprintf(scfg.dest_ip, sizeof(scfg.dest_ip), "%s", cfg.dest_ip.c_str());
    scfg.dest_port = cfg.dest_port;
    scfg.width = static_cast<int>(cfg.width);
    scfg.height = static_cast<int>(cfg.height);
    scfg.fps = static_cast<double>(cfg.fps);
    scfg.quality = cfg.quality;
    scfg.fec_group_size = cfg.fec_group;
    scfg.adaptive_quality = cfg.adaptive ? 1 : 0;
    scfg.codec = cfg.codec;
    scfg.bitrate_kbps = static_cast<uint32_t>(std::clamp(cfg.bitrate_kbps, 2000, 150000));

    if (lv_sdk_sender_start(tx_, &scfg) != LV_OK) {
        lv_sender_destroy(tx_);
        tx_ = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
    }
    frames_sent_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    return true;
}

void LanVidObsOutputEngine::stop() {
    running_.store(false, std::memory_order_release);
    if (tx_) {
        lv_sender_stop(tx_);
        lv_sender_destroy(tx_);
        tx_ = nullptr;
    }
}

bool LanVidObsOutputEngine::push_bgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride) {
    if (!running_.load(std::memory_order_acquire) || !tx_ || !bgra) return false;
    const int rc = lv_sender_push_packed_frame(tx_, bgra, static_cast<int>(width), static_cast<int>(height),
                                               static_cast<int>(stride), LV_PIXEL_FORMAT_BGRA8888, now_us32());
    if (rc == LV_OK) {
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool LanVidObsOutputEngine::push_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                      int stride_y, int stride_u, int stride_v, uint32_t width, uint32_t height) {
    if (!running_.load(std::memory_order_acquire) || !tx_ || !y || !u || !v) return false;
    LV_YuvPlanes planes{};
    planes.plane_y = y;
    planes.plane_u = u;
    planes.plane_v = v;
    planes.y_stride = stride_y;
    planes.u_stride = stride_u;
    planes.v_stride = stride_v;
    planes.uv_pixel_stride = 1;
    planes.pixel_format = LV_PIXEL_FORMAT_I420;
    const int rc = lv_sender_push_yuv_planes(tx_, &planes, static_cast<int>(width), static_cast<int>(height), now_us32());
    if (rc == LV_OK) {
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool LanVidObsOutputEngine::push_nv12(const uint8_t* y, const uint8_t* uv, int stride_y, int stride_uv,
                                      uint32_t width, uint32_t height) {
    if (!running_.load(std::memory_order_acquire) || !tx_ || !y || !uv) return false;
    LV_YuvPlanes planes{};
    planes.plane_y = y;
    planes.plane_u = uv;
    planes.plane_v = uv + 1;
    planes.y_stride = stride_y;
    planes.u_stride = stride_uv;
    planes.v_stride = stride_uv;
    planes.uv_pixel_stride = 2;
    planes.pixel_format = LV_PIXEL_FORMAT_NV12;
    const int rc = lv_sender_push_yuv_planes(tx_, &planes, static_cast<int>(width), static_cast<int>(height), now_us32());
    if (rc == LV_OK) {
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool LanVidObsOutputEngine::push_audio_pcm16(const int16_t* stereo_interleaved, uint32_t frames) {
    if (!running_.load(std::memory_order_acquire) || !tx_ || !stereo_interleaved || frames == 0) return false;
    return lv_sender_push_audio_chunk(tx_, stereo_interleaved, static_cast<int>(frames), 48000, 2,
                                      LV_AUDIO_FORMAT_PCM16, now_us32()) == LV_OK;
}

OutputConfig LanVidObsOutputEngine::config() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

// -----------------------------------------------------------------------------
// LanVidObsRuntime
// -----------------------------------------------------------------------------
LanVidObsRuntime& LanVidObsRuntime::instance() {
    static LanVidObsRuntime rt;
    return rt;
}

LanVidObsRuntime::~LanVidObsRuntime() {
    shutdown();
}

bool LanVidObsRuntime::startup(uint16_t tally_port) {
    if (started_.load(std::memory_order_acquire)) return true;
    bool ok = tally_intercom_.start_studio(tally_port);
    if (!ok && tally_port != 0) {
        // Port 5022 might already be bound by local LanVid Video — fall back to ephemeral port cleanly.
        ok = tally_intercom_.start_studio(0);
        tally_fallback_.store(ok, std::memory_order_relaxed);
    } else {
        tally_fallback_.store(false, std::memory_order_relaxed);
    }
    if (!ok) return false;

    stop_.store(false, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    run_discovery_pass();
    runtime_thread_ = std::thread(&LanVidObsRuntime::runtime_loop, this);
    return true;
}

void LanVidObsRuntime::shutdown() {
    started_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        stop_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (runtime_thread_.joinable()) {
        runtime_thread_.join();
    }
    tally_intercom_.stop();
    disc_shm_bus_.close();
    {
        std::lock_guard<std::mutex> lk(mu_);
        sources_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(disc_mu_);
        cached_cameras_.clear();
        injected_test_cameras_.clear();
    }
}

uint16_t LanVidObsRuntime::studio_port() const {
    return tally_intercom_.bound_port();
}

void LanVidObsRuntime::register_source(const std::shared_ptr<LanVidObsSourceEngine>& src) {
    if (!src) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = sources_.begin(); it != sources_.end();) {
        auto sp = it->lock();
        if (!sp) {
            it = sources_.erase(it);
            continue;
        }
        if (sp->uid() == src->uid()) return;
        ++it;
    }
    sources_.push_back(src);
}

void LanVidObsRuntime::unregister_source(uint64_t uid) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = sources_.begin(); it != sources_.end();) {
        auto sp = it->lock();
        if (!sp || sp->uid() == uid) {
            it = sources_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::shared_ptr<LanVidObsSourceEngine>> LanVidObsRuntime::active_sources_locked() const {
    std::vector<std::shared_ptr<LanVidObsSourceEngine>> alive;
    for (auto it = sources_.begin(); it != sources_.end();) {
        if (auto sp = it->lock()) {
            alive.push_back(std::move(sp));
            ++it;
        } else {
            it = sources_.erase(it);
        }
    }
    return alive;
}

uint16_t LanVidObsRuntime::allocate_next_port() {
    for (int tries = 0; tries < 32; ++tries) {
        const uint16_t p = next_port_.fetch_add(2, std::memory_order_relaxed);
        if (!check_port_conflict(p).has_conflict) return p;
    }
    return next_port_.fetch_add(2, std::memory_order_relaxed);
}

PortConflictInfo LanVidObsRuntime::check_port_conflict(uint16_t port, uint64_t exclude_uid) const {
    PortConflictInfo info{};
    info.port = port;
    if (port == 0) return info;
    std::vector<std::shared_ptr<LanVidObsSourceEngine>> srcs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        srcs = active_sources_locked();
    }
    for (const auto& s : srcs) {
        if (s->uid() == exclude_uid) continue;
        const auto scfg = s->config();
        if (scfg.shm_slot >= 0) continue;
        if (s->bound_port() == port || scfg.listen_port == port) {
            info.has_conflict = true;
            info.conflicting_source = scfg.source_name;
            break;
        }
    }
    return info;
}

void LanVidObsRuntime::trigger_discovery_refresh() {
    refresh_requested_.store(true, std::memory_order_release);
    cv_.notify_all();
}

void LanVidObsRuntime::refresh_discovery_now() {
    run_discovery_pass();
}

std::vector<DiscoveredCameraItem> LanVidObsRuntime::discovered_cameras() const {
    std::lock_guard<std::mutex> lk(disc_mu_);
    return cached_cameras_;
}

void LanVidObsRuntime::inject_discovered_camera_for_test(const DiscoveredCameraItem& item) {
    {
        std::lock_guard<std::mutex> lk(disc_mu_);
        injected_test_cameras_.push_back(item);
    }
    run_discovery_pass();
}

void LanVidObsRuntime::clear_test_cameras() {
    {
        std::lock_guard<std::mutex> lk(disc_mu_);
        injected_test_cameras_.clear();
    }
    run_discovery_pass();
}

void LanVidObsRuntime::run_discovery_pass() {
    std::vector<DiscoveredCameraItem> items;

    // 1. Test-injected items (first so tests stay deterministic)
    {
        std::lock_guard<std::mutex> lk(disc_mu_);
        items.insert(items.end(), injected_test_cameras_.begin(), injected_test_cameras_.end());
    }

    // 2. Local LanVidVideoBus_v1 Shared Memory slots ("LanVid Cam 1..8" + "LanVid Program Out")
    if (!disc_shm_bus_.is_open()) {
        disc_shm_bus_.open(nullptr);
    }
    if (disc_shm_bus_.is_open()) {
        const auto* hdr = disc_shm_bus_.header();
        const uint64_t now_ms = lv::video_bus_now_ms();
        if (hdr && hdr->host_heartbeat_ms.load(std::memory_order_relaxed) + 3000 > now_ms) {
            for (uint32_t s = 0; s < lv::kVideoBusSlots; ++s) {
                const auto* slot = disc_shm_bus_.slot(s);
                if (!slot) continue;
                const bool conn = slot->connected.load(std::memory_order_relaxed) != 0;
                const uint64_t seq = slot->frame_seq.load(std::memory_order_relaxed);
                if (!conn && seq == 0 && s != lv::kVideoBusProgramSlot) continue;
                DiscoveredCameraItem it{};
                it.id = "shm:" + std::to_string(s);
                it.shm_slot = int(s);
                it.origin = DiscoveryOrigin::kSharedMemory;
                it.ip = "127.0.0.1";
                it.port = 0;
                it.assigned_cam_number = uint8_t(s < lv::kVideoBusCameraSlots ? s + 1 : 0);
                it.assigned_cam_tag = lv::video_bus_device_name(s);
                it.display_name = std::string("[SHM] ") + lv::video_bus_device_name(s) +
                                  (conn ? " (LIVE)" : " (Standby)");
                it.online = conn;
                items.push_back(std::move(it));
            }
        }
    }

    // 3. Connected LanVid Camera phones on Tally (:5022)
    if (tally_intercom_.is_running()) {
        for (const auto& sc : tally_intercom_.studio_cameras()) {
            const std::string ip = strip_port(sc.address);
            DiscoveredCameraItem it{};
            it.id = "tally:" + ip + ":" + std::to_string(sc.camera_id);
            it.ip = ip;
            it.port = 5006;
            it.camera_id = sc.camera_id;
            it.assigned_cam_number = sc.assigned_cam_number;
            it.assigned_cam_tag = !sc.assigned_cam_tag.empty() ? sc.assigned_cam_tag : sc.label;
            it.origin = DiscoveryOrigin::kTallyPhone;
            it.role = "camera";
            it.online = sc.connected;
            char lbl[160];
            std::snprintf(lbl, sizeof(lbl), "[%s] %s (%s:%u)%s",
                          it.assigned_cam_tag.empty() ? "CAM" : it.assigned_cam_tag.c_str(),
                          sc.label.c_str(), ip.c_str(), unsigned(it.port),
                          sc.connected ? "" : " [Offline]");
            it.display_name = lbl;
            items.push_back(std::move(it));
        }
    }

    // 4. UDP Broadcast / mDNS (:5021, :5023, :5099)
    const auto peers = lv::discovery_scan_roles(150, true, true, true);
    for (const auto& p : peers) {
        DiscoveredCameraItem it{};
        it.id = "lan:" + p.ip + ":" + std::to_string(p.stream_port);
        it.ip = p.ip;
        it.port = p.stream_port;
        it.origin = DiscoveryOrigin::kLanBroadcast;
        it.role = p.role;
        it.online = true;
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "[%s] %s (%s:%u)",
                      p.role.empty() ? "LAN" : p.role.c_str(), p.device_name.c_str(), p.ip.c_str(),
                      unsigned(p.stream_port));
        it.display_name = lbl;
        items.push_back(std::move(it));
    }

    std::lock_guard<std::mutex> lk(disc_mu_);
    cached_cameras_ = std::move(items);
}

void LanVidObsRuntime::runtime_loop() {
    auto last_disc = std::chrono::steady_clock::now();
    while (!stop_.load(std::memory_order_acquire)) {
        if (tally_intercom_.is_running()) {
            tally_intercom_.poll();
        }
        const auto now_tp = std::chrono::steady_clock::now();
        const bool due = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_disc).count() >= 1500;
        if (due || refresh_requested_.exchange(false, std::memory_order_acq_rel)) {
            run_discovery_pass();
            last_disc = std::chrono::steady_clock::now();
        }
        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(15), [this] {
            return stop_.load(std::memory_order_acquire) || refresh_requested_.load(std::memory_order_acquire);
        });
    }
}

std::string LanVidObsRuntime::diagnose_network_and_ports() const {
    std::ostringstream os;
    os << "Tally UDP :" << studio_port()
       << (tally_port_fallback_active() ? " (Fallback — port 5022 used by LanVid Video)" : " (Primary)")
       << "\nNICs: ";
    const auto nics = lv::enumerate_network_interfaces(false, false);
    if (nics.empty()) {
        os << "none";
    } else {
        for (size_t i = 0; i < nics.size(); ++i) {
            if (i) os << ", ";
            os << nics[i].friendly_name << " (" << nics[i].ipv4_address << ")";
        }
    }
    const auto cams = snapshot_cameras();
    os << "\nOBS Sources: " << cams.size();
    for (const auto& c : cams) {
        os << "\n - " << c.name << " -> ";
        if (c.shm_slot >= 0) os << "SHM slot " << c.shm_slot;
        else os << "UDP :" << c.listen_port << (c.has_port_conflict ? " [CONFLICT!]" : " [OK]");
    }
    os << "\nDiscovered LAN/SHM items: " << discovered_cameras().size();
    return os.str();
}

void LanVidObsRuntime::set_obs_global_state(bool studio_mode, bool streaming, bool recording) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        obs_studio_mode_ = studio_mode;
        obs_streaming_ = streaming;
        obs_recording_ = recording;
    }
    sync_all_tallies();
}

void LanVidObsRuntime::sync_all_tallies() {
    std::vector<std::shared_ptr<LanVidObsSourceEngine>> copy;
    bool studio_mode = false, streaming = false, recording = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy = active_sources_locked();
        studio_mode = obs_studio_mode_;
        streaming = obs_streaming_;
        recording = obs_recording_;
    }
    if (!tally_intercom_.is_running()) return;

    for (const auto& src : copy) {
        const auto cfg = src->config();
        const auto dec = src->compute_current_tally(studio_mode, streaming, recording);
        std::string label = !cfg.assigned_cam_tag.empty() ? cfg.assigned_cam_tag
                          : (!cfg.source_name.empty() ? cfg.source_name : ("CAM " + std::to_string(cfg.camera_id)));
        if (label.size() > 23) label.resize(23);
        tally_intercom_.studio_set_camera_tally(cfg.camera_id, dec.tally_state, label, dec.rec_trigger);
    }
}

void LanVidObsRuntime::poll_studio() {
    if (tally_intercom_.is_running()) {
        tally_intercom_.poll();
    }
}

void LanVidObsRuntime::set_director_talk(uint32_t camera_id, bool talk_to_cam, bool listen_from_cam, bool all_call) {
    if (tally_intercom_.is_running()) {
        tally_intercom_.studio_set_camera_intercom(camera_id, talk_to_cam, listen_from_cam, all_call);
    }
}

void LanVidObsRuntime::set_director_mic_active(bool talk_active, bool listen_muted, float rx_gain, float sidetone_gain) {
    if (tally_intercom_.is_running()) {
        tally_intercom_.set_intercom_state(talk_active, listen_muted, rx_gain, sidetone_gain);
    }
}

void LanVidObsRuntime::push_director_mic_pcm(const float* mono_48k, int frames) {
    if (tally_intercom_.is_running()) {
        tally_intercom_.push_mic_pcm(mono_48k, frames);
    }
}

int LanVidObsRuntime::pull_director_rx_pcm(float* mono_48k, int frames) {
    if (!tally_intercom_.is_running()) return 0;
    return tally_intercom_.pull_rx_pcm(mono_48k, frames);
}

void LanVidObsRuntime::panic_reset_all_buffers() {
    std::vector<std::shared_ptr<LanVidObsSourceEngine>> copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy = active_sources_locked();
    }
    for (const auto& src : copy) {
        src->reset_buffers();
        src->request_idr_keyframe();
    }
}

std::vector<SourceTelemetry> LanVidObsRuntime::snapshot_cameras() const {
    std::vector<std::shared_ptr<LanVidObsSourceEngine>> copy;
    bool studio_mode = false, streaming = false, recording = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy = active_sources_locked();
        studio_mode = obs_studio_mode_;
        streaming = obs_streaming_;
        recording = obs_recording_;
    }

    const auto studio_cams = tally_intercom_.is_running() ? tally_intercom_.studio_cameras()
                                                          : std::vector<lv::StudioCameraInfo>{};
    std::vector<SourceTelemetry> out;
    out.reserve(copy.size());
    for (const auto& src : copy) {
        auto tel = src->telemetry();
        const auto dec = src->compute_current_tally(studio_mode, streaming, recording);
        tel.effective_tally = dec.tally_state;
        tel.remote_rec_active = dec.rec_trigger;
        tel.has_port_conflict = (tel.shm_slot < 0) && check_port_conflict(tel.listen_port, tel.uid).has_conflict;
        for (const auto& sc : studio_cams) {
            if (sc.camera_id == tel.camera_id) {
                tel.battery_pct = sc.battery_pct;
                tel.temp_c = sc.temp_c;
                tel.operator_speaking = sc.operator_speaking;
                tel.talk_to_cam = sc.talk_to_cam;
                tel.listen_from_cam = sc.listen_from_cam;
                if (tel.peer_ip.empty()) tel.peer_ip = strip_port(sc.address);
                if (sc.assigned_cam_number > 0) tel.assigned_cam_number = sc.assigned_cam_number;
                if (!sc.assigned_cam_tag.empty()) tel.assigned_cam_tag = sc.assigned_cam_tag;
                if (sc.config_synced) tel.config_synced = true;
                break;
            }
        }
        out.push_back(std::move(tel));
    }
    return out;
}

std::vector<LV_DiscoveredPeer> LanVidObsRuntime::scan_lan_devices(int timeout_ms) const {
    LV_DiscoveredPeer peers[16]{};
    const int n = lv_discovery_scan_peers(timeout_ms, 0, peers, 16);
    std::vector<LV_DiscoveredPeer> res;
    if (n > 0) res.assign(peers, peers + n);
    return res;
}

}  // namespace lv::obs
