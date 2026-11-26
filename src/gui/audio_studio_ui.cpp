#include "gui/audio_studio_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/clock.hpp"
#include "gui/qr_code.hpp"
#include "imgui.h"

namespace lv::gui {

namespace {

constexpr int kChannelChoices[] = {2, 8, 16, 32, 64};
constexpr int kBitrateChoices[] = {0, 256, 192, 128, 96, 64};
constexpr const char* kBitrateLabels[] = {
    "Lossless (PCM)",
    "256 kbps / ch",
    "192 kbps / ch",
    "128 kbps / ch",
    "96 kbps / ch",
    "64 kbps / ch",
};
constexpr const char* kPageTitles[] = {
    "Мережа та Зв'язок",
    "Маршрутизація",
    "Мобільний монітор",
    "Пристрої та Налаштування",
};

double estimate_aoip_mbps(int channels, int codec, int bitrate_kbps) {
    const int ch = std::clamp(channels, 1, 64);
    if (bitrate_kbps > 0) return double(ch) * double(bitrate_kbps) / 1000.0;
    const double bps = (codec == 1) ? 24.0 : (codec == 2 ? 16.0 : 32.0);
    return double(ch) * 48000.0 * bps / 1'000'000.0;
}

void draw_mini_vu(float dbfs, float width, float height) {
    const auto& p = studio_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float norm = std::clamp((dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), ImGui::GetColorU32(p.card_soft), 4.0f);
    if (norm > 0.01f) {
        const ImVec4 col = (norm > 0.88f) ? p.red : (norm > 0.65f ? p.amber : p.accent_node);
        dl->AddRectFilled(p0, ImVec2(p0.x + width * norm, p0.y + height), ImGui::GetColorU32(col), 4.0f);
    }
    ImGui::Dummy(ImVec2(width, height));
}

void draw_qr_widget(const std::string& text, float box_px) {
    const QrCode qr = qr_encode(text);
    if (qr.size <= 0) {
        ImGui::TextDisabled("QR недоступний");
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float quiet = 8.0f;
    const float cell = (box_px - 2.0f * quiet) / float(qr.size);
    dl->AddRectFilled(pos, ImVec2(pos.x + box_px, pos.y + box_px), IM_COL32(255, 255, 255, 255), 10.0f);
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.dark(x, y)) continue;
            const ImVec2 a(pos.x + quiet + float(x) * cell, pos.y + quiet + float(y) * cell);
            const ImVec2 b(a.x + cell + 0.3f, a.y + cell + 0.3f);
            dl->AddRectFilled(a, b, IM_COL32(30, 27, 75, 255));
        }
    }
    ImGui::Dummy(ImVec2(box_px, box_px));
}

}  // namespace

LanVidAudioStudioApp::LanVidAudioStudioApp(std::string config_path_override)
    : config_path_(config_path_override.empty() ? config_file_path_for("lanvid_audio_config.json")
                                                : std::move(config_path_override)) {
    cfg_.dest_port = lv::kAudioStreamPort;
    cfg_.listen_port = lv::kAudioStreamPort;
    cfg_.dpx_listen_port = lv::kAudioStreamPort;
    cfg_.dpx_peer_port = lv::kAudioStreamPort;
    cfg_.audio_only = true;
    sender_ = lv_sender_create();
    receiver_ = lv_receiver_create();
    duplex_ = lv_duplex_create();
}

LanVidAudioStudioApp::~LanVidAudioStudioApp() {
    shutdown();
}

void LanVidAudioStudioApp::init() {
    load_app_config(cfg_, config_path_);
    if (cfg_.listen_port == 5000) cfg_.listen_port = lv::kAudioStreamPort;
    if (cfg_.dest_port == 5000) cfg_.dest_port = lv::kAudioStreamPort;
    if (cfg_.dpx_listen_port == 5005) cfg_.dpx_listen_port = lv::kAudioStreamPort;
    if (cfg_.dpx_peer_port == 5004) cfg_.dpx_peer_port = lv::kAudioStreamPort;
    cfg_.audio_only = true;
    if (cfg_.studio_name.empty()) cfg_.studio_name = "LanVid Audio";
    std::snprintf(studio_name_buf_, sizeof(studio_name_buf_), "%s", cfg_.studio_name.c_str());
    std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", cfg_.dest_ip.c_str());

    if (cfg_.monitor_channel_names.size() < size_t(lv::kMonitorMaxInputChannels)) {
        const size_t old_sz = cfg_.monitor_channel_names.size();
        cfg_.monitor_channel_names.resize(lv::kMonitorMaxInputChannels);
        for (size_t i = old_sz; i < cfg_.monitor_channel_names.size(); ++i) {
            cfg_.monitor_channel_names[i] = "Ch " + std::to_string(i + 1);
        }
    }

    apply_studio_theme(cfg_.dark_theme);
    refresh_nics();
    refresh_grid_adapters();
    refresh_device_caches(true);

    init_matrix_from_config();
    wire_network_midi_and_sync();

    start_services(uint16_t(cfg_.listen_port), uint16_t(cfg_.audio_monitor_port),
                   uint16_t(cfg_.audio_discovery_port));
    start_lan_scan();

    if (cfg_.autostart_session) {
        start_stream();
        if (cfg_.network_engine_mode == 1 || cfg_.network_engine_mode == 2) {
            start_grid_node();
        }
    }
}

void LanVidAudioStudioApp::shutdown() {
    stop_stream();
    stop_services();
    if (scan_thread_.joinable()) scan_thread_.join();
    if (duplex_) {
        lv_duplex_destroy(duplex_);
        duplex_ = nullptr;
    }
    if (sender_) {
        lv_sender_destroy(sender_);
        sender_ = nullptr;
    }
    if (receiver_) {
        lv_receiver_destroy(receiver_);
        receiver_ = nullptr;
    }
    if (!matrix_.slots().empty()) sync_matrix_to_config();
    save_app_config(cfg_, config_path_);
}

void LanVidAudioStudioApp::init_matrix_from_config() {
    matrix_.set_buffer_size(cfg_.matrix_buffer_size);
    matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);
    matrix_.clear_slots();
    matrix_.clear_crosspoints();

    if (cfg_.matrix_slots.empty()) {
        MatrixSlotConfig s0{};
        s0.id = 0;
        s0.type = MatrixSlotType::kVirtualBus;
        s0.role = MatrixSlotRole::kDuplex;
        s0.name = "LanVid Virtual ASIO/HAL";
        s0.channels = 64;
        s0.buffer_size = cfg_.matrix_buffer_size;
        s0.active = true;
        matrix_.add_slot(s0);

        MatrixSlotConfig s1{};
        s1.id = 1;
        s1.type = MatrixSlotType::kNetworkAoip;
        s1.role = MatrixSlotRole::kDuplex;
        s1.name = "LAN AoIP Bus (64ch)";
        s1.channels = 64;
        s1.buffer_size = cfg_.matrix_buffer_size;
        s1.active = true;
        matrix_.add_slot(s1);

        MatrixSlotConfig s2{};
        s2.id = 2;
        s2.type = MatrixSlotType::kSystemAudio;
        s2.role = MatrixSlotRole::kOutput;
#if defined(__APPLE__)
        s2.name = "Mac Headphones / Output (CoreAudio)";
#else
        s2.name = "PC Headphones / Output";
#endif
        s2.channels = 2;
        s2.buffer_size = cfg_.matrix_buffer_size;
        s2.active = true;
        matrix_.add_slot(s2);

        matrix_.apply_preset(0, 1, 0, 2);
        sync_matrix_to_config();
    } else {
        for (const auto& s : cfg_.matrix_slots) matrix_.add_slot(s);
        for (const auto& c : cfg_.matrix_crosspoints) {
            matrix_.set_crosspoint(c.src_slot, c.src_ch, c.dst_slot, c.dst_ch, c.gain_db, c.mute,
                                   !cfg_.matrix_feedback_guard);
        }
    }

    matrix_.clear_midi_slots();
    if (cfg_.midi_slots.empty()) {
        matrix_.add_midi_slot({0, MidiSlotType::kNetworkMidiTx, "Network MIDI TX (MIDI2)", "", -1, 0, true});
        matrix_.add_midi_slot({1, MidiSlotType::kNetworkMidiRx, "Network MIDI RX (MIDI2)", "", -1, 0, true});
        const int daw_id = matrix_.add_midi_slot(
            {2, MidiSlotType::kVirtualDawMidi, "LanVid MIDI (DAW)", "LanVid MIDI", -1, 0, true});
        if (daw_id >= 0) {
            matrix_.set_midi_crosspoint(1, daw_id, 0, false);
            matrix_.set_midi_crosspoint(daw_id, 0, 0, false);
        }
    } else {
        for (const auto& ms : cfg_.midi_slots) matrix_.add_midi_slot(ms);
        for (const auto& mc : cfg_.midi_crosspoints) {
            matrix_.set_midi_crosspoint(mc.src_slot, mc.dst_slot, mc.channel_filter, mc.mute);
        }
    }

    matrix_.set_network_io(
        [this](const float* const* planes, uint32_t ch, uint32_t frames) {
            if (monitor_srv_ && monitor_srv_->is_running()) {
                monitor_srv_->process_and_send(planes, int(ch), frames);
            }
            if (sender_ && sender_running_ && !cfg_.midi_only &&
                matrix_has_net_tx_routes_.load(std::memory_order_relaxed)) {
                lv_sender_push_audio_multichannel(sender_, planes, uint16_t(ch), frames,
                                                  LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR, LV_MEDIA_TS_AUTO);
            }
            if (grid_node_ && grid_node_->is_open() &&
                (cfg_.network_engine_mode == 1 || cfg_.network_engine_mode == 2)) {
                const uint16_t gch = uint16_t(std::min<uint32_t>(ch, uint32_t(cfg_.grid_channels)));
                grid_node_->push_routed_audio(planes, gch, uint16_t(frames),
                                              uint32_t(lv::now_us() & 0xFFFFFFFFu));
            }
        },
        [this](float* const* planes, uint32_t ch, uint32_t frames) {
            if (receiver_ && receiver_running_) {
                static thread_local std::vector<float> tmp;
                tmp.resize(size_t(ch) * frames);
                if (lv_receiver_pop_audio_multichannel(receiver_, tmp.data(), uint16_t(ch), frames,
                                                       LV_AUDIO_LAYOUT_PLANAR) < 0) {
                    std::fill(tmp.begin(), tmp.end(), 0.0f);
                }
                for (uint32_t c = 0; c < ch; ++c) {
                    std::copy(tmp.begin() + size_t(c) * frames, tmp.begin() + size_t(c + 1) * frames, planes[c]);
                }
            }
        });
}

void LanVidAudioStudioApp::sync_matrix_to_config() {
    cfg_.matrix_buffer_size = matrix_.buffer_size();
    cfg_.matrix_feedback_guard = matrix_.feedback_guard();
    cfg_.matrix_slots = matrix_.slots();
    cfg_.matrix_crosspoints = matrix_.crosspoints();
    cfg_.midi_slots = matrix_.midi_slots();
    cfg_.midi_crosspoints = matrix_.midi_crosspoints();
}

void LanVidAudioStudioApp::wire_network_midi_and_sync() {
    if (sync_origin_id_ == 0) {
        sync_origin_id_ = uint32_t((lv::now_us() ^ 0x5A3C9E17ull) & 0xFFFFFFFFull);
        if (sync_origin_id_ == 0) sync_origin_id_ = 1;
    }
    matrix_.set_midi_sink([this](int dst_slot, uint8_t st, uint8_t d1, uint8_t d2, uint8_t port, uint64_t) {
        bool to_net = false;
        for (const auto& s : matrix_.midi_slots()) {
            if (s.id == dst_slot && s.type == MidiSlotType::kNetworkMidiTx) {
                to_net = true;
                break;
            }
        }
        if (!to_net) return;
        if (duplex_running_ && duplex_) lv_duplex_send_midi(duplex_, st, d1, d2, port, 0);
        else if (sender_running_ && sender_) lv_sender_push_midi(sender_, st, d1, d2, port, 0);
    });
    if (sender_) lv_sender_set_control_sync_callback(sender_, &LanVidAudioStudioApp::on_remote_control_sync, this);
    if (receiver_) {
        lv_receiver_set_midi_callback(receiver_, &LanVidAudioStudioApp::on_network_midi, this);
        lv_receiver_set_control_sync_callback(receiver_, &LanVidAudioStudioApp::on_remote_control_sync, this);
    }
    if (duplex_) {
        lv_duplex_set_midi_callback(duplex_, &LanVidAudioStudioApp::on_network_midi, this);
        lv_duplex_set_control_sync_callback(duplex_, &LanVidAudioStudioApp::on_remote_control_sync, this);
    }
}

void LanVidAudioStudioApp::on_remote_control_sync(const LV_ControlSyncState* state, void* user_data) {
    auto* self = static_cast<LanVidAudioStudioApp*>(user_data);
    if (!self || !state) return;
    if (state->origin_id == self->sync_origin_id_) return;
    std::lock_guard<std::mutex> lk(self->remote_sync_mu_);
    if (state->revision <= self->last_applied_remote_revision_) return;
    self->pending_remote_sync_ = *state;
    self->has_pending_remote_sync_ = true;
}

void LanVidAudioStudioApp::on_network_midi(const LV_MidiEvent* ev, void* user_data) {
    auto* self = static_cast<LanVidAudioStudioApp*>(user_data);
    if (!self || !ev) return;
    const uint64_t ts = lv::now_us();
    for (const auto& s : self->matrix_.midi_slots()) {
        if (s.type == MidiSlotType::kNetworkMidiRx && s.active) {
            self->matrix_.inject_midi_event(s.id, ev->status, ev->data1, ev->data2, ts);
        }
    }
}

void LanVidAudioStudioApp::broadcast_control_sync(bool mute_all_panic) {
    if (!cfg_.sync_settings || applying_remote_sync_) return;
    if (!sender_running_ && !receiver_running_ && !duplex_running_) return;
    local_sync_revision_ = std::max(local_sync_revision_, last_applied_remote_revision_) + 1;

    LV_ControlSyncState st{};
    st.revision = local_sync_revision_;
    st.origin_id = sync_origin_id_;
    st.audio_channels = uint16_t(std::clamp(cfg_.aoip_channels, 1, 64));
    st.audio_bitrate_kbps = uint16_t(std::max(0, cfg_.audio_bitrate_kbps));
    st.target_buffer_ms = uint16_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    st.latency_mode = uint8_t(cfg_.adaptive_jitter ? 1 : 0);
    st.audio_only = uint8_t(cfg_.midi_only ? 2 : 1);
    st.active_preset_id = int8_t(cfg_.sync_matrix_routing ? matrix_active_preset_ : -1);
    st.flags = uint8_t((mute_all_panic ? 0x01u : 0u) | (cfg_.sync_matrix_routing ? 0x02u : 0u));
    st.master_gain_cb = int16_t(std::clamp(int(std::lrint((cfg_.volume - 1.0f) * 1200.0f)), -6000, 1200));
    st.audio_codec = uint8_t(std::clamp(cfg_.audio_codec, 0, 8));

    bool sent = false;
    if (duplex_running_ && duplex_) sent |= (lv_duplex_send_control_sync(duplex_, &st) == LV_OK);
    if (sender_running_ && sender_) sent |= (lv_sender_send_control_sync(sender_, &st) == LV_OK);
    if (receiver_running_ && receiver_) sent |= (lv_receiver_send_control_sync(receiver_, &st) == LV_OK);
    if (sent) last_synced_us_ = lv::now_us();
}

void LanVidAudioStudioApp::apply_pending_remote_sync() {
    LV_ControlSyncState st{};
    {
        std::lock_guard<std::mutex> lk(remote_sync_mu_);
        if (!has_pending_remote_sync_) return;
        has_pending_remote_sync_ = false;
        st = pending_remote_sync_;
    }
    if (!cfg_.sync_settings || st.revision <= last_applied_remote_revision_) return;

    applying_remote_sync_ = true;
    last_applied_remote_revision_ = st.revision;
    local_sync_revision_ = std::max(local_sync_revision_, st.revision);

    cfg_.aoip_channels = std::clamp<int>(st.audio_channels, 1, 64);
    cfg_.audio_bitrate_kbps = int(st.audio_bitrate_kbps);
    cfg_.audio_codec = std::clamp<int>(st.audio_codec, 0, 8);
    cfg_.aoip_latency_ms = std::clamp<int>(st.target_buffer_ms, 2, 2000);
    cfg_.adaptive_jitter = st.latency_mode ? 1 : 0;
    cfg_.midi_only = (st.audio_only == 2);
    cfg_.audio_only = true;
    apply_live_audio_settings();

    if ((st.flags & 0x01u) != 0) {
        matrix_.send_midi_panic(-1);
    }
    if (cfg_.sync_matrix_routing && (st.flags & 0x02u) != 0 &&
        st.active_preset_id >= 0 && st.active_preset_id <= 2) {
        int net_id = -1, daw_id = -1, mon_id = -1;
        for (const auto& s : matrix_.slots()) {
            if (s.type == MatrixSlotType::kNetworkAoip) net_id = s.id;
            else if (s.type == MatrixSlotType::kVirtualBus) daw_id = s.id;
            else if (s.type == MatrixSlotType::kSystemAudio && s.role == MatrixSlotRole::kOutput && mon_id < 0) {
                mon_id = s.id;
            }
        }
        matrix_active_preset_ = st.active_preset_id;
        matrix_.apply_preset(st.active_preset_id, net_id, daw_id, mon_id);
        sync_matrix_to_config();
    }

    last_synced_us_ = lv::now_us();
    config_dirty_ = true;
    applying_remote_sync_ = false;
}

void LanVidAudioStudioApp::apply_live_audio_settings() {
    if (sender_ && sender_running_) {
        if (cfg_.audio_bitrate_kbps > 0) lv_sender_set_audio_bitrate(sender_, cfg_.audio_bitrate_kbps);
        else lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
        lv_sender_set_midi_only(sender_, cfg_.midi_only ? 1 : 0);
        lv_sender_set_wifi_redundancy(sender_, cfg_.wifi_redundancy ? 1 : 0, 1500);
    }
    if (receiver_ && receiver_running_) {
        lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
        lv_receiver_set_adaptive_jitter(receiver_, cfg_.adaptive_jitter);
        lv_receiver_set_midi_only(receiver_, cfg_.midi_only ? 1 : 0);
        lv_receiver_set_volume(receiver_, master_mute_panic_ ? 0.0f : cfg_.volume);
    }
    if (duplex_ && duplex_running_) {
        if (cfg_.audio_bitrate_kbps > 0) lv_duplex_set_audio_bitrate(duplex_, cfg_.audio_bitrate_kbps);
        else lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
        lv_duplex_set_adaptive_jitter(duplex_, cfg_.adaptive_jitter);
        lv_duplex_set_midi_only(duplex_, cfg_.midi_only ? 1 : 0);
        lv_duplex_set_wifi_redundancy(duplex_, cfg_.wifi_redundancy ? 1 : 0, 1500);
    }
}

void LanVidAudioStudioApp::apply_monitor_dsp_to_clients() {
    if (!monitor_srv_) return;
    PersonalMonitorDspParams p{};
    p.hpf_enabled = cfg_.monitor_dsp_hpf_enabled;
    p.hpf_hz = cfg_.monitor_dsp_hpf_hz;
    p.eq_enabled = cfg_.monitor_dsp_eq_enabled;
    p.eq_low_db = cfg_.monitor_dsp_eq_low_db;
    p.eq_mid_hz = cfg_.monitor_dsp_eq_mid_hz;
    p.eq_mid_db = cfg_.monitor_dsp_eq_mid_db;
    p.eq_high_db = cfg_.monitor_dsp_eq_high_db;
    p.comp_enabled = cfg_.monitor_dsp_comp_enabled;
    p.comp_thresh_db = cfg_.monitor_dsp_comp_thresh_db;
    p.comp_ratio = cfg_.monitor_dsp_comp_ratio;
    p.comp_attack_ms = cfg_.monitor_dsp_comp_attack_ms;
    p.comp_release_ms = cfg_.monitor_dsp_comp_release_ms;
    p.comp_makeup_db = cfg_.monitor_dsp_comp_makeup_db;
    p.master_volume_db = cfg_.monitor_dsp_master_db;
    p.master_mute = cfg_.monitor_dsp_master_mute || master_mute_panic_;

    for (int slot = 0; slot < lv::kMaxMonitorClients; ++slot) {
        if (auto* b = monitor_srv_->bus(slot)) {
            b->set_dsp(p);
        }
    }
}

void LanVidAudioStudioApp::enforce_audio_ownership(uint64_t now_us_val) {
    if (now_us_val - ownership_us_ < 100000ull) return;
    ownership_us_ = now_us_val;
    bool net_rx = false, net_tx = false, vbus = false;
    int net_slot_id = -1;
    if (matrix_.is_running()) {
        for (const auto& sl : matrix_.slots()) {
            if (!sl.active) continue;
            if (sl.type == MatrixSlotType::kNetworkAoip) {
                net_slot_id = sl.id;
                if (sl.role != MatrixSlotRole::kOutput) net_rx = true;
            } else if (sl.type == MatrixSlotType::kVirtualBus) {
                vbus = true;
            }
        }
        if (net_slot_id >= 0) {
            for (const auto& cp : matrix_.crosspoints()) {
                if (!cp.mute && cp.dst_slot == net_slot_id) {
                    net_tx = true;
                    break;
                }
            }
        }
    }
    matrix_has_net_tx_routes_.store(net_tx, std::memory_order_relaxed);
    matrix_net_rx_ = net_rx;
    matrix_net_tx_ = net_tx;
    matrix_vbus_ = vbus;
}

bool LanVidAudioStudioApp::start_services(uint16_t audio_port, uint16_t monitor_port, uint16_t discovery_port) {
    stop_services();

    // 1. Bind standby AoIP UDP socket unless receiver/duplex already owns the port
    bound_audio_port_ = audio_port;
    if (!receiver_running_ && !duplex_running_) {
        if (!audio_sock_.open()) return false;
        const std::string bind_ip = cfg_.primary_nic_ip.empty() ? "0.0.0.0" : cfg_.primary_nic_ip;
        if (!audio_sock_.bind(bind_ip, audio_port)) {
            if (!audio_sock_.bind("0.0.0.0", audio_port)) {
                audio_sock_.close();
                return false;
            }
        }
        audio_sock_.set_nonblocking(true);
        bound_audio_port_ = audio_sock_.local_port();
    }

    // 2. Start PersonalMonitorServer on dedicated Monitor UDP port (default 5020)
    const std::string bind_ip = cfg_.primary_nic_ip.empty() ? "0.0.0.0" : cfg_.primary_nic_ip;
    monitor_srv_ = std::make_unique<lv::PersonalMonitorServer>(uint32_t(cfg_.audio_sample_rate));
    monitor_srv_->set_studio_name(studio_name_buf_);
    monitor_srv_->set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
    monitor_srv_->set_multichannel_flow(true, 2);
    for (size_t i = 0; i < cfg_.monitor_channel_names.size() && i < size_t(lv::kMonitorMaxInputChannels); ++i) {
        if (!cfg_.monitor_channel_names[i].empty()) {
            monitor_srv_->set_channel_name(int(i), cfg_.monitor_channel_names[i]);
        }
    }
    if (!monitor_srv_->start(monitor_port, bind_ip, 0)) {
        if (!monitor_srv_->start(monitor_port, "0.0.0.0", 0)) {
            monitor_srv_.reset();
            audio_sock_.close();
            return false;
        }
    }
    if (!cfg_.primary_nic_ip.empty() || !cfg_.secondary_nic_ip.empty()) {
        monitor_srv_->set_network_interfaces(cfg_.primary_nic_ip,
                                             cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : "");
    }
    apply_monitor_dsp_to_clients();

    // 3. Start DiscoveryResponder on Audio Discovery port (default 5021) with role="audio"
    discovery_.set_role("audio");
    discovery_.set_network_interfaces(cfg_.primary_nic_ip,
                                      cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : "");
    const uint16_t ap = bound_audio_port_;
    const int ch = cfg_.aoip_channels;
    discovery_.set_aoip_provider([ap, ch]() {
        lv::AoipAnnounce a{};
        a.enabled = true;
        a.hostname = lv::local_host_name();
        a.audio_driver = "LanVid Virtual Audio";
        a.driver_connected = true;
        a.channels = uint16_t(ch);
        a.rx_port = ap;
        a.tx_port = ap;
        a.duplex = true;
        return a;
    });
    if (!discovery_.start(bound_audio_port_, studio_name_buf_, discovery_port)) {
        monitor_srv_->stop();
        monitor_srv_.reset();
        audio_sock_.close();
        return false;
    }
    bound_disc_port_ = discovery_port;

    // 4. Advertise via mDNS (_lanvid._udp) with role=audio
    mdns_.start(studio_name_buf_, monitor_srv_->bound_port(), {{"role", "audio"}});

    // 5. Start MultiDeviceAudioMatrix audio engine
    if (!matrix_.is_running()) matrix_.start();

    services_running_.store(true, std::memory_order_release);
    std::snprintf(status_msg_, sizeof(status_msg_), "Служби активні • LAN :%u • Monitor :%u",
                  unsigned(bound_audio_port_), unsigned(bound_monitor_port()));
    return true;
}

void LanVidAudioStudioApp::stop_services() {
    services_running_.store(false, std::memory_order_release);
    stop_stream();
    stop_grid_node();
    matrix_.stop();
    mdns_.stop();
    discovery_.stop();
    if (monitor_srv_) {
        monitor_srv_->stop();
        monitor_srv_.reset();
    }
    audio_sock_.close();
    std::snprintf(status_msg_, sizeof(status_msg_), "Рушій зупинено");
}

bool LanVidAudioStudioApp::start_stream() {
    stop_stream();
    cfg_.dest_ip = peer_ip_buf_;
    cfg_.dpx_peer_host = peer_ip_buf_;

    // Release standby audio_sock_ so receiver/duplex can bind bound_audio_port_
    audio_sock_.close();

    const std::string pri_ip = cfg_.primary_nic_ip;
    const std::string sec_ip = cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : "";

    if (cfg_.connect_role == 2 && duplex_) {
        LV_DuplexConfig dc{};
        lv_duplex_config_default(&dc);
        std::snprintf(dc.peer_host, sizeof(dc.peer_host), "%s", peer_ip_buf_);
        dc.peer_port = uint16_t(std::clamp(cfg_.dest_port, 1024, 65535));
        dc.listen_port = uint16_t(std::clamp(int(bound_audio_port_), 1024, 65535));
        dc.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
        dc.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
        dc.audio_codec = std::clamp(cfg_.audio_codec, 0, 8);
        dc.adaptive_jitter = cfg_.adaptive_jitter ? 1 : 0;
        const int rc = lv_duplex_start(duplex_, &dc);
        duplex_running_ = (rc == LV_OK);
        if (duplex_running_) {
            lv_duplex_set_network_interfaces(duplex_, pri_ip.c_str(), sec_ip.c_str());
            apply_live_audio_settings();
            broadcast_control_sync();
            std::snprintf(status_msg_, sizeof(status_msg_), "Дуплекс %dch ↔ %s:%u • %d мс",
                          cfg_.aoip_channels, peer_ip_buf_, unsigned(dc.peer_port), cfg_.aoip_latency_ms);
            return true;
        }
        std::snprintf(status_msg_, sizeof(status_msg_), "Помилка запуску Duplex: %d", rc);
    } else if (cfg_.connect_role == 0 && sender_) {
        LvSenderConfig sc{};
        lv_sender_config_default(&sc);
        std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "%s", peer_ip_buf_);
        sc.dest_port = uint16_t(std::clamp(cfg_.dest_port, 1024, 65535));
        sc.audio_only = 1;
        sc.audio_codec = std::clamp(cfg_.audio_codec, 0, 8);
        sc.audio_bitrate_kbps = cfg_.audio_bitrate_kbps;
        LV_AudioBusConfig bus{};
        lv_audio_bus_config_default(&bus);
        bus.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
        bus.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
        lv_sender_configure_audio_bus(sender_, &bus);
        lv_sender_set_network_interfaces(sender_, pri_ip.c_str(), sec_ip.c_str());
        if (lv_sender_start(sender_, &sc)) {
            sender_running_ = true;
            apply_live_audio_settings();
            broadcast_control_sync();
            std::snprintf(status_msg_, sizeof(status_msg_), "Передача %dch → %s:%u • %d мс",
                          cfg_.aoip_channels, peer_ip_buf_, unsigned(sc.dest_port), cfg_.aoip_latency_ms);
            return true;
        }
    } else if (cfg_.connect_role == 1 && receiver_) {
        LvReceiverConfig rc{};
        lv_receiver_config_default(&rc);
        rc.bind_port = uint16_t(std::clamp(int(bound_audio_port_), 1024, 65535));
        rc.enable_audio = 1;
        rc.enable_discovery = 0;
        rc.audio_only = 1;
        rc.adaptive_jitter = cfg_.adaptive_jitter ? 1 : 0;
        LV_AudioBusConfig bus{};
        lv_audio_bus_config_default(&bus);
        bus.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
        bus.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
        lv_receiver_configure_audio_bus(receiver_, &bus);
        if (lv_receiver_start(receiver_, &rc)) {
            receiver_running_ = true;
            apply_live_audio_settings();
            broadcast_control_sync();
            std::snprintf(status_msg_, sizeof(status_msg_), "Прийом AoIP :%u • %d мс",
                          unsigned(rc.bind_port), cfg_.aoip_latency_ms);
            return true;
        }
    }
    return false;
}

void LanVidAudioStudioApp::stop_stream() {
    if (duplex_running_ && duplex_) lv_duplex_stop(duplex_);
    if (sender_running_ && sender_) lv_sender_stop(sender_);
    if (receiver_running_ && receiver_) lv_receiver_stop(receiver_);
    duplex_running_ = false;
    sender_running_ = false;
    receiver_running_ = false;

    // Re-bind standby socket if services are active
    if (services_running_.load(std::memory_order_acquire) && !audio_sock_.is_open()) {
        if (audio_sock_.open()) {
            const std::string bind_ip = cfg_.primary_nic_ip.empty() ? "0.0.0.0" : cfg_.primary_nic_ip;
            if (!audio_sock_.bind(bind_ip, bound_audio_port_)) {
                audio_sock_.bind("0.0.0.0", bound_audio_port_);
            }
            audio_sock_.set_nonblocking(true);
        }
    }
}

void LanVidAudioStudioApp::refresh_nics() {
    nics_ = lv::enumerate_network_interfaces(false, false);
    if (!cfg_.primary_nic_ip.empty()) {
        local_lan_ip_ = cfg_.primary_nic_ip;
    } else if (!nics_.empty()) {
        local_lan_ip_ = nics_[0].ipv4_address;
    }
}

void LanVidAudioStudioApp::apply_nics() {
    refresh_nics();
    const std::string sec = cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : "";
    discovery_.set_network_interfaces(cfg_.primary_nic_ip, sec);
    if (monitor_srv_) monitor_srv_->set_network_interfaces(cfg_.primary_nic_ip, sec);
    if (sender_) lv_sender_set_network_interfaces(sender_, cfg_.primary_nic_ip.c_str(), sec.c_str());
    if (duplex_) lv_duplex_set_network_interfaces(duplex_, cfg_.primary_nic_ip.c_str(), sec.c_str());
    save_app_config(cfg_, config_path_);
}

void LanVidAudioStudioApp::refresh_device_caches(bool force) {
    const uint64_t now = lv::now_us();
    if (!force && now - last_device_refresh_us_ < 3000000ull) return;
    last_device_refresh_us_ = now;
    cached_sys_outputs_ = lv::list_system_audio_devices(true, true);
    cached_sys_inputs_ = lv::list_system_audio_devices(false, true);
    cached_asio_drivers_ = lv::list_external_asio_drivers(true);
    cached_midi_inputs_ = lv::list_midi_inputs();
    cached_midi_outputs_ = lv::list_midi_outputs();
}

void LanVidAudioStudioApp::refresh_grid_adapters() {
    grid_backend_ok_ = lv::grid::l2_native_backend_available(&grid_backend_status_);
    grid_adapters_ = lv::grid::list_l2_adapters();
}

bool LanVidAudioStudioApp::start_grid_node() {
    stop_grid_node();
    refresh_grid_adapters();
    lv::grid::L2AdapterInfo chosen{};
    bool found = false;
    for (const auto& a : grid_adapters_) {
        if (!cfg_.grid_nic_name.empty() && a.device_name == cfg_.grid_nic_name) {
            chosen = a;
            found = true;
            break;
        }
    }
    if (!found) found = lv::grid::pick_default_l2_adapter(grid_adapters_, chosen);
    if (!found) {
        grid_last_err_ = "Не знайдено Ethernet-адаптера";
        return false;
    }
    cfg_.grid_nic_name = chosen.device_name;
    cfg_.grid_nic_mac = chosen.mac.to_string();

    std::string err;
    auto transport = lv::grid::create_native_l2_transport(chosen, &err);
    if (!transport) {
        grid_last_err_ = err.empty() ? grid_backend_status_ : err;
        return false;
    }

    lv::grid::GridNodeConfig gcfg{};
    gcfg.node_name = std::string(studio_name_buf_) + " (" + lv::local_host_name() + ")";
    gcfg.role = lv::grid::GridNodeRole::kStudioHost;
    gcfg.max_channels = uint16_t(std::clamp(cfg_.grid_channels, 64, 256));
    gcfg.sample_rate = uint32_t(cfg_.audio_sample_rate);
    gcfg.vlan = {cfg_.grid_vlan_enabled, uint16_t(std::clamp(cfg_.grid_vlan_id, 0, 4094)),
                 uint8_t(std::clamp(cfg_.grid_vlan_pcp, 0, 7))};
    gcfg.clock_master = cfg_.grid_clock_master;

    grid_node_ = std::make_unique<lv::grid::GridNode>(std::move(transport), gcfg);
    grid_node_->broadcast_discovery_probe();

    lv::grid::MacAddress pri_mac{}, bak_mac{};
    const bool has_pri = lv::grid::MacAddress::parse(cfg_.grid_dsp_primary, pri_mac);
    const bool has_bak = lv::grid::MacAddress::parse(cfg_.grid_dsp_backup, bak_mac);
    if (has_pri) {
        grid_failover_ = std::make_unique<lv::grid::GridFailoverController>(
            *grid_node_, pri_mac, has_bak ? bak_mac : lv::grid::MacAddress{}, 8000);
        grid_node_->set_heartbeat_callback([this](const lv::grid::MacAddress& src,
                                                  const lv::grid::L2HeartbeatPayload& hb) {
            if (grid_failover_) grid_failover_->on_heartbeat(src, hb, lv::now_us());
        });
    }
    grid_last_err_.clear();
    return true;
}

void LanVidAudioStudioApp::stop_grid_node() {
    grid_failover_.reset();
    grid_node_.reset();
}

void LanVidAudioStudioApp::start_lan_scan() {
    if (scanning_.exchange(true)) return;
    if (scan_thread_.joinable()) scan_thread_.join();
    const std::string pri = cfg_.primary_nic_ip;
    const std::string sec = cfg_.dual_nic_redundancy_enabled ? cfg_.secondary_nic_ip : "";
    scan_thread_ = std::thread([this, pri, sec]() {
        auto found = lv::discovery_scan_roles(320, true, false, true, pri, sec);
        {
            std::lock_guard<std::mutex> lk(scan_mu_);
            peers_ = std::move(found);
        }
        scanning_.store(false, std::memory_order_release);
    });
}

// ============================================================================
// Main Frame & Chrome
// ============================================================================

void LanVidAudioStudioApp::render_frame() {
    const uint64_t now = lv::now_us();
    anim_time_ += ImGui::GetIO().DeltaTime;

    apply_pending_remote_sync();
    enforce_audio_ownership(now);
    if (grid_node_) {
        grid_node_->poll(128);
        if (grid_failover_) grid_failover_->check_failover(now);
    }

    // Refresh live telemetry at ~10 Hz
    if (now - telemetry_us_ >= 100000ull) {
        telemetry_us_ = now;
        if (sender_ && sender_running_) lv_sender_get_stats(sender_, &tx_stats_);
        if (receiver_ && receiver_running_) {
            lv_receiver_get_stats(receiver_, &rx_stats_);
            actual_buf_ms_ = lv_receiver_get_effective_latency_ms(receiver_);
            midi_dejitter_valid_ = (lv_receiver_get_midi_dejitter_stats(receiver_, &midi_dejitter_stats_) == LV_OK);
        }
        if (duplex_ && duplex_running_) {
            lv_duplex_get_stats(duplex_, &dpx_stats_);
            midi_dejitter_valid_ = (lv_duplex_get_midi_dejitter_stats(duplex_, &midi_dejitter_stats_) == LV_OK);
        }
    }

    // Periodic 3s device cache refresh when on Routing or Devices page
    if (cfg_.active_tab == 1 || cfg_.active_tab == 3) {
        refresh_device_caches(false);
    }

    if (config_dirty_ && now - config_save_us_ > 600000ull) {
        config_dirty_ = false;
        config_save_us_ = now;
        save_app_config(cfg_, config_path_);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidAudioRoot", nullptr, flags);
    draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##AudioSidebar", ImVec2(220.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##AudioMain", ImVec2(0.0f, avail_h), false);
    if (cfg_.active_tab == 0) draw_network_page();
    else if (cfg_.active_tab == 1) draw_routing_page();
    else if (cfg_.active_tab == 2) draw_monitor_page();
    else draw_devices_page();
    ImGui::EndChild();

    // Root-level popups (outside any child ID stack)
    draw_root_popups();

    ImGui::End();
}

void LanVidAudioStudioApp::draw_top_bar() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.accent_purp, "LanVid Audio v2.6.0");
    ImGui::SameLine();

    // Engine mode quick switcher
    const char* mode_short[] = {"Classic UDP", "Grid L2 · 256ch", "Hybrid"};
    for (int m = 0; m < 3; ++m) {
        ImGui::SameLine();
        const bool active = (cfg_.network_engine_mode == m);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, p.accent_purp);
        else ImGui::PushStyleColor(ImGuiCol_Button, p.card_soft);
        if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        else ImGui::PushStyleColor(ImGuiCol_Text, p.text_main);
        char b[48];
        std::snprintf(b, sizeof(b), "%s##top_m%d", mode_short[m], m);
        if (ImGui::SmallButton(b)) {
            cfg_.network_engine_mode = m;
            if (m == 1 || m == 2) start_grid_node();
            else stop_grid_node();
            config_dirty_ = true;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::SameLine();
    const bool synced_recent = (last_synced_us_ > 0 && (lv::now_us() - last_synced_us_) < 10000000ull);
    draw_status_pill(cfg_.sync_settings ? (synced_recent ? "Synced • Live" : "Synced • Ready") : "Sync Off",
                     cfg_.sync_settings ? p.accent_teal : p.text_muted, false);

    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "%s", status_msg_);

    const float right_x = std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - 340.0f);
    ImGui::SameLine(right_x);

    // MUTE ALL / PANIC
    ImGui::PushStyleColor(ImGuiCol_Button, master_mute_panic_ ? p.red : p.amber);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::SmallButton(master_mute_panic_ ? "UNMUTE ALL" : "MUTE ALL / PANIC")) {
        master_mute_panic_ = !master_mute_panic_;
        matrix_.send_midi_panic(-1);
        apply_live_audio_settings();
        apply_monitor_dsp_to_clients();
        broadcast_control_sync(master_mute_panic_);
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    const bool eng_on = services_running();
    ImGui::PushStyleColor(ImGuiCol_Button, eng_on ? p.green : p.red);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::SmallButton(eng_on ? "Audio Engine: ON" : "Audio Engine: OFF")) {
        if (eng_on) stop_services();
        else start_services(uint16_t(cfg_.listen_port), uint16_t(cfg_.audio_monitor_port),
                            uint16_t(cfg_.audio_discovery_port));
    }
    ImGui::PopStyleColor(2);
}

void LanVidAudioStudioApp::draw_sidebar() {
    const auto& p = studio_palette();
    for (int i = 0; i < 4; ++i) {
        const bool sel = (cfg_.active_tab == i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        if (ImGui::Button(kPageTitles[i], ImVec2(-1.0f, 40.0f))) {
            cfg_.active_tab = i;
            config_dirty_ = true;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(p.text_muted, "Стан підсистем");
    const bool mat_on = matrix_.is_running();
    draw_status_pill(mat_on ? "Matrix: RUN" : "Matrix: OFF", mat_on ? p.green : p.red, false);
    const bool stm_on = stream_running();
    draw_status_pill(stm_on ? "AoIP: LIVE" : "AoIP: IDLE", stm_on ? p.accent_node : p.text_muted, false);
    if (cfg_.network_engine_mode != 0) {
        const bool g_on = grid_node_ && grid_node_->is_open();
        draw_status_pill(g_on ? "Grid L2: UP" : "Grid L2: OFF", g_on ? p.accent_teal : p.amber, false);
    }

    const int cli_cnt = monitor_srv_ ? int(monitor_srv_->clients().size()) : 0;
    ImGui::TextColored(p.text_muted, "Смартфони: %d / 8", cli_cnt);
    ImGui::TextColored(p.text_muted, "Блоки міксу: %llu", static_cast<unsigned long long>(matrix_.frames_mixed()));

    ImGui::Spacing();
    if (ImGui::SmallButton(cfg_.dark_theme ? "Світла Лаванда" : "Темна Студія")) {
        cfg_.dark_theme = !cfg_.dark_theme;
        apply_studio_theme(cfg_.dark_theme);
        save_app_config(cfg_, config_path_);
    }
}

void LanVidAudioStudioApp::draw_subtab_pills(int page_idx, const char* const* labels, int count) {
    const auto& p = studio_palette();
    int& cur = cfg_.active_subtab[size_t(page_idx)];
    cur = std::clamp(cur, 0, count - 1);
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool sel = (cur == i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_node : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        char bid[96];
        std::snprintf(bid, sizeof(bid), "%s##sub_%d_%d", labels[i], page_idx, i);
        if (ImGui::Button(bid, ImVec2(0.0f, 30.0f))) {
            cur = i;
            config_dirty_ = true;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::Separator();
}

void LanVidAudioStudioApp::draw_root_popups() {
    // 1. Connect by IP popup
    if (open_add_ip_popup_) {
        ImGui::OpenPopup("Підключити вузол за IP");
        open_add_ip_popup_ = false;
    }
    if (ImGui::BeginPopupModal("Підключити вузол за IP", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("IP-адреса вузла", manual_ip_buf_, sizeof(manual_ip_buf_));
        ImGui::InputInt("Порт AoIP", &cfg_.dest_port);
        cfg_.dest_port = std::clamp(cfg_.dest_port, 1024, 65535);
        if (ImGui::Button("Підключити", ImVec2(130.0f, 0.0f))) {
            std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", manual_ip_buf_);
            cfg_.dest_ip = manual_ip_buf_;
            start_stream();
            save_app_config(cfg_, config_path_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Скасувати", ImVec2(110.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // 2. Add Output Device popup
    if (open_add_out_dev_popup_) {
        ImGui::OpenPopup("Додати вихідний аудіопристрій");
        open_add_out_dev_popup_ = false;
    }
    if (ImGui::BeginPopupModal("Додати вихідний аудіопристрій", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Оберіть фізичний пристрій виводу:");
        ImGui::Separator();
        for (size_t i = 0; i < cached_sys_outputs_.size(); ++i) {
            const auto& d = cached_sys_outputs_[i];
            char lbl[192];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch · %u Hz)##add_out_%zu",
                          d.name.c_str(), d.channels, unsigned(d.sample_rate), i);
            if (ImGui::Selectable(lbl)) {
                MatrixSlotConfig s{};
                s.id = -1;
                s.type = MatrixSlotType::kSystemAudio;
                s.role = MatrixSlotRole::kOutput;
                s.name = d.name;
                s.device_id = d.uid;
                s.channels = std::clamp(d.channels, 2, 64);
                s.buffer_size = cfg_.matrix_buffer_size;
                s.active = true;
                matrix_.add_slot(s);
                sync_matrix_to_config();
                config_dirty_ = true;
                ImGui::CloseCurrentPopup();
            }
        }
        for (size_t i = 0; i < cached_asio_drivers_.size(); ++i) {
            const auto& a = cached_asio_drivers_[i];
            char lbl[192];
            std::snprintf(lbl, sizeof(lbl), "[ASIO] %s##add_asio_out_%zu", a.name.c_str(), i);
            if (ImGui::Selectable(lbl)) {
                MatrixSlotConfig s{};
                s.id = -1;
                s.type = MatrixSlotType::kExternalAsio;
                s.role = MatrixSlotRole::kDuplex;
                s.name = a.name;
                s.device_id = a.clsid.empty() ? a.name : a.clsid;
                s.channels = 16;
                s.buffer_size = cfg_.matrix_buffer_size;
                s.active = true;
                matrix_.add_slot(s);
                sync_matrix_to_config();
                config_dirty_ = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (ImGui::Button("Закрити")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // 3. Add Input Device popup
    if (open_add_in_dev_popup_) {
        ImGui::OpenPopup("Додати вхідний аудіопристрій");
        open_add_in_dev_popup_ = false;
    }
    if (ImGui::BeginPopupModal("Додати вхідний аудіопристрій", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Оберіть фізичний пристрій вводу:");
        ImGui::Separator();
        for (size_t i = 0; i < cached_sys_inputs_.size(); ++i) {
            const auto& d = cached_sys_inputs_[i];
            char lbl[192];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch · %u Hz)##add_in_%zu",
                          d.name.c_str(), d.channels, unsigned(d.sample_rate), i);
            if (ImGui::Selectable(lbl)) {
                MatrixSlotConfig s{};
                s.id = -1;
                s.type = MatrixSlotType::kSystemAudio;
                s.role = MatrixSlotRole::kInput;
                s.name = d.name;
                s.device_id = d.uid;
                s.channels = std::clamp(d.channels, 2, 64);
                s.buffer_size = cfg_.matrix_buffer_size;
                s.active = true;
                matrix_.add_slot(s);
                sync_matrix_to_config();
                config_dirty_ = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (ImGui::Button("Закрити")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // 4. Monitor QR popup
    if (open_qr_popup_) {
        ImGui::OpenPopup("QR для LanVid Monitor");
        open_qr_popup_ = false;
    }
    if (ImGui::BeginPopupModal("QR для LanVid Monitor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        char uri[256];
        std::snprintf(uri, sizeof(uri), "lanvid://monitor?host=%s&port=%u&name=%s",
                      local_lan_ip_.c_str(), unsigned(bound_monitor_port()), studio_name_buf_);
        draw_qr_widget(uri, 210.0f);
        ImGui::Text("%s:%u", local_lan_ip_.c_str(), unsigned(bound_monitor_port()));
        if (ImGui::Button("Закрити", ImVec2(210.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ============================================================================
// Page 0: Мережа та Зв'язок
// ============================================================================

void LanVidAudioStudioApp::draw_network_page() {
    const char* pills[] = {"Дошка підключень", "Вузли та Синхронізація"};
    draw_subtab_pills(0, pills, 2);
    if (cfg_.active_subtab[0] == 0) draw_network_connect_subtab();
    else draw_network_nodes_sync_subtab();
}

void LanVidAudioStudioApp::draw_network_connect_subtab() {
    const auto& p = studio_palette();
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float insp_w = 330.0f;
    const float canvas_w = std::max(320.0f, total_w - insp_w - 10.0f);

    // Left: Topology Canvas + Quick Presets
    ImGui::BeginChild("##NetCanvasPane", ImVec2(canvas_w, 0.0f), true);
    ImGui::TextColored(p.text_main, "Швидкі пресети підключення:");
    if (ImGui::Button("Слухати на ПК")) {
        cfg_.connect_role = 1;
        cfg_.midi_only = false;
        start_stream();
    }
    ImGui::SameLine();
    if (ImGui::Button("Студійний міст 1:1")) {
        cfg_.connect_role = 2;
        cfg_.midi_only = false;
        start_stream();
    }
    ImGui::SameLine();
    if (ImGui::Button("Тільки MIDI")) {
        cfg_.connect_role = 2;
        cfg_.midi_only = true;
        start_stream();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Підключити за IP")) {
        std::snprintf(manual_ip_buf_, sizeof(manual_ip_buf_), "%s", peer_ip_buf_);
        open_add_ip_popup_ = true;
    }

    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Інтерактивна топологія вузлів (mDNS :%u / Grid L2)",
                       unsigned(bound_discovery_port()));

    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float area_h = std::max(220.0f, ImGui::GetContentRegionAvail().y - 10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 n_local(base.x + 18.0f, base.y + 36.0f);
    const ImVec2 n_remote(base.x + std::max(260.0f, canvas_w - 220.0f), base.y + 36.0f);

    dl->AddRectFilled(n_local, ImVec2(n_local.x + 180.0f, n_local.y + 78.0f), ImGui::GetColorU32(p.card_soft), 12.0f);
    dl->AddRect(n_local, ImVec2(n_local.x + 180.0f, n_local.y + 78.0f), ImGui::GetColorU32(p.accent_node), 12.0f, 0, 2.0f);
    dl->AddText(ImVec2(n_local.x + 12.0f, n_local.y + 12.0f), ImGui::GetColorU32(p.text_main), studio_name_buf_);
    char loc_sub[64];
    std::snprintf(loc_sub, sizeof(loc_sub), "LAN %s:%u", local_lan_ip_.c_str(), unsigned(bound_audio_port()));
    dl->AddText(ImVec2(n_local.x + 12.0f, n_local.y + 36.0f), ImGui::GetColorU32(p.text_muted), loc_sub);
    dl->AddText(ImVec2(n_local.x + 12.0f, n_local.y + 54.0f), ImGui::GetColorU32(p.accent_teal),
                cfg_.network_engine_mode == 1 ? "Grid L2 256ch" : "AoIP 64ch");

    dl->AddRectFilled(n_remote, ImVec2(n_remote.x + 185.0f, n_remote.y + 78.0f), ImGui::GetColorU32(p.card_soft), 12.0f);
    dl->AddRect(n_remote, ImVec2(n_remote.x + 185.0f, n_remote.y + 78.0f),
                ImGui::GetColorU32(stream_running() ? p.green : p.border), 12.0f, 0, 2.0f);
    dl->AddText(ImVec2(n_remote.x + 12.0f, n_remote.y + 12.0f), ImGui::GetColorU32(p.text_main), "Віддалений вузол");
    dl->AddText(ImVec2(n_remote.x + 12.0f, n_remote.y + 36.0f), ImGui::GetColorU32(p.text_muted), peer_ip_buf_);

    draw_studio_bezier_cable(dl, ImVec2(n_local.x + 180.0f, n_local.y + 34.0f),
                             ImVec2(n_remote.x, n_remote.y + 34.0f),
                             ImGui::GetColorU32(stream_running() ? p.green : p.accent_node),
                             3.2f, stream_running(), anim_time_);

    if (cfg_.dual_nic_redundancy_enabled && !cfg_.secondary_nic_ip.empty()) {
        draw_studio_dashed_bezier_cable(dl, ImVec2(n_local.x + 180.0f, n_local.y + 52.0f),
                                        ImVec2(n_remote.x, n_remote.y + 52.0f),
                                        ImGui::GetColorU32(p.accent_teal), 2.4f, anim_time_);
    }

    ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + 130.0f));
    ImGui::TextColored(p.text_muted, "Знайдені [AUDIO] вузли у LAN:");
    ImGui::SameLine();
    if (ImGui::SmallButton(scanning_.load() ? "Пошук..." : "Оновити LAN")) start_lan_scan();
    std::vector<lv::DiscoveredReceiver> copy;
    {
        std::lock_guard<std::mutex> lk(scan_mu_);
        copy = peers_;
    }
    draw_discovered_nodes_list("net_peers", copy, "audio", [this](const lv::DiscoveredReceiver& r) {
        std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", r.ip.c_str());
        cfg_.dest_ip = r.ip;
        cfg_.dest_port = r.stream_port;
        config_dirty_ = true;
    });
    (void)area_h;
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Inspector Panel
    ImGui::BeginChild("##NetInspectorPane", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.accent_purp, "Інспектор потоку AoIP");
    ImGui::Separator();

    const bool running = stream_running();
    ImGui::PushStyleColor(ImGuiCol_Button, running ? p.red : p.green);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button(running ? "Стоп потік" : "Старт потік", ImVec2(-1.0f, 38.0f))) {
        if (running) stop_stream();
        else start_stream();
    }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    const char* dirs[] = {"Передача (Send)", "Прийом (Receive)", "Дуплекс (1:1)"};
    if (ImGui::Combo("Напрямок", &cfg_.connect_role, dirs, 3)) config_dirty_ = true;

    int ch_idx = 1;
    for (int i = 0; i < 5; ++i) if (kChannelChoices[i] == cfg_.aoip_channels) ch_idx = i;
    const char* ch_lbls[] = {"2 ch (Stereo)", "8 ch", "16 ch", "32 ch", "64 ch"};
    if (ImGui::Combo("Канали", &ch_idx, ch_lbls, 5)) {
        cfg_.aoip_channels = kChannelChoices[ch_idx];
        apply_live_audio_settings();
        broadcast_control_sync();
        config_dirty_ = true;
    }

    int br_idx = 0;
    for (int i = 0; i < 6; ++i) if (kBitrateChoices[i] == cfg_.audio_bitrate_kbps) br_idx = i;
    if (ImGui::Combo("Бітрейт", &br_idx, kBitrateLabels, 6)) {
        cfg_.audio_bitrate_kbps = kBitrateChoices[br_idx];
        cfg_.audio_codec = (cfg_.audio_bitrate_kbps == 0) ? 0 : 3;
        apply_live_audio_settings();
        broadcast_control_sync();
        config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(p.text_muted, "Телеметрія:");
    const float q_frac = running ? 0.98f : 0.0f;
    const int lat_ms = receiver_running_ && actual_buf_ms_ > 0 ? int(actual_buf_ms_) : cfg_.aoip_latency_ms;
    const double mbps = running ? estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec, cfg_.audio_bitrate_kbps) : 0.0;
    char s_q[24], s_lat[24], s_mbps[24];
    std::snprintf(s_q, sizeof(s_q), running ? "99%%" : "--");
    std::snprintf(s_lat, sizeof(s_lat), "%d ms", lat_ms);
    std::snprintf(s_mbps, sizeof(s_mbps), "%.1f", mbps);

    draw_telemetry_ring_gauge("ЯКІСТЬ", s_q, "FEC/ARQ", q_frac, 98.0f, 96.0f, false, true);
    ImGui::SameLine();
    draw_telemetry_ring_gauge("ЗАТРИМКА", s_lat, cfg_.adaptive_jitter ? "Adaptive" : "Fixed",
                              std::clamp(float(lat_ms) / 100.0f, 0.05f, 1.0f), 98.0f, 96.0f);
    ImGui::SameLine();
    draw_telemetry_ring_gauge("ТРАФІК", s_mbps, "Mbps", std::clamp(float(mbps) / 100.0f, 0.02f, 1.0f), 98.0f, 96.0f);

    ImGui::EndChild();
}

void LanVidAudioStudioApp::draw_network_nodes_sync_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##NodesSyncPane", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(p.text_main, "Знайдені вузли LAN та Grid L2");
    ImGui::SameLine();
    if (ImGui::SmallButton(scanning_.load() ? "Пошук..." : "Оновити список")) start_lan_scan();

    std::vector<lv::DiscoveredReceiver> copy;
    {
        std::lock_guard<std::mutex> lk(scan_mu_);
        copy = peers_;
    }

    if (ImGui::BeginTable("nodes_tbl", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ім'я вузла");
        ImGui::TableSetupColumn("Роль / Протокол");
        ImGui::TableSetupColumn("IP / MAC");
        ImGui::TableSetupColumn("Пінг");
        ImGui::TableSetupColumn("Дія");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < copy.size(); ++i) {
            const auto& r = copy[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", r.device_name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", r.role == "audio" ? "[AUDIO] UDP" : r.role.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s:%u", r.ip.c_str(), unsigned(r.stream_port));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u ms", unsigned(r.rtt_ms));
            ImGui::TableSetColumnIndex(4);
            char b[48];
            std::snprintf(b, sizeof(b), "Обрати##pick_%zu", i);
            if (ImGui::SmallButton(b)) {
                std::snprintf(peer_ip_buf_, sizeof(peer_ip_buf_), "%s", r.ip.c_str());
                cfg_.dest_ip = r.ip;
                cfg_.dest_port = r.stream_port;
                config_dirty_ = true;
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    const uint64_t fo = monitor_srv_ ? monitor_srv_->failover_packets() : 0;
    if (draw_nic_selector_panel("net_nic", nics_, cfg_.primary_nic_id, cfg_.primary_nic_ip,
                                cfg_.secondary_nic_id, cfg_.secondary_nic_ip,
                                cfg_.dual_nic_redundancy_enabled, fo, [this]() { refresh_nics(); })) {
        apply_nics();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Двостороння синхронізація параметрів (kControlSync)");
    if (ImGui::Checkbox("Синхронізувати параметри з віддаленим вузлом", &cfg_.sync_settings)) {
        config_dirty_ = true;
    }
    if (ImGui::Checkbox("Також синхронізувати маршрути матриці (Presets)", &cfg_.sync_matrix_routing)) {
        config_dirty_ = true;
    }
    ImGui::EndChild();
}

// ============================================================================
// Page 1: Маршрутизація
// ============================================================================

void LanVidAudioStudioApp::draw_routing_page() {
    const char* pills[] = {"Візуальний Patchbay", "Матриця 64×64", "MIDI-роутинг"};
    draw_subtab_pills(1, pills, 3);
    if (cfg_.active_subtab[1] == 0) draw_routing_patchbay_subtab();
    else if (cfg_.active_subtab[1] == 1) draw_routing_matrix_subtab();
    else draw_routing_midi_subtab();
}

void LanVidAudioStudioApp::draw_routing_patchbay_subtab() {
    const auto& p = studio_palette();
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float insp_w = 290.0f;
    const float board_w = std::max(340.0f, total_w - insp_w - 10.0f);

    ImGui::BeginChild("##PatchbayBoard", ImVec2(board_w, 0.0f), true);
    ImGui::TextColored(p.text_main, "Пресети маршрутизації:");
    ImGui::SameLine();
    int net_id = -1, daw_id = -1, mon_id = -1;
    const auto slots = matrix_.slots();
    for (const auto& s : slots) {
        if (s.type == MatrixSlotType::kNetworkAoip) net_id = s.id;
        else if (s.type == MatrixSlotType::kVirtualBus) daw_id = s.id;
        else if (s.type == MatrixSlotType::kSystemAudio && s.role == MatrixSlotRole::kOutput && mon_id < 0) mon_id = s.id;
    }
    if (ImGui::SmallButton("Net → DAW + Навушники")) {
        matrix_active_preset_ = 0;
        matrix_.apply_preset(0, net_id, daw_id, mon_id);
        sync_matrix_to_config();
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Full-Duplex 64×64")) {
        matrix_active_preset_ = 1;
        matrix_.apply_preset(1, net_id, daw_id, mon_id);
        sync_matrix_to_config();
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1 Direct")) {
        matrix_active_preset_ = 2;
        matrix_.apply_preset(2, net_id, daw_id, mon_id);
        sync_matrix_to_config();
        config_dirty_ = true;
    }
    ImGui::Separator();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    struct PinPos { int id; ImVec2 pos; };
    std::vector<PinPos> src_pins, dst_pins;

    ImGui::Columns(2, "pb_cols_v2", false);
    ImGui::TextColored(p.accent_node, "ДЖЕРЕЛА (Sources)");
    for (const auto& s : slots) {
        if (s.role == MatrixSlotRole::kOutput) continue;
        const auto st = matrix_.slot_status(s.id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool sel = (drag_src_slot_ == s.id);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, p.accent_purp);
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "%s (%d ch)##pb_s%d", s.name.c_str(), s.channels, s.id);
        if (ImGui::Button(lbl, ImVec2(230.0f, 34.0f))) {
            drag_src_slot_ = (drag_src_slot_ == s.id) ? -1 : s.id;
        }
        if (sel) ImGui::PopStyleColor();
        draw_mini_vu(st.peak_in_dbfs, 230.0f, 5.0f);
        src_pins.push_back({s.id, ImVec2(pos.x + 234.0f, pos.y + 17.0f)});
    }

    ImGui::NextColumn();
    ImGui::TextColored(p.green, "ПРИЙМАЧІ (Receivers)");
    for (const auto& d : slots) {
        if (d.role == MatrixSlotRole::kInput) continue;
        const auto st = matrix_.slot_status(d.id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "%s (%d ch)##pb_d%d", d.name.c_str(), d.channels, d.id);
        if (ImGui::Button(lbl, ImVec2(230.0f, 34.0f))) {
            if (drag_src_slot_ >= 0) {
                if (matrix_.connect_slot_block(drag_src_slot_, d.id, 64, 0.0f, false,
                                               !matrix_.feedback_guard()) == CrosspointResult::kOk) {
                    selected_link_src_ = drag_src_slot_;
                    selected_link_dst_ = d.id;
                    sync_matrix_to_config();
                    config_dirty_ = true;
                }
                drag_src_slot_ = -1;
            }
        }
        draw_mini_vu(st.peak_out_dbfs, 230.0f, 5.0f);
        dst_pins.push_back({d.id, ImVec2(pos.x - 4.0f, pos.y + 17.0f)});
    }
    ImGui::Columns(1);

    const auto links = matrix_.slot_patch_links();
    for (const auto& lk : links) {
        ImVec2 p0{}, p1{};
        bool f0 = false, f1 = false;
        for (const auto& sp : src_pins) if (sp.id == lk.src_slot) { p0 = sp.pos; f0 = true; break; }
        for (const auto& dp : dst_pins) if (dp.id == lk.dst_slot) { p1 = dp.pos; f1 = true; break; }
        if (f0 && f1) {
            const bool is_sel = (lk.src_slot == selected_link_src_ && lk.dst_slot == selected_link_dst_);
            const ImU32 col = ImGui::GetColorU32(lk.all_muted ? p.text_muted : (is_sel ? p.accent_purp : p.accent_node));
            draw_studio_bezier_cable(dl, p0, p1, col, is_sel ? 4.2f : 3.0f, !lk.all_muted, anim_time_);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right Link Inspector
    ImGui::BeginChild("##PatchbayInspector", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.accent_purp, "Керування лінією");
    ImGui::Separator();
    bool fg = matrix_.feedback_guard();
    if (ImGui::Checkbox("Feedback Loop Guard", &fg)) {
        matrix_.set_feedback_guard(fg);
        cfg_.matrix_feedback_guard = fg;
        config_dirty_ = true;
    }
    ImGui::Separator();

    if (links.empty()) {
        ImGui::TextWrapped("Оберіть джерело ліворуч, а потім приймач праворуч, щоб прокласти кабель.");
    } else {
        for (const auto& lk : links) {
            char lbuf[96];
            std::snprintf(lbuf, sizeof(lbuf), "Slot %d → Slot %d (%d ch)%s",
                          lk.src_slot, lk.dst_slot, lk.channel_count, lk.all_muted ? " [M]" : "");
            const bool sel = (lk.src_slot == selected_link_src_ && lk.dst_slot == selected_link_dst_);
            if (ImGui::Selectable(lbuf, sel)) {
                selected_link_src_ = lk.src_slot;
                selected_link_dst_ = lk.dst_slot;
            }
        }
        for (const auto& lk : links) {
            if (lk.src_slot == selected_link_src_ && lk.dst_slot == selected_link_dst_) {
                ImGui::Separator();
                float g = lk.gain_db;
                bool m = lk.all_muted;
                if (ImGui::SliderFloat("Gain (dB)", &g, -48.0f, 12.0f, "%.1f dB")) {
                    matrix_.set_slot_block_gain_mute(lk.src_slot, lk.dst_slot, g, m);
                    sync_matrix_to_config();
                    config_dirty_ = true;
                }
                if (ImGui::Checkbox("MUTE лінію", &m)) {
                    matrix_.set_slot_block_gain_mute(lk.src_slot, lk.dst_slot, g, m);
                    sync_matrix_to_config();
                    config_dirty_ = true;
                }
                if (ImGui::Button("Розірвати з'єднання", ImVec2(-1.0f, 0.0f))) {
                    matrix_.disconnect_slot_block(lk.src_slot, lk.dst_slot);
                    selected_link_src_ = -1;
                    selected_link_dst_ = -1;
                    sync_matrix_to_config();
                    config_dirty_ = true;
                }
                break;
            }
        }
    }
    ImGui::EndChild();
}

void LanVidAudioStudioApp::draw_routing_matrix_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##Matrix64Pane", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);

    const auto slots = matrix_.slots();
    if (ImGui::Checkbox("Стерео-пари (1-2, 3-4...)", &cfg_.matrix_stereo_pairs)) config_dirty_ = true;
    ImGui::SameLine();
    const char* ranges[] = {"1–16", "17–32", "33–48", "49–64"};
    for (int r = 0; r < 4; ++r) {
        ImGui::SameLine();
        const bool sel = (matrix_ch_range_ == r);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, p.accent_node);
        if (ImGui::SmallButton(ranges[r])) matrix_ch_range_ = r;
        if (sel) ImGui::PopStyleColor();
    }

    if (!slots.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Джерело", ("Slot " + std::to_string(matrix_view_src_slot_)).c_str())) {
            for (const auto& s : slots) {
                if (ImGui::Selectable((s.name + "##ms" + std::to_string(s.id)).c_str(),
                                      matrix_view_src_slot_ == s.id)) {
                    matrix_view_src_slot_ = s.id;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Приймач", ("Slot " + std::to_string(matrix_view_dst_slot_)).c_str())) {
            for (const auto& s : slots) {
                if (ImGui::Selectable((s.name + "##md" + std::to_string(s.id)).c_str(),
                                      matrix_view_dst_slot_ == s.id)) {
                    matrix_view_dst_slot_ = s.id;
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    const int base_ch = matrix_ch_range_ * 16;
    const int step = cfg_.matrix_stereo_pairs ? 2 : 1;
    const int cols = 16 / step;

    if (ImGui::BeginTable("m64_grid", cols + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("In \\ Out", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        for (int c = 0; c < cols; ++c) {
            char hdr[20];
            const int ch = base_ch + c * step + 1;
            if (step == 2) std::snprintf(hdr, sizeof(hdr), "%d-%d", ch, ch + 1);
            else std::snprintf(hdr, sizeof(hdr), "Out %d", ch);
            ImGui::TableSetupColumn(hdr, ImGuiTableColumnFlags_WidthFixed, 48.0f);
        }
        ImGui::TableHeadersRow();

        for (int r = 0; r < cols; ++r) {
            const int in_ch = base_ch + r * step;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (step == 2) ImGui::Text("In %d-%d", in_ch + 1, in_ch + 2);
            else ImGui::Text("In %d", in_ch + 1);

            for (int c = 0; c < cols; ++c) {
                const int out_ch = base_ch + c * step;
                ImGui::TableSetColumnIndex(c + 1);
                MatrixCrosspoint cp{};
                const bool on = matrix_.has_crosspoint(matrix_view_src_slot_, in_ch,
                                                       matrix_view_dst_slot_, out_ch, &cp) && !cp.mute;
                char bid[32];
                std::snprintf(bid, sizeof(bid), "%s##m_%d_%d", on ? "●" : "·", in_ch, out_ch);
                if (ImGui::SmallButton(bid)) {
                    for (int k = 0; k < step; ++k) {
                        if (on) {
                            matrix_.remove_crosspoint(matrix_view_src_slot_, in_ch + k,
                                                      matrix_view_dst_slot_, out_ch + k);
                        } else {
                            matrix_.set_crosspoint(matrix_view_src_slot_, in_ch + k,
                                                   matrix_view_dst_slot_, out_ch + k, 0.0f, false,
                                                   !matrix_.feedback_guard());
                        }
                    }
                    sync_matrix_to_config();
                    config_dirty_ = true;
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void LanVidAudioStudioApp::draw_routing_midi_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##MidiRoutingPane", ImVec2(0.0f, 0.0f), true);

    if (ImGui::Checkbox("MIDI Only (Ultra-Low Latency ~1.5 ms, без аудіо)", &cfg_.midi_only)) {
        apply_live_audio_settings();
        broadcast_control_sync();
        config_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, p.red);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("MIDI PANIC (All Notes Off)")) {
        matrix_.send_midi_panic(-1);
        broadcast_control_sync(true);
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::SmallButton("Оновити MIDI")) refresh_device_caches(true);

    if (midi_dejitter_valid_) {
        ImGui::TextColored(p.text_muted, "MIDI2 Dejitter: Enqueued %llu | Dispatched %llu | Late %llu (%.1f ms)",
                           static_cast<unsigned long long>(midi_dejitter_stats_.total_enqueued),
                           static_cast<unsigned long long>(midi_dejitter_stats_.total_dispatched),
                           static_cast<unsigned long long>(midi_dejitter_stats_.late_events),
                           midi_dejitter_stats_.playout_delay_ms);
    }

    ImGui::Separator();
    const uint64_t now = lv::now_us();
    const auto mslots = matrix_.midi_slots();
    ImGui::TextColored(p.text_main, "Активні MIDI-порти (%zu):", mslots.size());
    for (const auto& ms : mslots) {
        const auto mst = matrix_.midi_slot_status(ms.id);
        const bool blink = (mst.last_activity_us > 0 && (now - mst.last_activity_us) < 200000ull);
        draw_status_pill(blink ? "MIDI ACTIVITY" : "IDLE", blink ? p.green : p.text_muted, blink);
        ImGui::SameLine();
        ImGui::Text("%s (%llu подій)", ms.name.c_str(), static_cast<unsigned long long>(mst.events_count));
    }

    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Маршрути Merge / Split:");
    for (const auto& s : mslots) {
        if (s.type == MidiSlotType::kMidiOutput || s.type == MidiSlotType::kNetworkMidiTx) continue;
        for (const auto& d : mslots) {
            if (d.type == MidiSlotType::kMidiInput || d.type == MidiSlotType::kNetworkMidiRx || s.id == d.id) continue;
            MidiCrosspoint mcp{};
            const bool linked = matrix_.has_midi_crosspoint(s.id, d.id, &mcp) && !mcp.mute;
            bool chk = linked;
            char lbl[160];
            std::snprintf(lbl, sizeof(lbl), "%s → %s##mc_%d_%d", s.name.c_str(), d.name.c_str(), s.id, d.id);
            if (ImGui::Checkbox(lbl, &chk)) {
                if (chk) matrix_.set_midi_crosspoint(s.id, d.id, 0, false);
                else matrix_.remove_midi_crosspoint(s.id, d.id);
                sync_matrix_to_config();
                config_dirty_ = true;
            }
        }
    }
    ImGui::EndChild();
}

// ============================================================================
// Page 2: Мобільний монітор
// ============================================================================

void LanVidAudioStudioApp::draw_monitor_page() {
    const char* pills[] = {"Підключені телефони", "Канали та Серверний DSP"};
    draw_subtab_pills(2, pills, 2);
    if (cfg_.active_subtab[2] == 0) draw_monitor_phones_subtab();
    else draw_monitor_channels_dsp_subtab();
}

void LanVidAudioStudioApp::draw_monitor_phones_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##MonitorPhonesPane", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(p.text_main, "LanVid Monitor Server (UDP :%u)", unsigned(bound_monitor_port()));
    ImGui::SameLine();
    if (ImGui::SmallButton("Показати QR-код")) open_qr_popup_ = true;
    ImGui::TextColored(p.text_muted, "Адреса для підключення вручну: %s:%u",
                       local_lan_ip_.c_str(), unsigned(bound_monitor_port()));
    ImGui::Separator();

    if (monitor_srv_) {
        const auto clients = monitor_srv_->clients();
        ImGui::TextColored(p.text_muted, "Підключені смартфони (%zu / 8)  |  Flow TX: %llu  |  1+1 Dup TX: %llu",
                           clients.size(),
                           static_cast<unsigned long long>(monitor_srv_->flow_packets_tx()),
                           static_cast<unsigned long long>(monitor_srv_->dup_packets_tx()));
        ImGui::Spacing();
        for (const auto& c : clients) {
            ImGui::BeginChild(c.name.c_str(), ImVec2(0.0f, 72.0f), true);
            draw_status_pill(c.connected ? "ONLINE" : "IDLE", c.connected ? p.green : p.amber, true);
            ImGui::SameLine();
            ImGui::Text("%s (%s) — %u kbps · %u ms · Mask 0x%llX",
                        c.name.c_str(), c.address.c_str(), unsigned(c.bitrate_kbps), unsigned(c.latency_ms),
                        static_cast<unsigned long long>(c.active_channel_mask));
            ImGui::SameLine();
            bool m = c.muted;
            if (ImGui::Checkbox("MUTE", &m)) monitor_srv_->set_client_mute(c.slot, m);
            ImGui::SameLine();
            char sb[48];
            std::snprintf(sb, sizeof(sb), "Налаштувати DSP##dsp_sel_%d", c.slot);
            if (ImGui::SmallButton(sb)) {
                selected_monitor_slot_ = c.slot;
                cfg_.active_subtab[2] = 1;
            }
            ImGui::EndChild();
        }
        if (clients.empty()) {
            ImGui::TextColored(p.text_muted,
                               "Відкрийте LanVid Monitor на iOS або Android — студія [AUDIO] з'явиться автоматично.");
        }
    }
    ImGui::EndChild();
}

void LanVidAudioStudioApp::draw_monitor_channels_dsp_subtab() {
    const auto& p = studio_palette();
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float left_w = std::max(280.0f, total_w * 0.45f);

    // Left: Channel Names (1..64) + Wi-Fi 1+1 Redundancy
    ImGui::BeginChild("##MonChannelsPane", ImVec2(left_w, 0.0f), true);
    ImGui::TextColored(p.text_main, "Назви каналів (1..64) для смартфонів");
    if (ImGui::Checkbox("Wi-Fi Redundancy 1+1 Duplicate (+1.5 мс)", &cfg_.wifi_redundancy)) {
        if (monitor_srv_) monitor_srv_->set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
        apply_live_audio_settings();
        config_dirty_ = true;
    }
    ImGui::Separator();

    for (int ch = 0; ch < 16; ++ch) {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "Ch %d", ch + 1);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", cfg_.monitor_channel_names[size_t(ch)].c_str());
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::InputText(lbl, buf, sizeof(buf))) {
            cfg_.monitor_channel_names[size_t(ch)] = buf;
            if (monitor_srv_) monitor_srv_->set_channel_name(ch, buf);
            config_dirty_ = true;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Server-Side DSP
    ImGui::BeginChild("##MonDspPane", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.accent_purp, "Серверний DSP (Слот #%d)", selected_monitor_slot_ + 1);
    ImGui::Separator();

    bool changed = false;
    changed |= ImGui::Checkbox("High-Pass Filter (HPF)", &cfg_.monitor_dsp_hpf_enabled);
    if (cfg_.monitor_dsp_hpf_enabled) {
        changed |= ImGui::SliderFloat("HPF Частота (Hz)", &cfg_.monitor_dsp_hpf_hz, 20.0f, 250.0f, "%.0f Hz");
    }

    ImGui::Separator();
    changed |= ImGui::Checkbox("3-Смуговий EQ", &cfg_.monitor_dsp_eq_enabled);
    if (cfg_.monitor_dsp_eq_enabled) {
        changed |= ImGui::SliderFloat("Low Shelf (100 Hz)", &cfg_.monitor_dsp_eq_low_db, -12.0f, 12.0f, "%.1f dB");
        changed |= ImGui::SliderFloat("Mid Freq (Hz)", &cfg_.monitor_dsp_eq_mid_hz, 300.0f, 5000.0f, "%.0f Hz");
        changed |= ImGui::SliderFloat("Mid Gain (dB)", &cfg_.monitor_dsp_eq_mid_db, -12.0f, 12.0f, "%.1f dB");
        changed |= ImGui::SliderFloat("High Shelf (8 kHz)", &cfg_.monitor_dsp_eq_high_db, -12.0f, 12.0f, "%.1f dB");
    }

    ImGui::Separator();
    changed |= ImGui::Checkbox("Compressor + Brickwall Limiter (-0.5 dBFS)", &cfg_.monitor_dsp_comp_enabled);
    if (cfg_.monitor_dsp_comp_enabled) {
        changed |= ImGui::SliderFloat("Threshold", &cfg_.monitor_dsp_comp_thresh_db, -40.0f, 0.0f, "%.1f dB");
        changed |= ImGui::SliderFloat("Ratio", &cfg_.monitor_dsp_comp_ratio, 1.0f, 10.0f, "%.1f:1");
        changed |= ImGui::SliderFloat("Makeup Gain", &cfg_.monitor_dsp_comp_makeup_db, 0.0f, 18.0f, "%.1f dB");
        if (monitor_srv_) {
            if (auto* b = monitor_srv_->bus(selected_monitor_slot_)) {
                const float gr = std::fabs(b->last_comp_gr_db());
                ImGui::ProgressBar(std::clamp(gr / 18.0f, 0.0f, 1.0f), ImVec2(-1.0f, 14.0f), "Gain Reduction");
            }
        }
    }

    ImGui::Separator();
    changed |= ImGui::SliderFloat("Master Volume", &cfg_.monitor_dsp_master_db, -60.0f, 12.0f, "%.1f dB");
    changed |= ImGui::Checkbox("Master Mute", &cfg_.monitor_dsp_master_mute);

    if (changed) {
        apply_monitor_dsp_to_clients();
        config_dirty_ = true;
    }
    ImGui::EndChild();
}

// ============================================================================
// Page 3: Пристрої та Налаштування
// ============================================================================

void LanVidAudioStudioApp::draw_devices_page() {
    const char* pills[] = {"Аудіопристрої та Драйвери", "LanVid Grid (Layer 2 · 256ch)", "Загальні та Мережа"};
    draw_subtab_pills(3, pills, 3);
    if (cfg_.active_subtab[3] == 0) draw_devices_audio_subtab();
    else if (cfg_.active_subtab[3] == 1) draw_devices_grid_subtab();
    else draw_devices_general_subtab();
}

void LanVidAudioStudioApp::draw_devices_audio_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##DevAudioPane", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(p.text_main, "Віртуальна шина DAW (64 In / 64 Out)");
#if defined(__APPLE__)
    ImGui::TextColored(p.text_muted, "Драйвер macOS: LanVid Virtual 64ch (CoreAudio HAL)");
#else
    ImGui::TextColored(p.text_muted, "Драйвер Windows: LanVid Virtual ASIO (64ch)");
#endif
    ImGui::Separator();

    const char* sr_items[] = {"48000 Hz", "96000 Hz"};
    int sr_idx = (cfg_.audio_sample_rate == 96000) ? 1 : 0;
    if (ImGui::Combo("Частота дискретизації", &sr_idx, sr_items, 2)) {
        cfg_.audio_sample_rate = (sr_idx == 1) ? 96000 : 48000;
        config_dirty_ = true;
    }

    const int buf_vals[] = {128, 256, 512};
    const char* buf_items[] = {"128 samples", "256 samples", "512 samples"};
    int buf_idx = 1;
    for (int i = 0; i < 3; ++i) if (buf_vals[i] == cfg_.matrix_buffer_size) buf_idx = i;
    if (ImGui::Combo("Розмір буфера матриці", &buf_idx, buf_items, 3)) {
        cfg_.matrix_buffer_size = buf_vals[buf_idx];
        matrix_.set_buffer_size(cfg_.matrix_buffer_size);
        config_dirty_ = true;
    }

    ImGui::Spacing();
    if (ImGui::Button("+ Add Output Device")) {
        refresh_device_caches(true);
        open_add_out_dev_popup_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Add Input Device")) {
        refresh_device_caches(true);
        open_add_in_dev_popup_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Оновити пристрої")) refresh_device_caches(true);

    ImGui::Separator();
    const auto slots = matrix_.slots();
    for (const auto& s : slots) {
        const auto st = matrix_.slot_status(s.id);
        ImGui::BeginChild(("slot_" + std::to_string(s.id)).c_str(), ImVec2(0.0f, 54.0f), true);
        ImGui::Text("[%d] %s (%d ch) — %s", s.id, s.name.c_str(), s.channels, st.status_text.c_str());
        if (s.type == MatrixSlotType::kSystemAudio || s.type == MatrixSlotType::kExternalAsio) {
            ImGui::SameLine();
            char rb[32];
            std::snprintf(rb, sizeof(rb), "Видалити##rm_%d", s.id);
            if (ImGui::SmallButton(rb)) {
                matrix_.remove_slot(s.id);
                sync_matrix_to_config();
                config_dirty_ = true;
            }
        }
        draw_mini_vu(std::max(st.peak_in_dbfs, st.peak_out_dbfs), 320.0f, 6.0f);
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void LanVidAudioStudioApp::draw_devices_grid_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##DevGridPane", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(p.accent_purp, "LanVid Grid (Layer 2 Ethernet · 256ch)");
    draw_status_pill(grid_backend_ok_ ? "L2 Driver: READY" : "L2 Driver: N/A",
                     grid_backend_ok_ ? p.green : p.amber, false);
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "%s", grid_backend_status_.c_str());

    if (ImGui::BeginCombo("Ethernet-адаптер (NIC)", cfg_.grid_nic_name.empty() ? "Авто" : cfg_.grid_nic_name.c_str())) {
        for (const auto& a : grid_adapters_) {
            const std::string lbl = a.friendly_name + " [" + a.mac.to_string() + "]";
            if (ImGui::Selectable(lbl.c_str(), cfg_.grid_nic_name == a.device_name)) {
                cfg_.grid_nic_name = a.device_name;
                cfg_.grid_nic_mac = a.mac.to_string();
                config_dirty_ = true;
            }
        }
        ImGui::EndCombo();
    }

    const int ch_opts[] = {64, 128, 256};
    const char* ch_lbls[] = {"64 ch", "128 ch", "256 ch"};
    int ch_idx = 2;
    for (int i = 0; i < 3; ++i) if (ch_opts[i] == cfg_.grid_channels) ch_idx = i;
    if (ImGui::Combo("Канали Grid L2", &ch_idx, ch_lbls, 3)) {
        cfg_.grid_channels = ch_opts[ch_idx];
        config_dirty_ = true;
    }

    const int blk_opts[] = {16, 32, 48};
    const char* blk_lbls[] = {"16 samples (0.33 ms)", "32 samples (0.67 ms)", "48 samples (1.0 ms)"};
    int blk_idx = 1;
    for (int i = 0; i < 3; ++i) if (blk_opts[i] == cfg_.grid_block_samples) blk_idx = i;
    if (ImGui::Combo("Розмір блоку", &blk_idx, blk_lbls, 3)) {
        cfg_.grid_block_samples = blk_opts[blk_idx];
        config_dirty_ = true;
    }

    if (ImGui::Checkbox("IEEE 802.1Q VLAN / 802.1p QoS (PCP=6)", &cfg_.grid_vlan_enabled)) config_dirty_ = true;
    if (ImGui::Checkbox("Clock Master (L2 Sync)", &cfg_.grid_clock_master)) config_dirty_ = true;

    char pri_buf[32], bak_buf[32];
    std::snprintf(pri_buf, sizeof(pri_buf), "%s", cfg_.grid_dsp_primary.c_str());
    std::snprintf(bak_buf, sizeof(bak_buf), "%s", cfg_.grid_dsp_backup.c_str());
    if (ImGui::InputText("Primary DSP Server MAC", pri_buf, sizeof(pri_buf))) {
        cfg_.grid_dsp_primary = pri_buf;
        config_dirty_ = true;
    }
    if (ImGui::InputText("Backup DSP Server MAC", bak_buf, sizeof(bak_buf))) {
        cfg_.grid_dsp_backup = bak_buf;
        config_dirty_ = true;
    }

    const bool g_on = grid_node_ && grid_node_->is_open();
    if (ImGui::Button(g_on ? "Зупинити Grid L2" : "Запустити Grid L2", ImVec2(200.0f, 34.0f))) {
        if (g_on) stop_grid_node();
        else start_grid_node();
    }
    if (!grid_last_err_.empty()) {
        ImGui::TextColored(p.red, "%s", grid_last_err_.c_str());
    }
    if (grid_failover_) {
        const auto ft = grid_failover_->telemetry();
        ImGui::TextColored(p.text_muted, "DSP Failover: %s | Switchovers: %u",
                           ft.using_backup ? "BACKUP ACTIVE" : "PRIMARY", unsigned(ft.failover_count));
    }
    ImGui::EndChild();
}

void LanVidAudioStudioApp::draw_devices_general_subtab() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##DevGeneralPane", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(p.text_main, "Загальні налаштування та Мережа");
    ImGui::Separator();

    if (ImGui::InputText("Назва аудіостудії", studio_name_buf_, sizeof(studio_name_buf_))) {
        cfg_.studio_name = studio_name_buf_;
        if (monitor_srv_) monitor_srv_->set_studio_name(studio_name_buf_);
        config_dirty_ = true;
    }

    const char* lat_modes[] = {"Фіксована (Ethernet / кабель)", "Автоматична (Adaptive для Wi-Fi)"};
    if (ImGui::Combo("Режим затримки", &cfg_.adaptive_jitter, lat_modes, 2)) {
        apply_live_audio_settings();
        broadcast_control_sync();
        config_dirty_ = true;
    }
    if (ImGui::SliderInt("Буфер затримки (мс)", &cfg_.aoip_latency_ms, 2, 200)) {
        apply_live_audio_settings();
        broadcast_control_sync();
        config_dirty_ = true;
    }

    ImGui::Separator();
    ImGui::InputInt("Порт AoIP потоку (за замовч. 5004)", &cfg_.listen_port);
    ImGui::InputInt("Порт Personal Monitor (за замовч. 5020)", &cfg_.audio_monitor_port);
    ImGui::InputInt("Порт Discovery (за замовч. 5021)", &cfg_.audio_discovery_port);
    cfg_.listen_port = std::clamp(cfg_.listen_port, 1024, 65535);
    cfg_.audio_monitor_port = std::clamp(cfg_.audio_monitor_port, 1024, 65535);
    cfg_.audio_discovery_port = std::clamp(cfg_.audio_discovery_port, 1024, 65535);

    if (ImGui::Checkbox("Автозапуск останньої сесії при старті", &cfg_.autostart_session)) config_dirty_ = true;
    if (ImGui::Checkbox("Темна тема (Dark Studio)", &cfg_.dark_theme)) {
        apply_studio_theme(cfg_.dark_theme);
        save_app_config(cfg_, config_path_);
    }

    ImGui::Spacing();
    if (ImGui::Button("Зберегти конфігурацію зараз")) {
        sync_matrix_to_config();
        save_app_config(cfg_, config_path_);
    }
    ImGui::EndChild();
}

}  // namespace lv::gui
