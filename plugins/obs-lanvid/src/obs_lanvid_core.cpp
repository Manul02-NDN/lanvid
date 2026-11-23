#include "obs_lanvid_core.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lv::obs {

namespace {

uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t now_us32() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

// -----------------------------------------------------------------------------
// Auto-Tally State Machine
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
                if (in.only_on_air_when_live && !is_live) {
                    d.tally_state = lv::kTallyPreview;
                } else {
                    d.tally_state = lv::kTallyProgram;
                }
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
        case JitterPreset::kUltraLow: return 8;
        case JitterPreset::kSmooth: return 65;
        case JitterPreset::kBalanced:
        default: return 20;
    }
}

LV_ControlSyncState make_quality_preset_control_sync(CameraQualityPreset preset, uint64_t revision, uint32_t origin_id) {
    LV_ControlSyncState st{};
    st.revision = revision;
    st.origin_id = origin_id;
    st.audio_channels = 2;
    st.audio_bitrate_kbps = 0;  // Lossless audio
    st.audio_only = 0;
    st.flags = 0;
    st.master_gain_cb = 0;
    st.audio_codec = 0;
    switch (preset) {
        case CameraQualityPreset::kProxy360p:
            st.active_preset_id = 0;
            st.target_buffer_ms = 20;
            st.latency_mode = 1;  // Adaptive Wi-Fi
            break;
        case CameraQualityPreset::kStudio1080p:
            st.active_preset_id = 1;
            st.target_buffer_ms = 15;
            st.latency_mode = 1;
            break;
        case CameraQualityPreset::kMaster4K60:
        default:
            st.active_preset_id = 2;
            st.target_buffer_ms = 10;
            st.latency_mode = 0;  // Fixed Ethernet / high-speed LAN
            break;
    }
    return st;
}

// -----------------------------------------------------------------------------
// LanVidObsSourceEngine
// -----------------------------------------------------------------------------
LanVidObsSourceEngine::LanVidObsSourceEngine() = default;

LanVidObsSourceEngine::~LanVidObsSourceEngine() {
    stop();
}

bool LanVidObsSourceEngine::start(const SourceConfig& cfg, VideoCallback on_video, AudioCallback on_audio) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
        on_video_ = std::move(on_video);
        on_audio_ = std::move(on_audio);
        last_w_ = 0;
        last_h_ = 0;
        last_frame_id_ = 0;
        frames_rx_ = 0;
        dropped_frames_ = 0;
        measured_fps_ = 0.0;
        measured_mbps_ = 0.0;
        measured_loss_pct_ = 0.0;
        tally_mode_ = cfg.auto_tally ? TallyControlMode::kAuto : TallyControlMode::kStandby;
    }

    rx_ = lv_receiver_create();
    if (!rx_) return false;

    LV_ReceiverConfig rcfg{};
    lv_sdk_receiver_config_default(&rcfg);
    std::snprintf(rcfg.bind_ip, sizeof(rcfg.bind_ip), "%s", cfg.bind_ip.empty() ? "0.0.0.0" : cfg.bind_ip.c_str());
    rcfg.bind_port = cfg.listen_port;
    rcfg.audio_target_ms = static_cast<double>(cfg.audio_latency_ms > 0 ? cfg.audio_latency_ms : jitter_preset_to_ms(cfg.jitter_preset));
    rcfg.enable_os_audio = 0;  // OBS handles audio rendering via obs_source_output_audio
    rcfg.enable_discovery = 1;
    std::snprintf(rcfg.device_name, sizeof(rcfg.device_name), "%s",
                  cfg.source_name.empty() ? "OBS LanVid Source" : cfg.source_name.c_str());

    if (cfg.audio_route != AudioRouteMode::kVideoOnly && cfg.audio_channels > 2) {
        LV_AudioBusConfig bcfg{};
        lv_audio_bus_config_default(&bcfg);
        bcfg.sample_rate = 48000;
        bcfg.num_channels = static_cast<uint16_t>(std::clamp<uint32_t>(cfg.audio_channels, 2, 64));
        bcfg.latency_ms = cfg.audio_latency_ms;
        lv_receiver_configure_audio_bus(rx_, &bcfg);
    }

    if (lv_sdk_receiver_start(rx_, &rcfg) != LV_OK) {
        lv_receiver_destroy(rx_);
        rx_ = nullptr;
        return false;
    }

    lv_receiver_set_adaptive_jitter(rx_, cfg.jitter_preset == JitterPreset::kUltraLow ? 0 : 1);

    running_.store(true, std::memory_order_release);
    LanVidObsManager::instance().register_source(this);
    worker_ = std::thread(&LanVidObsSourceEngine::worker_loop, this);
    return true;
}

void LanVidObsSourceEngine::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    LanVidObsManager::instance().unregister_source(this);
    if (rx_) {
        lv_receiver_stop(rx_);
        lv_receiver_destroy(rx_);
        rx_ = nullptr;
    }
}

void LanVidObsSourceEngine::update_config(const SourceConfig& cfg) {
    bool needs_restart = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        needs_restart = (cfg.listen_port != cfg_.listen_port || cfg.bind_ip != cfg_.bind_ip);
        cfg_ = cfg;
        if (rx_ && !needs_restart) {
            const uint32_t lat = cfg.audio_latency_ms > 0 ? cfg.audio_latency_ms : jitter_preset_to_ms(cfg.jitter_preset);
            lv_receiver_set_audio_latency_ms(rx_, lat);
            lv_receiver_set_adaptive_jitter(rx_, cfg.jitter_preset == JitterPreset::kUltraLow ? 0 : 1);
        }
    }
    if (needs_restart && running_.load(std::memory_order_acquire)) {
        VideoCallback vcb;
        AudioCallback acb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            vcb = on_video_;
            acb = on_audio_;
        }
        start(cfg, std::move(vcb), std::move(acb));
    }
    LanVidObsManager::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::request_idr_keyframe() {
    // Re-sync receiver concealment / trigger PLI feedback cycle via ControlSync ping
    std::lock_guard<std::mutex> lk(mu_);
    if (!rx_) return;
    LV_ControlSyncState st{};
    st.revision = ++control_sync_rev_;
    st.origin_id = 0x4F425300u | (cfg_.camera_id & 0xFFu);  // "OBS\0"
    st.audio_channels = static_cast<uint16_t>(cfg_.audio_channels);
    st.target_buffer_ms = static_cast<uint16_t>(jitter_preset_to_ms(cfg_.jitter_preset));
    st.latency_mode = cfg_.jitter_preset == JitterPreset::kUltraLow ? 0 : 1;
    st.active_preset_id = -1;
    st.flags = 0x04;  // IDR / Keyframe hint
    lv_receiver_send_control_sync(rx_, &st);
}

void LanVidObsSourceEngine::reset_buffers() {
    reset_requested_.store(true, std::memory_order_release);
}

bool LanVidObsSourceEngine::send_quality_preset(CameraQualityPreset preset) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!rx_) return false;
    const auto st = make_quality_preset_control_sync(preset, ++control_sync_rev_, 0x4F425300u | (cfg_.camera_id & 0xFFu));
    return lv_receiver_send_control_sync(rx_, &st) == LV_OK;
}

void LanVidObsSourceEngine::set_obs_visibility(bool in_program, bool in_preview) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        in_program_ = in_program;
        in_preview_ = in_preview;
    }
    LanVidObsManager::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::set_tally_mode(TallyControlMode mode) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tally_mode_ = mode;
    }
    LanVidObsManager::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::set_manual_rec(bool rec_active) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        manual_rec_ = rec_active;
    }
    LanVidObsManager::instance().sync_all_tallies();
}

void LanVidObsSourceEngine::ingest_video_frame_bgra(const uint8_t* bgra, uint32_t width, uint32_t height,
                                                    uint32_t stride, uint64_t timestamp_ns, ColorSpace cs) {
    if (!bgra || width == 0 || height == 0) return;
    VideoCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_w_ = width;
        last_h_ = height;
        ++frames_rx_;
        cb = on_video_;
    }
    if (cb) {
        VideoFrameView fv{};
        fv.data[0] = bgra;
        fv.linesize[0] = stride > 0 ? stride : width * 4;
        fv.width = width;
        fv.height = height;
        fv.timestamp_ns = timestamp_ns > 0 ? timestamp_ns : now_ns();
        fv.format = VideoPixelFormat::kBGRA;
        fv.color_space = cs;
        fv.full_range = true;
        cb(fv);
    }
}

void LanVidObsSourceEngine::route_multichannel_audio(const float* interleaved, uint16_t num_channels,
                                                     uint32_t num_frames, uint32_t sample_rate,
                                                     uint64_t timestamp_ns) {
    if (!interleaved || num_channels == 0 || num_frames == 0) return;
    AudioRouteMode route;
    int pair_idx = 0;
    AudioCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        route = cfg_.audio_route;
        pair_idx = cfg_.audio_pair_index;
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
    return rx_ ? lv_receiver_get_local_port(rx_) : cfg_.listen_port;
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
    std::lock_guard<std::mutex> lk(mu_);
    SourceTelemetry t{};
    t.camera_id = cfg_.camera_id;
    t.name = cfg_.source_name;
    t.peer_ip = cfg_.peer_ip;
    t.listen_port = rx_ ? lv_receiver_get_local_port(rx_) : cfg_.listen_port;
    t.width = last_w_;
    t.height = last_h_;
    t.fps = measured_fps_;
    t.mbps = measured_mbps_;
    t.loss_pct = measured_loss_pct_;
    t.frames_rx = frames_rx_;
    t.dropped_frames = dropped_frames_;
    t.effective_tally = last_effective_tally_;
    t.tally_mode = tally_mode_;
    t.remote_rec_active = manual_rec_;
    t.connected = (frames_rx_ > 0);
    return t;
}

void LanVidObsSourceEngine::worker_loop() {
    // Allocate initial 4K DCI BGRA buffer (4096 * 2160 * 4 = 35.4 MB) lazily on first frame
    constexpr uint32_t kAudioBlock = 480;  // 10 ms @ 48 kHz
    std::vector<int16_t> pcm16_stereo(kAudioBlock * 2);
    auto last_stats_tp = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        if (!rx_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (reset_requested_.exchange(false, std::memory_order_acq_rel)) {
            uint32_t lat = 20;
            {
                std::lock_guard<std::mutex> lk(mu_);
                lat = cfg_.audio_latency_ms > 0 ? cfg_.audio_latency_ms : jitter_preset_to_ms(cfg_.jitter_preset);
                last_frame_id_ = 0;
            }
            lv_receiver_set_audio_latency_ms(rx_, lat);
        }

        // 1. Poll latest decoded video frame (supports 360p..4K DCI + 9:16 dynamic resolution switching)
        if (frame_buf_.empty()) {
            frame_buf_.resize(4096u * 2160u * 4u);
        }
        int w = 0, h = 0;
        uint32_t fid = last_frame_id_;
        const int got_frame = lv_receiver_get_latest_frame(rx_, frame_buf_.data(), frame_buf_.size(),
                                                           &w, &h, &fid, /*want_rgba=*/0);
        if (got_frame > 0 && w > 0 && h > 0) {
            last_frame_id_ = fid;
            ingest_video_frame_bgra(frame_buf_.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                    static_cast<uint32_t>(w) * 4u, now_ns(), ColorSpace::kRec709);
        }

        // 2. Pull audio (64ch AoIP bus or legacy stereo PCM16)
        AudioRouteMode route;
        uint32_t num_ch = 2;
        {
            std::lock_guard<std::mutex> lk(mu_);
            route = cfg_.audio_route;
            num_ch = cfg_.audio_channels;
        }
        if (route != AudioRouteMode::kVideoOnly) {
            if (num_ch > 2) {
                audio_interleaved_.resize(size_t(kAudioBlock) * num_ch);
                const int got = lv_receiver_pop_audio_multichannel(
                    rx_, audio_interleaved_.data(), static_cast<uint16_t>(num_ch), kAudioBlock,
                    LV_AUDIO_LAYOUT_INTERLEAVED);
                if (got > 0) {
                    route_multichannel_audio(audio_interleaved_.data(), static_cast<uint16_t>(num_ch),
                                             static_cast<uint32_t>(got), 48000, now_ns());
                }
            } else {
                const int got = lv_receiver_pop_audio_pcm16(rx_, pcm16_stereo.data(), static_cast<int>(kAudioBlock));
                if (got > 0) {
                    audio_interleaved_.resize(size_t(got) * 2);
                    for (int i = 0; i < got * 2; ++i) {
                        audio_interleaved_[i] = static_cast<float>(pcm16_stereo[i]) / 32768.0f;
                    }
                    route_multichannel_audio(audio_interleaved_.data(), 2, static_cast<uint32_t>(got), 48000, now_ns());
                }
            }
        }

        // 3. Refresh stream stats at ~4 Hz
        const auto now_tp = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_stats_tp).count() >= 250) {
            last_stats_tp = now_tp;
            LV_StreamStats st{};
            if (lv_receiver_get_stream_stats(rx_, &st) == LV_OK) {
                std::lock_guard<std::mutex> lk(mu_);
                measured_fps_ = st.fps;
                measured_mbps_ = st.bitrate_mbps;
                measured_loss_pct_ = st.packet_loss_pct;
                dropped_frames_ = st.frames_concealed;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
// LanVidObsManager
// -----------------------------------------------------------------------------
LanVidObsManager& LanVidObsManager::instance() {
    static LanVidObsManager mgr;
    return mgr;
}

LanVidObsManager::~LanVidObsManager() {
    shutdown();
}

bool LanVidObsManager::ensure_studio_started(uint16_t port) {
    std::lock_guard<std::mutex> lk(mu_);
    if (tally_intercom_.is_running()) return true;
    return tally_intercom_.start_studio(port);
}

void LanVidObsManager::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    tally_intercom_.stop();
    sources_.clear();
}

uint16_t LanVidObsManager::studio_port() const {
    return tally_intercom_.bound_port();
}

void LanVidObsManager::register_source(LanVidObsSourceEngine* src) {
    if (!src) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (std::find(sources_.begin(), sources_.end(), src) == sources_.end()) {
        sources_.push_back(src);
    }
}

void LanVidObsManager::unregister_source(LanVidObsSourceEngine* src) {
    std::lock_guard<std::mutex> lk(mu_);
    sources_.erase(std::remove(sources_.begin(), sources_.end(), src), sources_.end());
}

uint16_t LanVidObsManager::allocate_next_port() {
    return next_port_.fetch_add(1, std::memory_order_relaxed);
}

void LanVidObsManager::set_obs_global_state(bool studio_mode, bool streaming, bool recording) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        obs_studio_mode_ = studio_mode;
        obs_streaming_ = streaming;
        obs_recording_ = recording;
    }
    sync_all_tallies();
}

void LanVidObsManager::sync_all_tallies() {
    std::vector<LanVidObsSourceEngine*> copy;
    bool studio_mode = false, streaming = false, recording = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy = sources_;
        studio_mode = obs_studio_mode_;
        streaming = obs_streaming_;
        recording = obs_recording_;
    }
    if (!tally_intercom_.is_running()) return;

    for (auto* src : copy) {
        const auto cfg = src->config();
        const auto dec = src->compute_current_tally(studio_mode, streaming, recording);
        std::string label = cfg.source_name.empty() ? ("CAM " + std::to_string(cfg.camera_id)) : cfg.source_name;
        if (label.size() > 23) label.resize(23);
        tally_intercom_.studio_set_camera_tally(cfg.camera_id, dec.tally_state, label, dec.rec_trigger);
    }
}

void LanVidObsManager::poll_studio() {
    if (tally_intercom_.is_running()) {
        tally_intercom_.poll();
    }
}

void LanVidObsManager::set_director_talk(uint32_t camera_id, bool talk_to_cam, bool listen_from_cam, bool all_call) {
    if (tally_intercom_.is_running()) {
        tally_intercom_.studio_set_camera_intercom(camera_id, talk_to_cam, listen_from_cam, all_call);
    }
}

void LanVidObsManager::set_director_mic_active(bool talk_active, bool listen_muted, float rx_gain, float sidetone_gain) {
    if (tally_intercom_.is_running()) {
        tally_intercom_.set_intercom_state(talk_active, listen_muted, rx_gain, sidetone_gain);
    }
}

void LanVidObsManager::push_director_mic_pcm(const float* mono_48k, int frames) {
    if (tally_intercom_.is_running()) {
        tally_intercom_.push_mic_pcm(mono_48k, frames);
    }
}

int LanVidObsManager::pull_director_rx_pcm(float* mono_48k, int frames) {
    if (!tally_intercom_.is_running()) return 0;
    return tally_intercom_.pull_rx_pcm(mono_48k, frames);
}

void LanVidObsManager::panic_reset_all_buffers() {
    std::vector<LanVidObsSourceEngine*> copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy = sources_;
    }
    for (auto* src : copy) {
        src->reset_buffers();
        src->request_idr_keyframe();
    }
}

std::vector<SourceTelemetry> LanVidObsManager::snapshot_cameras() const {
    std::vector<LanVidObsSourceEngine*> copy;
    bool studio_mode = false, streaming = false, recording = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy = sources_;
        studio_mode = obs_studio_mode_;
        streaming = obs_streaming_;
        recording = obs_recording_;
    }

    const auto studio_cams = tally_intercom_.is_running() ? tally_intercom_.studio_cameras()
                                                          : std::vector<lv::StudioCameraInfo>{};
    std::vector<SourceTelemetry> out;
    out.reserve(copy.size());
    for (auto* src : copy) {
        auto tel = src->telemetry();
        const auto dec = src->compute_current_tally(studio_mode, streaming, recording);
        tel.effective_tally = dec.tally_state;
        tel.remote_rec_active = dec.rec_trigger;
        for (const auto& sc : studio_cams) {
            if (sc.camera_id == tel.camera_id) {
                tel.battery_pct = sc.battery_pct;
                tel.temp_c = sc.temp_c;
                tel.operator_speaking = sc.operator_speaking;
                tel.talk_to_cam = sc.talk_to_cam;
                tel.listen_from_cam = sc.listen_from_cam;
                if (tel.peer_ip.empty()) tel.peer_ip = sc.address;
                break;
            }
        }
        out.push_back(std::move(tel));
    }
    return out;
}

std::vector<LV_DiscoveredPeer> LanVidObsManager::scan_lan_devices(int timeout_ms) const {
    LV_DiscoveredPeer peers[16]{};
    const int n = lv_discovery_scan_peers(timeout_ms, 0, peers, 16);
    std::vector<LV_DiscoveredPeer> res;
    if (n > 0) {
        res.assign(peers, peers + n);
    }
    return res;
}

}  // namespace lv::obs
