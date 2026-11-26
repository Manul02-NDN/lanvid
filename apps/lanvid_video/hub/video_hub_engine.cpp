#include "hub/video_hub_engine.hpp"

#include <algorithm>
#include <cstdio>

#include "common/clock.hpp"
#include "net/udp_socket.hpp"

namespace lv::hub {

namespace {

std::string ip_be_to_str(uint32_t ip_be) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip_be);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

std::string strip_addr_port(const std::string& addr) {
    const size_t c = addr.rfind(':');
    return (c == std::string::npos) ? addr : addr.substr(0, c);
}

uint64_t ep_key(uint32_t ip_be, uint16_t port) {
    return (uint64_t(ip_be) << 16) | uint64_t(port);
}

}  // namespace

VideoHubEngine::VideoHubEngine() {
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        slots_[i] = std::make_unique<CameraSlotEngine>(i);
    }
}

VideoHubEngine::~VideoHubEngine() {
    stop();
}

bool VideoHubEngine::start(const VideoHubConfig& cfg) {
    stop();
    cfg_ = cfg;
    cfg_.max_slots = std::clamp<uint32_t>(cfg.max_slots, 1, kMaxHubSlots);
    sync_remote_rec_.store(cfg_.sync_remote_rec, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(route_mu_);
        endpoint_to_slot_.clear();
    }
    phone_rec_active_.fill(false);

    // Load persistent assignments into HubDiscovery
    std::vector<std::string> uids(cfg_.max_slots), ips(cfg_.max_slots);
    for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
        uids[i] = cfg_.slot_uids[i];
        ips[i] = cfg_.slot_ips[i];
        slots_[i]->set_label(cfg_.slot_labels[i]);
        slots_[i]->set_pinned_ip(cfg_.slot_ips[i]);
        slots_[i]->set_device_uid(cfg_.slot_uids[i]);
        auto rs = slots_[i]->remote_settings();
        rs.assigned_cam_number = cfg_.slot_cam_numbers[i] > 0 ? cfg_.slot_cam_numbers[i] : uint8_t(i + 1);
        rs.assigned_cam_tag = !cfg_.slot_cam_tags[i].empty()
                                  ? cfg_.slot_cam_tags[i]
                                  : ("CAM " + std::to_string(rs.assigned_cam_number));
        rs.video_delay_ms = (cfg_.slot_delays_ms[i] >= 5 && cfg_.slot_delays_ms[i] <= 250)
                                ? cfg_.slot_delays_ms[i]
                                : std::clamp<uint16_t>(cfg_.default_delay_ms, 5, 250);
        rs.fixed_delay_mode = cfg_.slot_delays_ms[i] > 0 ? cfg_.slot_fixed_modes[i] : cfg_.default_fixed_delay;
        slots_[i]->set_remote_settings(rs);
    }
    discovery_.load_assignments(uids, ips);

    // 1. Start video receiver layer (shared base-port demux or dedicated per-slot SDK ports)
    if (cfg_.port_mode == 0) {
        demux_sock_ = std::make_unique<lv::UdpSocket>();
        if (!demux_sock_->open() ||
            !demux_sock_->bind(cfg_.bind_ip.empty() ? "0.0.0.0" : cfg_.bind_ip, cfg_.video_port)) {
            demux_sock_.reset();
            return false;
        }
        demux_sock_->set_buffer_sizes(4 * 1024 * 1024, 16 * 1024 * 1024);
        demux_sock_->set_recv_timeout_ms(5);
        bound_video_port_ = demux_sock_->local_port();
        for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
            if (!slots_[i]->start_demux_receiver(demux_sock_.get(), bound_video_port_)) {
                stop();
                return false;
            }
        }
    } else {
        for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
            const uint16_t p = cfg_.video_port > 0 ? uint16_t(cfg_.video_port + i * 2u) : 0;
            if (!slots_[i]->start_dedicated_port(cfg_.bind_ip, p, cfg_.hub_name)) {
                stop();
                return false;
            }
            if (i == 0) bound_video_port_ = slots_[i]->listen_port();
        }
    }

    // Wire per-slot callbacks (zero cross-slot locking)
    for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
        slots_[i]->set_bitstream_callback([this](uint32_t slot, int codec, const uint8_t* data, size_t size,
                                                 bool is_key, uint64_t arr_us) {
            if (codec == int(lv::VideoCodecId::kH264) || codec == int(lv::VideoCodecId::kH265)) {
                const auto ic = codec == int(lv::VideoCodecId::kH265) ? lv::IsoVideoCodec::kH265
                                                                       : lv::IsoVideoCodec::kH264;
                iso_.feed_encoded_access_unit(slot, ic, data, size, is_key, arr_us);
            }
        });
        slots_[i]->set_audio_callback([this](uint32_t slot, const lv::AudioHeader& hdr, const uint8_t* pcm) {
            if (hdr.sample_format == lv::AudioSampleFormat::kPcm16 && hdr.channels == 2) {
                iso_.feed_audio_pcm16(slot, reinterpret_cast<const int16_t*>(pcm), hdr.num_frames, lv::now_us());
            }
        });
        slots_[i]->set_playout_callback([this](uint32_t slot, const lv::Image& frame, double fps, uint64_t now_us) {
            const bool is_pgm = (int(slot) == program_slot_.load(std::memory_order_relaxed));
            const int cid = int(slots_[slot]->remote_settings().codec);
            iso_.feed_bgra_frame(slot, frame, cid, is_pgm, now_us);
            if (vcam_.is_open()) {
                vcam_.publish_frame(slot, frame, is_pgm, now_us, fps);
            }
        });
        slots_[i]->set_idr_callback([this](uint32_t /*slot*/, uint32_t cam_id) {
            if (cam_id > 0) tally_.request_keyframe(cam_id);
        });
    }

    // 2. Start Shared Memory VCam bus (LanVidVideoBus_v1)
    if (cfg_.publish_vcam_bus) {
        vcam_.open(cfg_.vcam_bus_name, cfg_.vcam_count, cfg_.vcam_placeholder);
    }

    // 3. Start Tally & Duplex Intercom hub
    tally_.set_config_sync_callback([this](const lv::VideoConfigSyncPayload& p) { on_incoming_config_sync(p); });
    if (!tally_.start(cfg_.tally_port, cfg_.bind_ip) && cfg_.tally_port != 0) {
        // Fallback cleanly to ephemeral port if a secondary studio instance is running
        tally_.start(0, cfg_.bind_ip);
    }

    // 4. Start LAN + mDNS discovery
    discovery_.start(cfg_.hub_name, bound_video_port_, tally_.bound_port(), cfg_.discovery_port,
                     cfg_.primary_nic_ip, cfg_.secondary_nic_ip);

    running_.store(true, std::memory_order_release);
    if (demux_sock_) {
        stop_demux_.store(false, std::memory_order_release);
        demux_thread_ = std::thread(&VideoHubEngine::demux_loop, this);
    }
    return true;
}

void VideoHubEngine::stop() {
    running_.store(false, std::memory_order_release);
    stop_demux_.store(true, std::memory_order_release);
    if (demux_thread_.joinable()) {
        demux_thread_.join();
    }
    iso_.stop_session();
    discovery_.stop();
    tally_.stop();
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        slots_[i]->stop();
    }
    demux_sock_.reset();
    vcam_.close();
    bound_video_port_ = 0;
}

int VideoHubEngine::resolve_sender_slot(uint32_t ip_be, uint16_t port, bool allow_assign) {
    const uint64_t k = ep_key(ip_be, port);
    {
        std::lock_guard<std::mutex> lk(route_mu_);
        const auto it = endpoint_to_slot_.find(k);
        if (it != endpoint_to_slot_.end()) return int(it->second);
    }
    const std::string ip = ip_be_to_str(ip_be);
    int slot = discovery_.resolve_or_assign_slot("", ip, cfg_.max_slots, allow_assign && cfg_.auto_assign);
    if (slot < 0) {
        // Check if another active slot hasn't bound a sender yet
        for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
            const auto st = slots_[i]->snapshot();
            if (st.sender_ip.empty() && st.pinned_ip.empty()) {
                slot = int(i);
                break;
            }
        }
    }
    if (slot >= 0) {
        std::lock_guard<std::mutex> lk(route_mu_);
        endpoint_to_slot_[k] = uint32_t(slot);
        slots_[uint32_t(slot)]->bind_sender(ip, port);
    }
    return slot;
}

void VideoHubEngine::inject_datagram(const uint8_t* data, size_t len, uint32_t src_ip_be, uint16_t src_port) {
    if (!data || len < lv::kPacketHeaderSize || !running_.load(std::memory_order_acquire)) return;
    lv::Endpoint ep{};
    ep.ipv4_be = src_ip_be;
    ep.port = src_port;
    const int slot = resolve_sender_slot(src_ip_be, src_port, true);
    if (slot >= 0 && uint32_t(slot) < cfg_.max_slots) {
        slots_[uint32_t(slot)]->enqueue_datagram(data, len, ep, lv::now_us());
    }
}

void VideoHubEngine::demux_loop() {
    std::vector<uint8_t> buf(4096);
    while (!stop_demux_.load(std::memory_order_acquire) && demux_sock_) {
        lv::Endpoint src{};
        const int n = demux_sock_->recv_from(buf.data(), buf.size(), src);
        if (n <= 0) continue;
        inject_datagram(buf.data(), size_t(n), src.ipv4_be, src.port);
    }
}

void VideoHubEngine::poll() {
    if (!running_.load(std::memory_order_acquire)) return;
    tally_.poll();

    const uint64_t now = lv::now_us();
    const auto tcams = tally_.cameras();
    std::vector<DiscoveredPhonePeer> tally_peers;
    tally_peers.reserve(tcams.size());

    for (const auto& tc : tcams) {
        const std::string ip = strip_addr_port(tc.address);
        const std::string uid = tc.label.empty() ? ("cam:" + std::to_string(tc.camera_id)) : tc.label;
        int slot = discovery_.resolve_or_assign_slot(uid, ip, cfg_.max_slots, cfg_.auto_assign);
        if (slot < 0 && tc.camera_id >= 1 && tc.camera_id <= cfg_.max_slots) {
            slot = int(tc.camera_id - 1);
            discovery_.pin_slot(uint32_t(slot), uid, ip);
        }

        DiscoveredPhonePeer dp{};
        dp.device_uid = uid;
        dp.ip = ip;
        dp.stream_port = bound_video_port_;
        dp.name = tc.label.empty() ? ("CAM " + std::to_string(tc.camera_id)) : tc.label;
        dp.os = "mobile";
        dp.role = "camera";
        dp.assigned_slot = slot;
        dp.online = tc.connected;
        tally_peers.push_back(dp);

        if (slot >= 0 && uint32_t(slot) < cfg_.max_slots) {
            const uint32_t si = uint32_t(slot);
            const auto prev = slots_[si]->snapshot();
            const uint8_t wanted_tally = (int(si) == program_slot_.load(std::memory_order_relaxed))
                                             ? lv::kTallyProgram
                                             : ((int(si) == preview_slot_.load(std::memory_order_relaxed))
                                                    ? lv::kTallyPreview
                                                    : lv::kTallyStandby);
            const bool rec_tr = phone_rec_active_[si] || (sync_remote_rec_.load(std::memory_order_relaxed) &&
                                                          (iso_.is_slot_recording(si) || iso_.is_recording_any()));
            slots_[si]->merge_tally_telemetry(tc.camera_id, tc.label, ip, tc.connected, tc.battery_pct, tc.temp_c,
                                              wanted_tally, rec_tr, tc.operator_speaking);
            if (!prev.tally_connected && tc.connected) {
                // Push studio-assigned camera badge and playout delay to newly connected phone
                auto payload = slots_[si]->build_config_sync_payload(tally_.origin_id());
                payload.camera_id = tc.camera_id;
                tally_.send_video_config_sync(tc.camera_id, payload);
                slots_[si]->mark_config_synced(true);
            }
        }
    }
    discovery_.merge_tally_peers(tally_peers);

    if (now - last_tally_sync_us_ >= 100'000ULL) {
        last_tally_sync_us_ = now;
        push_tallies_to_phones();
    }

    if (vcam_.is_open() && (now - last_vcam_meta_us_ >= 100'000ULL)) {
        last_vcam_meta_us_ = now;
        vcam_.sync_slots_metadata(all_snapshots(), program_slot_.load(std::memory_order_relaxed));
    }
}

void VideoHubEngine::on_incoming_config_sync(const lv::VideoConfigSyncPayload& p) {
    if (p.origin_id == tally_.origin_id()) return;  // Echo Guard

    int target = -1;
    for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
        const auto s = slots_[i]->snapshot();
        if (p.camera_id != 0 && s.camera_id == p.camera_id) {
            target = int(i);
            break;
        }
    }
    if (target < 0 && p.assigned_cam_number >= 1 && p.assigned_cam_number <= cfg_.max_slots) {
        target = int(p.assigned_cam_number - 1);
    }
    if (target >= 0 && uint32_t(target) < cfg_.max_slots) {
        slots_[uint32_t(target)]->apply_incoming_config_sync(p, tally_.origin_id());
    }
}

void VideoHubEngine::push_tallies_to_phones() {
    const int pgm = program_slot_.load(std::memory_order_relaxed);
    const int pvw = preview_slot_.load(std::memory_order_relaxed);
    const bool sync_rec = sync_remote_rec_.load(std::memory_order_relaxed);
    const bool any_rec = iso_.is_recording_any();

    for (uint32_t i = 0; i < cfg_.max_slots; ++i) {
        const auto st = slots_[i]->snapshot();
        if (st.camera_id == 0) continue;
        const uint8_t tstate = (int(i) == pgm) ? lv::kTallyProgram
                                               : ((int(i) == pvw) ? lv::kTallyPreview : lv::kTallyStandby);
        const bool rec = phone_rec_active_[i] || (sync_rec && any_rec);
        const std::string& lbl = !st.settings.assigned_cam_tag.empty() ? st.settings.assigned_cam_tag : st.label;
        tally_.set_camera_tally(st.camera_id, tstate, lbl, rec);
    }
}

CameraSlotSnapshot VideoHubEngine::slot_snapshot(uint32_t slot) const {
    if (slot >= kMaxHubSlots) return CameraSlotSnapshot{};
    return slots_[slot]->snapshot();
}

std::array<CameraSlotSnapshot, kMaxHubSlots> VideoHubEngine::all_snapshots() const {
    std::array<CameraSlotSnapshot, kMaxHubSlots> arr{};
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        arr[i] = slots_[i]->snapshot();
    }
    return arr;
}

bool VideoHubEngine::poll_slot_frame(uint32_t slot, lv::Image& out, uint64_t& inout_seq) {
    if (slot >= cfg_.max_slots) return false;
    return slots_[slot]->poll_playout_frame(lv::now_us(), out, inout_seq);
}

CameraSlotEngine* VideoHubEngine::slot_engine(uint32_t slot) {
    return slot < kMaxHubSlots ? slots_[slot].get() : nullptr;
}

void VideoHubEngine::set_slot_label(uint32_t slot, const std::string& label) {
    if (slot >= kMaxHubSlots) return;
    slots_[slot]->set_label(label);
    push_tallies_to_phones();
}

void VideoHubEngine::pin_slot(uint32_t slot, const std::string& ip, const std::string& uid) {
    if (slot >= kMaxHubSlots) return;
    slots_[slot]->set_pinned_ip(ip);
    if (!uid.empty()) slots_[slot]->set_device_uid(uid);
    discovery_.pin_slot(slot, uid, ip);
}

void VideoHubEngine::release_slot(uint32_t slot) {
    if (slot >= kMaxHubSlots) return;
    {
        std::lock_guard<std::mutex> lk(route_mu_);
        for (auto it = endpoint_to_slot_.begin(); it != endpoint_to_slot_.end();) {
            if (it->second == slot) it = endpoint_to_slot_.erase(it);
            else ++it;
        }
    }
    slots_[slot]->release_sender();
    discovery_.unpin_slot(slot);
}

void VideoHubEngine::connect_peer_to_slot(uint32_t slot, const std::string& ip, uint16_t /*port*/,
                                          const std::string& uid, const std::string& name) {
    if (slot >= cfg_.max_slots) return;
    pin_slot(slot, ip, uid);
    if (!name.empty()) {
        slots_[slot]->set_label(name);
    }
    assign_slot_badge(slot, uint8_t(slot + 1), "", true);
}

void VideoHubEngine::set_program_slot(int slot) {
    program_slot_.store(slot, std::memory_order_relaxed);
    iso_.set_program_slot(slot);
    push_tallies_to_phones();
}

void VideoHubEngine::set_preview_slot(int slot) {
    preview_slot_.store(slot, std::memory_order_relaxed);
    push_tallies_to_phones();
}

void VideoHubEngine::cut_preview_to_program() {
    const int pvw = preview_slot_.load(std::memory_order_relaxed);
    const int old_pgm = program_slot_.load(std::memory_order_relaxed);
    program_slot_.store(pvw, std::memory_order_relaxed);
    preview_slot_.store(old_pgm, std::memory_order_relaxed);
    iso_.set_program_slot(pvw);
    push_tallies_to_phones();
}

bool VideoHubEngine::assign_slot_badge(uint32_t slot, uint8_t cam_number, const std::string& custom_tag,
                                       bool push_sync) {
    if (slot >= cfg_.max_slots) return false;
    auto rs = slots_[slot]->remote_settings();
    rs.assigned_cam_number = std::clamp<uint8_t>(cam_number, 1, 16);
    rs.assigned_cam_tag = custom_tag.empty() ? ("CAM " + std::to_string(rs.assigned_cam_number))
                                             : custom_tag.substr(0, 15);
    slots_[slot]->set_remote_settings(rs);
    if (!push_sync) return true;

    const auto st = slots_[slot]->snapshot();
    if (st.camera_id == 0) return false;
    auto payload = slots_[slot]->build_config_sync_payload(tally_.origin_id());
    payload.camera_id = st.camera_id;
    const bool ok = tally_.send_video_config_sync(st.camera_id, payload);
    if (ok) slots_[slot]->mark_config_synced(true);
    push_tallies_to_phones();
    return ok;
}

bool VideoHubEngine::apply_slot_remote_settings(uint32_t slot, const CameraRemoteSettings& s, bool push_sync) {
    if (slot >= cfg_.max_slots) return false;
    slots_[slot]->set_remote_settings(s);
    if (!push_sync) return true;

    const auto st = slots_[slot]->snapshot();
    if (st.camera_id == 0) return false;
    auto payload = slots_[slot]->build_config_sync_payload(tally_.origin_id());
    payload.camera_id = st.camera_id;
    const bool ok = tally_.send_video_config_sync(st.camera_id, payload);
    if (ok) slots_[slot]->mark_config_synced(true);
    push_tallies_to_phones();
    return ok;
}

void VideoHubEngine::set_slot_playout_delay(uint32_t slot, uint32_t delay_ms, bool fixed_mode, bool push_sync) {
    if (slot >= cfg_.max_slots) return;
    slots_[slot]->set_playout_delay(delay_ms, fixed_mode);
    if (!push_sync) return;
    const auto st = slots_[slot]->snapshot();
    if (st.camera_id != 0) {
        auto payload = slots_[slot]->build_config_sync_payload(tally_.origin_id());
        payload.camera_id = st.camera_id;
        if (tally_.send_video_config_sync(st.camera_id, payload)) {
            slots_[slot]->mark_config_synced(true);
        }
    }
}

void VideoHubEngine::force_slot_idr(uint32_t slot) {
    if (slot < cfg_.max_slots) slots_[slot]->force_idr();
}

bool VideoHubEngine::rec_start_all(const IsoMultiSessionConfig& sess, bool sync_remote_rec) {
    sync_remote_rec_.store(sync_remote_rec, std::memory_order_relaxed);
    std::array<std::string, kMaxHubSlots> labels{};
    for (uint32_t i = 0; i < kMaxHubSlots; ++i) {
        const auto s = slots_[i]->snapshot();
        labels[i] = s.label;
        if (sync_remote_rec && (sess.slot_mask & (1u << i))) {
            phone_rec_active_[i] = true;
        }
    }
    const bool ok = iso_.start_session(sess, labels);
    if (ok) push_tallies_to_phones();
    return ok;
}

void VideoHubEngine::rec_stop_all(bool sync_remote_rec) {
    iso_.stop_session();
    if (sync_remote_rec || sync_remote_rec_.load(std::memory_order_relaxed)) {
        phone_rec_active_.fill(false);
    }
    push_tallies_to_phones();
}

bool VideoHubEngine::rec_slot(uint32_t slot, bool on, bool sync_remote_rec) {
    if (slot >= cfg_.max_slots) return false;
    const auto st = slots_[slot]->snapshot();
    const bool ok = iso_.set_slot_recording(slot, on, st.label);
    if (sync_remote_rec) {
        phone_rec_active_[slot] = on;
        push_tallies_to_phones();
    }
    return ok;
}

void VideoHubEngine::set_sync_remote_rec(bool enabled) {
    sync_remote_rec_.store(enabled, std::memory_order_relaxed);
    push_tallies_to_phones();
}

void VideoHubEngine::set_vcam_config(uint32_t count, bool placeholder, int lip_sync_ms) {
    vcam_.configure(count, placeholder);
    vcam_.set_lip_sync_ms(lip_sync_ms);
}

void VideoHubEngine::set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip) {
    discovery_.set_network_interfaces(primary_ip, secondary_ip);
}

}  // namespace lv::hub
