#include "audio/intercom_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#ifdef LV_HAVE_OPUS
#include <opus.h>
#endif

#include "common/clock.hpp"

namespace lv {
namespace {

inline float peak_to_dbfs(float peak) {
    if (peak <= 1e-6f) return -120.0f;
    return std::max(-120.0f, 20.0f * std::log10(peak));
}

}  // namespace

struct TallyIntercomEngine::Impl {
    struct CamSlot {
        bool used = false;
        uint32_t camera_id = 0;
        std::string label;
        Endpoint ep{};
        uint8_t tally_state = kTallyStandby;
        bool rec_trigger = false;
        uint32_t tally_rev = 0;
        bool talk_to_cam = false;
        bool listen_from_cam = true;
        uint8_t battery_pct = 100;
        int8_t temp_c = 30;
        uint64_t last_seen_us = 0;
        uint64_t last_voice_us = 0;
        float voice_peak_dbfs = -120.0f;
        uint64_t tally_tx = 0;
        uint64_t ic_rx = 0;
        uint64_t ic_tx = 0;
        uint8_t assigned_cam_number = 0;
        std::string assigned_cam_tag;
        bool config_synced = false;
        uint64_t last_rx_vcfg_rev = 0;
        VideoConfigSyncPayload last_vcfg{};
        std::array<uint32_t, 256> seen_seq{};
        std::array<uint8_t, 256> seen_valid{};
        std::deque<float> rx_fifo;
#ifdef LV_HAVE_OPUS
        OpusDecoder* opus_dec = nullptr;
#endif
    };

    struct PendingDup {
        uint64_t due_us = 0;
        Endpoint dst{};
        std::vector<uint8_t> dgram;
    };

    UdpSocket sock;
    mutable std::mutex mu;
    std::deque<PendingDup> pending_dups;
    std::array<uint32_t, 256> cam_seen_seq{};
    std::array<uint8_t, 256> cam_seen_valid{};

    // Studio state
    std::array<CamSlot, kMaxStudioCameras> cams{};

    // Camera client state
    Endpoint studio_ep{};
    uint32_t my_camera_id = 1;
    std::string my_label = "CAM 1";
    uint8_t my_battery = 100;
    int8_t my_temp = 30;
    TallyStatePayload last_tally{};
    TallyCallback tally_cb;
    CameraControlCallback ctl_cb;
    CameraControlPayload last_ctl{};
    uint32_t ctl_revision = 0;
    VideoConfigSyncCallback vcfg_cb;
    VideoConfigSyncPayload last_vcfg{};
    uint64_t vcfg_tx_revision = 0;
    uint64_t cam_last_rx_vcfg_rev = 0;
    uint32_t my_origin_id = 1;
    uint8_t my_assigned_cam_number = 0;
    std::string my_assigned_cam_tag;
    uint64_t last_director_voice_us = 0;

    // Local intercom controls
    bool talk_active = false;
    bool listen_muted = false;
    float rx_gain = 1.0f;
    float sidetone_gain = 0.0f;

    // Heartbeat & seq
    uint64_t last_hb_us = 0;
    uint32_t tx_seq = 1;

    // Audio FIFOs (48 kHz mono float32)
    std::vector<float> tx_stage;
    std::deque<float> rx_fifo;
    std::deque<float> sidetone_fifo;

#ifdef LV_HAVE_OPUS
    OpusEncoder* enc = nullptr;
    OpusDecoder* dec = nullptr;
#endif

    void init_codec() {
#ifdef LV_HAVE_OPUS
        if (!enc) {
            int err = OPUS_OK;
            enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
            if (enc && err == OPUS_OK) {
                opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));  // 48 kbps voice
                opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(4));
                opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            } else {
                enc = nullptr;
            }
        }
        if (!dec) {
            int err = OPUS_OK;
            dec = opus_decoder_create(48000, 1, &err);
            if (!dec || err != OPUS_OK) dec = nullptr;
        }
#endif
    }

    void destroy_cam_decoder_locked(CamSlot& c) {
#ifdef LV_HAVE_OPUS
        if (c.opus_dec) {
            opus_decoder_destroy(c.opus_dec);
            c.opus_dec = nullptr;
        }
#else
        (void)c;
#endif
    }

    void reset_cam_slot_locked(int i) {
        if (i < 0 || i >= kMaxStudioCameras) return;
        destroy_cam_decoder_locked(cams[i]);
        cams[i] = CamSlot{};
    }

    void destroy_codec() {
#ifdef LV_HAVE_OPUS
        if (enc) { opus_encoder_destroy(enc); enc = nullptr; }
        if (dec) { opus_decoder_destroy(dec); dec = nullptr; }
        for (auto& c : cams) destroy_cam_decoder_locked(c);
#endif
    }

    ~Impl() { destroy_codec(); }

    int find_or_alloc_cam_locked(uint32_t cid, const Endpoint& ep, uint64_t now = 0) {
        for (int i = 0; i < kMaxStudioCameras; ++i) {
            if (cams[i].used && ((cid != 0 && cams[i].camera_id == cid) || (ep.valid() && cams[i].ep == ep))) {
                return i;
            }
        }
        for (int i = 0; i < kMaxStudioCameras; ++i) {
            if (!cams[i].used) {
                reset_cam_slot_locked(i);
                cams[i].used = true;
                cams[i].camera_id = cid ? cid : uint32_t(i + 1);
                cams[i].label = "CAM " + std::to_string(i + 1);
                cams[i].ep = ep;
                cams[i].listen_from_cam = true;
                return i;
            }
        }
        // Reclaim the stalest disconnected camera slot (> 30 s unseen) when full.
        if (now > 0) {
            int stale = -1;
            uint64_t oldest = UINT64_MAX;
            for (int i = 0; i < kMaxStudioCameras; ++i) {
                if (cams[i].used && cams[i].last_seen_us != 0 && now > cams[i].last_seen_us &&
                    (now - cams[i].last_seen_us) > 30'000'000ull && cams[i].last_seen_us < oldest) {
                    oldest = cams[i].last_seen_us;
                    stale = i;
                }
            }
            if (stale >= 0) {
                reset_cam_slot_locked(stale);
                cams[stale].used = true;
                cams[stale].camera_id = cid ? cid : uint32_t(stale + 1);
                cams[stale].label = "CAM " + std::to_string(stale + 1);
                cams[stale].ep = ep;
                cams[stale].listen_from_cam = true;
                return stale;
            }
        }
        return -1;
    }

    static bool check_and_mark_seq(std::array<uint32_t, 256>& ring_seq, std::array<uint8_t, 256>& ring_valid,
                                   uint32_t seq) {
        const size_t slot = seq & 255u;
        if (ring_valid[slot] && ring_seq[slot] == seq) return true;  // duplicate!
        ring_seq[slot] = seq;
        ring_valid[slot] = 1;
        return false;
    }

    void queue_dup_locked(const uint8_t* dgram, size_t dlen, const Endpoint& dst, uint32_t stagger_us) {
        if (!dgram || dlen == 0 || !dst.valid()) return;
        if (pending_dups.size() >= 256) pending_dups.pop_front();
        PendingDup pd;
        pd.due_us = now_us() + uint64_t(stagger_us);
        pd.dst = dst;
        pd.dgram.assign(dgram, dgram + dlen);
        pending_dups.push_back(std::move(pd));
    }

    void flush_dups_locked(uint64_t now) {
        while (!pending_dups.empty() && now >= pending_dups.front().due_us) {
            const auto& pd = pending_dups.front();
            sock.send_to_qos(pd.dgram.data(), pd.dgram.size(), pd.dst, QosTrafficClass::kSyncControl);
            pending_dups.pop_front();
        }
    }

    void send_tally_dgram_locked(const TallyStatePayload& tp, const Endpoint& dst, int repeats,
                                 bool redundant = false, uint32_t stagger_us = 1500) {
        if (!dst.valid() || !sock.is_open()) return;
        uint8_t body[kTallyStateSize];
        write_tally_state(body, tp);
        PacketHeader ph{};
        ph.type = PacketType::kTallyState;
        ph.flags = kFlagHasCrc;
        ph.frame_id = tp.revision;
        ph.timestamp_us = now_us32();
        uint8_t dgram[kPacketHeaderSize + kTallyStateSize];
        const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, sizeof(body));
        if (dlen == 0) return;
        for (int r = 0; r < repeats; ++r) {
            sock.send_to_qos(dgram, dlen, dst, QosTrafficClass::kSyncControl);
        }
        if (redundant) queue_dup_locked(dgram, dlen, dst, stagger_us);
    }

    void send_vcfg_dgram_locked(const VideoConfigSyncPayload& p, const Endpoint& dst, int repeats,
                                bool redundant = false, uint32_t stagger_us = 1500) {
        if (!dst.valid() || !sock.is_open()) return;
        uint8_t body[kVideoConfigSyncSize];
        write_video_config_sync(body, p);
        PacketHeader ph{};
        ph.type = PacketType::kVideoConfigSync;
        ph.flags = kFlagHasCrc;
        ph.frame_id = uint32_t(p.config_revision & 0xFFFFFFFFu);
        ph.timestamp_us = now_us32();
        uint8_t dgram[kPacketHeaderSize + kVideoConfigSyncSize];
        const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, sizeof(body));
        if (dlen == 0) return;
        for (int r = 0; r < repeats; ++r) {
            sock.send_to_qos(dgram, dlen, dst, QosTrafficClass::kSyncControl);
        }
        if (redundant) queue_dup_locked(dgram, dlen, dst, stagger_us);
    }

    void encode_and_send_voice_locked(const float* pcm, int n, bool is_studio, bool all_call_flag,
                                      std::atomic<uint64_t>& tx_counter,
                                      bool redundant = false, uint32_t stagger_us = 1500) {
        uint8_t payload[kIntercomHeaderSize + 1024];
        IntercomAudioHeader ih{};
        ih.seq = tx_seq++;
        ih.num_samples = uint16_t(n);
        ih.sample_rate = 48000;
        ih.flags = kIntercomFlagPttActive;
        if (is_studio) {
            ih.flags |= kIntercomFlagStudioToCam | kIntercomFlagDuckProgram;
            if (all_call_flag) ih.flags |= kIntercomFlagAllCall;
        } else {
            ih.camera_id = my_camera_id;
        }

        size_t enc_len = 0;
#ifdef LV_HAVE_OPUS
        if (enc && (n == 240 || n == 480 || n == 960)) {
            const int nb = opus_encode_float(enc, pcm, n, payload + kIntercomHeaderSize, 512);
            if (nb > 0) {
                ih.codec = 1;  // Opus
                enc_len = size_t(nb);
            }
        }
#endif
        if (enc_len == 0) {
            // PCM16 fallback
            ih.codec = 0;
            enc_len = size_t(n) * 2;
            uint8_t* dst = payload + kIntercomHeaderSize;
            for (int i = 0; i < n; ++i) {
                const int16_t q = int16_t(std::clamp(int(std::lrint(double(std::clamp(pcm[i], -1.0f, 1.0f)) * 32767.0)),
                                                     -32768, 32767));
                dst[2 * i + 0] = uint8_t(q & 0xFF);
                dst[2 * i + 1] = uint8_t((q >> 8) & 0xFF);
            }
        }

        uint8_t dgram[kPacketHeaderSize + kIntercomHeaderSize + 1024];
        PacketHeader ph{};
        ph.type = PacketType::kIntercomAudio;
        ph.flags = kFlagHasCrc;
        ph.frame_id = ih.seq;
        ph.timestamp_us = now_us32();

        if (is_studio) {
            for (auto& c : cams) {
                if (!c.used || !c.ep.valid()) continue;
                if (!all_call_flag && !c.talk_to_cam) continue;
                ih.camera_id = all_call_flag ? 0u : c.camera_id;
                write_intercom_header(payload, ih);
                const size_t dlen = write_packet(dgram, sizeof(dgram), ph, payload, kIntercomHeaderSize + enc_len);
                if (dlen > 0 && sock.send_to_qos(dgram, dlen, c.ep, QosTrafficClass::kSyncControl) == int(dlen)) {
                    ++c.ic_tx;
                    tx_counter.fetch_add(1, std::memory_order_relaxed);
                    if (redundant) queue_dup_locked(dgram, dlen, c.ep, stagger_us);
                }
            }
        } else if (studio_ep.valid()) {
            write_intercom_header(payload, ih);
            const size_t dlen = write_packet(dgram, sizeof(dgram), ph, payload, kIntercomHeaderSize + enc_len);
            if (dlen > 0 && sock.send_to_qos(dgram, dlen, studio_ep, QosTrafficClass::kSyncControl) == int(dlen)) {
                tx_counter.fetch_add(1, std::memory_order_relaxed);
                if (redundant) queue_dup_locked(dgram, dlen, studio_ep, stagger_us);
            }
        }
    }

    int decode_voice_locked(const IntercomAudioHeader& ih, const uint8_t* data, size_t len, float* out,
                            int max_out_samples, CamSlot* cam_slot = nullptr) {
        const int cap = std::min<int>(int(ih.num_samples), max_out_samples);
        if (cap <= 0) return 0;
        if (ih.codec == 1) {
#ifdef LV_HAVE_OPUS
            OpusDecoder* d_inst = dec;
            if (cam_slot) {
                if (!cam_slot->opus_dec) {
                    int err = OPUS_OK;
                    cam_slot->opus_dec = opus_decoder_create(48000, 1, &err);
                    if (err != OPUS_OK) {
                        if (cam_slot->opus_dec) opus_decoder_destroy(cam_slot->opus_dec);
                        cam_slot->opus_dec = nullptr;
                    }
                }
                d_inst = cam_slot->opus_dec;
            }
            if (d_inst && len > 0) {
                const int d = opus_decode_float(d_inst, data, int(len), out, cap, 0);
                if (d > 0) return d;
            }
#else
            (void)cam_slot;
#endif
            return 0;
        }
        const int n = std::min<int>(cap, int(len / 2));
        for (int i = 0; i < n; ++i) {
            const int16_t v = int16_t(uint16_t(data[2 * i]) | (uint16_t(data[2 * i + 1]) << 8));
            out[i] = float(v) * (1.0f / 32768.0f);
        }
        return n;
    }
};

TallyIntercomEngine::TallyIntercomEngine() : impl_(std::make_unique<Impl>()) {
    std::random_device rd;
    uint32_t oid = rd() ^ now_us32();
    if (oid == 0) oid = 0x4C563031u;
    origin_id_ = oid;
    impl_->my_origin_id = oid;
}
TallyIntercomEngine::~TallyIntercomEngine() { stop(); }

bool TallyIntercomEngine::start_studio(uint16_t bind_port, const std::string& bind_ip) {
    stop();
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->sock.open()) return false;
    if (!impl_->sock.bind(bind_ip, bind_port)) {
        impl_->sock.close();
        return false;
    }
    impl_->sock.set_qos_class(QosTrafficClass::kSyncControl);  // DSCP CS7/CS6 for Tally & Intercom
    impl_->sock.set_nonblocking(true);
    impl_->init_codec();
    bound_port_ = impl_->sock.local_port();
    studio_mode_.store(true, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    return true;
}

bool TallyIntercomEngine::start_camera(const std::string& studio_ip, uint16_t studio_port,
                                       uint32_t camera_id, const std::string& label) {
    stop();
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!Endpoint::parse(studio_ip, studio_port, impl_->studio_ep)) return false;
    if (!impl_->sock.open()) return false;
    if (!impl_->sock.bind("0.0.0.0", 0)) {
        impl_->sock.close();
        return false;
    }
    impl_->sock.set_qos_class(QosTrafficClass::kSyncControl);  // DSCP CS7/CS6 for Tally & Intercom
    impl_->sock.set_nonblocking(true);
    impl_->init_codec();
    impl_->my_camera_id = camera_id ? camera_id : 1u;
    impl_->my_label = label.empty() ? ("CAM " + std::to_string(impl_->my_camera_id)) : label.substr(0, 23);
    bound_port_ = impl_->sock.local_port();
    studio_mode_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    // Send initial registration heartbeat to Studio
    TallyStatePayload hello{};
    hello.camera_id = impl_->my_camera_id;
    hello.flags = kTallyFlagFromCamera;
    hello.battery_pct = impl_->my_battery;
    hello.temp_c = impl_->my_temp;
    std::snprintf(hello.camera_label, sizeof(hello.camera_label), "%s", impl_->my_label.c_str());
    impl_->send_tally_dgram_locked(hello, impl_->studio_ep, 2);
    impl_->last_hb_us = now_us();
    return true;
}

void TallyIntercomEngine::stop() {
    running_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->sock.close();
    impl_->tx_stage.clear();
    impl_->rx_fifo.clear();
    impl_->sidetone_fifo.clear();
    impl_->pending_dups.clear();
    impl_->cam_seen_valid.fill(0);
    impl_->cam_last_rx_vcfg_rev = 0;
    impl_->last_vcfg = VideoConfigSyncPayload{};
    impl_->last_tally = TallyStatePayload{};
    impl_->my_assigned_cam_number = 0;
    impl_->my_assigned_cam_tag.clear();
    for (int i = 0; i < kMaxStudioCameras; ++i) impl_->reset_cam_slot_locked(i);
    bound_port_ = 0;
}

void TallyIntercomEngine::set_wifi_redundancy(bool enabled, uint32_t stagger_us) {
    dup_stagger_us_.store(std::clamp<uint32_t>(stagger_us, 0, 10000), std::memory_order_relaxed);
    wifi_redundancy_.store(enabled, std::memory_order_release);
}

bool TallyIntercomEngine::studio_set_camera_tally(uint32_t camera_id, uint8_t tally_state,
                                                  const std::string& label, bool rec_trigger) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const int idx = impl_->find_or_alloc_cam_locked(camera_id, Endpoint{});
    if (idx < 0) return false;
    auto& c = impl_->cams[idx];
    c.tally_state = tally_state <= 2 ? tally_state : 0;
    c.rec_trigger = rec_trigger;
    if (!label.empty()) c.label = label.substr(0, 23);
    ++c.tally_rev;

    TallyStatePayload tp{};
    tp.camera_id = c.camera_id;
    tp.tally_state = c.tally_state;
    tp.flags = uint8_t((c.rec_trigger ? kTallyFlagRecTrigger : 0) | kTallyFlagHighBright);
    tp.revision = c.tally_rev;
    std::snprintf(tp.camera_label, sizeof(tp.camera_label), "%s", c.label.c_str());

    // 3x redundant immediate UDP transmission for instant Wi-Fi tally switching
    if (c.ep.valid()) {
        const bool red = wifi_redundancy_.load(std::memory_order_acquire);
        const uint32_t stag = dup_stagger_us_.load(std::memory_order_relaxed);
        impl_->send_tally_dgram_locked(tp, c.ep, 3, red, stag);
        c.tally_tx += 3;
    }
    return true;
}

bool TallyIntercomEngine::studio_set_camera_control(uint32_t camera_id, const CameraControlPayload& ctl) {
    if (!running_.load(std::memory_order_acquire) || !studio_mode_.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (auto& c : impl_->cams) {
        if (!c.used || c.camera_id != camera_id || !c.ep.valid()) continue;
        CameraControlPayload p = ctl;
        p.camera_id = camera_id;
        p.revision = ++impl_->ctl_revision;
        uint8_t body[kCameraControlSize];
        write_camera_control(body, p);
        PacketHeader ph{};
        ph.type = PacketType::kCameraControl;
        ph.flags = kFlagHasCrc;
        ph.frame_id = p.revision;
        ph.timestamp_us = now_us32();
        uint8_t dgram[kPacketHeaderSize + kCameraControlSize];
        const size_t dlen = write_packet(dgram, sizeof(dgram), ph, body, sizeof(body));
        if (dlen == 0) return false;
        for (int r = 0; r < 3; ++r) impl_->sock.send_to_qos(dgram, dlen, c.ep, QosTrafficClass::kSyncControl);
        return true;
    }
    return false;
}

void TallyIntercomEngine::camera_set_control_callback(CameraControlCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->ctl_cb = std::move(cb);
}

CameraControlPayload TallyIntercomEngine::camera_last_control() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_ctl;
}

void TallyIntercomEngine::camera_set_tally_callback(TallyCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->tally_cb = std::move(cb);
}

TallyStatePayload TallyIntercomEngine::camera_last_tally() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_tally;
}

void TallyIntercomEngine::camera_update_telemetry(uint8_t battery_pct, int8_t temp_c) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->my_battery = std::min<uint8_t>(battery_pct, 100);
    impl_->my_temp = temp_c;
}

bool TallyIntercomEngine::send_video_config_sync(uint32_t camera_id, const VideoConfigSyncPayload& cfg) {
    if (!running_.load(std::memory_order_acquire)) return false;
    const bool is_studio = studio_mode_.load(std::memory_order_relaxed);
    const bool red = wifi_redundancy_.load(std::memory_order_acquire);
    const uint32_t stag = dup_stagger_us_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(impl_->mu);
    VideoConfigSyncPayload p = cfg;
    if (p.origin_id == 0) p.origin_id = impl_->my_origin_id;
    if (p.config_revision == 0) p.config_revision = ++impl_->vcfg_tx_revision;
    p.assigned_cam_tag[15] = '\0';

    if (is_studio) {
        p.flags &= uint8_t(~kVideoConfigFlagFromCamera);
        bool sent_any = false;
        for (auto& c : impl_->cams) {
            if (!c.used || !c.ep.valid()) continue;
            if (camera_id != 0 && c.camera_id != camera_id) continue;
            p.camera_id = c.camera_id;
            if (p.assigned_cam_number > 0) c.assigned_cam_number = p.assigned_cam_number;
            if (p.assigned_cam_tag[0] != '\0') {
                c.assigned_cam_tag = p.assigned_cam_tag;
                c.label = c.assigned_cam_tag;
            }
            c.last_vcfg = p;
            c.config_synced = true;
            impl_->send_vcfg_dgram_locked(p, c.ep, 3, red, stag);
            sent_any = true;
        }
        return sent_any;
    }

    if (!impl_->studio_ep.valid()) return false;
    p.camera_id = impl_->my_camera_id;
    p.flags |= kVideoConfigFlagFromCamera;
    impl_->last_vcfg = p;
    impl_->send_vcfg_dgram_locked(p, impl_->studio_ep, 3, red, stag);
    return true;
}

bool TallyIntercomEngine::studio_request_keyframe(uint32_t camera_id) {
    if (!running_.load(std::memory_order_acquire) || !studio_mode_.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    const VideoConfigSyncPayload req = make_video_keyframe_req(camera_id, ++impl_->vcfg_tx_revision, impl_->my_origin_id);
    bool sent = false;
    for (auto& c : impl_->cams) {
        if (!c.used || !c.ep.valid()) continue;
        if (camera_id != 0 && c.camera_id != camera_id) continue;
        VideoConfigSyncPayload p = req;
        p.camera_id = c.camera_id;
        impl_->send_vcfg_dgram_locked(p, c.ep, 3, false, 1500);
        sent = true;
    }
    return sent;
}

void TallyIntercomEngine::set_video_config_sync_callback(VideoConfigSyncCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->vcfg_cb = std::move(cb);
}

VideoConfigSyncPayload TallyIntercomEngine::last_video_config_sync() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_vcfg;
}

void TallyIntercomEngine::set_intercom_state(bool talk_active, bool listen_muted, float rx_gain,
                                             float sidetone_gain) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->talk_active = talk_active;
    impl_->listen_muted = listen_muted;
    impl_->rx_gain = std::clamp(rx_gain, 0.0f, 2.0f);
    impl_->sidetone_gain = std::clamp(sidetone_gain, 0.0f, 1.0f);
}

void TallyIntercomEngine::studio_set_camera_intercom(uint32_t camera_id, bool talk_to_cam,
                                                     bool listen_from_cam, bool all_call) {
    all_call_.store(all_call, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (camera_id > 0) {
        const int idx = impl_->find_or_alloc_cam_locked(camera_id, Endpoint{});
        if (idx >= 0) {
            impl_->cams[idx].talk_to_cam = talk_to_cam;
            impl_->cams[idx].listen_from_cam = listen_from_cam;
        }
    }
}

void TallyIntercomEngine::push_mic_pcm(const float* samples, int frames) {
    if (!running_.load(std::memory_order_acquire) || !samples || frames <= 0) return;
    float pk = 0.0f;
    for (int i = 0; i < frames; ++i) pk = std::max(pk, std::fabs(samples[i]));
    tx_peak_dbfs_.store(peak_to_dbfs(pk), std::memory_order_relaxed);

    const bool is_studio = studio_mode_.load(std::memory_order_relaxed);
    const bool all_call_flag = all_call_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(impl_->mu);
    bool should_tx = impl_->talk_active;
    if (is_studio) {
        if (all_call_flag) should_tx = true;
        else {
            for (const auto& c : impl_->cams) {
                if (c.used && c.talk_to_cam) { should_tx = true; break; }
            }
        }
    }
    if (!should_tx) {
        impl_->tx_stage.clear();
        return;
    }

    const bool red = wifi_redundancy_.load(std::memory_order_acquire);
    const uint32_t stag = dup_stagger_us_.load(std::memory_order_relaxed);
    if (impl_->sidetone_gain > 0.0f) {
        const float sg = impl_->sidetone_gain;
        for (int i = 0; i < frames; ++i) {
            impl_->sidetone_fifo.push_back(samples[i] * sg);
        }
        while (impl_->sidetone_fifo.size() > 9600) impl_->sidetone_fifo.pop_front();
    }
    impl_->tx_stage.insert(impl_->tx_stage.end(), samples, samples + frames);
    while (int(impl_->tx_stage.size()) >= kIntercomFrameSamples) {
        impl_->encode_and_send_voice_locked(impl_->tx_stage.data(), kIntercomFrameSamples,
                                            is_studio, all_call_flag, tx_pkts_, red, stag);
        impl_->tx_stage.erase(impl_->tx_stage.begin(), impl_->tx_stage.begin() + kIntercomFrameSamples);
    }
}

int TallyIntercomEngine::pull_rx_pcm(float* out_samples, int frames) {
    if (!out_samples || frames <= 0) return 0;
    std::lock_guard<std::mutex> lk(impl_->mu);
    const bool is_studio = studio_mode_.load(std::memory_order_relaxed);
    const float g = impl_->listen_muted ? 0.0f : impl_->rx_gain;
    std::fill(out_samples, out_samples + frames, 0.0f);
    int n = 0;
    if (is_studio) {
        for (auto& c : impl_->cams) {
            if (!c.used || !c.listen_from_cam || c.rx_fifo.empty()) continue;
            const int take = std::min<int>(frames, int(c.rx_fifo.size()));
            for (int i = 0; i < take; ++i) {
                out_samples[i] += c.rx_fifo.front() * g;
                c.rx_fifo.pop_front();
            }
            if (take > n) n = take;
        }
    } else {
        n = std::min<int>(frames, int(impl_->rx_fifo.size()));
        for (int i = 0; i < n; ++i) {
            out_samples[i] = impl_->rx_fifo.front() * g;
            impl_->rx_fifo.pop_front();
        }
    }
    if (!impl_->sidetone_fifo.empty()) {
        const int st_n = std::min<int>(frames, int(impl_->sidetone_fifo.size()));
        for (int i = 0; i < st_n; ++i) {
            out_samples[i] += impl_->sidetone_fifo.front();
            impl_->sidetone_fifo.pop_front();
        }
        if (st_n > n) n = st_n;
    }
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) {
        out_samples[i] = std::clamp(out_samples[i], -1.0f, 1.0f);
        pk = std::max(pk, std::fabs(out_samples[i]));
    }
    rx_peak_dbfs_.store(peak_to_dbfs(pk), std::memory_order_relaxed);
    return n;
}

bool TallyIntercomEngine::is_director_speaking() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_director_voice_us != 0 && (now_us() - impl_->last_director_voice_us) < 350'000ull;
}

void TallyIntercomEngine::poll() {
    if (!running_.load(std::memory_order_acquire)) return;
    const uint64_t now = now_us();
    const bool is_studio = studio_mode_.load(std::memory_order_relaxed);

    uint8_t buf[2048];
    Endpoint src{};
    std::vector<std::pair<TallyCallback, TallyStatePayload>> tally_events;

    for (int iter = 0; iter < 64; ++iter) {
        int n = 0;
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            if (!impl_->sock.is_open()) break;
            n = impl_->sock.recv_from(buf, sizeof(buf), src);
        }
        if (n <= 0) break;
        PacketHeader h{};
        const uint8_t* pl = nullptr;
        if (parse_packet(buf, size_t(n), h, &pl) != PacketParseStatus::kOk) continue;

        if (h.type == PacketType::kTallyState) {
            TallyStatePayload tp{};
            if (!parse_tally_state(pl, h.payload_size, tp)) continue;
            std::lock_guard<std::mutex> lk(impl_->mu);
            if (is_studio) {
                const int idx = impl_->find_or_alloc_cam_locked(tp.camera_id, src, now);
                if (idx >= 0) {
                    auto& c = impl_->cams[idx];
                    c.ep = src;
                    c.last_seen_us = now;
                    c.battery_pct = tp.battery_pct;
                    c.temp_c = tp.temp_c;
                    if (tp.camera_label[0] != '\0' && c.tally_rev == 0) c.label = tp.camera_label;
                }
            } else {
                if (tp.camera_id == 0 || tp.camera_id == impl_->my_camera_id) {
                    if (impl_->last_tally.revision != 0 && tp.revision != 0 &&
                        int32_t(tp.revision - impl_->last_tally.revision) < 0) {
                        continue;  // stale out-of-order Tally packet
                    }
                    const bool changed = (tp.tally_state != impl_->last_tally.tally_state) ||
                                         (tp.flags != impl_->last_tally.flags) ||
                                         (tp.revision != impl_->last_tally.revision);
                    impl_->last_tally = tp;
                    if (changed && impl_->tally_cb) {
                        tally_events.emplace_back(impl_->tally_cb, tp);
                    }
                }
            }
        } else if (h.type == PacketType::kCameraControl && !is_studio) {
            CameraControlPayload cc{};
            if (!parse_camera_control(pl, h.payload_size, cc)) continue;
            CameraControlCallback ccb;
            {
                std::lock_guard<std::mutex> lk(impl_->mu);
                if ((cc.camera_id != 0 && cc.camera_id != impl_->my_camera_id) ||
                    (impl_->last_ctl.revision != 0 && int32_t(cc.revision - impl_->last_ctl.revision) <= 0))
                    continue;  // other camera, duplicate or stale
                impl_->last_ctl = cc;
                ccb = impl_->ctl_cb;
            }
            if (ccb) ccb(cc);
        } else if (h.type == PacketType::kVideoConfigSync) {
            VideoConfigSyncPayload vc{};
            if (!parse_video_config_sync(pl, h.payload_size, vc)) continue;
            VideoConfigSyncCallback vcb;
            {
                std::lock_guard<std::mutex> lk(impl_->mu);
                if (vc.origin_id != 0 && vc.origin_id == impl_->my_origin_id) continue;  // Echo Guard
                if (is_studio) {
                    const int idx = impl_->find_or_alloc_cam_locked(vc.camera_id, src, now);
                    if (idx < 0) continue;
                    auto& c = impl_->cams[idx];
                    c.ep = src;
                    c.last_seen_us = now;
                    const bool is_idr = (vc.flags & kVideoConfigFlagRequestIdr) != 0;
                    if (!is_idr && vc.config_revision != 0 && c.last_rx_vcfg_rev != 0 &&
                        vc.config_revision <= c.last_rx_vcfg_rev) {
                        continue;  // duplicate / stale
                    }
                    if (vc.config_revision != 0) c.last_rx_vcfg_rev = vc.config_revision;
                    c.last_vcfg = vc;
                    c.config_synced = true;
                    if (vc.assigned_cam_number > 0) c.assigned_cam_number = vc.assigned_cam_number;
                    if (vc.assigned_cam_tag[0] != '\0') c.assigned_cam_tag = vc.assigned_cam_tag;
                    vcb = impl_->vcfg_cb;
                } else {
                    if (vc.camera_id != 0 && vc.camera_id != impl_->my_camera_id) continue;
                    const bool is_idr = (vc.flags & kVideoConfigFlagRequestIdr) != 0;
                    if (!is_idr && vc.config_revision != 0 && impl_->cam_last_rx_vcfg_rev != 0 &&
                        vc.config_revision <= impl_->cam_last_rx_vcfg_rev) {
                        continue;
                    }
                    if (vc.config_revision != 0) impl_->cam_last_rx_vcfg_rev = vc.config_revision;
                    impl_->last_vcfg = vc;
                    if (vc.assigned_cam_number > 0) impl_->my_assigned_cam_number = vc.assigned_cam_number;
                    if (vc.assigned_cam_tag[0] != '\0') {
                        impl_->my_assigned_cam_tag = vc.assigned_cam_tag;
                        impl_->my_label = impl_->my_assigned_cam_tag.substr(0, 23);
                    }
                    vcb = impl_->vcfg_cb;
                }
            }
            if (vcb) vcb(vc);
        } else if (h.type == PacketType::kIntercomAudio) {
            IntercomAudioHeader ih{};
            if (!parse_intercom_header(pl, h.payload_size, ih)) continue;
            const uint8_t* voice_data = pl + kIntercomHeaderSize;
            const size_t voice_len = h.payload_size - kIntercomHeaderSize;
            float pcm[1920];

            std::lock_guard<std::mutex> lk(impl_->mu);
            Impl::CamSlot* cam_slot = nullptr;
            if (is_studio) {
                const int idx = impl_->find_or_alloc_cam_locked(ih.camera_id, src, now);
                if (idx < 0) continue;  // all camera slots occupied
                auto& c = impl_->cams[idx];
                cam_slot = &c;
                c.ep = src;
                c.last_seen_us = now;
                if (Impl::check_and_mark_seq(c.seen_seq, c.seen_valid, ih.seq)) {
                    dup_drops_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                c.last_voice_us = now;
                ++c.ic_rx;
                if (!c.listen_from_cam) continue;
            } else {
                if (ih.camera_id != 0 && ih.camera_id != impl_->my_camera_id) continue;
                if (Impl::check_and_mark_seq(impl_->cam_seen_seq, impl_->cam_seen_valid, ih.seq)) {
                    dup_drops_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                impl_->last_director_voice_us = now;
            }

            const int dec_n = impl_->decode_voice_locked(ih, voice_data, voice_len, pcm, 1920, cam_slot);
            if (dec_n > 0) {
                float pk = 0.0f;
                auto& target_fifo = cam_slot ? cam_slot->rx_fifo : impl_->rx_fifo;
                for (int i = 0; i < dec_n; ++i) {
                    pk = std::max(pk, std::fabs(pcm[i]));
                    target_fifo.push_back(pcm[i]);
                }
                while (target_fifo.size() > 9600) target_fifo.pop_front();  // cap 200 ms max
                rx_pkts_.fetch_add(1, std::memory_order_relaxed);
                if (cam_slot) cam_slot->voice_peak_dbfs = peak_to_dbfs(pk);
            }
        }
    }

    for (const auto& ev : tally_events) {
        if (ev.first) ev.first(ev.second);
    }

    // Update smooth program ducking gain on Camera side (0.25 while director speaks, 1.0 otherwise)
    if (!is_studio) {
        const bool speaking = is_director_speaking();
        const float target = speaking ? 0.25f : 1.0f;
        const float cur = ducking_gain_.load(std::memory_order_relaxed);
        ducking_gain_.store(cur + (target - cur) * (speaking ? 0.35f : 0.12f), std::memory_order_relaxed);
    }

    // Flush staggered +1.5 ms Wi-Fi redundant duplicates & run 250 ms Tally heartbeat
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->flush_dups_locked(now);
    if (now - impl_->last_hb_us >= 250'000ull) {
        impl_->last_hb_us = now;
        if (is_studio) {
            for (auto& c : impl_->cams) {
                if (!c.used || !c.ep.valid()) continue;
                TallyStatePayload tp{};
                tp.camera_id = c.camera_id;
                tp.tally_state = c.tally_state;
                tp.flags = uint8_t((c.rec_trigger ? kTallyFlagRecTrigger : 0) | kTallyFlagHighBright);
                tp.revision = c.tally_rev;
                std::snprintf(tp.camera_label, sizeof(tp.camera_label), "%s", c.label.c_str());
                impl_->send_tally_dgram_locked(tp, c.ep, 1);
                ++c.tally_tx;
            }
        } else if (impl_->studio_ep.valid()) {
            TallyStatePayload hb{};
            hb.camera_id = impl_->my_camera_id;
            hb.flags = kTallyFlagFromCamera;
            hb.battery_pct = impl_->my_battery;
            hb.temp_c = impl_->my_temp;
            std::snprintf(hb.camera_label, sizeof(hb.camera_label), "%s", impl_->my_label.c_str());
            impl_->send_tally_dgram_locked(hb, impl_->studio_ep, 1);
        }
    }
}

std::vector<StudioCameraInfo> TallyIntercomEngine::studio_cameras() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const uint64_t now = now_us();
    std::vector<StudioCameraInfo> out;
    for (const auto& c : impl_->cams) {
        if (!c.used) continue;
        StudioCameraInfo info{};
        info.camera_id = c.camera_id;
        info.label = c.label;
        info.address = c.ep.valid() ? c.ep.to_string() : "offline";
        info.tally_state = c.tally_state;
        info.rec_trigger = c.rec_trigger;
        info.talk_to_cam = c.talk_to_cam;
        info.listen_from_cam = c.listen_from_cam;
        info.battery_pct = c.battery_pct;
        info.temp_c = c.temp_c;
        info.operator_speaking = (c.last_voice_us != 0 && (now - c.last_voice_us) < 350'000ull);
        info.voice_peak_dbfs = c.voice_peak_dbfs;
        info.connected = (c.last_seen_us != 0 && (now - c.last_seen_us) < 10'000'000ull);
        info.tally_tx_count = c.tally_tx;
        info.intercom_rx_pkts = c.ic_rx;
        info.intercom_tx_pkts = c.ic_tx;
        info.assigned_cam_number = c.assigned_cam_number;
        info.assigned_cam_tag = c.assigned_cam_tag;
        info.config_synced = c.config_synced;
        info.last_video_config = c.last_vcfg;
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace lv
