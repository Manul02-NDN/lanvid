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

constexpr const char* kAudioCodecItems[4] = {
    "Uncompressed (Float32 · ~98 Mbps)",
    "Lossless 24-bit (PCM24 · -25%)",
    "Compact 16-bit (PCM16 · -50%)",
    "Ultra-Light (Opus / ADPCM · -80%)"
};

double estimate_aoip_mbps(int channels, int codec) {
    const double bits_per_sample = (codec == 0) ? 32.0 : (codec == 1 ? 24.0 : (codec == 2 ? 16.0 : 4.2));
    return double(std::clamp(channels, 1, 64)) * 48000.0 * bits_per_sample / 1e6;
}

void noop_bus_callback(const LV_MultichannelAudioBlock*, void*) {}  // drives the playout clock + meters

void push_hist(float* arr, int len, float val) {
    for (int i = 0; i < len - 1; ++i) arr[i] = arr[i + 1];
    arr[len - 1] = val;
}

// Modern Dark Studio Palette
constexpr ImVec4 kColBgMain    = ImVec4(0.051f, 0.067f, 0.090f, 1.00f);  // #0D1117
constexpr ImVec4 kColSidebar   = ImVec4(0.067f, 0.086f, 0.122f, 1.00f);  // #11161F
constexpr ImVec4 kColCard      = ImVec4(0.086f, 0.106f, 0.149f, 1.00f);  // #161B26
constexpr ImVec4 kColBorder    = ImVec4(0.145f, 0.176f, 0.239f, 1.00f);  // #252D3D
constexpr ImVec4 kColCyan      = ImVec4(0.000f, 0.820f, 1.000f, 1.00f);  // #00D1FF
constexpr ImVec4 kColTeal      = ImVec4(0.000f, 0.898f, 0.765f, 1.00f);  // #00E5C3
constexpr ImVec4 kColGreen     = ImVec4(0.063f, 0.725f, 0.506f, 1.00f);  // #10B981
constexpr ImVec4 kColAmber     = ImVec4(0.961f, 0.620f, 0.043f, 1.00f);  // #F59E0B
constexpr ImVec4 kColRed       = ImVec4(0.937f, 0.267f, 0.267f, 1.00f);  // #EF4444
constexpr ImVec4 kColTextMain  = ImVec4(0.953f, 0.957f, 0.965f, 1.00f);  // #F3F4F6
constexpr ImVec4 kColTextMuted = ImVec4(0.545f, 0.580f, 0.620f, 1.00f);  // #8B949E

bool studio_toggle(const char* str_id, bool* v, const ImVec4& active_col = kColGreen) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 20.0f;
    const float w = 38.0f;
    const float r = h * 0.5f;
    const bool clicked = ImGui::InvisibleButton(str_id, ImVec2(w, h));
    if (clicked) *v = !*v;
    const ImU32 bg = *v ? ImGui::ColorConvertFloat4ToU32(active_col) : IM_COL32(37, 45, 61, 255);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(55, 68, 92, 180), r, 0, 1.0f);
    const float knob_x = *v ? (p.x + w - r) : (p.x + r);
    dl->AddCircleFilled(ImVec2(knob_x, p.y + r), r - 2.5f, IM_COL32(243, 244, 246, 255));
    return clicked;
}

}  // namespace

void LanVidStudioApp::apply_theme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.ChildRounding = 8.0f;
    st.FrameRounding = 6.0f;
    st.PopupRounding = 8.0f;
    st.GrabRounding = 6.0f;
    st.TabRounding = 6.0f;
    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.PopupBorderSize = 1.0f;
    st.WindowPadding = ImVec2(12.0f, 10.0f);
    st.FramePadding = ImVec2(10.0f, 5.0f);
    st.ItemSpacing = ImVec2(8.0f, 7.0f);
    st.ScrollbarSize = 10.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]         = kColBgMain;
    c[ImGuiCol_ChildBg]          = kColCard;
    c[ImGuiCol_PopupBg]          = ImVec4(0.086f, 0.106f, 0.149f, 0.98f);
    c[ImGuiCol_Border]           = kColBorder;
    c[ImGuiCol_BorderShadow]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]          = kColSidebar;
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.110f, 0.140f, 0.200f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.130f, 0.175f, 0.250f, 1.00f);
    c[ImGuiCol_TitleBg]          = kColSidebar;
    c[ImGuiCol_TitleBgActive]    = kColCard;
    c[ImGuiCol_CheckMark]        = kColCyan;
    c[ImGuiCol_SliderGrab]       = kColCyan;
    c[ImGuiCol_SliderGrabActive] = kColTeal;
    c[ImGuiCol_Button]           = ImVec4(0.118f, 0.153f, 0.220f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.000f, 0.650f, 0.820f, 0.35f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.000f, 0.820f, 1.000f, 0.50f);
    c[ImGuiCol_Header]           = ImVec4(0.000f, 0.820f, 1.000f, 0.20f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.000f, 0.820f, 1.000f, 0.32f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.000f, 0.820f, 1.000f, 0.45f);
    c[ImGuiCol_Tab]              = kColSidebar;
    c[ImGuiCol_TabHovered]       = ImVec4(0.000f, 0.820f, 1.000f, 0.28f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.000f, 0.820f, 1.000f, 0.22f);
    c[ImGuiCol_PlotLines]        = kColCyan;
    c[ImGuiCol_PlotLinesHovered] = kColTeal;
    c[ImGuiCol_PlotHistogram]    = kColGreen;
    c[ImGuiCol_Separator]        = kColBorder;
    c[ImGuiCol_Text]             = kColTextMain;
    c[ImGuiCol_TextDisabled]     = kColTextMuted;
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
    wire_network_midi();
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

// MIDI2 over IP <-> Audio & MIDI Matrix:
//   matrix "Network MIDI TX" slot -> duplex node (if running) or sender -> peer
//   peer -> receiver / duplex -> matrix "Network MIDI RX" slot -> crosspoints -> CoreMIDI / WinMM outputs
//   (e.g. "LanVid Virtual MIDI Out" into the DAW)
void LanVidStudioApp::wire_network_midi() {
    audio_matrix_.set_midi_sink([this](int dst_slot, uint8_t st, uint8_t d1, uint8_t d2, uint8_t port, uint64_t) {
        bool to_net = false;
        for (const auto& s : audio_matrix_.midi_slots())
            if (s.id == dst_slot && s.type == MidiSlotType::kNetworkMidiTx) to_net = true;
        if (!to_net) return;  // OS MIDI outputs are driven by the matrix itself
        if (duplex_running_ && duplex_) lv_duplex_send_midi(duplex_, st, d1, d2, port, 0);
        else if (sender_running_ && sender_) lv_sender_push_midi(sender_, st, d1, d2, port, 0);
    });
    if (receiver_) lv_receiver_set_midi_callback(receiver_, &LanVidStudioApp::on_network_midi, this);
    if (duplex_) lv_duplex_set_midi_callback(duplex_, &LanVidStudioApp::on_network_midi, this);
}

void LanVidStudioApp::on_network_midi(const LV_MidiEvent* ev, void* user_data) {
    auto* self = static_cast<LanVidStudioApp*>(user_data);
    if (!self || !ev) return;
    for (const auto& s : self->audio_matrix_.midi_slots()) {
        if (s.type == MidiSlotType::kNetworkMidiRx && s.active) {
            self->audio_matrix_.inject_midi_event(s.id, ev->status, ev->data1, ev->data2, ev->media_ts_us);
            return;
        }
    }
}

void LanVidStudioApp::refresh_devices() {
    monitors_ = list_monitors();
    cameras_ = list_cameras();
    loop_devs_ = list_audio_devices(true);
    mic_devs_ = list_audio_devices(false);
    asio_devs_ = list_external_asio_drivers(true);
    midi_in_devs_ = list_midi_inputs();
    midi_out_devs_ = list_midi_outputs();
    sys_out_devs_ = list_system_audio_devices(true);   // macOS CoreAudio (empty on Windows/Linux)
    sys_in_devs_ = list_system_audio_devices(false);
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
    sc.audio_codec = std::clamp(cfg_.audio_codec, 0, 3);

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
    dc.audio_codec = std::clamp(cfg_.audio_codec, 0, 3);
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
    if (ImGui::Combo("Audio Compression", &cfg_.audio_codec, kAudioCodecItems, 4)) {
        if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        config_dirty_ = true;
    }
    ImGui::TextDisabled("Est. AoIP TX: %.1f Mbps (%d ch @ 48 kHz)",
                        estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec), cfg_.aoip_channels);
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
#if defined(__APPLE__)
        hp.name = "Mac Headphones / Output (CoreAudio)";
#else
        hp.name = "PC Headphones / Output";
#endif
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

    // Restore or initialize MIDI slots (Network MIDI TX/RX + saved OS MIDI devices)
    audio_matrix_.clear_midi_slots();
    if (cfg_.midi_slots.empty()) {
        audio_matrix_.add_midi_slot({0, MidiSlotType::kNetworkMidiTx, "Network MIDI TX (MIDI2)", "", -1, 0, true});
        audio_matrix_.add_midi_slot({1, MidiSlotType::kNetworkMidiRx, "Network MIDI RX (MIDI2)", "", -1, 0, true});
    } else {
        for (const auto& ms : cfg_.midi_slots) audio_matrix_.add_midi_slot(ms);
        for (const auto& mc : cfg_.midi_crosspoints) {
            audio_matrix_.set_midi_crosspoint(mc.src_slot, mc.dst_slot, mc.channel_filter, mc.mute);
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
    cfg_.midi_slots = audio_matrix_.midi_slots();
    cfg_.midi_crosspoints = audio_matrix_.midi_crosspoints();
}

void LanVidStudioApp::draw_segmented_vu(float dbfs, float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    constexpr int kSegs = 20;
    const float gap = 2.0f;
    const float seg_w = std::max(2.0f, (width - gap * float(kSegs - 1)) / float(kSegs));
    const float norm = std::clamp((dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
    const int lit = int(std::round(norm * float(kSegs)));
    for (int i = 0; i < kSegs; ++i) {
        const float x0 = p0.x + float(i) * (seg_w + gap);
        const float seg_db = -60.0f + (float(i + 1) / float(kSegs)) * 60.0f;
        ImU32 col = IM_COL32(28, 35, 48, 255);
        if (i < lit) {
            if (seg_db > -1.0f) col = ImGui::ColorConvertFloat4ToU32(kColRed);
            else if (seg_db > -12.0f) col = ImGui::ColorConvertFloat4ToU32(kColAmber);
            else col = ImGui::ColorConvertFloat4ToU32(kColGreen);
        }
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x0 + seg_w, p0.y + height), col, 1.5f);
    }
    ImGui::Dummy(ImVec2(width, height));
}

void LanVidStudioApp::draw_sidebar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColSidebar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 12.0f));
    // Width follows the longest "ICON LABEL" entry so nothing is clipped with any font / DPI.
    float sidebar_w = 138.0f;
    {
        const char* longest[][2] = {{"TX", "SENDER"}, {"RX", "RECEIVER"}, {"DX", "DUPLEX"}, {"MX", "AUDIO MATRIX"}};
        for (const auto& it : longest) {
            const float need = 10.0f + ImGui::CalcTextSize(it[0]).x + 8.0f + ImGui::CalcTextSize(it[1]).x + 14.0f + 20.0f;
            sidebar_w = std::max(sidebar_w, need);
        }
    }
    ImGui::BeginChild("##Sidebar", ImVec2(sidebar_w, 0), true, ImGuiWindowFlags_NoScrollbar);

#ifndef LANVID_VERSION_STRING
#define LANVID_VERSION_STRING "2.4.0"
#endif
    ImGui::TextColored(kColCyan, "LanVid");
    ImGui::SameLine();
    ImGui::TextColored(kColTextMain, "Studio");
    ImGui::TextColored(kColTextMuted, "v%s", LANVID_VERSION_STRING);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    struct NavItem {
        int id;
        const char* icon;
        const char* label;
    };
    const NavItem nav[] = {
        {0, "TX", "SENDER"},
        {1, "RX", "RECEIVER"},
        {2, "DX", "DUPLEX"},
        {3, "MX", "AUDIO & MIDI"},
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& item : nav) {
        const bool selected = (cfg_.active_tab == item.id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = 40.0f;
        if (ImGui::InvisibleButton(item.label, ImVec2(w, h))) {
            if (cfg_.active_tab != item.id) {
                cfg_.active_tab = item.id;
                config_dirty_ = true;
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        if (selected || hovered) {
            const ImU32 bg = selected ? ImGui::ColorConvertFloat4ToU32(kColCard)
                                      : IM_COL32(22, 27, 38, 150);
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, 6.0f);
            if (selected) {
                dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), ImGui::ColorConvertFloat4ToU32(kColBorder), 6.0f);
                dl->AddRectFilled(ImVec2(pos.x, pos.y + 6.0f), ImVec2(pos.x + 3.0f, pos.y + h - 6.0f),
                                  ImGui::ColorConvertFloat4ToU32(kColCyan), 2.0f);
            }
        }
        const ImU32 icon_col = selected ? ImGui::ColorConvertFloat4ToU32(kColCyan)
                                        : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
        const ImU32 txt_col = selected ? ImGui::ColorConvertFloat4ToU32(kColTextMain)
                                       : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
        dl->AddText(ImVec2(pos.x + 10.0f, pos.y + 12.0f), icon_col, item.icon);
        const float label_x = pos.x + 10.0f + ImGui::CalcTextSize(item.icon).x + 8.0f;
        dl->AddText(ImVec2(label_x, pos.y + 12.0f), txt_col, item.label);
    }

    // Bottom connection & latency indicator pinned to bottom of sidebar
    const float rem = ImGui::GetContentRegionAvail().y;
    if (rem > 64.0f) ImGui::Dummy(ImVec2(1.0f, rem - 60.0f));
    ImGui::Separator();
    ImGui::Spacing();
    const bool any_active = sender_running_ || receiver_running_ || duplex_running_ || audio_matrix_.is_running();
    ImGui::TextColored(any_active ? kColGreen : kColTextMuted, "%s", any_active ? "● Connected" : "● Standby");
    ImGui::TextColored(kColTextMuted, "LATENCY: %d ms", cfg_.aoip_latency_ms);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void LanVidStudioApp::draw_studio_config_panel() {
    ImGui::BeginChild("##StudioConfig", ImVec2(0, 0), true);
    ImGui::TextColored(kColCyan, "STUDIO CONFIG");
    ImGui::Separator();

    // ---- NETWORK ----
    ImGui::TextColored(kColTextMuted, "NETWORK");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##CfgIP", ip_buf_, sizeof(ip_buf_))) config_dirty_ = true;
    ImGui::TextDisabled("IP Address");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##CfgPort", &cfg_.dest_port, 0, 0)) {
        cfg_.dest_port = std::clamp(cfg_.dest_port, 1024, 65535);
        cfg_.listen_port = cfg_.dest_port;
        config_dirty_ = true;
    }
    ImGui::TextDisabled("UDP Port (TX / RX)");

    const bool running = audio_matrix_.is_running();
    if (!running) {
        ImGui::PushStyleColor(ImGuiCol_Button, kColGreen);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.82f, 0.58f, 1.0f));
        if (ImGui::Button("Start Audio Matrix", ImVec2(-1, 34))) {
            stop_vdev_tx();
            vdev_rx_.stop();
            audio_matrix_.start();
            toast_msg_ = "Audio Matrix active (48 kHz, buf " + std::to_string(audio_matrix_.buffer_size()) + ")";
            toast_until_us_ = now_us() + 3000000ull;
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, kColRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.98f, 0.36f, 0.36f, 1.0f));
        if (ImGui::Button("Stop Audio Matrix", ImVec2(-1, 34))) {
            audio_matrix_.stop();
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- AUDIO BUS ----
    ImGui::TextColored(kColTextMuted, "AUDIO BUS");
    int bus_idx = 4;
    for (int i = 0; i < 5; ++i) if (kBusChoices[i] == cfg_.aoip_channels) bus_idx = i;
    const char* bus_items[] = {"2 Channels", "8 Channels", "16 Channels", "32 Channels", "64 Channels"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##CfgBusCh", &bus_idx, bus_items, 5)) {
        cfg_.aoip_channels = kBusChoices[bus_idx];
        config_dirty_ = true;
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##CfgLat", &cfg_.aoip_latency_ms, 2, 2000, "LATENCY: %d ms", ImGuiSliderFlags_Logarithmic)) {
        if (sender_) lv_sender_set_audio_latency_ms(sender_, uint32_t(cfg_.aoip_latency_ms));
        if (receiver_) lv_receiver_set_audio_latency_ms(receiver_, uint32_t(cfg_.aoip_latency_ms));
        if (duplex_running_) lv_duplex_set_latency_ms(duplex_, uint32_t(cfg_.aoip_latency_ms));
        config_dirty_ = true;
    }
    if (ImGui::SmallButton("5 ms")) { cfg_.aoip_latency_ms = 5; config_dirty_ = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("10 ms")) { cfg_.aoip_latency_ms = 10; config_dirty_ = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("20 ms")) { cfg_.aoip_latency_ms = 20; config_dirty_ = true; }

    int buf_idx = (cfg_.matrix_buffer_size <= 128) ? 0 : (cfg_.matrix_buffer_size <= 256 ? 1 : 2);
    const char* buf_items[] = {"Buffer: 128 (2.7 ms)", "Buffer: 256 (5.3 ms)", "Buffer: 512 (10.7 ms)"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##CfgBuf", &buf_idx, buf_items, 3)) {
        cfg_.matrix_buffer_size = (buf_idx == 0) ? 128 : (buf_idx == 1 ? 256 : 512);
        audio_matrix_.set_buffer_size(cfg_.matrix_buffer_size);
        config_dirty_ = true;
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##CfgAudioCodec", &cfg_.audio_codec, kAudioCodecItems, 4)) {
        if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        config_dirty_ = true;
    }
    ImGui::TextDisabled("Audio Compression · Est: %.1f Mbps (%d ch)",
                        estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec), cfg_.aoip_channels);

    if (ImGui::Checkbox("Feedback Loop Guard", &cfg_.matrix_feedback_guard)) {
        audio_matrix_.set_feedback_guard(cfg_.matrix_feedback_guard);
        config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- STREAM MODE ----
    ImGui::TextColored(kColTextMuted, "STREAM MODE");
    if (studio_toggle("##AudioOnlyToggle", &cfg_.audio_only, kColCyan)) {
        config_dirty_ = true;
        if (sender_running_) start_sender();
        if (receiver_running_) start_receiver();
    }
    ImGui::SameLine();
    ImGui::TextColored(cfg_.audio_only ? kColCyan : kColTextMain, "AUDIO ONLY");
    ImGui::TextDisabled(cfg_.audio_only ? "Video disabled (~0 Mbps video, pure AoIP)"
                                        : "Video + Audio streaming enabled");

    ImGui::Spacing();
    ImGui::Separator();

    // ---- LIVE TELEMETRY ----
    ImGui::TextColored(kColTextMuted, "LIVE TELEMETRY");
    const double total_mbps = tx_stats_.tx_mbps + rx_stats_.rx_mbps +
                              (tx_stats_.audio_kbps + rx_stats_.audio_kbps) / 1000.0;
    const double loss_pct = receiver_running_ ? rx_stats_.loss_pct : tx_stats_.rx_loss_pct;
    uint64_t total_xruns = 0;
    for (const auto& s : audio_matrix_.slots()) total_xruns += audio_matrix_.slot_status(s.id).xruns;

    ImGui::Columns(2, "##TelGrid", false);
    ImGui::TextDisabled("TRAFFIC");
    ImGui::TextColored(kColCyan, "%.2f Mbps", total_mbps);
    ImGui::NextColumn();
    ImGui::TextDisabled("PACKET LOSS");
    ImGui::TextColored(loss_pct > 0.1 ? kColAmber : kColGreen, "%.2f%%", loss_pct);
    ImGui::NextColumn();
    ImGui::TextDisabled("UNDERRUNS");
    ImGui::TextColored(total_xruns > 0 ? kColAmber : kColGreen, "%llu", (unsigned long long)total_xruns);
    ImGui::NextColumn();
    ImGui::TextDisabled("CROSSPOINTS");
    ImGui::TextColored(kColTextMain, "%zu active", audio_matrix_.crosspoints().size());
    ImGui::Columns(1);

    ImGui::EndChild();
}

void LanVidStudioApp::draw_audio_matrix_tab() {
    auto slots = audio_matrix_.slots();
    auto find_slot_by_type = [&](MatrixSlotType t, MatrixSlotRole role_hint = MatrixSlotRole::kDuplex,
                                 bool check_role = false) -> int {
        for (const auto& s : slots) {
            if (s.type == t && (!check_role || s.role == role_hint)) return s.id;
        }
        return -1;
    };

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float col1_w = std::max(270.0f, total_w * 0.30f);
    const float col2_w = std::max(360.0f, total_w * 0.45f);

    // ==================== COLUMN 1: DEVICES & SLOTS (~30%) ====================
    ImGui::BeginChild("##MatrixDevices", ImVec2(col1_w, 0), true);
    ImGui::TextColored(kColCyan, "DEVICES & SLOTS");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu / %d)", slots.size(), kMatrixMaxSlots);
    ImGui::Separator();

    if (ImGui::Button("+ Audio", ImVec2(col1_w * 0.34f, 28.0f))) ImGui::OpenPopup("##AddAudioDev");
    ImGui::SameLine();
    if (ImGui::Button("+ Add MIDI Device", ImVec2(col1_w * 0.42f, 28.0f))) ImGui::OpenPopup("##AddMidiDev");
    ImGui::SameLine();
    if (ImGui::Button("Ref", ImVec2(-1, 28.0f))) refresh_devices();

    if (ImGui::BeginPopup("##AddMidiDev")) {
        ImGui::TextColored(kColCyan, "MIDI Inputs (Keyboards / E-Drums / Controllers)");
        if (midi_in_devs_.empty()) ImGui::TextDisabled("  No OS MIDI inputs found");
        for (const auto& m : midi_in_devs_) {
            if (ImGui::Selectable((m.name + " [MIDI IN]").c_str())) {
                audio_matrix_.add_midi_slot({-1, MidiSlotType::kMidiInput, m.name, m.name, m.index, 0, true});
                config_dirty_ = true;
            }
        }
        ImGui::Separator();
        ImGui::TextColored(kColTeal, "MIDI Outputs (DAW / Synths)");
        if (midi_out_devs_.empty()) ImGui::TextDisabled("  No OS MIDI outputs found");
        for (const auto& m : midi_out_devs_) {
            if (ImGui::Selectable((m.name + " [MIDI OUT]").c_str())) {
                audio_matrix_.add_midi_slot({-1, MidiSlotType::kMidiOutput, m.name, m.name, m.index, 0, true});
                config_dirty_ = true;
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##AddAudioDev")) {
#if defined(__APPLE__)
        auto add_sys_slot = [this](const SystemAudioDeviceEntry* d, MatrixSlotRole role) {
            MatrixSlotConfig s{};
            s.id = -1;
            s.type = MatrixSlotType::kSystemAudio;
            s.role = role;
            s.name = d ? d->name : (role == MatrixSlotRole::kOutput ? "Default Output (CoreAudio)" : "Default Input (CoreAudio)");
            s.device_id = d ? d->uid : "";
            s.device_index = -1;
            s.channels = d ? std::clamp(d->channels, 1, 64) : 2;
            s.buffer_size = cfg_.matrix_buffer_size;
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        };
        ImGui::TextColored(kColCyan, "CoreAudio Outputs (Speakers / Headphones / USB)");
        if (ImGui::Selectable("Default Output (CoreAudio)")) add_sys_slot(nullptr, MatrixSlotRole::kOutput);
        for (const auto& d : sys_out_devs_) {
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch, %u Hz)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [default]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kOutput);
        }
        ImGui::Separator();
        ImGui::TextColored(kColTeal, "CoreAudio Inputs (Mic / Line-In / USB)");
        if (ImGui::Selectable("Default Input (CoreAudio)")) add_sys_slot(nullptr, MatrixSlotRole::kInput);
        for (const auto& d : sys_in_devs_) {
            char lbl[320];
            std::snprintf(lbl, sizeof(lbl), "%s (%d ch, %u Hz)%s", d.name.c_str(), d.channels, d.sample_rate,
                          d.is_default ? " [default]" : "");
            if (ImGui::Selectable(lbl)) add_sys_slot(&d, MatrixSlotRole::kInput);
        }
#else
        ImGui::TextColored(kColCyan, "WASAPI Outputs (Headphones / Interfaces)");
        if (ImGui::Selectable("Default PC Output (WASAPI)")) {
            MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                               "PC Headphones / Output", "", -1, 2, cfg_.matrix_buffer_size, true};
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : loop_devs_) {
            if (ImGui::Selectable((d.name + " [OUT]").c_str())) {
                MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput,
                                   d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true};
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        ImGui::Separator();
        ImGui::TextColored(kColTeal, "WASAPI Inputs (Mic / Line-In)");
        if (ImGui::Selectable("Default Mic / Line-In (WASAPI)")) {
            MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                               "System Mic / Line-In", "", -1, 2, cfg_.matrix_buffer_size, true};
            audio_matrix_.add_slot(s);
            config_dirty_ = true;
        }
        for (const auto& d : mic_devs_) {
            if (ImGui::Selectable((d.name + " [IN]").c_str())) {
                MatrixSlotConfig s{-1, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput,
                                   d.name, d.name, d.index, 2, cfg_.matrix_buffer_size, true};
                audio_matrix_.add_slot(s);
                config_dirty_ = true;
            }
        }
        if (!asio_devs_.empty()) {
            ImGui::Separator();
            ImGui::TextColored(kColAmber, "External ASIO Drivers (Duplex)");
            for (const auto& a : asio_devs_) {
                if (ImGui::Selectable((a.name + " [ASIO]").c_str())) {
                    MatrixSlotConfig s{-1, MatrixSlotType::kExternalAsio, MatrixSlotRole::kDuplex,
                                       a.name, a.name, -1, 16, cfg_.matrix_buffer_size, true};
                    audio_matrix_.add_slot(s);
                    config_dirty_ = true;
                }
            }
        }
#endif
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    slots = audio_matrix_.slots();
    for (auto s : slots) {
        ImGui::PushID(s.id);
        const auto st = audio_matrix_.slot_status(s.id);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kColSidebar);
        ImGui::BeginChild("##SlotCard", ImVec2(0, 86.0f), true, ImGuiWindowFlags_NoScrollbar);

        const char* badge = (s.type == MatrixSlotType::kVirtualBus)
                                ? "ASIO SHM"
                                : (s.type == MatrixSlotType::kNetworkAoip
                                       ? "AoIP"
                                       : (s.type == MatrixSlotType::kExternalAsio ? "EXT ASIO" : "WASAPI"));
        ImGui::TextColored(kColTextMain, "%s", s.name.c_str());
        ImGui::SameLine(std::max(140.0f, ImGui::GetContentRegionAvail().x - 56.0f));
        bool act = s.active;
        if (studio_toggle("##SlotAct", &act, kColGreen)) {
            s.active = act;
            audio_matrix_.update_slot(s.id, s);
            config_dirty_ = true;
        }

        ImGui::TextColored(st.connected ? kColGreen : kColTextMuted, "%s",
                           s.active ? (st.connected ? "ACTIVE" : "READY") : "OFF");
        ImGui::SameLine();
        ImGui::TextDisabled("| %s | %d ch | %u Hz", badge, s.channels, st.device_sample_rate);
        if (s.type != MatrixSlotType::kVirtualBus && s.type != MatrixSlotType::kNetworkAoip) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                audio_matrix_.remove_slot(s.id);
                config_dirty_ = true;
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopID();
                break;
            }
        }

        const float vu_w = ImGui::GetContentRegionAvail().x;
        const float vu_db = (s.role == MatrixSlotRole::kOutput) ? st.peak_out_dbfs
                                                                : std::max(st.peak_in_dbfs, st.peak_out_dbfs);
        draw_segmented_vu(vu_db, vu_w, 8.0f);

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    // ---- MIDI DEVICES SECTION IN COLUMN 1 ----
    const auto mslots = audio_matrix_.midi_slots();
    if (!mslots.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kColTeal, "MIDI DEVICES (%zu / %d)", mslots.size(), kMatrixMaxMidiSlots);
        ImGui::SameLine(std::max(150.0f, col1_w - 88.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, kColRed);
        if (ImGui::SmallButton("PANIC ALL")) {
            audio_matrix_.send_midi_panic(-1);
            toast_msg_ = "MIDI PANIC: CC 123 (All Notes Off) sent to all 16 channels";
            toast_until_us_ = now_us() + 2500000ull;
        }
        ImGui::PopStyleColor();
        const uint64_t now_t = now_us();
        for (auto ms : mslots) {
            ImGui::PushID(2000 + ms.id);
            const auto mst = audio_matrix_.midi_slot_status(ms.id);
            const bool blinking = (mst.last_activity_us > 0 && now_t - mst.last_activity_us < 150000ull);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kColSidebar);
            ImGui::BeginChild("##MidiCard", ImVec2(0, 62.0f), true, ImGuiWindowFlags_NoScrollbar);

            // Blinking MIDI ACTIVITY LED
            ImGui::TextColored(blinking ? kColTeal : kColTextMuted, "●");
            ImGui::SameLine();
            ImGui::TextColored(kColTextMain, "%s", ms.name.c_str());
            ImGui::SameLine(std::max(140.0f, ImGui::GetContentRegionAvail().x - 56.0f));
            bool act = ms.active;
            if (studio_toggle("##MidiAct", &act, kColTeal)) {
                ms.active = act;
                audio_matrix_.update_midi_slot(ms.id, ms);
                config_dirty_ = true;
            }

            const char* mtag = (ms.type == MidiSlotType::kMidiInput)
                                   ? "MIDI IN"
                                   : (ms.type == MidiSlotType::kMidiOutput
                                          ? "MIDI OUT"
                                          : (ms.type == MidiSlotType::kNetworkMidiTx ? "NET TX" : "NET RX"));
            ImGui::TextColored(blinking ? kColCyan : kColTextMuted, "MIDI ACTIVITY: %llu ev | %s",
                               (unsigned long long)mst.events_count, mtag);
            if (ms.type == MidiSlotType::kMidiOutput || ms.type == MidiSlotType::kNetworkMidiTx) {
                ImGui::SameLine();
                if (ImGui::SmallButton("PANIC")) audio_matrix_.send_midi_panic(ms.id);
            }
            if (ms.type == MidiSlotType::kMidiInput || ms.type == MidiSlotType::kMidiOutput) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    audio_matrix_.remove_midi_slot(ms.id);
                    config_dirty_ = true;
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // ==================== COLUMN 2: PATCHBAY MATRIX GRID (~45%) ====================
    ImGui::SameLine();
    ImGui::BeginChild("##MatrixGrid", ImVec2(col2_w, 0), true);
    ImGui::TextColored(kColCyan, matrix_patch_mode_ == 0 ? "AUDIO PATCHBAY GRID" : "MIDI PATCHBAY GRID");
    ImGui::SameLine(std::max(180.0f, col2_w - 205.0f));
    auto mode_pill = [&](int mode, const char* lbl) {
        const bool cur = (matrix_patch_mode_ == mode);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, cur ? kColCyan : ImVec4(0.118f, 0.153f, 0.220f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, cur ? ImVec4(0.05f, 0.07f, 0.10f, 1.0f) : kColTextMain);
        if (ImGui::SmallButton(lbl)) matrix_patch_mode_ = mode;
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    };
    mode_pill(0, "Audio Patch");
    ImGui::SameLine();
    mode_pill(1, "MIDI Patch");
    ImGui::Separator();

    if (matrix_patch_mode_ == 1) {
        // ---- MIDI ROUTING MATRIX (many-to-1 merge & 1-to-many split with channel filter) ----
        const char* ch_filters[17] = {"All Ch", "Ch 1",  "Ch 2",  "Ch 3",  "Ch 4",  "Ch 5",  "Ch 6",  "Ch 7", "Ch 8",
                                      "Ch 9",   "Ch 10", "Ch 11", "Ch 12", "Ch 13", "Ch 14", "Ch 15", "Ch 16"};
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Channel Filter", &midi_channel_filter_, ch_filters, 17);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear MIDI Routes")) {
            audio_matrix_.clear_midi_crosspoints();
            config_dirty_ = true;
        }
        ImGui::Spacing();

        std::vector<MidiSlotConfig> srcs, dsts;
        for (const auto& s : audio_matrix_.midi_slots()) {
            if (s.type == MidiSlotType::kMidiInput || s.type == MidiSlotType::kNetworkMidiRx) srcs.push_back(s);
            if (s.type == MidiSlotType::kMidiOutput || s.type == MidiSlotType::kNetworkMidiTx) dsts.push_back(s);
        }
        if (srcs.empty() || dsts.empty()) {
            ImGui::TextDisabled("Add MIDI Input / Output devices on the left to patch MIDI routes.");
        } else if (ImGui::BeginTable("##MidiPatchTbl", int(dsts.size()) + 1,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("MIDI Source \\ Dest");
            for (const auto& d : dsts) ImGui::TableSetupColumn(d.name.c_str());
            ImGui::TableHeadersRow();
            for (const auto& s : srcs) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", s.name.c_str());
                for (size_t j = 0; j < dsts.size(); ++j) {
                    ImGui::TableSetColumnIndex(int(j) + 1);
                    MidiCrosspoint mcp{};
                    const bool linked = audio_matrix_.has_midi_crosspoint(s.id, dsts[j].id, &mcp);
                    ImGui::PushID(s.id * 32 + dsts[j].id);
                    const ImVec4 bcol = linked ? (mcp.mute ? kColRed : kColTeal) : ImVec4(0.16f, 0.19f, 0.25f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, bcol);
                    char blbl[16];
                    if (!linked) std::snprintf(blbl, sizeof(blbl), "·");
                    else if (mcp.mute) std::snprintf(blbl, sizeof(blbl), "M");
                    else if (mcp.channel_filter == 0) std::snprintf(blbl, sizeof(blbl), "ALL");
                    else std::snprintf(blbl, sizeof(blbl), "C%d", mcp.channel_filter);
                    if (ImGui::SmallButton(blbl)) {
                        if (linked) audio_matrix_.remove_midi_crosspoint(s.id, dsts[j].id);
                        else audio_matrix_.set_midi_crosspoint(s.id, dsts[j].id, midi_channel_filter_, false);
                        config_dirty_ = true;
                    }
                    if (linked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        audio_matrix_.set_midi_crosspoint(s.id, dsts[j].id, mcp.channel_filter, !mcp.mute);
                        config_dirty_ = true;
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Many-to-1 Merge & 1-to-Many Split | LMB: Patch/Unpatch | RMB: Mute");
        ImGui::EndChild();
        ImGui::SameLine();
        draw_studio_config_panel();
        return;
    }

    const int net_id = find_slot_by_type(MatrixSlotType::kNetworkAoip);
    const int daw_id = find_slot_by_type(MatrixSlotType::kVirtualBus);
    const int mon_id = find_slot_by_type(MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, true);

    auto preset_pill = [&](int idx, const char* label) {
        const bool is_cur = (matrix_active_preset_ == idx);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, is_cur ? kColCyan : ImVec4(0.118f, 0.153f, 0.220f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, is_cur ? ImVec4(0.05f, 0.07f, 0.10f, 1.0f) : kColTextMain);
        if (ImGui::Button(label)) {
            matrix_active_preset_ = idx;
            audio_matrix_.apply_preset(idx, net_id, daw_id, mon_id);
            config_dirty_ = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    };
    preset_pill(0, "Mac/Net -> DAW + HP");
    ImGui::SameLine();
    preset_pill(1, "Full-Duplex 64x64");
    ImGui::SameLine();
    preset_pill(2, "Direct Patch 1:1");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        matrix_active_preset_ = -1;
        audio_matrix_.clear_crosspoints();
        config_dirty_ = true;
    }

    ImGui::Spacing();
    slots = audio_matrix_.slots();
    MatrixSlotConfig src_cfg{}, dst_cfg{};
    bool has_src = false, has_dst = false;
    for (const auto& s : slots) {
        if (s.id == matrix_src_slot_) { src_cfg = s; has_src = true; }
        if (s.id == matrix_dst_slot_) { dst_cfg = s; has_dst = true; }
    }
    if (!has_src && !slots.empty()) { src_cfg = slots.front(); matrix_src_slot_ = src_cfg.id; has_src = true; }
    if (!has_dst && !slots.empty()) { dst_cfg = slots.back(); matrix_dst_slot_ = dst_cfg.id; has_dst = true; }

    ImGui::SetNextItemWidth(165.0f);
    if (ImGui::BeginCombo("##MxSrc", has_src ? src_cfg.name.c_str() : "Source")) {
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
    ImGui::TextColored(kColCyan, "->");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(165.0f);
    if (ImGui::BeginCombo("##MxDst", has_dst ? dst_cfg.name.c_str() : "Destination")) {
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
    ImGui::SetNextItemWidth(86.0f);
    ImGui::SliderFloat("##MxGain", &matrix_new_gain_db_, -24.0f, 6.0f, "%.0fdB");

    const char* page_lbl[] = {"1-16", "17-32", "33-48", "49-64"};
    if (has_src && src_cfg.channels > 16) {
        ImGui::SetNextItemWidth(76.0f);
        ImGui::Combo("Src##SrcPg", &matrix_src_page_, page_lbl, std::min(4, (src_cfg.channels + 15) / 16));
        ImGui::SameLine();
    } else {
        matrix_src_page_ = 0;
    }
    if (has_dst && dst_cfg.channels > 16) {
        ImGui::SetNextItemWidth(76.0f);
        ImGui::Combo("Dst##DstPg", &matrix_dst_page_, page_lbl, std::min(4, (dst_cfg.channels + 15) / 16));
    } else {
        matrix_dst_page_ = 0;
    }

    // Custom interactive glowing node grid with row/column hover highlight
    if (has_src && has_dst) {
        const int r0 = matrix_src_page_ * 16;
        const int r1 = std::min(src_cfg.channels, r0 + 16);
        const int c0 = matrix_dst_page_ * 16;
        const int c1 = std::min(dst_cfg.channels, c0 + 16);
        const int rows = r1 - r0;
        const int cols = c1 - c0;

        if (rows > 0 && cols > 0) {
            const float label_w = 46.0f;
            const float header_h = 24.0f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float cell = std::clamp(std::min((avail.x - label_w - 8.0f) / float(cols),
                                                   (avail.y - header_h - 26.0f) / float(rows)),
                                          18.0f, 28.0f);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            const ImVec2 mouse = ImGui::GetIO().MousePos;
            int hov_r = -1, hov_c = -1;
            if (ImGui::IsWindowHovered()) {
                const float mx = mouse.x - (origin.x + label_w);
                const float my = mouse.y - (origin.y + header_h);
                if (mx >= 0 && my >= 0 && mx < float(cols) * cell && my < float(rows) * cell) {
                    hov_c = int(mx / cell);
                    hov_r = int(my / cell);
                }
            }

            // Highlight hovered row & column
            if (hov_r >= 0) {
                dl->AddRectFilled(ImVec2(origin.x, origin.y + header_h + float(hov_r) * cell),
                                  ImVec2(origin.x + label_w + float(cols) * cell,
                                         origin.y + header_h + float(hov_r + 1) * cell),
                                  IM_COL32(0, 209, 255, 22), 3.0f);
            }
            if (hov_c >= 0) {
                dl->AddRectFilled(ImVec2(origin.x + label_w + float(hov_c) * cell, origin.y),
                                  ImVec2(origin.x + label_w + float(hov_c + 1) * cell,
                                         origin.y + header_h + float(rows) * cell),
                                  IM_COL32(0, 209, 255, 22), 3.0f);
            }

            // Column headers (Destinations)
            for (int j = 0; j < cols; ++j) {
                char hdr[8];
                std::snprintf(hdr, sizeof(hdr), "%d", c0 + j + 1);
                const ImU32 col = (j == hov_c) ? ImGui::ColorConvertFloat4ToU32(kColCyan)
                                               : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
                dl->AddText(ImVec2(origin.x + label_w + float(j) * cell + 5.0f, origin.y + 4.0f), col, hdr);
            }

            // Row labels + clickable glowing nodes
            for (int i = 0; i < rows; ++i) {
                const int sc = r0 + i;
                const float ry = origin.y + header_h + float(i) * cell;
                char rlbl[12];
                std::snprintf(rlbl, sizeof(rlbl), "Ch %02d", sc + 1);
                const ImU32 rcol = (i == hov_r) ? ImGui::ColorConvertFloat4ToU32(kColCyan)
                                                : ImGui::ColorConvertFloat4ToU32(kColTextMuted);
                dl->AddText(ImVec2(origin.x + 2.0f, ry + (cell - 14.0f) * 0.5f), rcol, rlbl);

                for (int j = 0; j < cols; ++j) {
                    const int dc = c0 + j;
                    const float cx = origin.x + label_w + float(j) * cell + cell * 0.5f;
                    const float cy = ry + cell * 0.5f;
                    MatrixCrosspoint cp{};
                    const bool linked = audio_matrix_.has_crosspoint(src_cfg.id, sc, dst_cfg.id, dc, &cp);

                    ImGui::SetCursorScreenPos(ImVec2(origin.x + label_w + float(j) * cell, ry));
                    ImGui::PushID(sc * 100 + dc);
                    if (ImGui::InvisibleButton("##Node", ImVec2(cell, cell))) {
                        matrix_active_preset_ = -1;
                        if (linked) {
                            audio_matrix_.remove_crosspoint(src_cfg.id, sc, dst_cfg.id, dc);
                            config_dirty_ = true;
                        } else {
                            const auto res = audio_matrix_.set_crosspoint(
                                src_cfg.id, sc, dst_cfg.id, dc, matrix_new_gain_db_, false, !cfg_.matrix_feedback_guard);
                            if (res == CrosspointResult::kBlockedFeedbackLoop) {
                                toast_msg_ = "Feedback Loop Guard: self-loop blocked!";
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
                            ImGui::SetTooltip("%s Ch%d -> %s Ch%d\nGain: %.1f dB | %s\n(LMB: Remove, RMB: Mute)",
                                              src_cfg.name.c_str(), sc + 1, dst_cfg.name.c_str(), dc + 1, cp.gain_db,
                                              cp.mute ? "MUTED" : "ACTIVE");
                        } else {
                            ImGui::SetTooltip("Patch %s Ch%d -> %s Ch%d (%.1f dB)",
                                              src_cfg.name.c_str(), sc + 1, dst_cfg.name.c_str(), dc + 1,
                                              matrix_new_gain_db_);
                        }
                    }
                    ImGui::PopID();

                    const float rad = cell * 0.27f;
                    if (linked) {
                        if (cp.mute) {
                            dl->AddCircleFilled(ImVec2(cx, cy), rad + 2.5f, IM_COL32(239, 68, 68, 55));
                            dl->AddCircleFilled(ImVec2(cx, cy), rad, ImGui::ColorConvertFloat4ToU32(kColRed));
                        } else {
                            // Subtle cyan glow halo + bright core
                            dl->AddCircleFilled(ImVec2(cx, cy), rad + 3.5f, IM_COL32(0, 209, 255, 50));
                            dl->AddCircleFilled(ImVec2(cx, cy), rad, ImGui::ColorConvertFloat4ToU32(kColCyan));
                        }
                    } else {
                        const ImU32 dot = (i == hov_r && j == hov_c) ? IM_COL32(0, 209, 255, 160)
                                                                     : IM_COL32(45, 55, 72, 200);
                        dl->AddCircleFilled(ImVec2(cx, cy), rad * 0.65f, dot);
                    }
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header_h + float(rows) * cell + 6.0f));
        }
    }
    ImGui::TextDisabled("LMB: Toggle Node  |  RMB: Mute Node  |  Hover: Crosshair");
    ImGui::EndChild();

    // ==================== COLUMN 3: STUDIO CONFIG (~25%) ====================
    ImGui::SameLine();
    draw_studio_config_panel();
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
    if (ImGui::Combo("Audio Compression", &cfg_.audio_codec, kAudioCodecItems, 4)) {
        if (sender_) lv_sender_set_audio_codec(sender_, cfg_.audio_codec);
        if (duplex_running_) lv_duplex_set_audio_codec(duplex_, cfg_.audio_codec);
        config_dirty_ = true;
    }
    ImGui::TextDisabled("Est. AoIP: %.1f Mbps (%d ch @ 48 kHz)",
                        estimate_aoip_mbps(cfg_.aoip_channels, cfg_.audio_codec), cfg_.aoip_channels);
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColSidebar);
    ImGui::BeginChild("##TopBar", ImVec2(0, 42), true, ImGuiWindowFlags_NoScrollbar);
#ifndef LANVID_VERSION_STRING
#define LANVID_VERSION_STRING "2.4.0"
#endif
    const char* mode_names[] = {"SENDER", "RECEIVER", "FULL-DUPLEX 64x64", "AUDIO MATRIX"};
    const char* cur_mode = mode_names[std::clamp(cfg_.active_tab, 0, 3)];
    ImGui::TextColored(kColCyan, "%s", cur_mode);
    ImGui::SameLine();
    ImGui::TextDisabled("  |  SIMD: %s  |  GPU: %s  |  LAN IP: %s",
                        lv_get_cpu_simd_info(), host_.renderer_name, local_lan_ip_.c_str());

    // Live status pill on right side
    const bool any_live = sender_running_ || receiver_running_ || duplex_running_ || audio_matrix_.is_running();
    const char* badge = audio_matrix_.is_running()
                            ? "● MATRIX ACTIVE"
                            : ((sender_running_ && receiver_running_)
                                   ? "● TX + RX ACTIVE"
                                   : (sender_running_ ? "● TX ACTIVE"
                                                      : (receiver_running_ ? "● RX ACTIVE"
                                                                           : (duplex_running_ ? "● DUPLEX ACTIVE"
                                                                                              : "● STANDBY"))));
    const ImVec4 col = any_live ? kColGreen : kColTextMuted;
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

    draw_sidebar();
    ImGui::SameLine();

    ImGui::BeginChild("##MainWorkspace", ImVec2(0, 0), false);
    draw_top_bar();
    ImGui::Spacing();

    switch (std::clamp(cfg_.active_tab, 0, 3)) {
        case 0: draw_sender_tab(); break;
        case 1: draw_receiver_tab(); break;
        case 2: draw_duplex_tab(); break;
        default: draw_audio_matrix_tab(); break;
    }
    ImGui::EndChild();

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
