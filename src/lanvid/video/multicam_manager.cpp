#include "video/multicam_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "net/assembler.hpp"
#include "net/packet.hpp"
#include "net/video_receiver.hpp"
#include "common/udp_socket.hpp"

namespace lv {

namespace fs = std::filesystem;

struct MultiCamManager::Slot {
    uint32_t index = 0;
    std::unique_ptr<UdpReceiver> rx;
    // Raw pointer for the frame callback: the receiver joins its decode worker in its destructor, so a
    // callback in flight always sees a live object (unique_ptr::reset nulls `rx` before deleting).
    std::atomic<UdpReceiver*> rx_raw{nullptr};
    std::thread th;                      // sequential-port mode receive loop
    std::atomic<bool> th_stop{false};

    // binding (slots_mu_)
    bool bound = false;
    uint32_t ip_be = 0;
    uint16_t port = 0;
    uint64_t last_pkt_us = 0;          // last datagram from the bound sender (demux thread)
    std::string pinned_ip;
    uint16_t listen_port = 0;

    // identity / telemetry (info_mu)
    mutable std::mutex info_mu;
    std::string label;
    std::string source_name;
    uint32_t camera_id = 0;
    int battery = -1;
    int temp = -100;
    uint8_t tally = 0;
    uint8_t assigned_cam_number = 0;
    std::string assigned_cam_tag;
    uint8_t badge_accent_rgba[4] = {0, 0, 0, 0};
    bool config_synced = false;

    // video + playout buffer (pub_mu -> frame_mu -> bus_mu_)
    mutable std::mutex pub_mu;
    mutable VideoPlayoutBuffer playout;
    mutable uint64_t playout_pub_seq = 0;
    mutable std::mutex frame_mu;
    Image frame;
    uint64_t frame_seq = 0;
    std::atomic<uint64_t> last_frame_us{0};
    std::atomic<int> codec{-1};
    std::atomic<uint64_t> frames{0};
    double fps = 0.0;
    uint64_t fps_t0 = 0;
    uint32_t fps_n = 0;
    double latency_ms = 0.0;
    std::atomic<float> audio_peak{-120.0f};
    mutable uint64_t br_t0 = 0, br_b0 = 0;
    mutable double br_mbps = 0.0;

    // recording (rec_mu)
    mutable std::mutex rec_mu;
    bool rec_on = false;
    std::unique_ptr<IsoRecorder> rec;
    std::string rec_path;
    std::unique_ptr<IVideoEncoder> enc;   // DCT -> H.264
    int enc_w = 0, enc_h = 0;
    mutable uint64_t wm_t0 = 0, wm_b0 = 0;
    mutable double wm_mbps = 0.0;
};

namespace {
std::string ip_string(uint32_t ip_be) {
    Endpoint e;
    e.ipv4_be = ip_be;
    e.port = 1;
    std::string s = e.to_string();
    const auto c = s.rfind(':');
    return c == std::string::npos ? s : s.substr(0, c);
}

std::string sanitize(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_') o += char(c);
        else if (c == ' ' || c == '.' || c == '/') o += '_';
        // non-ASCII (e.g. "•") dropped: file names stay portable on every OS / NLE
    }
    while (o.find("__") != std::string::npos) o.erase(o.find("__"), 1);
    while (!o.empty() && o.back() == '_') o.pop_back();
    while (!o.empty() && o.front() == '_') o.erase(o.begin());
    return o.empty() ? "Camera" : o;
}

const char* codec_name(int c) {
    switch (c) {
        case int(VideoCodecId::kH264): return "H.264";
        case int(VideoCodecId::kH265): return "H.265";
        case int(VideoCodecId::kLanVidDct): return "DCT";
        default: return "";
    }
}

// Age of a timestamp written by another thread: it may be newer than `now` (taken a moment earlier).
inline uint64_t age_us(uint64_t now, uint64_t t) { return t >= now ? 0 : now - t; }

float peak_dbfs_pcm16(const int16_t* p, size_t n) {
    int m = 0;
    for (size_t i = 0; i < n; ++i) m = std::max(m, std::abs(int(p[i])));
    return m > 0 ? 20.0f * std::log10(float(m) / 32768.0f) : -120.0f;
}
}  // namespace

MultiCamManager::MultiCamManager() = default;
MultiCamManager::~MultiCamManager() { stop(); }

MultiCamManager::Slot* MultiCamManager::slot_ptr(uint32_t i) const {
    return i < slots_.size() ? slots_[i].get() : nullptr;
}

bool MultiCamManager::start(const MultiCamConfig& cfg) {
    stop();
    cfg_ = cfg;
    cfg_.max_slots = std::clamp<uint32_t>(cfg_.max_slots, 1, kMultiCamMaxSlots);
    slots_.clear();
    for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
        auto s = std::make_unique<Slot>();
        s->index = i;
        s->label = "CAM " + std::to_string(i + 1);
        s->assigned_cam_number = uint8_t(i + 1);
        s->assigned_cam_tag = s->label;
        s->listen_port = cfg_.port_mode == MultiCamPortMode::kSequentialPorts ? uint16_t(cfg_.base_port + 2 * i)
                                                                               : cfg_.base_port;
        VideoPlayoutConfig pc;
        pc.fixed_delay_mode = cfg_.video_fixed_delay;
        pc.target_delay_ms = std::clamp<uint32_t>(cfg_.video_delay_ms, 5, 250);
        s->playout.set_config(pc);
        s->playout.set_idr_request_callback([this, i]() { request_idr(i); });
        slots_.push_back(std::move(s));
    }
    stop_.store(false);
    iso_stop_.store(false);
    if (cfg_.publish_video_bus) {
        if (bus_.open(cfg_.video_bus_name.empty() ? nullptr : cfg_.video_bus_name.c_str())) {
            bus_.set_active_vcams(cfg_.vcam_count, cfg_.vcam_placeholder);
            bus_.heartbeat();
        }
    }
    if (cfg_.port_mode == MultiCamPortMode::kSingleDemux) {
        demux_sock_ = std::make_unique<UdpSocket>();
        if (!demux_sock_->open() || !demux_sock_->bind(cfg_.bind_ip, cfg_.base_port)) {
            demux_sock_.reset();
            bus_.close();
            return false;
        }
        demux_sock_->set_buffer_sizes(UdpSocket::kMinSocketBuffer, 32 * 1024 * 1024);
        demux_sock_->set_low_latency_qos();
        demux_sock_->set_recv_timeout_ms(2);
        bound_base_port_ = demux_sock_->local_port();
    } else {
        // every slot listens on its own port right away
        for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
            if (!ensure_receiver(i)) {
                stop();
                return false;
            }
        }
        bound_base_port_ = cfg_.base_port;
    }
    iso_thread_ = std::thread([this] { iso_worker_loop(); });
    running_.store(true, std::memory_order_release);
    demux_thread_ = std::thread([this] { demux_loop(); });
    return true;
}

void MultiCamManager::stop() {
    rec_stop_all();
    stop_.store(true);
    if (demux_thread_.joinable()) demux_thread_.join();
    for (auto& s : slots_) {
        if (!s) continue;
        s->th_stop.store(true);
        if (s->th.joinable()) s->th.join();
    }
    // Move receivers out under slots_mu_ and destroy them outside the lock (avoids AB-BA deadlock with vdec_thread -> request_idr).
    std::vector<std::unique_ptr<UdpReceiver>> rx_to_destroy;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        for (auto& s : slots_) {
            if (s && s->rx) {
                s->rx_raw.store(nullptr, std::memory_order_release);
                rx_to_destroy.push_back(std::move(s->rx));
            }
        }
    }
    rx_to_destroy.clear();
    {
        std::lock_guard<std::mutex> lk(iso_q_mu_);
        iso_stop_.store(true);
    }
    iso_q_cv_.notify_all();
    if (iso_thread_.joinable()) iso_thread_.join();
    demux_sock_.reset();
    {
        std::lock_guard<std::mutex> lk(bus_mu_);
        if (bus_.is_open()) {
            for (uint32_t i = 0; i < kVideoBusSlots; ++i) bus_.set_slot_info(i, nullptr, nullptr, false, false);
            bus_.close();
        }
    }
    running_.store(false, std::memory_order_release);
}

bool MultiCamManager::ensure_receiver(uint32_t i) {
    Slot* s = slot_ptr(i);
    if (!s) return false;
    if (s->rx) return true;
    ReceiverConfig rc;
    rc.bind_ip = cfg_.bind_ip;
    rc.bind_port = s->listen_port;
    rc.assembler.output_order = PixelOrder::kBGRA;
    rc.audio_target_ms = double(cfg_.audio_latency_ms);
    rc.decode_threads = 2;
    auto rx = std::make_unique<UdpReceiver>(rc, [this, i](const CompletedFrame& cf) { on_frame(i, cf); });
    rx->set_video_bitstream_callback([this, i](VideoCodecId c, const uint8_t* d, size_t n, bool key, uint32_t,
                                               uint64_t arrival_us) { on_bitstream(i, int(c), d, n, key, arrival_us); });
    rx->set_audio_callback([this, i](uint32_t, uint32_t, const AudioHeader& h, const uint8_t* pcm) { on_audio(i, h, pcm); });
    if (cfg_.port_mode == MultiCamPortMode::kSingleDemux) {
        if (!rx->init_external(demux_sock_.get())) return false;
        s->rx = std::move(rx);
        s->rx_raw.store(s->rx.get(), std::memory_order_release);
    } else {
        if (!rx->init()) return false;
        s->rx = std::move(rx);
        s->rx_raw.store(s->rx.get(), std::memory_order_release);
        s->th_stop.store(false);
        UdpReceiver* r = s->rx.get();
        s->th = std::thread([this, s, r] {
            while (!s->th_stop.load(std::memory_order_relaxed)) {
                r->step_once();
                Endpoint ep;
                if (r->sender_endpoint(ep)) {
                    std::lock_guard<std::mutex> lk(slots_mu_);
                    if (!s->bound) {
                        s->ip_be = ep.ipv4_be;
                        s->port = ep.port;
                        s->bound = true;
                    }
                }
            }
        });
    }
    return true;
}

int MultiCamManager::slot_for_sender(uint32_t ip_be, uint16_t port, bool allow_assign) {
    // exact match (same phone, same socket)
    for (auto& s : slots_)
        if (s->bound && s->ip_be == ip_be && s->port == port) return int(s->index);
    // same phone reconnected from a new source port (its old stream has been silent > 2 s), or a slot
    // pinned to this IP. Several cameras behind one IP (NAT, emulators, tests) keep separate slots.
    const std::string ip = ip_string(ip_be);
    const uint64_t now = now_us();
    for (auto& s : slots_) {
        const bool reconnect = s->bound && s->ip_be == ip_be && age_us(now, s->last_pkt_us) > 2'000'000;
        if (reconnect || (!s->bound && !s->pinned_ip.empty() && s->pinned_ip == ip)) {
            s->bound = true;
            s->ip_be = ip_be;
            s->port = port;
            s->last_pkt_us = now;
            return int(s->index);
        }
    }
    if (!allow_assign || !cfg_.auto_assign) return -1;
    for (auto& s : slots_) {
        if (!s->bound && s->pinned_ip.empty()) {
            s->bound = true;
            s->ip_be = ip_be;
            s->port = port;
            {
                std::lock_guard<std::mutex> lk(s->info_mu);
                if (s->source_name.empty()) s->source_name = ip;
            }
            return int(s->index);
        }
    }
    return -1;  // all slots busy
}

void MultiCamManager::inject_datagram(const uint8_t* data, size_t len, uint32_t src_ip_be, uint16_t src_port) {
    std::lock_guard<std::mutex> lk(slots_mu_);
    if (cfg_.port_mode != MultiCamPortMode::kSingleDemux) return;
    const int si = slot_for_sender(src_ip_be, src_port, true);
    if (si < 0 || !ensure_receiver(uint32_t(si))) return;
    Endpoint ep;
    ep.ipv4_be = src_ip_be;
    ep.port = src_port;
    slots_[si]->last_pkt_us = now_us();
    slots_[si]->rx->ingest_datagram(data, len, ep, now_us());
}

void MultiCamManager::demux_loop() {
    constexpr int kBatch = 64;
    std::vector<uint8_t> storage(size_t(kBatch) * 2048);
    RecvDatagram batch[kBatch];
    uint64_t next_house = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        uint64_t now = 0;
        if (demux_sock_) {
            for (int k = 0; k < kBatch; ++k) batch[k] = {storage.data() + size_t(k) * 2048, 2048, 0, {}};
            const int got = demux_sock_->recv_batch(batch, kBatch);
            now = now_us();
            std::lock_guard<std::mutex> lk(slots_mu_);
            for (int k = 0; k < got; ++k) {
                if (!batch[k].len) continue;
                const int si = slot_for_sender(batch[k].src.ipv4_be, batch[k].src.port, true);
                if (si < 0 || !ensure_receiver(uint32_t(si))) continue;
                slots_[si]->last_pkt_us = now;
                slots_[si]->rx->ingest_datagram(batch[k].data, batch[k].len, batch[k].src, now);
            }
            for (auto& s : slots_)
                if (s->rx) s->rx->service(now);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            now = now_us();
        }
        for (uint32_t i = 0; i < slots_.size(); ++i) publish_slot_playout(i, now);
        if (now >= next_house) {  // 5 Hz: virtual-camera slot state + bus heartbeat
            next_house = now + 200000;
            std::lock_guard<std::mutex> blk(bus_mu_);
            if (bus_.is_open()) {
                bus_.heartbeat();
                for (auto& s : slots_) {
                    if (s->index >= kVideoBusCameraSlots) continue;
                    const bool streaming = age_us(now, s->last_frame_us.load()) < 1000000;
                    std::string label, src;
                    {
                        std::lock_guard<std::mutex> lk(s->info_mu);
                        label = s->label;
                        src = s->source_name;
                    }
                    bus_.set_slot_info(s->index, label.c_str(), src.c_str(), streaming, false);
                }
                const int pgm = program_slot();
                const Slot* ps = slot_ptr(uint32_t(std::max(0, pgm)));
                const bool pgm_live = ps && age_us(now, ps->last_frame_us.load()) < 1000000;
                std::string pgm_label;
                if (ps) {
                    std::lock_guard<std::mutex> lk(ps->info_mu);
                    pgm_label = ps->label;
                }
                bus_.set_slot_info(kVideoBusProgramSlot, "LanVid Program Out", pgm_label.c_str(), pgm_live,
                                   false);
            }
        }
    }
}

void MultiCamManager::publish_slot_playout(uint32_t i, uint64_t now) const {
    Slot* s = slot_ptr(i);
    if (!s) return;
    Image out;
    bool got = false;
    {
        std::lock_guard<std::mutex> plk(s->pub_mu);
        uint64_t seq = s->playout_pub_seq;
        if (!s->playout.poll_frame(now, out, seq)) return;
        s->playout_pub_seq = seq;
        got = true;
    }
    if (!got || out.width <= 0) return;

    double cur_fps = 0.0;
    {
        std::lock_guard<std::mutex> lk(s->frame_mu);
        s->frame = out;
        ++s->frame_seq;
        cur_fps = s->fps;
    }

    // ---- OS virtual cameras (isolated from ISO recording) ----
    const bool to_vcam = bus_.is_open() && i < kVideoBusCameraSlots && i < cfg_.vcam_count;
    const bool to_pgm = bus_.is_open() && int(i) == program_slot();
    if (to_vcam || to_pgm) {
        std::lock_guard<std::mutex> lk(bus_mu_);
        const uint32_t w = uint32_t(out.width), h = uint32_t(out.height);
        const uint32_t fps100 = uint32_t(cur_fps * 100.0);
        if (w * h <= 1920u * 1080u) {
            if (to_vcam) bus_.publish(i, VideoBusFormat::kBGRA, w, h, out.data.data(), uint32_t(out.stride), now, fps100);
            if (to_pgm)
                bus_.publish(kVideoBusProgramSlot, VideoBusFormat::kBGRA, w, h, out.data.data(), uint32_t(out.stride), now,
                             fps100);
        } else {
            const uint32_t ew = w & ~1u, eh = h & ~1u;
            nv12_scratch_.resize(size_t(ew) * eh * 3 / 2);
            video_bus_bgra_to_nv12(out.data.data(), ew, eh, uint32_t(out.stride), nv12_scratch_.data(), ew);
            if (to_vcam) bus_.publish(i, VideoBusFormat::kNV12, ew, eh, nv12_scratch_.data(), ew, now, fps100);
            if (to_pgm)
                bus_.publish(kVideoBusProgramSlot, VideoBusFormat::kNV12, ew, eh, nv12_scratch_.data(), ew, now, fps100);
        }
    }
}

void MultiCamManager::iso_worker_loop() {
    for (;;) {
        IsoJob job;
        {
            std::unique_lock<std::mutex> lk(iso_q_mu_);
            iso_q_cv_.wait(lk, [this] { return iso_stop_.load(std::memory_order_acquire) || !iso_q_.empty(); });
            if (iso_stop_.load(std::memory_order_acquire) && iso_q_.empty()) break;
            job = std::move(iso_q_.front());
            iso_q_.pop_front();
            iso_busy_ = true;
        }

        if (!job.is_program) {
            Slot* s = slot_ptr(job.slot);
            if (s) {
                std::lock_guard<std::mutex> lk(s->rec_mu);
                if (s->rec_on) {
                    if (!s->enc || s->enc_w != job.image.width || s->enc_h != job.image.height) {
                        VideoEncoderConfig ec;
                        ec.width = job.image.width;
                        ec.height = job.image.height;
                        double fps_val = 30.0;
                        {
                            std::lock_guard<std::mutex> fl(s->frame_mu);
                            if (s->fps > 1.0) fps_val = s->fps;
                        }
                        ec.fps = std::max(24.0, fps_val);
                        ec.bitrate_kbps = uint32_t(std::clamp(job.image.width * job.image.height / 100, 4000, 40000));
                        ec.keyframe_interval = 60;
                        s->enc = create_video_encoder(VideoCodecId::kH264, ec);
                        s->enc_w = job.image.width;
                        s->enc_h = job.image.height;
                    }
                    if (s->enc) {
                        EncodedVideoPacket pkt;
                        if (s->enc->encode_image(job.image, pkt)) {
                            if (!s->rec) open_slot_recorder(job.slot, IsoVideoCodec::kH264, job.image.width, job.image.height);
                            if (s->rec)
                                s->rec->write_video(pkt.data.data(), pkt.data.size(), pkt.is_keyframe, job.timestamp_us);
                        }
                    }
                }
            }
        } else {
            std::lock_guard<std::mutex> lk(rec_mu_);
            if (session_active_ && session_.record_program) {
                constexpr int kW = 1920, kH = 1080;
                if (!program_enc_) {
                    VideoEncoderConfig ec;
                    ec.width = kW;
                    ec.height = kH;
                    ec.fps = 30.0;
                    ec.bitrate_kbps = 16000;
                    ec.keyframe_interval = 60;
                    program_enc_ = create_video_encoder(VideoCodecId::kH264, ec);
                }
                if (program_enc_ && !program_rec_) {
                    IsoRecorderConfig rc;
                    rc.path = session_dir_ + "/PGM_Master." + iso_container_extension(session_.container);
                    rc.container = session_.container;
                    rc.codec = IsoVideoCodec::kH264;
                    rc.width = kW;
                    rc.height = kH;
                    rc.timecode_fps = 30;
                    rc.timecode_start_frame = session_tc_frame_;
                    rc.creation_unix_s = session_unix_s_;
                    rc.camera_label = "PGM Master Mix";
                    program_rec_ = std::make_unique<IsoRecorder>();
                    if (!program_rec_->open(rc)) program_rec_.reset();
                }
                if (program_enc_ && program_rec_) {
                    if (program_scaled_img_.width != kW || program_scaled_img_.height != kH ||
                        program_scaled_img_.order != PixelOrder::kBGRA) {
                        program_scaled_img_.allocate(kW, kH, PixelOrder::kBGRA);
                    }
                    VideoBus::FrameInfo fi;
                    fi.width = uint32_t(job.image.width);
                    fi.height = uint32_t(job.image.height);
                    fi.stride = uint32_t(job.image.stride);
                    fi.format = VideoBusFormat::kBGRA;
                    video_bus_convert_to_bgra(job.image.data.data(), fi, program_scaled_img_.data.data(), kW, kH,
                                              uint32_t(program_scaled_img_.stride));
                    EncodedVideoPacket pkt;
                    if (program_enc_->encode_image(program_scaled_img_, pkt))
                        program_rec_->write_video(pkt.data.data(), pkt.data.size(), pkt.is_keyframe, job.timestamp_us);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(iso_q_mu_);
            iso_busy_ = false;
            if (iso_q_.empty()) iso_idle_cv_.notify_all();
        }
    }
}

void MultiCamManager::drain_iso_queue() {
    std::unique_lock<std::mutex> lk(iso_q_mu_);
    iso_idle_cv_.wait(lk, [this] { return iso_q_.empty() && !iso_busy_; });
}

void MultiCamManager::on_frame(uint32_t i, const CompletedFrame& cf) {
    Slot* s = slot_ptr(i);
    if (!s || !cf.image || cf.image->width <= 0) return;
    const Image& img = *cf.image;
    const uint64_t now = now_us();
    {
        std::lock_guard<std::mutex> lk(s->frame_mu);
        if (!s->fps_t0) s->fps_t0 = now;
        ++s->fps_n;
        if (now - s->fps_t0 >= 1000000) {
            s->fps = double(s->fps_n) * 1e6 / double(now - s->fps_t0);
            s->fps_t0 = now;
            s->fps_n = 0;
        }
        if (UdpReceiver* r = s->rx_raw.load(std::memory_order_acquire))
            s->latency_ms = std::max(0.0, r->estimate_latency_ms(cf.complete_time_us, cf.sender_timestamp_us));
    }
    s->codec.store(int(cf.codec));
    s->frames.fetch_add(1);
    s->last_frame_us.store(now);

    // 1. Enqueue into per-slot VideoPlayoutBuffer (non-blocking, absorbs Wi-Fi jitter)
    const bool corrupt = (cf.slices_corrupt > 0 || cf.slices_lost > 0) && cf.codec != VideoCodecId::kLanVidDct;
    s->playout.push_frame(img, cf.frame_id, cf.sender_timestamp_us, now, cf.is_keyframe, corrupt);
    publish_slot_playout(i, now);

    // 2. Non-blocking dispatch to background ISO encoder/writer (never stalls preview or vCam!)
    bool need_dct_iso = false;
    if (cf.codec == VideoCodecId::kLanVidDct) {
        std::lock_guard<std::mutex> lk(s->rec_mu);
        need_dct_iso = s->rec_on;
    }
    bool need_pgm_iso = false;
    if (int(i) == program_slot()) {
        std::lock_guard<std::mutex> lk(rec_mu_);
        need_pgm_iso = session_active_ && session_.record_program;
    }
    if (need_dct_iso || need_pgm_iso) {
        const uint64_t ts_rel = now > session_t0_us_ ? now - session_t0_us_ : 0;
        std::lock_guard<std::mutex> qlk(iso_q_mu_);
        if (need_dct_iso) {
            if (iso_q_.size() >= 16) iso_q_.pop_front();
            iso_q_.push_back(IsoJob{i, img, ts_rel, false});
        }
        if (need_pgm_iso) {
            if (iso_q_.size() >= 16) iso_q_.pop_front();
            iso_q_.push_back(IsoJob{i, img, ts_rel, true});
        }
        iso_q_cv_.notify_one();
    }
}

void MultiCamManager::on_bitstream(uint32_t i, int codec, const uint8_t* data, size_t size, bool key, uint64_t arrival_us) {
    Slot* s = slot_ptr(i);
    if (!s) return;
    s->codec.store(codec);
    if (codec != int(VideoCodecId::kH264) && codec != int(VideoCodecId::kH265)) return;
    std::lock_guard<std::mutex> lk(s->rec_mu);
    if (!s->rec_on) return;
    if (!s->rec) {
        int w = 1920, h = 1080;
        {
            std::lock_guard<std::mutex> fl(s->frame_mu);
            if (s->frame.width > 0) {
                w = s->frame.width;
                h = s->frame.height;
            }
        }
        if (!open_slot_recorder(i, codec == int(VideoCodecId::kH265) ? IsoVideoCodec::kH265 : IsoVideoCodec::kH264, w, h))
            return;
    }
    const uint64_t t = arrival_us > session_t0_us_ ? arrival_us - session_t0_us_ : 0;
    s->rec->write_video(data, size, key, t);
}

void MultiCamManager::on_audio(uint32_t i, const AudioHeader& hdr, const uint8_t* pcm) {
    Slot* s = slot_ptr(i);
    if (!s || !pcm || hdr.num_frames == 0 || hdr.sample_format != AudioSampleFormat::kPcm16) return;
    const auto* in = reinterpret_cast<const int16_t*>(pcm);
    const size_t n = size_t(hdr.num_frames) * hdr.channels;
    const float pk = peak_dbfs_pcm16(in, n);
    const float prev = s->audio_peak.load();
    s->audio_peak.store(std::max(pk, prev - 1.5f));
    std::lock_guard<std::mutex> lk(s->rec_mu);
    if (!s->rec_on || !s->rec || hdr.sample_rate != 48000) return;
    std::vector<int16_t> st(size_t(hdr.num_frames) * 2);
    for (uint32_t f = 0; f < hdr.num_frames; ++f) {
        const int16_t l = in[size_t(f) * hdr.channels];
        const int16_t r = hdr.channels > 1 ? in[size_t(f) * hdr.channels + 1] : l;
        st[f * 2] = l;
        st[f * 2 + 1] = r;
    }
    const uint64_t now = now_us();
    s->rec->write_audio(st.data(), hdr.num_frames, now > session_t0_us_ ? now - session_t0_us_ : 0);
}

std::string MultiCamManager::slot_file_path(uint32_t i) const {
    const Slot* s = slot_ptr(i);
    std::string label;
    {
        std::lock_guard<std::mutex> lk(s->info_mu);
        label = s->label;
    }
    // "CAM 1 • Wide" -> "CAM1_Wide"; the prefix is always CAM<n>_
    std::string tail = label;
    const std::string pfx = "CAM " + std::to_string(i + 1);
    if (tail.rfind(pfx, 0) == 0) tail = tail.substr(pfx.size());
    std::string clean = sanitize(tail);
    if (clean == "Camera" && tail.find_first_not_of(" ") == std::string::npos) clean.clear();
    std::string base = session_dir_ + "/CAM" + std::to_string(i + 1) + (clean.empty() ? "" : "_" + clean);
    std::string path = base + "." + iso_container_extension(session_.container);
    for (int k = 2; fs::exists(path) && k < 100; ++k)
        path = base + "_" + std::to_string(k) + "." + iso_container_extension(session_.container);
    return path;
}

bool MultiCamManager::open_slot_recorder(uint32_t i, IsoVideoCodec codec, int w, int h) {
    Slot* s = slot_ptr(i);  // caller holds s->rec_mu
    IsoRecorderConfig rc;
    {
        std::lock_guard<std::mutex> lk(rec_mu_);
        rc.path = slot_file_path(i);
        rc.container = session_.container;
        rc.timecode_start_frame = session_tc_frame_;
        rc.creation_unix_s = session_unix_s_;
    }
    rc.codec = codec;
    rc.width = w;
    rc.height = h;
    rc.timecode_fps = 30;
    rc.audio_channels = 2;
    rc.audio_sample_rate = 48000;
    {
        std::lock_guard<std::mutex> lk(s->info_mu);
        rc.camera_label = s->label;
    }
    auto rec = std::make_unique<IsoRecorder>();
    if (!rec->open(rc)) return false;
    s->rec = std::move(rec);
    s->rec_path = rc.path;
    s->wm_t0 = now_us();
    s->wm_b0 = 0;
    return true;
}

std::string MultiCamManager::default_output_dir() {
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    const char* up = std::getenv("USERPROFILE");
    return std::string(up ? up : "C:\\") + "\\Videos\\LanVid";
#elif defined(__APPLE__)
    return std::string(home ? home : "/tmp") + "/Movies/LanVid";
#else
    return std::string(home ? home : "/tmp") + "/Videos/LanVid";
#endif
}

uint64_t MultiCamManager::disk_free_bytes(const std::string& dir) {
    std::error_code ec;
    fs::path p = dir.empty() ? fs::path(default_output_dir()) : fs::path(dir);
    while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
    if (p.empty()) return 0;
    const auto sp = fs::space(p, ec);
    return ec ? 0 : uint64_t(sp.available);
}

bool MultiCamManager::rec_start_all(const IsoSessionConfig& session) {
    {
        std::lock_guard<std::mutex> lk(rec_mu_);
        if (session_active_) return true;
        session_ = session;
        if (session_.output_dir.empty()) session_.output_dir = default_output_dir();
        const std::time_t t = std::time(nullptr);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &lt);
        std::string dir = session_.output_dir + "/" + sanitize(session_.session_name) + "_" + stamp;
        for (int k = 2; fs::exists(dir) && k < 100; ++k)
            dir = session_.output_dir + "/" + sanitize(session_.session_name) + "_" + stamp + "_" + std::to_string(k);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return false;
        session_dir_ = dir;
        session_t0_us_.store(now_us(), std::memory_order_release);
        session_unix_s_ = uint64_t(t);
        session_tc_frame_ = iso_timecode_now(30);
        session_active_ = true;
    }
    for (auto& s : slots_) {
        if (!(session.slot_mask & (1u << s->index))) continue;
        {
            std::lock_guard<std::mutex> lk(s->rec_mu);
            s->rec_on = true;  // file opens with the first keyframe of this camera
        }
        request_idr(s->index);
    }
    return true;
}

void MultiCamManager::request_idr(uint32_t i) {
    Slot* s = slot_ptr(i);
    if (!s) return;
    uint32_t cam_id = 0;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        if (s->bound && s->rx) s->rx->request_keyframe();  // start the file now, not at the next GOP
    }
    {
        std::lock_guard<std::mutex> ilk(s->info_mu);
        cam_id = s->camera_id;
    }
    std::function<void(uint32_t, uint32_t)> cb;
    {
        std::lock_guard<std::mutex> clk(idr_cb_mu_);
        cb = idr_cb_;
    }
    if (cb) cb(i, cam_id);
}

void MultiCamManager::rec_stop_all() {
    for (auto& s : slots_) {
        if (!s) continue;
        std::lock_guard<std::mutex> lk(s->rec_mu);
        s->rec_on = false;
    }
    {
        std::lock_guard<std::mutex> lk(rec_mu_);
        session_active_ = false;
    }
    drain_iso_queue();
    for (auto& s : slots_) {
        if (!s) continue;
        std::lock_guard<std::mutex> lk(s->rec_mu);
        if (s->rec) s->rec->close();
        s->enc.reset();
    }
    std::lock_guard<std::mutex> lk(rec_mu_);
    if (program_rec_) program_rec_->close();
    program_enc_.reset();
}

bool MultiCamManager::rec_slot(uint32_t i, bool on) {
    Slot* s = slot_ptr(i);
    if (!s) return false;
    if (on) {
        bool active;
        IsoSessionConfig sc;
        {
            std::lock_guard<std::mutex> lk(rec_mu_);
            active = session_active_;
            sc = session_;
        }
        if (!active) {
            sc.slot_mask = 0;  // start a session for just this camera
            if (!rec_start_all(sc)) return false;
        }
        {
            std::lock_guard<std::mutex> lk(s->rec_mu);
            s->rec_on = true;
        }
        request_idr(i);
        return true;
    }
    std::lock_guard<std::mutex> lk(s->rec_mu);
    s->rec_on = false;
    if (s->rec) s->rec->close();
    s->enc.reset();
    return true;
}

bool MultiCamManager::recording_any() const {
    for (const auto& s : slots_) {
        std::lock_guard<std::mutex> lk(s->rec_mu);
        if (s->rec_on) return true;
    }
    return false;
}

IsoSlotStats MultiCamManager::rec_stats(uint32_t i) const {
    IsoSlotStats st;
    const Slot* s = slot_ptr(i);
    if (!s) return st;
    std::lock_guard<std::mutex> lk(s->rec_mu);
    st.recording = s->rec_on;
    st.path = s->rec_path;
    if (s->rec) {
        const auto rs = s->rec->stats();
        st.bytes = rs.bytes_written;
        st.duration_us = rs.duration_us;
        st.frames = rs.video_frames;
        st.waiting_for_keyframe = s->rec_on && rs.waiting_for_keyframe;
        const uint64_t now = now_us();
        if (now - s->wm_t0 >= 1000000) {
            const double mb = double(st.bytes - std::min(st.bytes, s->wm_b0)) / (1024.0 * 1024.0);
            s->wm_mbps = mb * 1e6 / double(now - s->wm_t0);
            s->wm_t0 = now;
            s->wm_b0 = st.bytes;
        }
        st.write_mbps = s->rec_on ? s->wm_mbps : 0.0;
    } else {
        st.waiting_for_keyframe = s->rec_on;
    }
    return st;
}

IsoSlotStats MultiCamManager::rec_program_stats() const {
    IsoSlotStats st;
    std::lock_guard<std::mutex> lk(rec_mu_);
    st.recording = session_active_ && session_.record_program;
    if (program_rec_) {
        const auto rs = program_rec_->stats();
        st.path = program_rec_->path();
        st.bytes = rs.bytes_written;
        st.duration_us = rs.duration_us;
        st.frames = rs.video_frames;
    }
    return st;
}

std::string MultiCamManager::session_dir() const {
    std::lock_guard<std::mutex> lk(rec_mu_);
    return session_dir_;
}

uint64_t MultiCamManager::session_elapsed_us() const {
    std::lock_guard<std::mutex> lk(rec_mu_);
    if (!session_active_) return 0;
    const uint64_t now = now_us();
    const uint64_t t0 = session_t0_us_.load(std::memory_order_acquire);
    return now > t0 ? (now - t0) : 0;
}

std::string MultiCamManager::session_timecode() const {
    std::lock_guard<std::mutex> lk(rec_mu_);
    if (!session_active_) return "00:00:00:00";
    const uint64_t now = now_us();
    const uint64_t t0 = session_t0_us_.load(std::memory_order_acquire);
    const uint64_t elapsed = now > t0 ? (now - t0) : 0;
    return iso_timecode_string(session_tc_frame_ + elapsed * 30 / 1000000, 30);
}

MultiCamSlotInfo MultiCamManager::slot_info(uint32_t i) const {
    MultiCamSlotInfo info;
    const Slot* s = slot_ptr(i);
    if (!s) return info;
    const uint64_t now = now_us();
    info.index = int(i);
    info.listen_port = s->listen_port;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        info.bound = s->bound;
        info.sender_ip = s->bound ? ip_string(s->ip_be) : "";
        info.pinned_ip = s->pinned_ip;
    }
    {
        std::lock_guard<std::mutex> lk(s->info_mu);
        info.label = s->label;
        info.source_name = s->source_name;
        info.camera_id = s->camera_id;
        info.battery_pct = s->battery;
        info.temp_c = s->temp;
        info.tally = s->tally;
        info.assigned_cam_number = s->assigned_cam_number;
        info.assigned_cam_tag = s->assigned_cam_tag;
        info.config_synced = s->config_synced;
    }
    const auto ps = s->playout.stats();
    info.playout_delay_ms = ps.target_delay_ms;
    info.playout_effective_ms = ps.effective_delay_ms;
    info.playout_fixed_mode = ps.fixed_mode;
    info.playout_jitter_ms = ps.jitter_ms;
    info.playout_drops = ps.frames_dropped_late + ps.frames_dropped_corrupt;
    info.playout_holds = ps.frames_held;
    {
        std::lock_guard<std::mutex> lk(s->frame_mu);
        info.width = s->frame.width;
        info.height = s->frame.height;
        info.fps = s->fps;
        info.latency_ms = s->latency_ms;
    }
    info.streaming = age_us(now, s->last_frame_us.load()) < 1000000;
    if (!info.streaming) info.fps = 0.0;
    info.codec = codec_name(s->codec.load());
    info.frames = s->frames.load();
    info.audio_peak_dbfs = s->audio_peak.load();
    uint64_t rx_bytes = 0;
    bool has_rx = false;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        if (s->rx) {
            rx_bytes = s->rx->assembler_stats().bytes_total;
            has_rx = true;
        }
    }
    if (has_rx) {
        std::lock_guard<std::mutex> flk(s->frame_mu);
        if (now - s->br_t0 >= 1000000) {
            s->br_mbps = s->br_t0 ? double(rx_bytes - std::min(rx_bytes, s->br_b0)) * 8.0 / double(now - s->br_t0) : 0.0;
            s->br_t0 = now;
            s->br_b0 = rx_bytes;
        }
        info.bitrate_mbps = info.streaming ? s->br_mbps : 0.0;
    }
    {
        std::lock_guard<std::mutex> lk(s->rec_mu);
        info.recording = s->rec_on;
    }
    info.vcam_active = bus_.is_open() && i < kVideoBusCameraSlots && i < cfg_.vcam_count;
    return info;
}

std::vector<MultiCamSlotInfo> MultiCamManager::slots() const {
    std::vector<MultiCamSlotInfo> out;
    for (uint32_t i = 0; i < slots_.size(); ++i) out.push_back(slot_info(i));
    return out;
}

bool MultiCamManager::set_slot_label(uint32_t i, const std::string& label) {
    Slot* s = slot_ptr(i);
    if (!s) return false;
    std::lock_guard<std::mutex> lk(s->info_mu);
    s->label = label.empty() ? "CAM " + std::to_string(i + 1) : label.substr(0, 60);
    return true;
}

bool MultiCamManager::pin_slot(uint32_t i, const std::string& ip) {
    Slot* s = slot_ptr(i);
    if (!s) return false;
    Endpoint ep;
    if (!ip.empty() && !Endpoint::parse(ip, 1, ep)) return false;
    std::lock_guard<std::mutex> lk(slots_mu_);
    s->pinned_ip = ip;
    if (s->bound && !ip.empty() && ip_string(s->ip_be) != ip) s->bound = false;  // free it for the pinned phone
    return true;
}

void MultiCamManager::release_slot(uint32_t i) {
    Slot* s = slot_ptr(i);
    if (!s) return;
    rec_slot(i, false);
    std::unique_ptr<UdpReceiver> rx_to_destroy;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        s->bound = false;
        s->ip_be = 0;
        s->port = 0;
        if (cfg_.port_mode == MultiCamPortMode::kSingleDemux) {  // fresh receiver for the next phone
            s->rx_raw.store(nullptr, std::memory_order_release);
            rx_to_destroy = std::move(s->rx);
        }
    }
    // Destroy UdpReceiver outside slots_mu_ to prevent AB-BA deadlock with vdec_thread -> request_idr.
    rx_to_destroy.reset();
    {
        std::lock_guard<std::mutex> plk(s->pub_mu);
        s->playout.reset();
        s->playout_pub_seq = 0;
    }
    {
        std::lock_guard<std::mutex> fl(s->frame_mu);
        s->frame = Image{};
        s->last_frame_us.store(0);
    }
}

bool MultiCamManager::latest_frame(uint32_t i, Image& out, uint64_t& seq) const {
    const Slot* s = slot_ptr(i);
    if (!s) return false;
    publish_slot_playout(i, now_us());
    std::lock_guard<std::mutex> lk(s->frame_mu);
    if (s->frame_seq == seq || s->frame.width <= 0) return false;
    out = s->frame;
    seq = s->frame_seq;
    return true;
}

void MultiCamManager::update_camera_telemetry(const std::vector<StudioCameraInfo>& cams) {
    for (const auto& c : cams) {
        const std::string ip = c.address.substr(0, c.address.rfind(':'));
        for (auto& s : slots_) {
            bool match;
            {
                std::lock_guard<std::mutex> lk(slots_mu_);
                match = s->bound && ip_string(s->ip_be) == ip;
            }
            if (!match) continue;
            std::lock_guard<std::mutex> lk(s->info_mu);
            s->camera_id = c.camera_id;
            if (!c.label.empty()) s->source_name = c.label;
            s->battery = c.battery_pct;
            s->temp = c.temp_c;
            s->tally = c.tally_state;
        }
    }
}

void MultiCamManager::set_program_slot(int slot) {
    if (slot < 0 || slot >= int(slots_.size())) return;
    program_slot_.store(slot, std::memory_order_relaxed);
}

void MultiCamManager::set_vcam(uint32_t count, bool placeholder) {
    cfg_.vcam_count = std::min(count, kVideoBusCameraSlots);
    cfg_.vcam_placeholder = placeholder;
    if (bus_.is_open()) bus_.set_active_vcams(cfg_.vcam_count, placeholder);
}

bool MultiCamManager::video_bus_open() const { return bus_.is_open(); }

void MultiCamManager::set_slot_playout_config(uint32_t slot, uint32_t delay_ms, bool fixed_mode) {
    Slot* s = slot_ptr(slot);
    if (!s) return;
    s->playout.set_delay(delay_ms, fixed_mode);
}

VideoPlayoutStats MultiCamManager::slot_playout_stats(uint32_t slot) const {
    const Slot* s = slot_ptr(slot);
    return s ? s->playout.stats() : VideoPlayoutStats{};
}

void MultiCamManager::force_slot_idr(uint32_t slot) {
    request_idr(slot);
}

void MultiCamManager::set_idr_callback(std::function<void(uint32_t slot, uint32_t camera_id)> cb) {
    std::lock_guard<std::mutex> lk(idr_cb_mu_);
    idr_cb_ = std::move(cb);
}

bool MultiCamManager::set_slot_assigned_camera(uint32_t slot, uint8_t cam_number, const std::string& tag,
                                             const uint8_t rgba[4]) {
    Slot* s = slot_ptr(slot);
    if (!s) return false;
    std::lock_guard<std::mutex> lk(s->info_mu);
    s->assigned_cam_number = std::min<uint8_t>(cam_number, 16);
    s->assigned_cam_tag = tag.empty() ? (cam_number > 0 ? "CAM " + std::to_string(cam_number) : "") : tag.substr(0, 15);
    if (rgba) std::memcpy(s->badge_accent_rgba, rgba, 4);
    if (!s->assigned_cam_tag.empty()) s->label = s->assigned_cam_tag;
    return true;
}

void MultiCamManager::set_slot_config_synced(uint32_t slot, bool synced) {
    Slot* s = slot_ptr(slot);
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->info_mu);
    s->config_synced = synced;
}

}  // namespace lv
