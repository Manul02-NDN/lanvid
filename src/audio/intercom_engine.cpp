#include "audio/intercom_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
        std::array<uint32_t, 256> seen_seq{};
        std::array<uint8_t, 256> seen_valid{};
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

    void destroy_codec() {
#ifdef LV_HAVE_OPUS
        if (enc) { opus_encoder_destroy(enc); enc = nullptr; }
        if (dec) { opus_decoder_destroy(dec); dec = nullptr; }
#endif
    }

    ~Impl() { destroy_codec(); }

    int find_or_alloc_cam_locked(uint32_t cid, const Endpoint& ep) {
        for (int i = 0; i < kMaxStudioCameras; ++i) {
            if (cams[i].used && ((cid != 0 && cams[i].camera_id == cid) || (ep.valid() && cams[i].ep == ep))) {
                return i;
            }
        }
        for (int i = 0; i < kMaxStudioCameras; ++i) {
            if (!cams[i].used) {
                cams[i] = CamSlot{};
                cams[i].used = true;
                cams[i].camera_id = cid ? cid : uint32_t(i + 1);
                cams[i].label = "CAM " + std::to_string(i + 1);
                cams[i].ep = ep;
                cams[i].listen_from_cam = true;
                return i;
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

    int decode_voice_locked(const IntercomAudioHeader& ih, const uint8_t* data, size_t len, float* out) {
        if (ih.codec == 1) {
#ifdef LV_HAVE_OPUS
            if (dec && len > 0) {
                const int d = opus_decode_float(dec, data, int(len), out, int(ih.num_samples), 0);
                if (d > 0) return d;
            }
#endif
            return 0;
        }
        const int n = std::min<int>(int(ih.num_samples), int(len / 2));
        for (int i = 0; i < n; ++i) {
            const int16_t v = int16_t(uint16_t(data[2 * i]) | (uint16_t(data[2 * i + 1]) << 8));
            out[i] = float(v) * (1.0f / 32768.0f);
        }
        return n;
    }
};

TallyIntercomEngine::TallyIntercomEngine() : impl_(std::make_unique<Impl>()) {}
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
    impl_->pending_dups.clear();
    impl_->cam_seen_valid.fill(0);
    for (auto& c : impl_->cams) c = Impl::CamSlot{};
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
    const int avail = int(impl_->rx_fifo.size());
    const int n = std::min(frames, avail);
    const float g = impl_->listen_muted ? 0.0f : impl_->rx_gain;
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float s = std::clamp(impl_->rx_fifo.front() * g, -1.0f, 1.0f);
        impl_->rx_fifo.pop_front();
        out_samples[i] = s;
        pk = std::max(pk, std::fabs(s));
    }
    for (int i = n; i < frames; ++i) out_samples[i] = 0.0f;
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
    TallyCallback cb_to_fire;
    TallyStatePayload cb_payload{};
    bool fire_cb = false;

    for (int iter = 0; iter < 64; ++iter) {
        const int n = impl_->sock.recv_from(buf, sizeof(buf), src);
        if (n <= 0) break;
        PacketHeader h{};
        const uint8_t* pl = nullptr;
        if (parse_packet(buf, size_t(n), h, &pl) != PacketParseStatus::kOk) continue;

        if (h.type == PacketType::kTallyState) {
            TallyStatePayload tp{};
            if (!parse_tally_state(pl, h.payload_size, tp)) continue;
            std::lock_guard<std::mutex> lk(impl_->mu);
            if (is_studio) {
                const int idx = impl_->find_or_alloc_cam_locked(tp.camera_id, src);
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
                    const bool changed = (tp.tally_state != impl_->last_tally.tally_state) ||
                                         (tp.flags != impl_->last_tally.flags) ||
                                         (tp.revision != impl_->last_tally.revision);
                    impl_->last_tally = tp;
                    if (changed && impl_->tally_cb) {
                        cb_to_fire = impl_->tally_cb;
                        cb_payload = tp;
                        fire_cb = true;
                    }
                }
            }
        } else if (h.type == PacketType::kIntercomAudio) {
            IntercomAudioHeader ih{};
            if (!parse_intercom_header(pl, h.payload_size, ih)) continue;
            const uint8_t* voice_data = pl + kIntercomHeaderSize;
            const size_t voice_len = h.payload_size - kIntercomHeaderSize;
            float pcm[1920];

            std::lock_guard<std::mutex> lk(impl_->mu);
            if (is_studio) {
                const int idx = impl_->find_or_alloc_cam_locked(ih.camera_id, src);
                if (idx >= 0) {
                    auto& c = impl_->cams[idx];
                    c.ep = src;
                    c.last_seen_us = now;
                    if (Impl::check_and_mark_seq(c.seen_seq, c.seen_valid, ih.seq)) {
                        dup_drops_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    c.last_voice_us = now;
                    ++c.ic_rx;
                    if (!c.listen_from_cam) continue;
                }
            } else {
                if (ih.camera_id != 0 && ih.camera_id != impl_->my_camera_id) continue;
                if (Impl::check_and_mark_seq(impl_->cam_seen_seq, impl_->cam_seen_valid, ih.seq)) {
                    dup_drops_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                impl_->last_director_voice_us = now;
            }

            const int dec_n = impl_->decode_voice_locked(ih, voice_data, voice_len, pcm);
            if (dec_n > 0) {
                float pk = 0.0f;
                for (int i = 0; i < dec_n; ++i) {
                    pk = std::max(pk, std::fabs(pcm[i]));
                    impl_->rx_fifo.push_back(pcm[i]);
                }
                while (impl_->rx_fifo.size() > 9600) impl_->rx_fifo.pop_front();  // cap 200 ms max
                rx_pkts_.fetch_add(1, std::memory_order_relaxed);
                if (is_studio) {
                    const int idx = impl_->find_or_alloc_cam_locked(ih.camera_id, src);
                    if (idx >= 0) impl_->cams[idx].voice_peak_dbfs = peak_to_dbfs(pk);
                }
            }
        }
    }

    if (fire_cb && cb_to_fire) cb_to_fire(cb_payload);

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
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace lv
