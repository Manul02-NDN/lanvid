#include "gui/lanvid_studio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "common/clock.hpp"
#include "imgui.h"

namespace lv::gui {

namespace {

constexpr int kBusChoices[] = {2, 8, 16, 32, 64};

const char* latency_mode_label(int ms) {
    if (ms <= 10) return "Ultra Low Latency: 0.5-1 мс пакети, XOR FEC 1+1/2+1, без ARQ";
    if (ms <= 100) return "Balanced / Live: 2.5-5 мс пакети, XOR FEC 4+1 + 1 NACK";
    return "High-Reliability / Bulletproof: 10-20 мс пакети, інтерлівінг FEC + до 12 NACK + дублювання";
}

void noop_bus_callback(const LV_MultichannelAudioBlock*, void*) {}  // drives the playout clock + meters

void push_hist(float* arr, int len, float val) {
    for (int i = 0; i < len - 1; ++i) arr[i] = arr[i + 1];
    arr[len - 1] = val;
}

}  // namespace

void LanVidStudioApp::apply_theme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.ChildRounding = 8.0f;
    st.FrameRounding = 6.0f;
    st.PopupRounding = 6.0f;
    st.GrabRounding = 6.0f;
    st.TabRounding = 6.0f;
    st.WindowPadding = ImVec2(14.0f, 12.0f);
    st.FramePadding = ImVec2(10.0f, 6.0f);
    st.ItemSpacing = ImVec2(10.0f, 8.0f);
    st.ScrollbarSize = 12.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.12f, 0.16f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.26f, 0.34f, 0.65f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.18f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.25f, 0.33f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.31f, 0.42f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.13f, 0.19f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(0.26f, 0.68f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.62f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.74f, 1.00f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.42f, 0.76f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.52f, 0.90f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.36f, 0.68f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.36f, 0.62f, 0.80f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.48f, 0.80f, 0.90f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.55f, 0.90f, 1.00f);
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.16f, 0.22f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.50f, 0.85f, 0.85f);
    c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.46f, 0.82f, 1.00f);
    c[ImGuiCol_PlotLines] = ImVec4(0.28f, 0.78f, 1.00f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = ImVec4(0.50f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.95f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.57f, 0.65f, 1.00f);
}

LanVidStudioApp::LanVidStudioApp(PlatformHost host) : host_(host) {}

LanVidStudioApp::~LanVidStudioApp() { shutdown(); }

void LanVidStudioApp::init() {
    lv_init();
    load_app_config(cfg_);
    std::snprintf(ip_buf_, sizeof(ip_buf_), "%s", cfg_.dest_ip.c_str());
    std::snprintf(file_buf_, sizeof(file_buf_), "%s", cfg_.file_path.c_str());
    std::snprintf(dpx_peer_buf_, sizeof(dpx_peer_buf_), "%s", cfg_.dpx_peer_host.c_str());
    duplex_ = lv_duplex_create();
    for (int d = 0; d < 2; ++d)
        for (int c = 0; c < 64; ++c) route_[d][c] = int8_t(c);

    sender_ = lv_sender_create();
    receiver_ = lv_receiver_create();
    if (host_.create_texture) {
        sender_tex_ = host_.create_texture(host_.ctx);
        receiver_tex_ = host_.create_texture(host_.ctx);
    }

    refresh_devices();
    init_audio_matrix();
    if (monitors_.empty() && cfg_.source_type == LV_SOURCE_SCREEN) {
        cfg_.source_type = LV_SOURCE_PATTERN;
    }
    start_lan_scan();
}

void LanVidStudioApp::shutdown() {
    audio_matrix_.stop();
    stop_sender();
    stop_receiver();
    if (disc_thread_.joinable()) disc_thread_.join();
    if (nodes_thread_.joinable()) nodes_thread_.join();
    if (duplex_) {
        lv_duplex_stop(duplex_);
        lv_duplex_destroy(duplex_);
        duplex_ = nullptr;
        duplex_running_ = false;
    }

    if (sender_) {
        stop_tone();
        stop_vdev_tx();
        vdev_rx_.stop();
        lv_sender_destroy(sender_);
        sender_ = nullptr;
    }
    if (receiver_) {
        lv_receiver_destroy(receiver_);
        receiver_ = nullptr;
    }
    if (host_.destroy_texture) {
        if (sender_tex_) {
            host_.destroy_texture(host_.ctx, sender_tex_);
            sender_tex_ = nullptr;
        }
        if (receiver_tex_) {
            host_.destroy_texture(host_.ctx, receiver_tex_);
            receiver_tex_ = nullptr;
        }
    }
    cfg_.dest_ip = ip_buf_;
    cfg_.file_path = file_buf_;
    cfg_.dpx_peer_host = dpx_peer_buf_;
    sync_matrix_to_config();
    save_app_config(cfg_);
    lv_shutdown();
}

void LanVidStudioApp::refresh_devices() {
    monitors_ = list_monitors();
    cameras_ = list_cameras();
    loop_devs_ = list_audio_devices(true);
    mic_devs_ = list_audio_devices(false);
    asio_devs_ = list_external_asio_drivers(true);
    ifaces_ = list_network_interfaces();
    local_lan_ip_ = "127.0.0.1";
    for (const auto& iface : ifaces_) {
        if (!iface.is_loopback && !iface.ipv4.empty()) {
            local_lan_ip_ = iface.ipv4;
            break;
        }
    }
    if (!monitors_.empty() && cfg_.monitor_index >= int(monitors_.size())) cfg_.monitor_index = 0;
    if (!cameras_.empty() && cfg_.camera_index >= int(cameras_.size())) cfg_.camera_index = 0;
}

void LanVidStudioApp::start_lan_scan() {
    if (scanning_.exchange(true)) return;
    if (disc_thread_.joinable()) disc_thread_.join();
    disc_thread_ = std::thread([this]() {
        auto res = discovery_scan(450, kDiscoveryPort);
        {
            std::lock_guard<std::mutex> lk(disc_mu_);
            discovered_ = std::move(res);
        }
        scanning_.store(false, std::memory_order_release);
    });
}

void LanVidStudioApp::apply_preset(int preset_idx) {
    cfg_.preset = preset_idx;
    if (preset_idx == 0) {  // Low Latency 1080p60
        cfg_.pattern_width = 1920;
        cfg_.pattern_height = 1080;
        cfg_.fps = 60;
        cfg_.quality = 80;
        cfg_.fec_group = 0;
        cfg_.adaptive_quality = false;
    } else if (preset_idx == 1) {  // Ultra 4K60
        cfg_.pattern_width = 3840;
        cfg_.pattern_height = 2160;
        cfg_.fps = 60;
        cfg_.quality = 80;
        cfg_.fec_group = 6;
        cfg_.adaptive_quality = false;
    } else if (preset_idx == 2) {  // Wi-Fi Adaptive
        cfg_.fps = 60;
        cfg_.quality = 75;
        cfg_.fec_group = 6;
        cfg_.adaptive_quality = true;
        cfg_.q_min = 35;
        cfg_.q_max = 85;
    }
    config_dirty_ = true;
}

void LanVidStudioApp::start_sender() {
    if (!sender_) return;
    cfg_.dest_ip = ip_buf_;
    cfg_.file_path = file_buf_;
    save_app_config(cfg_);

    LvSenderConfig sc;
    lv_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "%s", cfg_.dest_ip.c_str());
    sc.dest_port = uint16_t(std::clamp(cfg_.dest_port, 1024, 65535));
    sc.source_type = cfg_.source_type;
    sc.monitor_index = cfg_.monitor_index;
    sc.capture_backend = cfg_.capture_backend;
    sc.camera_index = cfg_.camera_index;
    std::snprintf(sc.file_path, sizeof(sc.file_path), "%s", cfg_.file_path.c_str());
    sc.width = (cfg_.source_type == LV_SOURCE_PATTERN) ? cfg_.pattern_width : 0;
    sc.height = (cfg_.source_type == LV_SOURCE_PATTERN) ? cfg_.pattern_height : 0;
    sc.fps = double(cfg_.fps);
    sc.quality = cfg_.quality;
    sc.codec = std::clamp(cfg_.codec, 0, 2);
    sc.bitrate_kbps = uint32_t(std::clamp(cfg_.bitrate_mbps, 1, 50) * 1000);
    sc.keyframe_interval = uint32_t(std::clamp(cfg_.keyframe_interval, 15, 300));
    sc.fec_group_size = cfg_.fec_group;
    sc.adaptive_quality = cfg_.adaptive_quality ? 1 : 0;
    sc.q_min = cfg_.q_min;
    sc.q_max = cfg_.q_max;
    sc.audio_mode = cfg_.audio_mode;
    sc.audio_device_index = cfg_.audio_device_index;
    sc.audio_chunk_ms = cfg_.audio_chunk_ms;
    sc.audio_only = cfg_.audio_only ? 1 : 0;

    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    bus.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
    lv_sender_configure_audio_bus(sender_, &bus);

    last_tx_preview_fid_ = 0;
    if (lv_sender_start(sender_, &sc)) {
        sender_running_ = true;
        if (cfg_.aoip_tone && cfg_.aoip_source == 0) start_tone();
        if (cfg_.aoip_source == 1) start_vdev_tx();
        toast_msg_ = std::string(cfg_.audio_only ? "Передача [Audio Only] -> " : "Передача запущена -> ") +
                     cfg_.dest_ip + ":" + std::to_string(cfg_.dest_port);
        toast_until_us_ = now_us() + 3000000ull;
    } else {
        sender_running_ = false;
        toast_msg_ = "Помилка запуску передачі (перевірте IP / джерело)";
        toast_until_us_ = now_us() + 4000000ull;
    }
}

void LanVidStudioApp::start_tone() {
    stop_tone();
    tone_channels_.store(std::clamp(cfg_.aoip_channels, 1, 64));
    tone_stop_.store(false);
    tone_thread_ = std::thread([this]() {
        constexpr uint32_t kRate = 48000, kBlk = 240;  // 5 ms blocks
        std::vector<float> planes(size_t(LV_AUDIO_MAX_CHANNELS) * kBlk);
        const void* ptrs[LV_AUDIO_MAX_CHANNELS];
        for (int c = 0; c < LV_AUDIO_MAX_CHANNELS; ++c) ptrs[c] = planes.data() + size_t(c) * kBlk;
        uint64_t pos = 0;
        uint64_t next = now_us();
        while (!tone_stop_.load()) {
            next += uint64_t(kBlk) * 1000000ull / kRate;
            sleep_until_us(next, [] {});
            const int n = tone_channels_.load();
            for (int c = 0; c < n; ++c) {
                const double w = 6.283185307179586 * 110.0 * double(c + 1) / kRate;
                float* d = planes.data() + size_t(c) * kBlk;
                for (uint32_t i = 0; i < kBlk; ++i) d[i] = float(0.2 * std::sin(w * double(pos + i)));
            }
            pos += kBlk;
            lv_sender_push_audio_multichannel(sender_, ptrs, uint16_t(n), kBlk, LV_AUDIO_FORMAT_F32,
                                              LV_AUDIO_LAYOUT_PLANAR, LV_MEDIA_TS_AUTO);
        }
    });
}

void LanVidStudioApp::start_vdev_tx() {
    stop_vdev_tx();
    ShmAudioHost::Config hc;
    hc.channels = uint32_t(std::clamp(cfg_.aoip_channels, 1, 64));
    vdev_tx_.start(hc, [this](const float* const* p, uint32_t ch, uint32_t n, bool disc) {
        // Re-anchor to the sender Master Clock after DAW start/stall; otherwise stay contiguous.
        const uint64_t ts = disc ? lv_sender_get_master_clock_samples(sender_) - n : LV_MEDIA_TS_AUTO;
        lv_sender_push_audio_multichannel(sender_, p, uint16_t(ch), n, LV_AUDIO_FORMAT_F32,
                                          LV_AUDIO_LAYOUT_PLANAR, ts);
    }, nullptr);
}

void LanVidStudioApp::stop_vdev_tx() { vdev_tx_.stop(); }

void LanVidStudioApp::apply_receiver_output() {
    if (!receiver_) return;
    vdev_rx_.stop();
    if (!receiver_running_) return;
    if (cfg_.aoip_output == 1) {
        // The bridge becomes the single consumer of the playout matrix (it also drives the meters).
        lv_receiver_set_multichannel_audio_callback(receiver_, nullptr, nullptr);
        ShmAudioHost::Config hc;
        hc.channels = uint32_t(std::clamp(cfg_.aoip_channels, 1, 64));
        vdev_rx_.start(hc, nullptr, [this](float* const* p, uint32_t ch, uint32_t n) {
            static thread_local std::vector<float> tmp;
            tmp.resize(size_t(ch) * n);
            lv_receiver_pop_audio_multichannel(receiver_, tmp.data(), uint16_t(ch), n, LV_AUDIO_LAYOUT_PLANAR);
            for (uint32_t c = 0; c < ch; ++c) std::copy(tmp.begin() + c * n, tmp.begin() + (c + 1) * n, p[c]);
        });
    } else {
        lv_receiver_set_multichannel_audio_callback(receiver_, &noop_bus_callback, nullptr);
    }
}

void LanVidStudioApp::draw_vdev_status(const ShmHostStatus& st, bool tx_side) {
    if (!st.bus_open) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "Шина %s недоступна", kShmBusName);
        return;
    }
    if (st.driver_connected) {
        const uint32_t fill = tx_side ? st.tx_fill : st.rx_fill;
        ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.52f, 1.0f), "DAW підключена через %s",
                           st.driver_name.empty() ? "драйвер" : st.driver_name.c_str());
        ImGui::Text("%u Гц | буфер %u | %s %u / %u кадрів", st.sample_rate, st.buffer_size,
                    tx_side ? "TX" : "RX", fill, unsigned(kShmBusFrames));
        ImGui::ProgressBar(float(fill) / float(kShmBusFrames), ImVec2(-1, 6), "");
        ImGui::TextDisabled("%s: %llu кадрів | переповнень %llu | недоборів %llu | переякорень %llu",
                            tx_side ? "Відправлено" : "Доставлено",
                            (unsigned long long)(tx_side ? st.tx_frames : st.rx_frames),
                            (unsigned long long)st.tx_overruns, (unsigned long long)st.rx_underruns,
                            (unsigned long long)st.reanchors);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Очікування DAW: виберіть пристрій «LanVid»");
        ImGui::TextDisabled("Windows: ASIO «LanVid Virtual ASIO» | macOS: «LanVid Virtual 64ch» | Linux: lanvid-audio-bridge");
    }
}

// ---------------------------------------------------------------- Full-duplex 64x64 AoIP node

void LanVidStudioApp::scan_aoip_nodes() {
    if (nodes_scanning_.exchange(true)) return;
    if (nodes_thread_.joinable()) nodes_thread_.join();
    nodes_thread_ = std::thread([this] {
        std::vector<LV_AudioNode> found(32);
        const int n = lv_discovery_scan_audio_nodes(600, 0, found.data(), int(found.size()));
        found.resize(size_t(std::max(n, 0)));
        {
            std::lock_guard<std::mutex> lk(nodes_mu_);
            nodes_ = std::move(found);
        }
        nodes_scanning_.store(false);
    });
}

void LanVidStudioApp::start_duplex() {
    if (!duplex_) return;
    // The duplex node owns both rings of the virtual device: release the single-direction bridges.
    stop_vdev_tx();
    vdev_rx_.stop();
    cfg_.dpx_peer_host = dpx_peer_buf_;
    save_app_config(cfg_);
    LV_DuplexConfig dc;
    lv_duplex_config_default(&dc);
    std::snprintf(dc.peer_host, sizeof(dc.peer_host), "%s", dpx_peer_buf_);
    dc.peer_port = uint16_t(std::clamp(cfg_.dpx_peer_port, 1024, 65535));
    dc.listen_port = uint16_t(std::clamp(cfg_.dpx_listen_port, 1024, 65535));
    dc.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    dc.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
    const int rc = lv_duplex_start(duplex_, &dc);
    duplex_running_ = rc == LV_OK;
    if (duplex_running_) {
        lv_audio_set_tx_routing(duplex_, route_[0]);
        lv_audio_set_rx_routing(duplex_, route_[1]);
        toast_msg_ = "Duplex 64x64: DAW -> " + std::string(dpx_peer_buf_) + ":" + std::to_string(dc.peer_port) +
                     " | :" + std::to_string(dc.listen_port) + " -> DAW";
    } else {
        toast_msg_ = std::string("Не вдалося запустити duplex: ") + lv_error_string(rc);
    }
    toast_until_us_ = now_us() + 3500000ull;
}

void LanVidStudioApp::stop_duplex() {
    if (duplex_) lv_duplex_stop(duplex_);
    duplex_running_ = false;
}

void LanVidStudioApp::draw_patchbay_block() {
    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "Channel Routing & Solo/Mute");
    ImGui::SameLine();
    ImGui::RadioButton("TX (DAW -> мережа)", &patch_dir_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("RX (мережа -> DAW)", &patch_dir_, 1);
    const LV_AudioDirection dir = patch_dir_ == 0 ? LV_AUDIO_DIR_TX : LV_AUDIO_DIR_RX;
    const int d = patch_dir_;

    // Routing editor
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("Джерело", &patch_src_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("-> Призначення (0 = вимк.)", &patch_dst_);
    patch_src_ = std::clamp(patch_src_, 1, 64);
    patch_dst_ = std::clamp(patch_dst_, 0, 64);
    bool apply = false;
    if (ImGui::Button("Призначити")) { route_[d][patch_src_ - 1] = int8_t(patch_dst_ - 1); apply = true; }
    ImGui::SameLine();
    if (ImGui::Button("1:1")) { for (int c = 0; c < 64; ++c) route_[d][c] = int8_t(c); apply = true; }
    ImGui::SameLine();
    if (ImGui::Button("Реверс 1..N")) {
        const int n = std::clamp(cfg_.aoip_channels, 1, 64);
        for (int c = 0; c < 64; ++c) route_[d][c] = int8_t(c < n ? n - 1 - c : c);
        apply = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Скинути M/S")) {
        for (int c = 0; c < 64; ++c) {
            mute_[d][c] = solo_[d][c] = false;
            if (duplex_) lv_audio_set_channel_mute_solo(duplex_, dir, uint16_t(c), 0, 0);
        }
    }
    if (apply && duplex_) {
        if (d == 0) lv_audio_set_tx_routing(duplex_, route_[0]);
        else lv_audio_set_rx_routing(duplex_, route_[1]);
    }
    std::string routes;
    for (int c = 0; c < 64; ++c)
        if (route_[d][c] != c) routes += std::to_string(c + 1) + "->" + (route_[d][c] < 0 ? std::string("off") : std::to_string(route_[d][c] + 1)) + "  ";
    ImGui::TextDisabled("Маршрути: %s", routes.empty() ? "1:1 (без змін)" : routes.c_str());

    // 8 x 8 grid of channels with M / S toggles (5 ms ramps in the core: no clicks)
    const int n = std::clamp(cfg_.aoip_channels, 1, 64);
    const ImVec4 on_m(0.85f, 0.25f, 0.22f, 1.0f), on_s(0.95f, 0.78f, 0.15f, 1.0f), off(0.20f, 0.23f, 0.29f, 1.0f);
    for (int c = 0; c < n; ++c) {
        ImGui::PushID(d * 64 + c);
        if (c % 8) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%2d", c + 1);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, mute_[d][c] ? on_m : off);
        if (ImGui::SmallButton("M")) {
            mute_[d][c] = !mute_[d][c];
            if (duplex_) lv_audio_set_channel_mute_solo(duplex_, dir, uint16_t(c), mute_[d][c] ? 1 : 0, -1);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, solo_[d][c] ? on_s : off);
        if (ImGui::SmallButton("S")) {
            solo_[d][c] = !solo_[d][c];
            if (duplex_) lv_audio_set_channel_mute_solo(duplex_, dir, uint16_t(c), -1, solo_[d][c] ? 1 : 0);
        }
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

void LanVidStudioApp::draw_duplex_tab() {
    const uint64_t now = now_us();
    if (duplex_running_ && now - dpx_stats_us_ > 250000) {
        dpx_stats_us_ = now;
        lv_duplex_get_stats(duplex_, &dpx_stats_);
    }
    ImGui::BeginChild("##DpxLeft", ImVec2(420.0f, 0), true);
    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "Full-Duplex 64x64 AoIP Transceiver");
    ImGui::TextDisabled("DAW-виходи -> вузол-партнер | партнер -> DAW-входи");
    ImGui::Separator();

    if (ImGui::Button(nodes_scanning_.load() ? "Пошук..." : "Знайти AoIP-вузли в LAN")) scan_aoip_nodes();
    {
        std::lock_guard<std::mutex> lk(nodes_mu_);
        const char* preview = nodes_.empty() ? "(натисніть «Знайти»)" : "Виберіть вузол...";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##DiscoveredAoIP", preview)) {
            for (size_t i = 0; i < nodes_.size(); ++i) {
                const LV_AudioNode& nd = nodes_[i];
                char lbl[320];
                std::snprintf(lbl, sizeof(lbl), "%s  %s:%u  %uch %s  [%s%s]", nd.name, nd.ip, nd.rx_port, nd.channels,
                              nd.duplex ? "duplex" : "rx", nd.audio_driver[0] ? nd.audio_driver : "без DAW",
                              nd.driver_connected ? ", DAW on" : "");
                if (ImGui::Selectable(lbl, false)) {  // one click: take the node's receive port as our target
                    std::snprintf(dpx_peer_buf_, sizeof(dpx_peer_buf_), "%s", nd.ip);
                    cfg_.dpx_peer_port = nd.rx_port;
                    if (nd.duplex && nd.tx_port) cfg_.dpx_listen_port = nd.tx_port;  // where it sends to us
                    config_dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Discovered AoIP Nodes: %zu", nodes_.size());
    }
    if (ImGui::InputText("IP партнера (вручну)", dpx_peer_buf_, sizeof(dpx_peer_buf_))) config_dirty_ = true;
    if (ImGui::InputInt("Порт партнера", &cfg_.dpx_peer_port)) {
        cfg_.dpx_peer_port = std::clamp(cfg_.dpx_peer_port, 1024, 65535);
        config_dirty_ = true;
    }
    if (ImGui::InputInt("Мій порт прийому", &cfg_.dpx_listen_port)) {
        cfg_.dpx_listen_port = std::clamp(cfg_.dpx_listen_port, 1024, 65535);
        config_dirty_ = true;
    }
    {
        int bus_idx = 1;
        for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
        const char* bus_items[] = {"2 канали", "8 каналів", "16 каналів", "32 канали", "64 канали"};
        if (ImGui::Combo("Канали AoIP", &bus_idx, bus_items, 5)) {
            cfg_.aoip_channels = kBusChoices[bus_idx];
            config_dirty_ = true;
        }
    }
    if (ImGui::SliderInt("Затримка", &cfg_.aoip_latency_ms, 2, 2000, "%d мс", ImGuiSliderFlags_Logarithmic)) {
        if (duplex_running_) lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    if (ImGui::Checkbox("Audio Only (Disable Video)", &cfg_.audio_only)) {
        config_dirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Duplex 64x64 завжди передає лише AoIP без відео; перемикач також вимикає відео у Sender/Receiver.");
    }
    ImGui::Spacing();
    if (!duplex_running_) {
        if (ImGui::Button("Старт Duplex 64x64", ImVec2(-1, 34))) start_duplex();
    } else if (ImGui::Button("Стоп Duplex", ImVec2(-1, 34))) {
        stop_duplex();
    }
    ImGui::Separator();
    if (duplex_running_) {
        const LV_DuplexStats& st = dpx_stats_;
        st.driver_connected ? ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.52f, 1.0f), "DAW: %s | %u Гц | буфер %u",
                                                 st.driver_name, st.sample_rate, st.buffer_size)
                            : ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Очікування DAW на пристрої «LanVid»");
        ImGui::Text("Партнер -> ми: годинник %s | RTT %.2f мс | %+.1f ppm", st.rx_clock_synced ? "synced" : "free-run",
                    double(st.rx_rtt_us) / 1000.0, st.rx_clock_skew_ppm);
        ImGui::TextDisabled("  пакети %llu | втрачено %llu | у DAW %llu кадрів | недоборів %llu",
                            (unsigned long long)st.rx_packets, (unsigned long long)st.rx_lost,
                            (unsigned long long)st.rx_frames_to_daw, (unsigned long long)st.rx_underruns);
        ImGui::Text("Ми -> партнер: пакети %llu | з DAW %llu кадрів", (unsigned long long)st.tx_packets,
                    (unsigned long long)st.tx_frames_from_daw);
        ImGui::TextDisabled("  переповнень %llu | відповідей clock-sync %llu (годинник цього напрямку вимірює партнер)",
                            (unsigned long long)st.tx_overruns, (unsigned long long)st.tx_pongs_sent);
    } else {
        ImGui::TextDisabled("Партнер запускає дзеркальні порти: його «Порт партнера» = мій порт прийому.");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##DpxPatch", ImVec2(0, 0), true);
    draw_patchbay_block();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- Multi-Device Audio Matrix (64x64)

void LanVidStudioApp::init_audio_matrix() {
    audio_matrix_.clear_slots();
    audio_matrix_.set_buffer_size(cfg_.matrix_buffer_size);
    audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);

    if (cfg_.matrix_slots.empty()) {
        MatrixSlotConfig daw{};
        daw.id = 0;
        daw.type = MatrixSlotType::kVirtualBus;
        daw.role = MatrixSlotRole::kDuplex;
        daw.name = "DAW Virtual Bus (64ch)";
        daw.channels = 64;
        daw.buffer_size = cfg_.matrix_buffer_size;
        daw.active = true;
        audio_matrix_.add_slot(daw);

        MatrixSlotConfig net{};
        net.id = 1;
        net.type = MatrixSlotType::kNetworkAoip;
        net.role = MatrixSlotRole::kDuplex;
        net.name = "Network AoIP (64ch)";
        net.channels = 64;
        net.buffer_size = cfg_.matrix_buffer_size;
        net.active = true;
        audio_matrix_.add_slot(net);

        MatrixSlotConfig hp{};
        hp.id = 2;
        hp.type = MatrixSlotType::kSystemAudio;
        hp.role = MatrixSlotRole::kOutput;
        hp.name = "PC Headphones / Output";
        hp.device_index = -1;
        hp.channels = 2;
        hp.buffer_size = cfg_.matrix_buffer_size;
        hp.active = true;
        audio_matrix_.add_slot(hp);

        audio_matrix_.apply_preset(0, 1, 0, 2);
        sync_matrix_to_config();
    } else {
        for (const auto& s : cfg_.matrix_slots) audio_matrix_.add_slot(s);
        for (const auto& c : cfg_.matrix_crosspoints) {
            audio_matrix_.set_crosspoint(c.src_slot, c.src_ch, c.dst_slot, c.dst_ch, c.gain_db, c.mute,
                                         !cfg_.matrix_feedback_guard);
        }
    }

    audio_matrix_.set_network_io(
        [this](const float* const* planes, uint32_t ch, uint32_t frames) {
            if (sender_ && sender_running_) {
                lv_sender_push_audio_multichannel(sender_, planes, uint16_t(ch), frames,
                                                  LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR, LV_MEDIA_TS_AUTO);
            }
        },
        [this](float* const* planes, uint32_t ch, uint32_t frames) {
            if (receiver_ && receiver_running_) {
                static thread_local std::vector<float> tmp;
                tmp.resize(size_t(ch) * frames);
                lv_receiver_pop_audio_multichannel(receiver_, tmp.data(), uint16_t(ch), frames,
                                                   LV_AUDIO_LAYOUT_PLANAR);
                for (uint32_t c = 0; c < ch; ++c) {
                    std::copy(tmp.begin() + size_t(c) * frames, tmp.begin() + size_t(c + 1) * frames, planes[c]);
                }
            }
        });
}

void LanVidStudioApp::sync_matrix_to_config() {
    cfg_.matrix_buffer_size = audio_matrix_.buffer_size();
    cfg_.matrix_feedback_guard = audio_matrix_.feedback_guard();
    cfg_.matrix_slots = audio_matrix_.slots();
    cfg_.matrix_crosspoints = audio_matrix_.crosspoints();
}

void LanVidStudioApp::draw_audio_matrix_tab() {
    auto slots = audio_matrix_.slots();
    const bool running = audio_matrix_.is_running();

    auto find_slot_by_type = [&](MatrixSlotType t, MatrixSlotRole role_hint = MatrixSlotRole::kDuplex,
                                 bool check_role = false) -> int {
        for (const auto& s : slots) {
            if (s.type == t && (!check_role || s.role == role_hint)) return s.id;
        }
        return -1;
    };

    const float left_w = std::min(430.0f, ImGui::GetContentRegionAvail().x * 0.40f);

    // ---- LEFT PANEL: Audio Device Slots (up to 8) ----
    ImGui::BeginChild("##MatrixDevices", ImVec2(left_w, 0), true);
    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "Multi-Device Audio Matrix (48 kHz)");
    ImGui::TextDisabled("Слоти пристроїв (%zu / %d): WASAPI + ASIO + DAW SHM + AoIP", slots.size(), kMatrixMaxSlots);
    ImGui::Separator();

    if (!running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.62f, 0.34f, 1.0f));
        if (ImGui::Button("Старт Audio Matrix", ImVec2(-1, 34))) {
            stop_vdev_tx();
            vdev_rx_.stop();
            audio_matrix_.start();
            toast_msg_ = "Audio Matrix запущено (48 kHz, буфер " + std::to_string(audio_matrix_.buffer_size()) + ")";
            toast_until_us_ = now_us() + 3000000ull;
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.76f, 0.20f, 0.22f, 1.0f));
        if (ImGui::Button("Стоп Audio Matrix", ImVec2(-1, 34))) {
            audio_matrix_.stop();
        }
        ImGui::PopStyleColor();
    }

    int buf_idx = (cfg_.matrix_buffer_size <= 128) ? 0 : (cfg_.matrix_buffer_size <= 256 ? 1 : 2);
    const char* buf_items[] = {"128 семплів (2.7 мс)", "256 семплів (5.3 мс)", "512 семплів (10.7 мс)"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##MatrixBuf", &buf_idx, buf_items, 3)) {
        cfg_.matrix_buffer_size = (buf_idx == 0) ? 128 : (buf_idx == 1 ? 256 : 512);
        audio_matrix_.set_buffer_size(cfg_.matrix_buffer_size);
        config_dirty_ = true;
    }

    if (ImGui::Checkbox("Feedback Loop Guard (захист від замикання)", &cfg_.matrix_feedback_guard)) {
        audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);
        config_dirty_ = true;
    }

    if (ImGui::Button("+ Add Output Device")) ImGui::OpenPopup("##AddOutDev");
    ImGui::SameLine();
    if (ImGui::Button("+ Add Input Device")) ImGui::OpenPopup("##AddInDev");
    ImGui::SameLine();
    if (ImGui::Button("Оновити")) refresh_devices();

    if (ImGui::BeginPopup("##AddOutDev")) {
        ImGui::TextDisabled("WASAPI Виходи (навушники / звукові карти):");
        if (ImGui::Selectable("Системний вихід за замовчуванням (WASAPI)")) {
            MatrixSlotConfig s{};
            s.id = -1;
            s.type = MatrixSlotType::kSystemAudio;
            s.role = MatrixSlotRole::kOutput;
            s.name = "Default Output (WASAPI)";
            s.device_index = -1;
            s.channels = 2;
            s.buffer_size = cfg_.matrix_buffer_size;
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : loop_devs_) {
            if (ImGui::Selectable(d.name.c_str())) {
                MatrixSlotConfig s{};
                s.id = -1;
                s.type = MatrixSlotType::kSystemAudio;
                s.role = MatrixSlotRole::kOutput;
                s.name = d.name;
                s.device_id = d.name;
                s.device_index = d.index;
                s.channels = 2;
                s.buffer_size = cfg_.matrix_buffer_size;
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        if (!asio_devs_.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Зовнішні ASIO-драйвери (Duplex):");
            for (const auto& a : asio_devs_) {
                if (ImGui::Selectable((a.name + " [ASIO]").c_str())) {
                    MatrixSlotConfig s{};
                    s.id = -1;
                    s.type = MatrixSlotType::kExternalAsio;
                    s.role = MatrixSlotRole::kDuplex;
                    s.name = a.name;
                    s.device_id = a.name;
                    s.channels = 16;
                    s.buffer_size = cfg_.matrix_buffer_size;
                    audio_matrix_.add_slot(s);
                    config_dirty_ = true;
                }
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##AddInDev")) {
        ImGui::TextDisabled("WASAPI Входи (мікрофон / Line-In):");
        if (ImGui::Selectable("Системний мікрофон за замовчуванням (WASAPI)")) {
            MatrixSlotConfig s{};
            s.id = -1;
            s.type = MatrixSlotType::kSystemAudio;
            s.role = MatrixSlotRole::kInput;
            s.name = "Default Mic/Line In (WASAPI)";
            s.device_index = -1;
            s.channels = 2;
            s.buffer_size = cfg_.matrix_buffer_size;
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : mic_devs_) {
            if (ImGui::Selectable(d.name.c_str())) {
                MatrixSlotConfig s{};
                s.id = -1;
                s.type = MatrixSlotType::kSystemAudio;
                s.role = MatrixSlotRole::kInput;
                s.name = d.name;
                s.device_id = d.name;
                s.device_index = d.index;
                s.channels = 2;
                s.buffer_size = cfg_.matrix_buffer_size;
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    slots = audio_matrix_.slots();
    for (auto s : slots) {
        ImGui::PushID(s.id);
        const auto st = audio_matrix_.slot_status(s.id);
        bool act = s.active;
        if (ImGui::Checkbox("##Act", &act)) {
            s.active = act;
            audio_matrix_.update_slot(s.id, s);
            config_dirty_ = true;
        }
        ImGui::SameLine();
        const char* type_tag = (s.type == MatrixSlotType::kVirtualBus)
                                   ? "SHM"
                                   : (s.type == MatrixSlotType::kNetworkAoip
                                          ? "AoIP"
                                          : (s.type == MatrixSlotType::kExternalAsio ? "ASIO" : "WASAPI"));
        ImGui::Text("[%d:%s] %s (%dch)", s.id, type_tag, s.name.c_str(), s.channels);
        if (s.type != MatrixSlotType::kVirtualBus && s.type != MatrixSlotType::kNetworkAoip) {
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                audio_matrix_.remove_slot(s.id);
                config_dirty_ = true;
                ImGui::PopID();
                break;
            }
        }
        ImGui::TextColored(st.connected ? ImVec4(0.24f, 0.90f, 0.52f, 1.0f) : ImVec4(0.75f, 0.78f, 0.85f, 1.0f),
                           "  %s | %u Hz | xruns: %llu", st.status_text.c_str(), st.device_sample_rate,
                           (unsigned long long)st.xruns);
        const float vu_in = std::clamp((st.peak_in_dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
        const float vu_out = std::clamp((st.peak_out_dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
        ImGui::ProgressBar(vu_in, ImVec2(left_w * 0.44f, 8.0f), "");
        ImGui::SameLine();
        ImGui::ProgressBar(vu_out, ImVec2(left_w * 0.44f, 8.0f), "");
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- RIGHT PANEL: Presets + Interactive Crosspoint Patchbay Grid ----
    ImGui::SameLine();
    ImGui::BeginChild("##MatrixGrid", ImVec2(0, 0), true);
    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "Crosspoint Routing Grid (1-to-many / many-to-1)");

    const int net_id = find_slot_by_type(MatrixSlotType::kNetworkAoip);
    const int daw_id = find_slot_by_type(MatrixSlotType::kVirtualBus);
    const int mon_id = find_slot_by_type(MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, true);

    if (ImGui::Button("Preset 1: Mac/Net -> DAW (64ch) + Headphones (1/2)")) {
        audio_matrix_.apply_preset(0, net_id, daw_id, mon_id);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Preset 2: Full-Duplex 64x64 + Monitor")) {
        audio_matrix_.apply_preset(1, net_id, daw_id, mon_id);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Preset 3: 1:1 Direct")) {
        audio_matrix_.apply_preset(2, net_id, daw_id, mon_id);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Очистити")) {
        audio_matrix_.clear_crosspoints();
        config_dirty_ = true;
    }
    ImGui::Separator();

    // Slot pair selectors for the interactive 16x16 grid view
    slots = audio_matrix_.slots();
    MatrixSlotConfig src_cfg{}, dst_cfg{};
    bool has_src = false, has_dst = false;
    for (const auto& s : slots) {
        if (s.id == matrix_src_slot_) { src_cfg = s; has_src = true; }
        if (s.id == matrix_dst_slot_) { dst_cfg = s; has_dst = true; }
    }
    if (!has_src && !slots.empty()) { src_cfg = slots.front(); matrix_src_slot_ = src_cfg.id; has_src = true; }
    if (!has_dst && !slots.empty()) { dst_cfg = slots.back(); matrix_dst_slot_ = dst_cfg.id; has_dst = true; }

    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Джерело (рядки)", has_src ? src_cfg.name.c_str() : "(немає)")) {
        for (const auto& s : slots) {
            if (s.role == MatrixSlotRole::kOutput) continue;
            if (ImGui::Selectable(s.name.c_str(), s.id == matrix_src_slot_)) {
                matrix_src_slot_ = s.id;
                matrix_src_page_ = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Приймач (стовпці)", has_dst ? dst_cfg.name.c_str() : "(немає)")) {
        for (const auto& s : slots) {
            if (s.role == MatrixSlotRole::kInput) continue;
            if (ImGui::Selectable(s.name.c_str(), s.id == matrix_dst_slot_)) {
                matrix_dst_slot_ = s.id;
                matrix_dst_page_ = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("Gain", &matrix_new_gain_db_, -24.0f, 6.0f, "%.1f dB");

    const char* page_lbl[] = {"Ch 1-16", "Ch 17-32", "Ch 33-48", "Ch 49-64"};
    if (has_src && src_cfg.channels > 16) {
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Рядки", &matrix_src_page_, page_lbl, std::min(4, (src_cfg.channels + 15) / 16));
        ImGui::SameLine();
    } else {
        matrix_src_page_ = 0;
    }
    if (has_dst && dst_cfg.channels > 16) {
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Стовпці", &matrix_dst_page_, page_lbl, std::min(4, (dst_cfg.channels + 15) / 16));
    } else {
        matrix_dst_page_ = 0;
    }

    if (has_src && has_dst) {
        const int r0 = matrix_src_page_ * 16;
        const int r1 = std::min(src_cfg.channels, r0 + 16);
        const int c0 = matrix_dst_page_ * 16;
        const int c1 = std::min(dst_cfg.channels, c0 + 16);
        const int cols = c1 - c0;

        if (cols > 0 && ImGui::BeginTable("##PatchTable", cols + 1,
                                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Src \\ Dst");
            for (int dc = c0; dc < c1; ++dc) {
                char ch_lbl[16];
                std::snprintf(ch_lbl, sizeof(ch_lbl), "D%d", dc + 1);
                ImGui::TableSetupColumn(ch_lbl);
            }
            ImGui::TableHeadersRow();

            for (int sc = r0; sc < r1; ++sc) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("S%02d", sc + 1);
                for (int dc = c0; dc < c1; ++dc) {
                    ImGui::TableSetColumnIndex(dc - c0 + 1);
                    MatrixCrosspoint cp{};
                    const bool linked = audio_matrix_.has_crosspoint(src_cfg.id, sc, dst_cfg.id, dc, &cp);
                    ImGui::PushID(sc * 100 + dc);
                    const ImVec4 btn_col = linked
                                               ? (cp.mute ? ImVec4(0.80f, 0.30f, 0.22f, 1.0f)
                                                          : ImVec4(0.18f, 0.72f, 0.42f, 1.0f))
                                               : ImVec4(0.16f, 0.19f, 0.25f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
                    if (ImGui::SmallButton(linked ? (cp.mute ? "M" : "●") : "·")) {
                        if (linked) {
                            audio_matrix_.remove_crosspoint(src_cfg.id, sc, dst_cfg.id, dc);
                            config_dirty_ = true;
                        } else {
                            const auto res = audio_matrix_.set_crosspoint(
                                src_cfg.id, sc, dst_cfg.id, dc, matrix_new_gain_db_, false, !cfg_.matrix_feedback_guard);
                            if (res == CrosspointResult::kBlockedFeedbackLoop) {
                                toast_msg_ = "Feedback Loop Guard: пряме замикання слоту на себе заблоковано!";
                                toast_until_us_ = now_us() + 3500000ull;
                            } else if (res == CrosspointResult::kOk) {
                                config_dirty_ = true;
                            }
                        }
                    }
                    if (linked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        audio_matrix_.set_crosspoint(src_cfg.id, sc, dst_cfg.id, dc, cp.gain_db, !cp.mute, true);
                        config_dirty_ = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        if (linked) {
                            ImGui::SetTooltip("%s Ch%d -> %s Ch%d\nGain: %.1f dB | %s\n(ЛКМ: видалити, ПКМ: Mute)",
                                              src_cfg.name.c_str(), sc + 1, dst_cfg.name.c_str(), dc + 1, cp.gain_db,
                                              cp.mute ? "MUTED" : "ACTIVE");
                        } else {
                            ImGui::SetTooltip("З'єднати %s Ch%d -> %s Ch%d (%.1f dB)",
                                              src_cfg.name.c_str(), sc + 1, dst_cfg.name.c_str(), dc + 1,
                                              matrix_new_gain_db_);
                        }
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    const auto all_cps = audio_matrix_.crosspoints();
    ImGui::Spacing();
    ImGui::TextDisabled("Активних з'єднань (crosspoints): %zu  |  ЛКМ: з'єднати/розірвати, ПКМ: Mute",
                        all_cps.size());
    ImGui::EndChild();
}

void LanVidStudioApp::stop_tone() {
    tone_stop_.store(true);
    if (tone_thread_.joinable()) tone_thread_.join();
}

void LanVidStudioApp::stop_sender() {
    stop_tone();
    stop_vdev_tx();
    if (sender_) lv_sender_stop(sender_);
    sender_running_ = false;
}

void LanVidStudioApp::start_receiver() {
    if (!receiver_) return;
    save_app_config(cfg_);

    LvReceiverConfig rc;
    lv_receiver_config_default(&rc);
    rc.bind_port = uint16_t(std::clamp(cfg_.listen_port, 1024, 65535));
    rc.conceal_mode = cfg_.conceal_mode;
    rc.audio_target_ms = cfg_.audio_target_ms;
    rc.enable_audio = 1;
    rc.enable_discovery = 1;
    rc.audio_only = cfg_.audio_only ? 1 : 0;

    apply_receiver_bus();

    last_rx_fid_ = 0;
    lv_receiver_set_volume(receiver_, cfg_.volume);
    if (lv_receiver_start(receiver_, &rc)) {
        receiver_running_ = true;
        apply_receiver_output();
        toast_msg_ = std::string(cfg_.audio_only ? "Прийом [Audio Only] на UDP :" : "Прийом запущено на UDP :") +
                     std::to_string(rc.bind_port) + " (LAN автопошук увімкнено)";
        toast_until_us_ = now_us() + 3000000ull;
    } else {
        receiver_running_ = false;
        toast_msg_ = "Не вдалося відкрити UDP порт " + std::to_string(rc.bind_port) + " (можливо, зайнятий)";
        toast_until_us_ = now_us() + 4500000ull;
    }
}

void LanVidStudioApp::apply_receiver_bus() {
    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.latency_ms = uint32_t(std::clamp(cfg_.aoip_latency_ms, 2, 2000));
    bus.num_channels = std::clamp(cfg_.aoip_channels, 1, 64);
    lv_receiver_configure_audio_bus(receiver_, &bus);
}

void LanVidStudioApp::draw_aoip_sender_controls() {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "AoIP шина (до 64 каналів, Master Clock)");
    ImGui::Separator();
    int bus_idx = 1;
    for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
    const char* bus_items[] = {"2 канали", "8 каналів", "16 каналів", "32 канали", "64 канали"};
    if (ImGui::Combo("Канали", &bus_idx, bus_items, 5)) {
        cfg_.aoip_channels = kBusChoices[bus_idx];
        tone_channels_.store(cfg_.aoip_channels);
        config_dirty_ = true;
    }
    if (ImGui::SliderInt("Буфер затримки та надійності", &cfg_.aoip_latency_ms, 2, 2000, "%d мс",
                         ImGuiSliderFlags_Logarithmic)) {
        if (sender_) lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    ImGui::TextDisabled("2 мс — Ultra Low Latency  <->  2000 мс — Bulletproof");
    ImGui::TextWrapped("%s", latency_mode_label(cfg_.aoip_latency_ms));
    const char* src_items[] = {"Built-in Test Tone Generator", "Virtual Audio Device (ASIO / CoreAudio / PipeWire)"};
    if (ImGui::Combo("Джерело шини", &cfg_.aoip_source, src_items, 2)) {
        if (sender_running_) {
            stop_tone();
            stop_vdev_tx();
            if (cfg_.aoip_source == 1) start_vdev_tx();
            else if (cfg_.aoip_tone) start_tone();
        }
        config_dirty_ = true;
    }
    if (cfg_.aoip_source == 0) {
        if (ImGui::Checkbox("Генерувати тон (110 Гц x номер каналу)", &cfg_.aoip_tone)) {
            if (sender_running_) {
                if (cfg_.aoip_tone) start_tone();
                else stop_tone();
            }
            config_dirty_ = true;
        }
    } else if (sender_running_) {
        draw_vdev_status(vdev_tx_.status(), true);
    } else {
        ImGui::TextDisabled("DAW -> виходи «LanVid Out 1..%d» -> мережа (після «Старт передачі»)", cfg_.aoip_channels);
    }
}

void LanVidStudioApp::draw_aoip_meter_grid() {
    const int n = std::clamp(cfg_.aoip_channels, 1, 64);
    const int cols = std::min(n, 16);
    const int rows = (n + cols - 1) / cols;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cell_w = std::max(18.0f, (avail - 4.0f) / float(cols)) - 4.0f;
    const float cell_h = 22.0f;
    for (int c = 0; c < n; ++c) {
        const int r = c / cols, k = c % cols;
        const ImVec2 p0(origin.x + float(k) * (cell_w + 4.0f), origin.y + float(r) * (cell_h + 4.0f));
        const ImVec2 p1(p0.x + cell_w, p0.y + cell_h);
        const LV_AudioChannelStats& st = ch_stats_[c];
        dl->AddRectFilled(p0, p1, IM_COL32(28, 32, 40, 255), 3.0f);
        if (st.active) {
            // -60..0 dBFS -> bar fill; green < -18, yellow < -6, red above.
            const float t = std::clamp((st.level_dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
            const ImU32 col = st.level_dbfs > -6.0f    ? IM_COL32(240, 80, 70, 255)
                              : st.level_dbfs > -18.0f ? IM_COL32(240, 200, 60, 255)
                                                       : IM_COL32(60, 220, 120, 255);
            dl->AddRectFilled(p0, ImVec2(p0.x + cell_w * t, p1.y), col, 3.0f);
        }
        const bool trouble = st.packets_lost > 0 || st.underruns > 0;
        dl->AddRect(p0, p1, trouble ? IM_COL32(255, 70, 70, 255) : IM_COL32(70, 78, 92, 255), 3.0f);
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%d", c + 1);
        dl->AddText(ImVec2(p0.x + 3.0f, p0.y + 3.0f), IM_COL32(230, 235, 245, 255), lbl);
        if (ImGui::IsMouseHoveringRect(p0, p1)) {
            ImGui::SetTooltip("Канал %d: %.1f dBFS%s\nПакети: %llu | втрачено: %llu\nFEC: %llu | NACK: %llu (запитів %llu)\n"
                              "Underruns: %llu | пізні: %llu | джитер: %.2f мс",
                              c + 1, st.level_dbfs, st.active ? "" : " (неактивний)",
                              (unsigned long long)st.packets_rx, (unsigned long long)st.packets_lost,
                              (unsigned long long)st.fec_recovered, (unsigned long long)st.nack_recovered,
                              (unsigned long long)st.nacks_sent, (unsigned long long)st.underruns,
                              (unsigned long long)st.late_packets, double(st.jitter_us) / 1000.0);
        }
    }
    ImGui::Dummy(ImVec2(avail, float(rows) * (cell_h + 4.0f)));
}

void LanVidStudioApp::stop_receiver() {
    vdev_rx_.stop();
    if (receiver_) lv_receiver_stop(receiver_);
    receiver_running_ = false;
}

void LanVidStudioApp::poll_telemetry_and_frames() {
    const uint64_t now = now_us();

    // Fetch latest GPU preview frame from Sender
    if (sender_ && sender_running_ && !cfg_.audio_only && sender_tex_) {
        int w = 0, h = 0;
        const int want_rgba = sender_tex_->wants_rgba() ? 1 : 0;
        int rc = lv_sender_get_preview_frame(sender_, sender_frame_buf_.data(), sender_frame_buf_.size(),
                                             &w, &h, &last_tx_preview_fid_, want_rgba);
        if (rc == -1 && w > 0 && h > 0) {
            sender_frame_buf_.resize(size_t(w) * size_t(h) * 4);
            rc = lv_sender_get_preview_frame(sender_, sender_frame_buf_.data(), sender_frame_buf_.size(),
                                             &w, &h, &last_tx_preview_fid_, want_rgba);
        }
        if (rc == 1 && w > 0 && h > 0) {
            sender_tex_->update(sender_frame_buf_.data(), w, h);
        }
    }

    // Fetch latest decoded video frame from Receiver
    if (receiver_ && receiver_running_ && !cfg_.audio_only && receiver_tex_) {
        int w = 0, h = 0;
        const int want_rgba = receiver_tex_->wants_rgba() ? 1 : 0;
        int rc = lv_receiver_get_latest_frame(receiver_, receiver_frame_buf_.data(),
                                              receiver_frame_buf_.size(), &w, &h, &last_rx_fid_,
                                              want_rgba);
        if (rc == -1 && w > 0 && h > 0) {
            receiver_frame_buf_.resize(size_t(w) * size_t(h) * 4);
            rc = lv_receiver_get_latest_frame(receiver_, receiver_frame_buf_.data(),
                                              receiver_frame_buf_.size(), &w, &h, &last_rx_fid_,
                                              want_rgba);
        }
        if (rc == 1 && w > 0 && h > 0) {
            receiver_tex_->update(receiver_frame_buf_.data(), w, h);
        }
    }

    // Update stats + rolling graphs every 500 ms
    if (now - last_stat_poll_us_ >= 500000ull) {
        last_stat_poll_us_ = now;
        if (sender_) {
            lv_sender_get_stats(sender_, &tx_stats_);
            sender_running_ = (tx_stats_.running != 0);
            if (sender_running_) {
                push_hist(tx_mbps_hist_, kHistLen, float(tx_stats_.tx_mbps));
                push_hist(tx_loss_hist_, kHistLen, float(tx_stats_.rx_loss_pct));
            }
        }
        if (receiver_ && receiver_running_ && now - ch_stats_us_ >= 100000) {
            ch_stats_us_ = now;
            for (int c = 0; c < std::clamp(cfg_.aoip_channels, 1, 64); ++c)
                lv_receiver_get_channel_stats(receiver_, uint16_t(c), &ch_stats_[c]);
        }
        if (receiver_) {
            lv_receiver_get_stats(receiver_, &rx_stats_);
            receiver_running_ = (rx_stats_.running != 0);
            if (receiver_running_) {
                push_hist(rx_lat_hist_, kHistLen, float(rx_stats_.latency_p50_ms));
                push_hist(rx_mbps_hist_, kHistLen, float(rx_stats_.rx_mbps));
            }
        }
    }

    // Auto-save config debounced every 2s when modified
    if (config_dirty_ && (now - last_save_us_ >= 2000000ull)) {
        cfg_.dest_ip = ip_buf_;
        cfg_.file_path = file_buf_;
        cfg_.dpx_peer_host = dpx_peer_buf_;
        sync_matrix_to_config();
        save_app_config(cfg_);
        config_dirty_ = false;
        last_save_us_ = now;
    }
}

void LanVidStudioApp::draw_aspect_image(IGpuTexture* tex, const ImVec2& avail, const char* empty_label) {
    const float aw = std::max(1.0f, avail.x);
    const float ah = std::max(1.0f, avail.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + aw, p0.y + ah);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 10, 14, 255), 6.0f);

    if (tex && tex->width() > 0 && tex->height() > 0 && tex->imgui_id()) {
        const float src_ar = float(tex->width()) / float(tex->height());
        const float dst_ar = aw / ah;
        float dw = aw, dh = ah;
        if (dst_ar > src_ar) {
            dh = ah;
            dw = ah * src_ar;
        } else {
            dw = aw;
            dh = aw / src_ar;
        }
        const float dx = (aw - dw) * 0.5f;
        const float dy = (ah - dh) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + dx, p0.y + dy));
        ImGui::Image(tex->imgui_id(), ImVec2(dw, dh));
    } else {
        const ImVec2 tsz = ImGui::CalcTextSize(empty_label);
        dl->AddText(ImVec2(p0.x + (aw - tsz.x) * 0.5f, p0.y + (ah - tsz.y) * 0.5f),
                    IM_COL32(130, 145, 170, 255), empty_label);
        ImGui::Dummy(ImVec2(aw, ah));
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
}

void LanVidStudioApp::draw_top_bar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.13f, 0.18f, 1.0f));
    ImGui::BeginChild("##TopBar", ImVec2(0, 46), true, ImGuiWindowFlags_NoScrollbar);
#ifndef LANVID_VERSION_STRING
#define LANVID_VERSION_STRING "2.4.0"
#endif
    ImGui::TextColored(ImVec4(0.32f, 0.78f, 1.0f, 1.0f), "LanVid Studio");
    ImGui::SameLine();
    ImGui::TextDisabled("v%s  |  SIMD: %s  |  GPU: %s  |  LAN IP: %s", LANVID_VERSION_STRING,
                        lv_get_cpu_simd_info(), host_.renderer_name, local_lan_ip_.c_str());

    // Live status pill on right side
    const char* badge = (sender_running_ && receiver_running_)
                            ? "TX + RX АКТИВНІ"
                            : (sender_running_ ? "ПЕРЕДАЧА АКТИВНА"
                                               : (receiver_running_ ? "ПРИЙОМ АКТИВНИЙ" : "ОЧІКУВАННЯ"));
    const ImVec4 col = (sender_running_ || receiver_running_) ? ImVec4(0.18f, 0.85f, 0.48f, 1.0f)
                                                              : ImVec4(0.58f, 0.64f, 0.74f, 1.0f);
    const float tw = ImGui::CalcTextSize(badge).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - tw - 24.0f));
    ImGui::TextColored(col, "%s", badge);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void LanVidStudioApp::draw_sender_tab() {
    const float left_w = std::min(430.0f, ImGui::GetContentRegionAvail().x * 0.44f);

    // ---- LEFT COLUMN: Controls ----
    ImGui::BeginChild("##SenderControls", ImVec2(left_w, 0), true);

    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "1. Джерело відео та аудіо");
    ImGui::Separator();

    if (ImGui::Checkbox("Audio Only (Disable Video)", &cfg_.audio_only)) {
        config_dirty_ = true;
        if (sender_running_) start_sender();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Аналог CLI --audio-only: без захоплення відео, енкодера та відеопакетів (лише AoIP + MasterClock).");
    }

    ImGui::BeginDisabled(cfg_.audio_only);
    const char* src_items[] = {"Тестовий патерн", "Екран / Монітор", "Веб-камера", "Файл (.y4m / .ppm)"};
    int src_idx = std::clamp(cfg_.source_type - 1, 0, 3);
    if (ImGui::Combo("Джерело", &src_idx, src_items, 4)) {
        cfg_.source_type = src_idx + 1;
        config_dirty_ = true;
    }

    if (cfg_.source_type == LV_SOURCE_SCREEN) {
        if (!monitors_.empty()) {
            const auto& m = monitors_[size_t(std::clamp(cfg_.monitor_index, 0, int(monitors_.size()) - 1))];
            char cur_lbl[128];
            std::snprintf(cur_lbl, sizeof(cur_lbl), "[%d] %dx%d%s", m.index, m.width, m.height,
                          m.primary ? " (Основний)" : "");
            if (ImGui::BeginCombo("Монітор", cur_lbl)) {
                for (size_t i = 0; i < monitors_.size(); ++i) {
                    char lbl[128];
                    std::snprintf(lbl, sizeof(lbl), "[%d] %s %dx%d%s", monitors_[i].index,
                                  monitors_[i].name.c_str(), monitors_[i].width, monitors_[i].height,
                                  monitors_[i].primary ? " (Основний)" : "");
                    if (ImGui::Selectable(lbl, cfg_.monitor_index == int(i))) {
                        cfg_.monitor_index = int(i);
                        config_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("Захоплення екрана недоступне на цій платформі");
        }
        const char* cap_be[] = {"Auto (DXGI -> GDI)", "DXGI Desktop Duplication", "Win32 GDI"};
        if (ImGui::Combo("Захоплення", &cfg_.capture_backend, cap_be, 3)) config_dirty_ = true;
    } else if (cfg_.source_type == LV_SOURCE_CAMERA) {
        if (!cameras_.empty()) {
            const auto& c = cameras_[size_t(std::clamp(cfg_.camera_index, 0, int(cameras_.size()) - 1))];
            if (ImGui::BeginCombo("Камера", c.name.c_str())) {
                for (size_t i = 0; i < cameras_.size(); ++i) {
                    if (ImGui::Selectable(cameras_[i].name.c_str(), cfg_.camera_index == int(i))) {
                        cfg_.camera_index = int(i);
                        config_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("Веб-камери не знайдено");
        }
    } else if (cfg_.source_type == LV_SOURCE_FILE) {
        if (ImGui::InputText("Шлях файлу", file_buf_, sizeof(file_buf_))) config_dirty_ = true;
    } else {
        const char* res_lbl[] = {"1280x720 (HD)", "1920x1080 (Full HD)", "3840x2160 (4K UHD)"};
        int ridx = (cfg_.pattern_width >= 3840) ? 2 : (cfg_.pattern_width >= 1920 ? 1 : 0);
        if (ImGui::Combo("Роздільна здатність", &ridx, res_lbl, 3)) {
            if (ridx == 0) { cfg_.pattern_width = 1280; cfg_.pattern_height = 720; }
            else if (ridx == 1) { cfg_.pattern_width = 1920; cfg_.pattern_height = 1080; }
            else { cfg_.pattern_width = 3840; cfg_.pattern_height = 2160; }
            config_dirty_ = true;
        }
    }
    ImGui::EndDisabled();

    const char* aud_items[] = {"Вимкнено", "Системний звук (Loopback)", "Мікрофон"};
    if (ImGui::Combo("Аудіо", &cfg_.audio_mode, aud_items, 3)) {
        cfg_.audio_device_index = -1;
        config_dirty_ = true;
    }
    if (cfg_.audio_mode != LV_AUDIO_NONE) {
        const auto& adevs = (cfg_.audio_mode == LV_AUDIO_LOOPBACK) ? loop_devs_ : mic_devs_;
        std::string cur_aud = "Системний за замовчуванням";
        if (cfg_.audio_device_index >= 0 && cfg_.audio_device_index < int(adevs.size())) {
            cur_aud = adevs[size_t(cfg_.audio_device_index)].name;
        }
        if (ImGui::BeginCombo("Аудіопристрій", cur_aud.c_str())) {
            if (ImGui::Selectable("Системний за замовчуванням", cfg_.audio_device_index == -1)) {
                cfg_.audio_device_index = -1;
                config_dirty_ = true;
            }
            for (size_t i = 0; i < adevs.size(); ++i) {
                if (ImGui::Selectable(adevs[i].name.c_str(), cfg_.audio_device_index == int(i))) {
                    cfg_.audio_device_index = int(i);
                    config_dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        int chunk_idx = (cfg_.audio_chunk_ms <= 3.0) ? 0 : 1;
        const char* chunk_items[] = {"2.5 ms (Ultra-Low Latency)", "5.0 ms (Standard LAN)"};
        if (ImGui::Combo("Пакет аудіо", &chunk_idx, chunk_items, 2)) {
            cfg_.audio_chunk_ms = (chunk_idx == 0) ? 2.5 : 5.0;
            config_dirty_ = true;
        }
    }

    draw_aoip_sender_controls();

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "2. Отримувач у LAN");
    ImGui::Separator();

    if (ImGui::Button(scanning_.load() ? "Сканування LAN..." : "Сканувати LAN")) {
        start_lan_scan();
    }
    ImGui::SameLine();
    if (ImGui::Button("Оновити пристрої")) refresh_devices();

    // Discovered receivers clickable list
    ImGui::BeginChild("##DiscoveredList", ImVec2(0, 74), true);
    {
        std::lock_guard<std::mutex> lk(disc_mu_);
        if (discovered_.empty()) {
            ImGui::TextDisabled("Активних Receiver у LAN не знайдено. Запустіть прийом або введіть IP.");
        } else {
            for (const auto& d : discovered_) {
                char row[256];
                std::snprintf(row, sizeof(row), "%s [%s]  %s:%u  (%u ms)", d.device_name.c_str(),
                              d.os.c_str(), d.ip.c_str(), unsigned(d.stream_port), unsigned(d.rtt_ms));
                if (ImGui::Selectable(row, cfg_.dest_ip == d.ip && cfg_.dest_port == int(d.stream_port))) {
                    cfg_.dest_ip = d.ip;
                    cfg_.dest_port = int(d.stream_port);
                    std::snprintf(ip_buf_, sizeof(ip_buf_), "%s", d.ip.c_str());
                    config_dirty_ = true;
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(left_w * 0.56f);
    if (ImGui::InputText("IP", ip_buf_, sizeof(ip_buf_))) config_dirty_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(92.0f);
    if (ImGui::InputInt("Порт", &cfg_.dest_port, 0, 0)) {
        cfg_.dest_port = std::clamp(cfg_.dest_port, 1024, 65535);
        config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "3. Кодек та якість");
    ImGui::Separator();

    ImGui::BeginDisabled(cfg_.audio_only);
    const char* codec_items[] = {
        "LanVid DCT (~2 ms, без затримки)",
        "H.264 / AVC (Low Bandwidth, HW)",
        "H.265 / HEVC (Ultra-Low Bandwidth, HW)"
    };
    if (ImGui::Combo("Відеокодек", &cfg_.codec, codec_items, 3)) config_dirty_ = true;

    if (ImGui::Button("Low Latency 1080p60")) apply_preset(0);
    ImGui::SameLine();
    if (ImGui::Button("Ultra 4K60")) apply_preset(1);
    ImGui::SameLine();
    if (ImGui::Button("Wi-Fi Adaptive")) apply_preset(2);

    if (cfg_.codec == LV_CODEC_LANVID_DCT) {
        if (ImGui::SliderInt("Якість (Q)", &cfg_.quality, 20, 98)) config_dirty_ = true;
    } else {
        if (ImGui::SliderInt("Цільовий бітрейт", &cfg_.bitrate_mbps, 1, 50, "%d Mbps")) {
            config_dirty_ = true;
            if (sender_ && sender_running_) {
                lv_sender_set_bitrate(sender_, uint32_t(cfg_.bitrate_mbps * 1000));
            }
        }
        if (ImGui::SliderInt("Інтервал IDR", &cfg_.keyframe_interval, 15, 300, "%d кадр.")) {
            config_dirty_ = true;
        }
        if (sender_running_ && tx_stats_.codec_backend[0]) {
            ImGui::TextColored(tx_stats_.codec_is_hardware ? ImVec4(0.24f, 0.90f, 0.52f, 1.0f)
                                                           : ImVec4(1.0f, 0.78f, 0.25f, 1.0f),
                               "Енкодер: %s", tx_stats_.codec_backend);
        }
    }
    if (ImGui::SliderInt("FPS", &cfg_.fps, 15, 120)) config_dirty_ = true;
    if (ImGui::SliderInt("XOR FEC група (0=вимк)", &cfg_.fec_group, 0, 12)) config_dirty_ = true;
    if (ImGui::Checkbox("Адаптивний контроль (AIMD для Wi-Fi)", &cfg_.adaptive_quality)) config_dirty_ = true;
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (!sender_running_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.62f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.74f, 0.42f, 1.0f));
        if (ImGui::Button("СТАРТ ПЕРЕДАЧІ", ImVec2(-1, 42))) start_sender();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.76f, 0.20f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.26f, 0.28f, 1.0f));
        if (ImGui::Button("СТОП ПЕРЕДАЧІ", ImVec2(-1, 42))) stop_sender();
        ImGui::PopStyleColor(2);
    }
    ImGui::EndChild();

    // ---- RIGHT COLUMN: Live Sender Preview + Bitrate/Loss Graphs ----
    ImGui::SameLine();
    ImGui::BeginChild("##SenderPreview", ImVec2(0, 0), true);

    ImGui::TextColored(ImVec4(0.38f, 0.80f, 1.0f, 1.0f), "Живий міні-прев'ю передачі");
    ImGui::SameLine();
    if (sender_running_ && tx_stats_.width > 0) {
        ImGui::TextDisabled("(%dx%d @ %.1f FPS | enc+tx: %.2f ms | Q=%d)",
                            tx_stats_.width, tx_stats_.height, tx_stats_.tx_fps,
                            tx_stats_.encode_tx_ms, tx_stats_.active_quality);
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float prev_h = std::max(180.0f, avail.y - 215.0f);
    draw_aspect_image((sender_running_ && !cfg_.audio_only) ? sender_tex_ : nullptr, ImVec2(avail.x, prev_h),
                      cfg_.audio_only ? "Режим Audio Only (відео вимкнено) — активна AoIP шина"
                                      : "Натисніть «СТАРТ ПЕРЕДАЧІ» для живого прев'ю");

    ImGui::Spacing();
    ImGui::Columns(4, "##TxCards", false);
    ImGui::TextDisabled("TX FPS");
    ImGui::Text("%.1f", tx_stats_.tx_fps);
    ImGui::NextColumn();
    ImGui::TextDisabled("Бітрейт");
    ImGui::Text("%.1f Mbps", tx_stats_.tx_mbps);
    ImGui::NextColumn();
    ImGui::TextDisabled("Втрати RX / FEC");
    ImGui::Text("%.2f%% / %.2f%%", tx_stats_.rx_loss_pct, tx_stats_.rx_fec_pct);
    ImGui::NextColumn();
    ImGui::TextDisabled("Аудіо");
    ImGui::Text("%.0f kbps", tx_stats_.audio_kbps);
    ImGui::Columns(1);

    ImGui::Spacing();
    char mbps_ov[64];
    std::snprintf(mbps_ov, sizeof(mbps_ov), "Бітрейт: %.1f Mbps", tx_stats_.tx_mbps);
    ImGui::PlotLines("##TxMbps", tx_mbps_hist_, kHistLen, 0, mbps_ov, 0.0f, FLT_MAX, ImVec2(-1, 62));

    char loss_ov[64];
    std::snprintf(loss_ov, sizeof(loss_ov), "Втрати пакетів RX: %.2f%%", tx_stats_.rx_loss_pct);
    ImGui::PlotLines("##TxLoss", tx_loss_hist_, kHistLen, 0, loss_ov, 0.0f, 10.0f, ImVec2(-1, 52));

    ImGui::EndChild();
}

void LanVidStudioApp::draw_receiver_tab() {
    // Top compact control bar
    ImGui::BeginChild("##RxControls", ImVec2(0, 54), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetNextItemWidth(86.0f);
    if (ImGui::InputInt("Порт", &cfg_.listen_port, 0, 0)) {
        cfg_.listen_port = std::clamp(cfg_.listen_port, 1024, 65535);
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (!receiver_running_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.62f, 0.34f, 1.0f));
        if (ImGui::Button("Старт прийому")) start_receiver();
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.76f, 0.20f, 0.22f, 1.0f));
        if (ImGui::Button("Стоп прийому")) stop_receiver();
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    if (ImGui::Button("Повний екран (F11)")) {
        if (host_.set_fullscreen && host_.is_fullscreen) {
            host_.set_fullscreen(host_.ctx, !host_.is_fullscreen(host_.ctx));
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Скріншот")) {
        char fname[128];
        std::snprintf(fname, sizeof(fname), "lanvid_shot_%04d.ppm", ++screenshot_counter_);
        if (receiver_ && lv_receiver_save_screenshot(receiver_, fname)) {
            toast_msg_ = std::string("Збережено скріншот: ") + fname;
        } else {
            toast_msg_ = "Немає активного кадру для скріншота";
        }
        toast_until_us_ = now_us() + 3000000ull;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int vol_pct = int(cfg_.volume * 100.0f + 0.5f);
    if (ImGui::SliderInt("Гучність", &vol_pct, 0, 150, "%d%%")) {
        cfg_.volume = float(vol_pct) / 100.0f;
        if (receiver_) lv_receiver_set_volume(receiver_, cfg_.volume);
        config_dirty_ = true;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    const char* conc_items[] = {"Чорний", "Попер. кадр"};
    if (ImGui::Combo("Втрати", &cfg_.conceal_mode, conc_items, 2)) {
        if (receiver_) lv_receiver_set_concealment(receiver_, cfg_.conceal_mode);
        config_dirty_ = true;
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("HUD", &cfg_.show_hud)) config_dirty_ = true;

    ImGui::SameLine();
    if (ImGui::Checkbox("Audio Only (Disable Video)", &cfg_.audio_only)) {
        config_dirty_ = true;
        if (receiver_running_) start_receiver();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Аналог CLI --audio-only: ігнорувати відеопакети до декодування (чистий AoIP-міст).");
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    int bus_idx = 1;
    for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
    const char* bus_items[] = {"2 кан.", "8 кан.", "16 кан.", "32 кан.", "64 кан."};
    if (ImGui::Combo("AoIP", &bus_idx, bus_items, 5)) {
        cfg_.aoip_channels = kBusChoices[bus_idx];
        if (receiver_) apply_receiver_bus();
        config_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Затримка", &cfg_.aoip_latency_ms, 2, 2000, "%d мс", ImGuiSliderFlags_Logarithmic)) {
        if (receiver_) lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", latency_mode_label(cfg_.aoip_latency_ms));
    ImGui::EndChild();

    // 64-channel LED meter grid (phase-aligned playout bus)
    const int bus_n = std::clamp(cfg_.aoip_channels, 1, 64);
    const int bus_rows = (bus_n + 15) / 16;
    const float meter_h = 30.0f + float(bus_rows) * 26.0f + (cfg_.aoip_output == 1 ? 22.0f : 0.0f);
    ImGui::BeginChild("##AoipMeters", ImVec2(0, meter_h), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("AoIP шина: %d кан. | затримка %d мс — %s", bus_n, cfg_.aoip_latency_ms,
                        cfg_.aoip_latency_ms <= 10 ? "Ultra Low" : (cfg_.aoip_latency_ms <= 100 ? "Balanced" : "Bulletproof"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    const char* out_items[] = {"Лише індикатори", "Virtual Audio Device (DAW inputs)"};
    if (ImGui::Combo("Вихід шини", &cfg_.aoip_output, out_items, 2)) {
        apply_receiver_output();
        config_dirty_ = true;
    }
    if (cfg_.aoip_output == 1 && receiver_running_) {
        const auto vs = vdev_rx_.status();
        vs.driver_connected ? ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.52f, 1.0f), "DAW: %s | %u Гц | буфер %u | RX %u кадрів",
                                                 vs.driver_name.c_str(), vs.sample_rate, vs.buffer_size, vs.rx_fill)
                            : ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Очікування DAW на пристрої «LanVid»");
        vdev_rx_.publish_network_state(uint32_t(cfg_.aoip_latency_ms), rx_stats_.running != 0, 0.0);
    }
    draw_aoip_meter_grid();
    ImGui::EndChild();

    // Live GPU Video Viewport
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float vid_h = std::max(160.0f, avail.y - 86.0f);
    ImGui::BeginChild("##RxCanvas", ImVec2(0, vid_h), false, ImGuiWindowFlags_NoScrollbar);
    draw_aspect_image((receiver_running_ && !cfg_.audio_only) ? receiver_tex_ : nullptr,
                      ImGui::GetContentRegionAvail(),
                      cfg_.audio_only
                          ? (receiver_running_ ? "Режим Audio Only (відео вимкнено) — AoIP прийом активний"
                                               : "Режим Audio Only — натисніть «Старт прийому» для AoIP")
                          : (receiver_running_ ? "Очікування відеопотоку по UDP..."
                                               : "Натисніть «Старт прийому», щоб приймати відео та звук"));
    ImGui::EndChild();

    // Bottom Live Telemetry Strip
    ImGui::BeginChild("##RxTelemetry", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::Columns(6, "##RxCols", false);
    ImGui::TextDisabled("Потік / FPS");
    ImGui::Text("%dx%d  %.1f FPS", rx_stats_.width, rx_stats_.height, rx_stats_.fps);
    ImGui::NextColumn();
    ImGui::TextDisabled("Кодек / Бекенд");
    {
        const char* cname = (rx_stats_.active_codec == LV_CODEC_H265)
                                ? "H.265"
                                : (rx_stats_.active_codec == LV_CODEC_H264 ? "H.264" : "DCT");
        const char* accel = rx_stats_.codec_is_hardware ? "HW" : "SW";
        ImGui::TextColored(rx_stats_.codec_is_hardware ? ImVec4(0.24f, 0.90f, 0.52f, 1.0f)
                                                       : ImVec4(0.50f, 0.84f, 1.0f, 1.0f),
                           "%s [%s]", cname, accel);
    }
    ImGui::NextColumn();
    ImGui::TextDisabled("E2E Latency (p50 / p99)");
    ImGui::TextColored(rx_stats_.latency_p50_ms < 6.0 ? ImVec4(0.24f, 0.90f, 0.52f, 1.0f)
                                                      : ImVec4(1.0f, 0.78f, 0.25f, 1.0f),
                       "%.2f / %.2f ms", rx_stats_.latency_p50_ms, rx_stats_.latency_p99_ms);
    ImGui::NextColumn();
    ImGui::TextDisabled("Бітрейт / PLI");
    if (rx_stats_.active_codec == LV_CODEC_LANVID_DCT) {
        ImGui::Text("%.1f Mbps (Q=%d)", rx_stats_.rx_mbps, rx_stats_.active_quality);
    } else {
        ImGui::Text("%.2f Mbps (PLI:%llu)", rx_stats_.rx_mbps,
                    static_cast<unsigned long long>(rx_stats_.pli_requests));
    }
    ImGui::NextColumn();
    ImGui::TextDisabled("Втрати / FEC");
    ImGui::Text("%.2f%% (FEC:%llu)", rx_stats_.loss_pct,
                static_cast<unsigned long long>(rx_stats_.fec_recovered));
    ImGui::NextColumn();
    ImGui::TextDisabled("Аудіо / Буфер");
    ImGui::Text("%.0f kbps (%.1f ms)", rx_stats_.audio_kbps, rx_stats_.audio_buffer_ms);
    ImGui::Columns(1);
    ImGui::EndChild();
}

void LanVidStudioApp::draw_fullscreen_receiver() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##FullscreenCanvas", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_aspect_image(receiver_tex_, vp->Size, "Очікування відеопотоку по UDP...");

    if (cfg_.show_hud) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 16, vp->Pos.y + 16));
        ImGui::SetNextWindowBgAlpha(0.68f);
        ImGui::Begin("##FsHud", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings);
        const char* cname = (rx_stats_.active_codec == LV_CODEC_H265)
                                ? "H.265"
                                : (rx_stats_.active_codec == LV_CODEC_H264 ? "H.264" : "DCT");
        ImGui::Text("[%s %s] %dx%d  %.1f FPS | E2E %.2f ms | %.2f Mbps | FEC:%llu | PLI:%llu | Loss:%.2f%% | Aud:%.0f kbps",
                    cname, rx_stats_.codec_is_hardware ? "HW" : "SW",
                    rx_stats_.width, rx_stats_.height, rx_stats_.fps, rx_stats_.latency_p50_ms,
                    rx_stats_.rx_mbps, static_cast<unsigned long long>(rx_stats_.fec_recovered),
                    static_cast<unsigned long long>(rx_stats_.pli_requests),
                    rx_stats_.loss_pct, rx_stats_.audio_kbps);
        ImGui::SameLine();
        if (ImGui::SmallButton("Вийти (ESC/F11)")) {
            if (host_.set_fullscreen) host_.set_fullscreen(host_.ctx, false);
        }
        ImGui::End();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void LanVidStudioApp::render_frame() {
    poll_telemetry_and_frames();

    // Global hotkeys (F11 fullscreen toggle, ESC exit fullscreen)
    const bool is_fs = host_.is_fullscreen && host_.is_fullscreen(host_.ctx);
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        if (host_.set_fullscreen) host_.set_fullscreen(host_.ctx, !is_fs);
    }
    if (is_fs && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (host_.set_fullscreen) host_.set_fullscreen(host_.ctx, false);
    }

    if (is_fs) {
        draw_fullscreen_receiver();
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##LanVidMain", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_top_bar();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##MainTabs")) {
        const ImGuiTabItemFlags f0 = (!initial_tab_applied_ && cfg_.active_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags f1 = (!initial_tab_applied_ && cfg_.active_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags f2 = (!initial_tab_applied_ && cfg_.active_tab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags f3 = (!initial_tab_applied_ && cfg_.active_tab == 3) ? ImGuiTabItemFlags_SetSelected : 0;
        initial_tab_applied_ = true;

        if (ImGui::BeginTabItem("Передача (Sender)", nullptr, f0)) {
            if (cfg_.active_tab != 0) { cfg_.active_tab = 0; config_dirty_ = true; }
            draw_sender_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Прийом (Receiver)", nullptr, f1)) {
            if (cfg_.active_tab != 1) { cfg_.active_tab = 1; config_dirty_ = true; }
            draw_receiver_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AoIP Duplex 64x64", nullptr, f2)) {
            if (cfg_.active_tab != 2) { cfg_.active_tab = 2; config_dirty_ = true; }
            draw_duplex_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio Matrix", nullptr, f3)) {
            if (cfg_.active_tab != 3) { cfg_.active_tab = 3; config_dirty_ = true; }
            draw_audio_matrix_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Toast overlay
    if (!toast_msg_.empty() && now_us() < toast_until_us_) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 20.0f, vp->Pos.y + vp->Size.y - 20.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("##Toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(0.42f, 0.88f, 1.0f, 1.0f), "%s", toast_msg_.c_str());
        ImGui::End();
    }

    ImGui::End();
}

}  // namespace lv::gui
