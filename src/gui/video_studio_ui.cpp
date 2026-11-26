#include "gui/video_studio_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "common/clock.hpp"
#include "gui/qr_code.hpp"
#include "imgui.h"
#include "net/packet.hpp"
#include "platform/audio_capture.hpp"
#include "platform/audio_playback.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace lv::gui {

namespace fs = std::filesystem;

namespace {
#if defined(_WIN32)
// Virtual camera driver CLSID of "LanVid Cam 1" (drivers/windows_vcam/lanvid_vcam_guids.h).
constexpr const char* kVcamClsid1 = "{6C4A8B10-2E5D-4F7A-9B3C-1D2E3F405161}";
#endif

std::string human_bytes(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", double(b) / double(1ull << 20));
    else std::snprintf(buf, sizeof(buf), "%.0f KB", double(b) / 1024.0);
    return buf;
}

std::string hms(uint64_t us) {
    const uint64_t s = us / 1000000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", unsigned(s / 3600), unsigned((s / 60) % 60), unsigned(s % 60));
    return buf;
}

const char* tally_name(uint8_t t) { return t == lv::kTallyProgram ? "PGM" : t == lv::kTallyPreview ? "PVW" : "STBY"; }

void open_folder(const std::string& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    const std::string cmd = "open \"" + dir + "\" &";
    (void)std::system(cmd.c_str());
#else
    const std::string cmd = "xdg-open \"" + dir + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

std::string url_encode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') o += char(c);
        else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            o += b;
        }
    }
    return o;
}
}  // namespace

// ================================================================================================
// Lifecycle
// ================================================================================================
LanVidVideoStudioApp::LanVidVideoStudioApp(std::string config_path_override)
    : config_path_(config_path_override.empty() ? config_file_path_for("lanvid_video_config.json")
                                                : std::move(config_path_override)) {
    cfg_.listen_port = lv::kVideoStreamPort;
    cfg_.dest_port = lv::kVideoStreamPort;
    cfg_.audio_only = false;
    af_.fill(true);
    quality_.fill(int(lv::kQuality1080p));
    focus_.fill(500);
    tex_slot_.fill(-1);
}

LanVidVideoStudioApp::~LanVidVideoStudioApp() { shutdown(); }

void LanVidVideoStudioApp::init() {
    load_app_config(cfg_, config_path_);
    if (cfg_.listen_port == 5000) cfg_.listen_port = lv::kVideoStreamPort;
    if (cfg_.dest_port == 5000) cfg_.dest_port = lv::kVideoStreamPort;
    std::snprintf(iso_dir_, sizeof(iso_dir_), "%s",
                  (cfg_.iso_output_dir.empty() ? lv::MultiCamManager::default_output_dir() : cfg_.iso_output_dir).c_str());
    std::snprintf(iso_session_, sizeof(iso_session_), "%s", cfg_.iso_session_name.c_str());
    apply_studio_theme(cfg_.dark_theme);
    refresh_nics();
    start_services(uint16_t(cfg_.listen_port), lv::kVideoTallyPort, lv::kVideoDiscoveryPort);
    start_lan_scan();
    vcam_status_ = vcam_driver_status();
}

void LanVidVideoStudioApp::shutdown() {
    stop_intercom_audio();
    stop_services();
    if (scan_thread_.joinable()) scan_thread_.join();
    for (auto& t : tex_) {
        if (t && host_.destroy_texture) host_.destroy_texture(host_.ctx, t);
        t = nullptr;
    }
    persist_slot_labels();
    save_app_config(cfg_, config_path_);
}

bool LanVidVideoStudioApp::start_services(uint16_t video_port, uint16_t tally_port, uint16_t discovery_port) {
    stop_services();

    // 1. Multi-Cam receiver (all phones on :5006 demultiplexed by sender, or 5006/5008/...)
    lv::MultiCamConfig mc;
    mc.bind_ip = cfg_.primary_nic_ip.empty() ? "0.0.0.0" : cfg_.primary_nic_ip;
    mc.base_port = video_port;
    mc.port_mode = cfg_.video_port_mode == 1 ? lv::MultiCamPortMode::kSequentialPorts : lv::MultiCamPortMode::kSingleDemux;
    mc.max_slots = uint32_t(std::clamp(cfg_.video_max_slots, 1, 16));
    mc.vcam_count = uint32_t(std::clamp(cfg_.vcam_count, 0, 8));
    mc.vcam_placeholder = cfg_.vcam_placeholder;
    if (!multicam_.start(mc) && mc.bind_ip != "0.0.0.0") {
        mc.bind_ip = "0.0.0.0";
        if (!multicam_.start(mc)) return false;
    } else if (!multicam_.running()) {
        return false;
    }
    bound_video_port_ = multicam_.base_port();
    apply_slot_config();

    // 2. Studio Tally & Full-Duplex Intercom server (UDP 5022)
    const std::string bind_ip = cfg_.primary_nic_ip.empty() ? "0.0.0.0" : cfg_.primary_nic_ip;
    if (!intercom_.start_studio(tally_port, bind_ip) && !intercom_.start_studio(tally_port, "0.0.0.0")) {
        multicam_.stop();
        return false;
    }
    intercom_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);

    // 3. Discovery on the Video port (5023, role="video") + best-effort legacy :5099
    discovery_.set_role("video");
    discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    if (!discovery_.start(bound_video_port_, studio_name_buf_, discovery_port)) {
        intercom_.stop();
        multicam_.stop();
        return false;
    }
    bound_disc_port_ = discovery_port;
    legacy_discovery_.set_role("video");
    legacy_discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    (void)legacy_discovery_.start(bound_video_port_, studio_name_buf_, lv::kDiscoveryPort);

    // 4. mDNS (_lanvid._udp, role=video)
    mdns_.start(studio_name_buf_, bound_video_port_, {{"role", "video"}});
    services_running_.store(true, std::memory_order_release);
    return true;
}

void LanVidVideoStudioApp::stop_services() {
    services_running_.store(false, std::memory_order_release);
    mdns_.stop();
    legacy_discovery_.stop();
    discovery_.stop();
    intercom_.stop();
    multicam_.stop();
}

void LanVidVideoStudioApp::apply_slot_config() {
    for (uint32_t i = 0; i < multicam_.slot_count(); ++i) {
        if (i < cfg_.video_slot_labels.size() && !cfg_.video_slot_labels[i].empty())
            multicam_.set_slot_label(i, cfg_.video_slot_labels[i]);
        if (i < cfg_.video_slot_ips.size() && !cfg_.video_slot_ips[i].empty())
            multicam_.pin_slot(i, cfg_.video_slot_ips[i]);
    }
    multicam_.set_vcam(uint32_t(std::clamp(cfg_.vcam_count, 0, 8)), cfg_.vcam_placeholder);
    multicam_.set_program_slot(pgm_slot_);
    multicam_.set_preview_slot(pvw_slot_);
}

void LanVidVideoStudioApp::persist_slot_labels() {
    const uint32_t n = multicam_.slot_count();
    cfg_.video_slot_labels.resize(n);
    cfg_.video_slot_ips.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const auto info = multicam_.slot_info(i);
        cfg_.video_slot_labels[i] = info.label;
        cfg_.video_slot_ips[i] = info.pinned_ip;
    }
}

void LanVidVideoStudioApp::refresh_nics() { nics_ = lv::enumerate_network_interfaces(false, false); }

void LanVidVideoStudioApp::apply_nics() {
    discovery_.set_network_interfaces(cfg_.primary_nic_ip, cfg_.secondary_nic_ip);
    save_app_config(cfg_, config_path_);
}

void LanVidVideoStudioApp::start_lan_scan() {
    if (scanning_.exchange(true)) return;
    if (scan_thread_.joinable()) scan_thread_.join();
    const std::string pri = cfg_.primary_nic_ip, sec = cfg_.secondary_nic_ip;
    scan_thread_ = std::thread([this, pri, sec]() {
        auto found = lv::discovery_scan_roles(320, false, true, true, pri, sec);
        {
            std::lock_guard<std::mutex> lk(scan_mu_);
            peers_ = std::move(found);
        }
        scanning_.store(false, std::memory_order_release);
    });
}

// ================================================================================================
// Polling: previews, telemetry, Tally, intercom audio
// ================================================================================================
void LanVidVideoStudioApp::update_slot_texture(uint32_t slot, uint32_t tex_index) {
    if (tex_index >= kTexCount || !host_.create_texture) return;
    if (!tex_[tex_index]) tex_[tex_index] = host_.create_texture(host_.ctx);
    IGpuTexture* t = tex_[tex_index];
    if (!t) return;
    if (tex_slot_[tex_index] != int(slot)) {  // a different camera now feeds this view
        tex_slot_[tex_index] = int(slot);
        tex_seq_[tex_index] = 0;
    }
    if (!multicam_.latest_frame(slot, scratch_, tex_seq_[tex_index])) return;
    if (t->wants_rgba()) {
        for (size_t p = 0; p + 3 < scratch_.data.size(); p += 4) std::swap(scratch_.data[p], scratch_.data[p + 2]);
    }
    t->update(scratch_.data.data(), scratch_.width, scratch_.height);
}

void LanVidVideoStudioApp::sync_tally() {
    const auto cams = intercom_.studio_cameras();
    multicam_.update_camera_telemetry(cams);
    if (last_tally_sent_.size() != multicam_.slot_count()) last_tally_sent_.assign(multicam_.slot_count(), 0xFF);
    for (uint32_t i = 0; i < multicam_.slot_count(); ++i) {
        const auto info = multicam_.slot_info(i);
        if (!info.camera_id) continue;
        const uint8_t want = int(i) == pgm_slot_ ? lv::kTallyProgram : int(i) == pvw_slot_ ? lv::kTallyPreview : lv::kTallyStandby;
        if (last_tally_sent_[i] == want) continue;
        const bool rec = info.recording && want == lv::kTallyProgram;
        if (intercom_.studio_set_camera_tally(info.camera_id, want, info.label, rec)) last_tally_sent_[i] = want;
    }
}

void LanVidVideoStudioApp::poll_services() {
    intercom_.poll();
    sync_tally();
    // AUTO transition: after 0.5 s the preview becomes program
    if (auto_t0_ >= 0.0 && clock_s_ - auto_t0_ >= 0.5) {
        auto_t0_ = -1.0;
        take_program(pvw_slot_);
    }
    // Intercom: per-camera TALK / LATCH / ALL CALL
    const bool any_talk = director_ptt_ || director_latch_ ||
                          std::any_of(talk_.begin(), talk_.end(), [](bool b) { return b; }) ||
                          std::any_of(latch_.begin(), latch_.end(), [](bool b) { return b; });
    intercom_.set_intercom_state(any_talk, false, cfg_.intercom_ifb_volume, 0.0f);
    for (const auto& c : intercom_.studio_cameras()) {
        bool to_cam = all_call_ && (director_ptt_ || director_latch_);
        for (uint32_t i = 0; i < multicam_.slot_count() && i < lv::kMultiCamMaxSlots; ++i)
            if (multicam_.slot_info(i).camera_id == c.camera_id) to_cam = to_cam || talk_[i] || latch_[i];
        if (c.talk_to_cam != to_cam) intercom_.studio_set_camera_intercom(c.camera_id, to_cam, true, all_call_);
    }
    // Headphones: operator voices -> playback buffer
    if (phones_) {
        ifb_scratch_.resize(480);
        std::vector<int16_t> st(960);
        for (int k = 0; k < 4; ++k) {
            const int n = intercom_.pull_rx_pcm(ifb_scratch_.data(), 480);
            if (n <= 0) break;
            for (int i = 0; i < n; ++i) {
                const int16_t v = int16_t(std::clamp(ifb_scratch_[size_t(i)], -1.0f, 1.0f) * 32767.0f);
                st[size_t(i) * 2] = v;
                st[size_t(i) * 2 + 1] = v;
            }
            phones_buf_.push_pcm16(st.data(), size_t(n));
        }
    }
}

void LanVidVideoStudioApp::take_program(int slot) {
    if (slot < 0 || slot >= int(multicam_.slot_count())) return;
    if (slot == pvw_slot_) pvw_slot_ = pgm_slot_;  // classic swap: the old program goes to preview
    pgm_slot_ = slot;
    multicam_.set_program_slot(pgm_slot_);
    multicam_.set_preview_slot(pvw_slot_);
    sync_tally();
}

void LanVidVideoStudioApp::set_preview(int slot) {
    if (slot < 0 || slot >= int(multicam_.slot_count()) || slot == pgm_slot_) return;
    pvw_slot_ = slot;
    multicam_.set_preview_slot(slot);
    sync_tally();
}

void LanVidVideoStudioApp::start_intercom_audio() {
    stop_intercom_audio();
    mic_ = std::make_unique<lv::AudioCapture>();
    const bool mic_ok = mic_->start(lv::AudioCaptureMode::kMic, cfg_.intercom_mic_index, 240,
                                    [this](uint32_t, uint32_t, const int16_t* pcm, uint16_t n) {
                                        float mono[240];
                                        const uint16_t m = std::min<uint16_t>(n, 240);
                                        for (uint16_t i = 0; i < m; ++i)
                                            mono[i] = (float(pcm[2 * i]) + float(pcm[2 * i + 1])) / 65536.0f;
                                        intercom_.push_mic_pcm(mono, m);
                                    });
    if (!mic_ok) mic_.reset();
    phones_ = std::make_unique<lv::AudioPlayback>();
    if (!phones_->start(&phones_buf_)) phones_.reset();
    intercom_audio_status_ = std::string(mic_ ? "Мікрофон: активний" : "Мікрофон: недоступний") + "  ·  " +
                             (phones_ ? "Навушники: " + phones_->device_name() : std::string("Навушники: недоступні"));
}

void LanVidVideoStudioApp::stop_intercom_audio() {
    if (mic_) mic_->stop();
    mic_.reset();
    if (phones_) phones_->stop();
    phones_.reset();
}

std::string LanVidVideoStudioApp::quick_connect_url() const {
    std::string host = cfg_.primary_nic_ip;
    if (host.empty()) {
        for (const auto& n : nics_)
            if (!n.ipv4_address.empty() && n.ipv4_address != "127.0.0.1") {
                host = n.ipv4_address;
                break;
            }
    }
    std::string url = "lanvid://connect?host=" + host + "&port=" + std::to_string(bound_video_port_) +
                      "&tally=" + std::to_string(bound_tally_port()) + "&name=";
    std::string name = url_encode(studio_name_buf_);
    if (url.size() + name.size() > 104) name = name.substr(0, 104 - url.size());  // QR v6 (ECC M) capacity
    return url + name;
}

// ================================================================================================
// Virtual camera driver status / install
// ================================================================================================
std::string LanVidVideoStudioApp::vcam_driver_status() {
#if defined(_WIN32)
    HKEY key{};
    const std::string path = std::string("CLSID\\") + kVcamClsid1 + "\\InprocServer32";
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return "Installed (DirectShow: LanVid Cam 1..8 + Program Out)";
    }
    return "Not installed";
#elif defined(__APPLE__)
    // CoreMediaIO Camera Extension (legacy DAL plug-ins are no longer loaded since macOS 14).
    std::string out;
    if (FILE* f = popen("/usr/bin/systemextensionsctl list 2>/dev/null", "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f)) out += buf;
        pclose(f);
    }
    const size_t pos = out.find("com.manul02.lanvid.vcam.extension");
    if (pos != std::string::npos) {
        const size_t a = out.rfind('\n', pos), b = out.find('\n', pos);
        const std::string line = out.substr(a == std::string::npos ? 0 : a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        if (line.find("activated enabled") != std::string::npos)
            return "Installed (CoreMediaIO Camera Extension: LanVid Cam 1..8 + Program Out)";
        if (line.find("waiting for user") != std::string::npos)
            return "Waiting for approval: System Settings > General > Login Items & Extensions > Camera Extensions";
    }
    std::error_code ec;
    if (fs::exists("/Applications/LanVid Virtual Camera.app", ec)) return "Not activated (click Register)";
    return "Not installed";
#else
    std::error_code ec;
    int n = 0;
    for (int i = 10; i <= 18; ++i)
        if (fs::exists("/dev/video" + std::to_string(i), ec)) ++n;
    return n ? "v4l2loopback: " + std::to_string(n) + " devices (/dev/video10..)" : "v4l2loopback not loaded";
#endif
}

bool LanVidVideoStudioApp::install_vcam_driver(std::string& message) {
#if defined(_WIN32)
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    fs::path dll = fs::path(exe).parent_path() / "LanVidVCam.dll";
    if (!fs::exists(dll)) {
        message = "LanVidVCam.dll не знайдено поруч із lanvid_video_studio.exe";
        return false;
    }
    const std::string args = "/s \"" + dll.string() + "\"";
    const auto r = ShellExecuteA(nullptr, "runas", "regsvr32.exe", args.c_str(), nullptr, SW_HIDE);
    message = (INT_PTR(r) > 32) ? "Реєстрацію запущено (потрібне підтвердження адміністратора)" : "Не вдалося запустити regsvr32";
    return INT_PTR(r) > 32;
#elif defined(__APPLE__)
    std::error_code ec;
    if (!fs::exists("/Applications/LanVid Virtual Camera.app", ec)) {
        message = "Скопіюйте «LanVid Virtual Camera.app» (dist/apple або пакет LanVid) у /Applications і повторіть";
        return false;
    }
    const int rc = std::system("/usr/bin/open -a \"/Applications/LanVid Virtual Camera.app\" --args --install");
    message = rc == 0 ? "Запит надіслано. Якщо macOS попросить — увімкніть «LanVid Virtual Camera» у Системні параметри → "
                        "Загальні → Об'єкти входу та розширення → Розширення камери"
                      : "Не вдалося запустити LanVid Virtual Camera.app";
    return rc == 0;
#else
    message = "sudo modprobe v4l2loopback devices=9 video_nr=10,11,12,13,14,15,16,17,18 "
              "card_label=\"LanVid Cam 1,LanVid Cam 2,LanVid Cam 3,LanVid Cam 4,LanVid Cam 5,LanVid Cam 6,"
              "LanVid Cam 7,LanVid Cam 8,LanVid Program Out\" exclusive_caps=1";
    return false;
#endif
}

// ================================================================================================
// Frame
// ================================================================================================
void LanVidVideoStudioApp::render_frame() {
    anim_time_ += ImGui::GetIO().DeltaTime;
    clock_s_ += ImGui::GetIO().DeltaTime;
    poll_services();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LanVidVideoRoot", nullptr, flags);
    draw_lavender_backdrop(ImGui::GetWindowDrawList(), vp->Pos, vp->Size);

    draw_top_bar();
    ImGui::Separator();
    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##VideoSidebar", ImVec2(236.0f, avail_h), true);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##VideoMain", ImVec2(0.0f, avail_h), false);
    switch (page_) {
        case 0: page_cameras(); break;
        case 1: page_record_live(); break;
        case 2: page_outputs(); break;
        default: page_intercom_settings(); break;
    }
    ImGui::EndChild();
    ImGui::End();
}

void LanVidVideoStudioApp::draw_top_bar() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.accent_blue, "LanVid Video");
    ImGui::SameLine();
    draw_status_pill("[VIDEO]", p.accent_blue, true);
    int live = 0;
    for (uint32_t i = 0; i < multicam_.slot_count(); ++i) live += multicam_.slot_info(i).streaming ? 1 : 0;
    ImGui::SameLine();
    char pill[48];
    std::snprintf(pill, sizeof(pill), "%d / %u камер у мережі", live, multicam_.slot_count());
    draw_status_pill(pill, live ? p.green : p.text_muted, live > 0);
    if (multicam_.recording_any()) {
        ImGui::SameLine();
        const std::string rec = "● REC " + hms(multicam_.session_elapsed_us());
        draw_status_pill(rec.c_str(), p.red, std::fmod(anim_time_, 1.0f) < 0.6f);
    }
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "Video :%u | Tally/IFB :%u | Discovery :%u", unsigned(bound_video_port()),
                       unsigned(bound_tally_port()), unsigned(bound_discovery_port()));
    const float right_x = ImGui::GetWindowWidth() - 150.0f;
    if (right_x > ImGui::GetCursorPosX()) ImGui::SameLine(right_x);
    if (ImGui::SmallButton(cfg_.dark_theme ? "Світла Лаванда" : "Темна Студія")) {
        cfg_.dark_theme = !cfg_.dark_theme;
        apply_studio_theme(cfg_.dark_theme);
        save_app_config(cfg_, config_path_);
    }
}

void LanVidVideoStudioApp::draw_sidebar() {
    const auto& p = studio_palette();
    struct Item {
        const char* title;
        const char* hint;
    };
    const Item items[4] = {{"Камери та Підключення", "Cameras · Multiview · Discovery"},
                           {"Запис та Ефір", "ISO Recorder · Program / Preview"},
                           {"Віртуальні камери та Вивід", "OS Virtual Cams · OBS"},
                           {"Інтерком та Налаштування", "Intercom · Network · System"}};
    for (int i = 0; i < 4; ++i) {
        const bool sel = page_ == i;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_blue : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "%d. %s##page%d", i + 1, items[i].title, i);
        if (ImGui::Button(lbl, ImVec2(-1.0f, 40.0f))) page_ = i;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::TextColored(p.text_muted, "   %s", items[i].hint);
        ImGui::Spacing();
    }
    ImGui::Separator();
    ImGui::TextColored(p.text_muted, "PGM: CAM %d   PVW: CAM %d", pgm_slot_ + 1, pvw_slot_ + 1);
    ImGui::TextColored(p.text_muted, "Віртуальні камери: %d + Program", cfg_.vcam_count);
    ImGui::TextColored(p.text_muted, "Конфігурація: lanvid_video_config.json");
    ImGui::Spacing();
    if (ImGui::Button(services_running() ? "Перезапустити прийом" : "Запустити прийом", ImVec2(-1.0f, 30.0f))) {
        persist_slot_labels();
        start_services(uint16_t(cfg_.listen_port), lv::kVideoTallyPort, lv::kVideoDiscoveryPort);
    }
}

bool LanVidVideoStudioApp::draw_pill_tabs(const char* id, const char* const* labels, int count, int& active) {
    const auto& p = studio_palette();
    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 16.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f, 7.0f));
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine();
        const bool sel = active == i;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? p.accent_purp : p.card);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : p.text_main);
        if (ImGui::Button(labels[i])) {
            changed = active != i;
            active = i;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::Spacing();
    return changed;
}

void LanVidVideoStudioApp::draw_preview_image(int tex_index, float w, float h, const char* overlay, const ImVec4& border) {
    const auto& p = studio_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(p.card_soft), 10.0f);
    IGpuTexture* t = (tex_index >= 0 && tex_index < int(kTexCount)) ? tex_[size_t(tex_index)] : nullptr;
    if (t && t->width() > 0 && t->height() > 0) {
        const float aspect = float(t->width()) / float(std::max(1, t->height()));
        float dw = w, dh = w / aspect;
        if (dh > h) {
            dh = h;
            dw = h * aspect;
        }
        const ImVec2 q0(p0.x + (w - dw) * 0.5f, p0.y + (h - dh) * 0.5f);
        dl->AddImageRounded(t->imgui_id(), q0, ImVec2(q0.x + dw, q0.y + dh), ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, 8.0f);
    } else {
        const char* msg = "No Signal / Standby";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(p.text_muted), msg);
    }
    dl->AddRect(p0, p1, ImGui::GetColorU32(border), 10.0f, 0, 3.0f);
    if (overlay) dl->AddText(ImVec2(p0.x + 10, p0.y + 8), IM_COL32(255, 255, 255, 230), overlay);
    ImGui::Dummy(ImVec2(w, h));
}

// ================================================================================================
// Page 1: Cameras & Connections
// ================================================================================================
void LanVidVideoStudioApp::page_cameras() {
    const char* tabs[] = {"Усі камери (Multiview Grid)", "Додати та знайти в мережі (Discovery)"};
    draw_pill_tabs("cam_tabs", tabs, 2, sub_[0]);
    if (sub_[0] == 0) draw_multiview();
    else draw_discovery();
}

void LanVidVideoStudioApp::draw_multiview() {
    const auto& p = studio_palette();
    ImGui::TextColored(p.text_main, "Сітка:");
    ImGui::SameLine();
    const char* layouts[] = {"1×1", "2×2", "3×3"};
    for (int l = 1; l <= 3; ++l) {
        ImGui::SameLine();
        const bool sel = cfg_.video_multiview_layout == l;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, p.accent_blue);
        if (ImGui::SmallButton(layouts[l - 1])) {
            cfg_.video_multiview_layout = l;
            save_app_config(cfg_, config_path_);
        }
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextColored(p.text_muted, "  Камери надсилають на UDP :%u (%s)", unsigned(bound_video_port()),
                       cfg_.video_port_mode == 1 ? "порти 5006/5008/5010…" : "один порт, розподіл за відправником");

    const int grid = std::clamp(cfg_.video_multiview_layout, 1, 3);
    const uint32_t shown = std::min<uint32_t>(uint32_t(grid * grid), multicam_.slot_count());
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 10.0f;
    const float cw = (avail.x - gap * float(grid - 1)) / float(grid);
    const float ch = std::max(220.0f, (avail.y - gap * float(grid - 1)) / float(grid));
    for (uint32_t i = 0; i < shown; ++i) {
        if (i % uint32_t(grid)) ImGui::SameLine(0.0f, gap);
        draw_camera_card(i, cw, ch);
    }
}

void LanVidVideoStudioApp::draw_camera_card(uint32_t slot, float w, float h) {
    const auto& p = studio_palette();
    const auto info = multicam_.slot_info(slot);
    update_slot_texture(slot, slot);
    ImGui::PushID(int(slot));
    ImGui::BeginChild("##card", ImVec2(w, h), true, ImGuiWindowFlags_NoScrollbar);

    // header: editable label + tally / vcam pills
    if (editing_slot_ == int(slot)) {
        ImGui::SetNextItemWidth(w * 0.5f);
        if (ImGui::InputText("##lbl", label_buf_, sizeof(label_buf_), ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivated()) {
            multicam_.set_slot_label(slot, label_buf_);
            editing_slot_ = -1;
            persist_slot_labels();
            save_app_config(cfg_, config_path_);
        }
    } else {
        ImGui::TextColored(p.text_main, "%s", info.label.c_str());
        if (ImGui::IsItemClicked()) {
            editing_slot_ = int(slot);
            std::snprintf(label_buf_, sizeof(label_buf_), "%s", info.label.c_str());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Клацніть, щоб перейменувати");
    }
    ImGui::SameLine();
    const bool is_pgm = int(slot) == pgm_slot_, is_pvw = int(slot) == pvw_slot_;
    draw_status_pill(is_pgm ? "PGM" : is_pvw ? "PVW" : "STBY", is_pgm ? p.red : is_pvw ? p.green : p.text_muted, is_pgm || is_pvw);
    ImGui::SameLine();
    draw_status_pill(info.vcam_active ? "Virtual Cam: Active" : "Virtual Cam: Off", info.vcam_active ? p.accent_teal : p.text_muted,
                     false);

    // live preview
    const float pv_h = std::max(90.0f, h - 120.0f);
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay), "CAM %u", slot + 1);
    draw_preview_image(int(slot), ImGui::GetContentRegionAvail().x, pv_h, overlay,
                       is_pgm ? p.red : is_pvw ? p.green : p.border);

    // telemetry line
    if (info.streaming) {
        ImGui::TextColored(p.text_muted, "%dx%d · %.0f fps · %s · %.1f Mbps · %.0f ms", info.width, info.height, info.fps,
                           info.codec.c_str(), info.bitrate_mbps, info.latency_ms);
    } else {
        ImGui::TextColored(p.text_muted, info.bound ? "Камера %s: немає сигналу" : "Вільний слот — підключіть LanVid Camera",
                           info.sender_ip.c_str());
    }
    if (info.battery_pct >= 0) {
        ImGui::SameLine();
        ImGui::TextColored(info.battery_pct < 20 ? p.red : p.text_muted, " · 🔋%d%% · %d°C", info.battery_pct, info.temp_c);
    }

    // controls
    if (info.recording) ImGui::PushStyleColor(ImGuiCol_Button, p.red);
    if (ImGui::SmallButton(info.recording ? "■ STOP" : "● REC")) multicam_.rec_slot(slot, !info.recording);
    if (info.recording) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("PVW")) set_preview(int(slot));
    ImGui::SameLine();
    if (ImGui::SmallButton("PGM")) take_program(int(slot));
    if (slot < lv::kMultiCamMaxSlots && info.camera_id) {
        ImGui::SameLine();
        const char* lenses[] = {"Wide", "Ultra", "Tele", "Front"};
        const char* q[] = {"360p", "720p", "1080p", "4K"};
        ImGui::SetNextItemWidth(70);
        bool send = ImGui::Combo("##lens", &lens_[slot], lenses, 4);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        send |= ImGui::Combo("##q", &quality_[slot], q, 4);
        ImGui::SameLine();
        send |= ImGui::Checkbox("AF", &af_[slot]);
        if (!af_[slot]) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            send |= ImGui::SliderInt("##focus", &focus_[slot], 0, 1000, "F %d");
        }
        if (send) {
            lv::CameraControlPayload cc;
            cc.lens = uint8_t(lens_[slot]);
            cc.quality = uint8_t(quality_[slot]);
            cc.focus_auto = af_[slot] ? 1 : 0;
            cc.focus_pos = uint16_t(focus_[slot]);
            intercom_.studio_set_camera_control(info.camera_id, cc);
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void LanVidVideoStudioApp::draw_discovery() {
    const auto& p = studio_palette();
    const float col_w = ImGui::GetContentRegionAvail().x * 0.58f;
    ImGui::BeginChild("##disc_left", ImVec2(col_w, 0.0f), true);
    ImGui::TextColored(p.text_main, "Телефони LanVid Camera у мережі (Tally :%u)", unsigned(bound_tally_port()));
    ImGui::Separator();
    const auto cams = intercom_.studio_cameras();
    if (cams.empty()) ImGui::TextColored(p.text_muted, "Поки нікого. Відкрийте LanVid Camera на смартфоні або відскануйте QR-код.");
    for (const auto& c : cams) {
        ImGui::PushID(int(c.camera_id));
        const std::string ip = c.address.substr(0, c.address.rfind(':'));
        int bound_slot = -1;
        for (uint32_t i = 0; i < multicam_.slot_count(); ++i)
            if (multicam_.slot_info(i).sender_ip == ip) bound_slot = int(i);
        ImGui::Text("%s  (%s) · 🔋%u%% · %d°C", c.label.c_str(), ip.c_str(), unsigned(c.battery_pct), int(c.temp_c));
        ImGui::SameLine();
        if (bound_slot >= 0) {
            char s[32];
            std::snprintf(s, sizeof(s), "Слот CAM %d", bound_slot + 1);
            draw_status_pill(s, p.green, true);
        }
        for (uint32_t i = 0; i < multicam_.slot_count(); ++i) {
            const auto si = multicam_.slot_info(i);
            if (si.bound && si.sender_ip != ip) continue;
            ImGui::SameLine();
            char b[40];
            std::snprintf(b, sizeof(b), "Підключити в Слот %u", i + 1);
            if (ImGui::SmallButton(b)) {
                multicam_.pin_slot(i, ip);
                persist_slot_labels();
                save_app_config(cfg_, config_path_);
            }
            break;  // offer the first free slot only
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Ручне підключення за IP");
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##ip", "192.168.1.20", manual_ip_, sizeof(manual_ip_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderInt("##slot", &manual_slot_, 0, int(multicam_.slot_count()) - 1, "Слот %d");
    ImGui::SameLine();
    if (ImGui::Button("Закріпити телефон за слотом")) {
        multicam_.pin_slot(uint32_t(manual_slot_), manual_ip_);
        persist_slot_labels();
        save_app_config(cfg_, config_path_);
    }
    ImGui::TextColored(p.text_muted, "Слоти: ");
    for (uint32_t i = 0; i < multicam_.slot_count(); ++i) {
        const auto si = multicam_.slot_info(i);
        ImGui::TextColored(p.text_muted, "  CAM %u · %s · %s%s", i + 1, si.label.c_str(),
                           si.bound ? si.sender_ip.c_str() : "вільний", si.pinned_ip.empty() ? "" : " (закріплено)");
        if (si.bound || !si.pinned_ip.empty()) {
            ImGui::SameLine();
            ImGui::PushID(int(i) + 1000);
            if (ImGui::SmallButton("Звільнити")) {
                multicam_.pin_slot(i, "");
                multicam_.release_slot(i);
                persist_slot_labels();
                save_app_config(cfg_, config_path_);
            }
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    if (draw_nic_selector_panel("video_nic", nics_, cfg_.primary_nic_id, cfg_.primary_nic_ip, cfg_.secondary_nic_id,
                                cfg_.secondary_nic_ip, cfg_.dual_nic_redundancy_enabled, 0, [this]() { refresh_nics(); })) {
        apply_nics();
    }
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Інші відеостудії у LAN ([VIDEO])");
    ImGui::SameLine();
    if (ImGui::SmallButton(scanning_.load() ? "Пошук..." : "Оновити")) start_lan_scan();
    std::vector<lv::DiscoveredReceiver> copy;
    {
        std::lock_guard<std::mutex> lk(scan_mu_);
        copy = peers_;
    }
    draw_discovered_nodes_list("video_peers", copy, "video", nullptr);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##disc_qr", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.text_main, "QR-код швидкого підключення");
    ImGui::TextColored(p.text_muted, "Відскануйте камерою смартфона або в LanVid Camera");
    const std::string url = quick_connect_url();
    const auto qr = qr_encode(url);
    if (qr.size > 0) {
        const float avail = std::min(ImGui::GetContentRegionAvail().x, 320.0f);
        const float quiet = 4.0f;
        const float module = std::floor(avail / (float(qr.size) + 2.0f * quiet));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float side = module * (float(qr.size) + 2.0f * quiet);
        dl->AddRectFilled(o, ImVec2(o.x + side, o.y + side), IM_COL32_WHITE, 12.0f);
        for (int y = 0; y < qr.size; ++y)
            for (int x = 0; x < qr.size; ++x)
                if (qr.dark(x, y)) {
                    const ImVec2 a(o.x + (quiet + float(x)) * module, o.y + (quiet + float(y)) * module);
                    dl->AddRectFilled(a, ImVec2(a.x + module, a.y + module), IM_COL32(40, 30, 80, 255));
                }
        ImGui::Dummy(ImVec2(side, side));
    }
    ImGui::TextWrapped("%s", url.c_str());
    ImGui::EndChild();
}

// ================================================================================================
// Page 2: Record & Live
// ================================================================================================
void LanVidVideoStudioApp::page_record_live() {
    const char* tabs[] = {"Багатоканальний запис (ISO Recorder)", "Пульт ефіру та Tally (Program / Preview)"};
    draw_pill_tabs("rec_tabs", tabs, 2, sub_[1]);
    if (sub_[1] == 0) draw_iso_recorder();
    else draw_switcher();
}

void LanVidVideoStudioApp::draw_iso_recorder() {
    const auto& p = studio_palette();
    const bool rec = multicam_.recording_any();
    ImGui::BeginChild("##iso_head", ImVec2(0.0f, 150.0f), true);
    ImGui::PushStyleColor(ImGuiCol_Button, rec ? p.red : p.accent_purp);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f);
    if (ImGui::Button(rec ? "■  ЗУПИНИТИ ЗАПИС (STOP ALL)" : "●  ЗАПИСАТИ ВСІ КАМЕРИ (REC ALL)", ImVec2(380.0f, 64.0f))) {
        if (rec) {
            multicam_.rec_stop_all();
        } else {
            cfg_.iso_output_dir = iso_dir_;
            cfg_.iso_session_name = iso_session_;
            save_app_config(cfg_, config_path_);
            lv::IsoSessionConfig sc;
            sc.output_dir = iso_dir_;
            sc.session_name = iso_session_[0] ? iso_session_ : "LanVid";
            sc.container = cfg_.iso_container == 1 ? lv::IsoContainer::kMkv
                           : cfg_.iso_container == 2 ? lv::IsoContainer::kMov
                                                     : lv::IsoContainer::kMp4;
            sc.record_program = cfg_.iso_record_program;
            sc.slot_mask = uint32_t(cfg_.iso_slot_mask);
            multicam_.rec_start_all(sc);
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(p.text_main, "Таймкод сесії  %s", multicam_.session_timecode().c_str());
    ImGui::TextColored(p.text_muted, "Тривалість  %s", hms(multicam_.session_elapsed_us()).c_str());
    const uint64_t free_b = lv::MultiCamManager::disk_free_bytes(iso_dir_);
    ImGui::TextColored(free_b < (20ull << 30) ? p.red : p.text_muted, "Вільно на диску  %s", human_bytes(free_b).c_str());
    if (rec) ImGui::TextColored(p.text_muted, "%s", multicam_.session_dir().c_str());
    ImGui::EndGroup();
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("Папка записів", iso_dir_, sizeof(iso_dir_));
    ImGui::SameLine();
    if (ImGui::Button("Відкрити папку записів"))
        open_folder(rec && !multicam_.session_dir().empty() ? multicam_.session_dir() : std::string(iso_dir_));
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("Назва сесії", iso_session_, sizeof(iso_session_));
    ImGui::SameLine();
    const char* fmts[] = {"MP4", "MKV", "MOV"};
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("Формат", &cfg_.iso_container, fmts, 3)) save_app_config(cfg_, config_path_);
    ImGui::SameLine();
    if (ImGui::Checkbox("+ PGM Master Mix", &cfg_.iso_record_program)) save_app_config(cfg_, config_path_);
    ImGui::EndChild();

    ImGui::BeginChild("##iso_table", ImVec2(0.0f, 0.0f), true);
    if (ImGui::BeginTable("iso", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("У записі");
        ImGui::TableSetupColumn("Камера");
        ImGui::TableSetupColumn("Сигнал");
        ImGui::TableSetupColumn("Файл");
        ImGui::TableSetupColumn("Розмір");
        ImGui::TableSetupColumn("Швидкість");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (uint32_t i = 0; i < multicam_.slot_count(); ++i) {
            const auto info = multicam_.slot_info(i);
            const auto st = multicam_.rec_stats(i);
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = (cfg_.iso_slot_mask >> i) & 1;
            if (ImGui::Checkbox("##inc", &on)) {
                cfg_.iso_slot_mask = on ? (cfg_.iso_slot_mask | (1 << i)) : (cfg_.iso_slot_mask & ~(1 << i));
                save_app_config(cfg_, config_path_);
            }
            ImGui::TableNextColumn();
            ImGui::Text("CAM %u · %s", i + 1, info.label.c_str());
            ImGui::TableNextColumn();
            if (info.streaming) ImGui::Text("%dx%d %s %.0f fps", info.width, info.height, info.codec.c_str(), info.fps);
            else ImGui::TextColored(p.text_muted, "немає сигналу");
            ImGui::TableNextColumn();
            if (st.recording && st.waiting_for_keyframe) ImGui::TextColored(p.amber, "очікування ключового кадру…");
            else if (!st.path.empty()) ImGui::Text("%s", fs::path(st.path).filename().string().c_str());
            else ImGui::TextColored(p.text_muted, "—");
            ImGui::TableNextColumn();
            if (st.bytes) ImGui::Text("%s · %s", human_bytes(st.bytes).c_str(), hms(st.duration_us).c_str());
            ImGui::TableNextColumn();
            if (st.recording) ImGui::Text("%.1f MB/s", st.write_mbps);
            ImGui::TableNextColumn();
            if (st.recording) ImGui::PushStyleColor(ImGuiCol_Button, p.red);
            if (ImGui::SmallButton(st.recording ? "■ STOP" : "● REC")) multicam_.rec_slot(i, !st.recording);
            if (st.recording) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        const auto pg = multicam_.rec_program_stats();
        if (pg.recording || pg.bytes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextColored(p.red, "PGM Master Mix");
            ImGui::TableNextColumn();
            ImGui::Text("CAM %d в ефірі", pgm_slot_ + 1);
            ImGui::TableNextColumn();
            ImGui::Text("%s", fs::path(pg.path).filename().string().c_str());
            ImGui::TableNextColumn();
            if (pg.bytes) ImGui::Text("%s · %s", human_bytes(pg.bytes).c_str(), hms(pg.duration_us).c_str());
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(p.text_muted,
                       "H.264 / HEVC камери записуються без перекодування (оригінальний бітрейт); LanVid DCT і PGM — "
                       "апаратним кодером. Усі файли мають однаковий стартовий таймкод для мультикам-монтажу.");
    ImGui::EndChild();
}

void LanVidVideoStudioApp::draw_switcher() {
    const auto& p = studio_palette();
    update_slot_texture(uint32_t(std::max(0, pvw_slot_)), lv::kMultiCamMaxSlots + 1);
    update_slot_texture(uint32_t(std::max(0, pgm_slot_)), lv::kMultiCamMaxSlots);
    const float w = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const float h = w * 9.0f / 16.0f;
    ImGui::BeginGroup();
    ImGui::TextColored(p.green, "PREVIEW · CAM %d", pvw_slot_ + 1);
    draw_preview_image(int(lv::kMultiCamMaxSlots + 1), w, h, "PVW", p.green);
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::BeginGroup();
    ImGui::TextColored(p.red, "PROGRAM · CAM %d  →  LanVid Program Out", pgm_slot_ + 1);
    draw_preview_image(int(lv::kMultiCamMaxSlots), w, h, "PGM", p.red);
    ImGui::EndGroup();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, p.red);
    if (ImGui::Button("CUT", ImVec2(140.0f, 48.0f))) take_program(pvw_slot_);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, p.accent_purp);
    if (ImGui::Button(auto_t0_ >= 0.0 ? "AUTO…" : "AUTO (0.5 s)", ImVec2(160.0f, 48.0f)) && auto_t0_ < 0.0) auto_t0_ = clock_s_;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (auto_t0_ >= 0.0) {
        ImGui::SameLine();
        ImGui::ProgressBar(float((clock_s_ - auto_t0_) / 0.5), ImVec2(160.0f, 48.0f), "");
    }

    ImGui::Spacing();
    ImGui::TextColored(p.text_main, "Шина камер (клік = PREVIEW, подвійний клік = PROGRAM)");
    for (uint32_t i = 0; i < multicam_.slot_count(); ++i) {
        if (i) ImGui::SameLine();
        const auto info = multicam_.slot_info(i);
        const bool pg = int(i) == pgm_slot_, pv = int(i) == pvw_slot_;
        ImGui::PushStyleColor(ImGuiCol_Button, pg ? p.red : pv ? p.green : p.card_soft);
        ImGui::PushStyleColor(ImGuiCol_Text, (pg || pv) ? ImVec4(1, 1, 1, 1) : p.text_main);
        char b[48];
        std::snprintf(b, sizeof(b), "CAM %u%s##bus%u", i + 1, info.streaming ? "" : " ⨯", i);
        if (ImGui::Button(b, ImVec2(92.0f, 40.0f))) set_preview(int(i));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) take_program(int(i));
        ImGui::PopStyleColor(2);
    }
    ImGui::Spacing();
    ImGui::TextColored(p.text_muted, "Tally на смартфонах синхронізується автоматично: червона рамка — PGM, зелена — PVW.");
    for (const auto& c : intercom_.studio_cameras())
        ImGui::TextColored(p.text_muted, "  %s → %s%s", c.label.c_str(), tally_name(c.tally_state), c.rec_trigger ? " · REC" : "");
}

// ================================================================================================
// Page 3: Virtual cameras & outputs
// ================================================================================================
void LanVidVideoStudioApp::page_outputs() {
    const char* tabs[] = {"Пристрої захоплення ОС (Virtual Cameras)", "OBS Studio та Аудіо-міст (Integrations)"};
    draw_pill_tabs("out_tabs", tabs, 2, sub_[2]);
    if (sub_[2] == 0) draw_virtual_cameras();
    else draw_integrations();
}

void LanVidVideoStudioApp::draw_virtual_cameras() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##vcam", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.text_main, "Віртуальні камери ОС — OBS, Zoom, Teams, Meet, vMix, Discord бачать кожну камеру окремо");
    ImGui::TextColored(p.text_muted, "Драйвер: %s", vcam_status_.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Оновити статус")) vcam_status_ = vcam_driver_status();
    if (ImGui::Button("Зареєструвати віртуальні камери")) {
        install_vcam_driver(vcam_message_);
        vcam_status_ = vcam_driver_status();
    }
    if (!vcam_message_.empty()) ImGui::TextWrapped("%s", vcam_message_.c_str());
    ImGui::Separator();
    if (ImGui::SliderInt("Кількість віртуальних камер", &cfg_.vcam_count, 0, 8, "%d + Program Out")) {
        multicam_.set_vcam(uint32_t(cfg_.vcam_count), cfg_.vcam_placeholder);
        save_app_config(cfg_, config_path_);
    }
    if (ImGui::Checkbox("Лавандова заглушка «No Signal / Standby», коли камери немає", &cfg_.vcam_placeholder)) {
        multicam_.set_vcam(uint32_t(cfg_.vcam_count), cfg_.vcam_placeholder);
        save_app_config(cfg_, config_path_);
    }
    ImGui::TextColored(multicam_.video_bus_open() ? p.green : p.red, "Шина LanVidVideoBus_v1: %s",
                       multicam_.video_bus_open() ? "відкрита" : "недоступна");
    if (ImGui::BeginTable("vcams", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Пристрій ОС");
        ImGui::TableSetupColumn("Джерело");
        ImGui::TableSetupColumn("Формат");
        ImGui::TableSetupColumn("Стан");
        ImGui::TableHeadersRow();
        for (uint32_t i = 0; i <= lv::kVideoBusCameraSlots; ++i) {
            const bool is_pgm = i == lv::kVideoBusProgramSlot;
            if (!is_pgm && int(i) >= cfg_.vcam_count) continue;
            const uint32_t src_slot = is_pgm ? uint32_t(pgm_slot_) : i;
            const auto info = src_slot < multicam_.slot_count() ? multicam_.slot_info(src_slot) : lv::MultiCamSlotInfo{};
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", lv::video_bus_device_name(i));
            ImGui::TableNextColumn();
            ImGui::Text("→ %s%s%s", info.label.c_str(), info.source_name.empty() ? "" : " · ", info.source_name.c_str());
            ImGui::TableNextColumn();
            if (info.streaming) ImGui::Text("%dx%d @ %.0f", info.width, info.height, info.fps);
            else ImGui::TextColored(p.text_muted, "720p / 1080p / 4K / 9:16");
            ImGui::TableNextColumn();
            if (info.streaming) ImGui::TextColored(p.green, "Active");
            else ImGui::TextColored(p.text_muted, cfg_.vcam_placeholder ? "Заглушка" : "Чорний кадр");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void LanVidVideoStudioApp::draw_integrations() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##integr", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.text_main, "OBS Studio (obs-lanvid)");
#if defined(_WIN32)
    const bool obs_plugin = fs::exists("C:\\Program Files\\obs-studio\\obs-plugins\\64bit\\obs-lanvid.dll");
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    const bool obs_plugin = home && fs::exists(std::string(home) + "/Library/Application Support/obs-studio/plugins/obs-lanvid.plugin");
#else
    const char* home = std::getenv("HOME");
    const bool obs_plugin = home && fs::exists(std::string(home) + "/.config/obs-studio/plugins/obs-lanvid");
#endif
    ImGui::TextColored(obs_plugin ? p.green : p.text_muted, "Плагін obs-lanvid: %s", obs_plugin ? "встановлено" : "не знайдено");
    ImGui::TextColored(p.text_muted, "У OBS додайте джерело «LanVid Camera / Stream Source» або «Video Capture Device → LanVid Cam N».");
    if (ImGui::Checkbox("Маршрутизувати PGM у OBS Studio (obs-lanvid)", &cfg_.video_obs_output_enabled)) save_app_config(cfg_, config_path_);
    if (ImGui::SliderInt("Корекція A/V Lip-Sync (мс)", &cfg_.video_lip_sync_ms, -200, 200, "%d ms")) save_app_config(cfg_, config_path_);
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Вбудований звук камер");
    const char* routes[] = {"LanVid Audio (AoIP-міст)", "Системний вихід", "Вимкнено"};
    if (ImGui::Combo("Маршрутизація", &cfg_.video_audio_route, routes, 3)) save_app_config(cfg_, config_path_);
    for (uint32_t i = 0; i < multicam_.slot_count(); ++i) {
        const auto info = multicam_.slot_info(i);
        if (!info.streaming) continue;
        const float lvl = std::clamp((info.audio_peak_dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
        char ov[64];
        std::snprintf(ov, sizeof(ov), "%s  %.0f dBFS", info.label.c_str(), info.audio_peak_dbfs);
        ImGui::ProgressBar(lvl, ImVec2(-1.0f, 16.0f), ov);
    }
    ImGui::TextColored(p.text_muted, "Звук камер також записується в ISO-файл кожної камери (PCM 48 кГц стерео).");
    ImGui::EndChild();
}

// ================================================================================================
// Page 4: Intercom & Settings
// ================================================================================================
void LanVidVideoStudioApp::page_intercom_settings() {
    const char* tabs[] = {"Службовий зв'язок (Duplex Intercom)", "Мережа та Система (System Settings)"};
    draw_pill_tabs("ic_tabs", tabs, 2, sub_[3]);
    if (sub_[3] == 0) draw_intercom();
    else draw_system_settings();
}

void LanVidVideoStudioApp::draw_intercom() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##ic", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(p.text_main, "Пульт режисера (UDP :%u)", unsigned(bound_tally_port()));
#if defined(_WIN32)
    static std::vector<lv::AudioDeviceInfo> mics, outs;
    if (mics.empty()) mics = lv::list_audio_devices(false);
    if (outs.empty()) outs = lv::list_audio_devices(true);
    auto combo = [](const char* lbl, const std::vector<lv::AudioDeviceInfo>& devs, int& idx) {
        std::string cur = idx < 0 ? "Системний за замовчуванням" : "#" + std::to_string(idx);
        for (const auto& d : devs)
            if (d.index == idx) cur = d.name;
        bool changed = false;
        if (ImGui::BeginCombo(lbl, cur.c_str())) {
            if (ImGui::Selectable("Системний за замовчуванням", idx < 0)) { idx = -1; changed = true; }
            for (const auto& d : devs)
                if (ImGui::Selectable(d.name.c_str(), d.index == idx)) { idx = d.index; changed = true; }
            ImGui::EndCombo();
        }
        return changed;
    };
    bool dev_changed = combo("Мікрофон режисера", mics, cfg_.intercom_mic_index);
    dev_changed |= combo("Навушники режисера", outs, cfg_.intercom_out_index);
    if (dev_changed) save_app_config(cfg_, config_path_);
    if (ImGui::Button(mic_ || phones_ ? "Перезапустити аудіо інтеркому" : "Увімкнути мікрофон і навушники")) start_intercom_audio();
    if (!intercom_audio_status_.empty()) ImGui::TextColored(p.text_muted, "%s", intercom_audio_status_.c_str());
#else
    ImGui::TextColored(p.text_muted, "Мікрофон і навушники режисера на цій ОС — через LanVid Audio (вибір пристроїв там).");
#endif
    ImGui::Separator();
    const bool talking = director_ptt_ || director_latch_;
    ImGui::PushStyleColor(ImGuiCol_Button, talking ? p.red : p.accent_purp);
    ImGui::Button(talking ? "ALL CALL: В ЕФІРІ" : "ALL CALL (утримуйте)", ImVec2(260.0f, 52.0f));
    director_ptt_ = ImGui::IsItemActive();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("LATCH усіх", &director_latch_);
    ImGui::SameLine();
    ImGui::Checkbox("All Call", &all_call_);
    if (ImGui::SliderFloat("Гучність IFB", &cfg_.intercom_ifb_volume, 0.0f, 2.0f, "%.2f×")) save_app_config(cfg_, config_path_);
    if (ImGui::SliderFloat("Auto-Ducking програми", &cfg_.intercom_ducking_db, -24.0f, 0.0f, "%.1f dB")) save_app_config(cfg_, config_path_);
    ImGui::Separator();
    const auto cams = intercom_.studio_cameras();
    if (cams.empty()) ImGui::TextColored(p.text_muted, "Камери з’являться тут після підключення LanVid Camera.");
    for (const auto& c : cams) {
        int slot = -1;
        for (uint32_t i = 0; i < multicam_.slot_count(); ++i)
            if (multicam_.slot_info(i).camera_id == c.camera_id) slot = int(i);
        ImGui::PushID(int(c.camera_id));
        ImGui::Text("%-18s", c.label.c_str());
        ImGui::SameLine();
        if (slot >= 0 && slot < int(lv::kMultiCamMaxSlots)) {
            ImGui::PushStyleColor(ImGuiCol_Button, talk_[size_t(slot)] ? p.red : p.card_soft);
            ImGui::Button("TALK (PTT)", ImVec2(110.0f, 30.0f));
            talk_[size_t(slot)] = ImGui::IsItemActive();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            bool l = latch_[size_t(slot)];
            if (ImGui::Checkbox("LATCH", &l)) latch_[size_t(slot)] = l;
            ImGui::SameLine();
        }
        draw_status_pill(c.operator_speaking ? "Оператор говорить" : "тиша", c.operator_speaking ? p.amber : p.text_muted,
                         c.operator_speaking);
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextColored(p.text_muted, "Intercom TX: %llu  |  RX: %llu  |  1+1 Dedup: %llu",
                       static_cast<unsigned long long>(intercom_.intercom_tx_packets()),
                       static_cast<unsigned long long>(intercom_.intercom_rx_packets()),
                       static_cast<unsigned long long>(intercom_.intercom_duplicates_dropped()));
    ImGui::EndChild();
}

void LanVidVideoStudioApp::draw_system_settings() {
    const auto& p = studio_palette();
    ImGui::BeginChild("##sys", ImVec2(0.0f, 0.0f), true);
    ImGui::InputText("Назва відеостудії", studio_name_buf_, sizeof(studio_name_buf_));
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Порти");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Video (базовий)", &cfg_.listen_port);
    cfg_.listen_port = std::clamp(cfg_.listen_port, 1024, 65000);
    ImGui::TextColored(p.text_muted, "Tally / Intercom :%u   Discovery :%u", unsigned(lv::kVideoTallyPort),
                       unsigned(lv::kVideoDiscoveryPort));
    const char* modes[] = {"Один порт, розподіл за відправником (LanVid Camera)", "Окремий порт на слот (5006, 5008, 5010…)"};
    ImGui::SetNextItemWidth(420);
    ImGui::Combo("Режим портів", &cfg_.video_port_mode, modes, 2);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Кількість слотів камер", &cfg_.video_max_slots, 1, 16);
    if (ImGui::Button("Застосувати та перезапустити прийом")) {
        persist_slot_labels();
        save_app_config(cfg_, config_path_);
        start_services(uint16_t(cfg_.listen_port), lv::kVideoTallyPort, lv::kVideoDiscoveryPort);
    }
    ImGui::Separator();
    if (draw_nic_selector_panel("sys_nic", nics_, cfg_.primary_nic_id, cfg_.primary_nic_ip, cfg_.secondary_nic_id,
                                cfg_.secondary_nic_ip, cfg_.dual_nic_redundancy_enabled, 0, [this]() { refresh_nics(); })) {
        apply_nics();
    }
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Якість обслуговування (QoS / DSCP) та декодування");
    if (ImGui::Checkbox("DSCP AF41 (34) для відеопотоків", &cfg_.video_dscp_af41)) save_app_config(cfg_, config_path_);
    if (ImGui::Checkbox("DSCP CS7 / EF (46) для Tally та Intercom", &cfg_.tally_dscp_ef)) save_app_config(cfg_, config_path_);
    if (ImGui::Checkbox("Wi-Fi Redundancy 1+1 для Tally / Intercom", &cfg_.wifi_redundancy)) {
        intercom_.set_wifi_redundancy(cfg_.wifi_redundancy, 1500);
        save_app_config(cfg_, config_path_);
    }
    if (ImGui::Checkbox("Апаратне декодування H.264 / HEVC", &cfg_.video_hw_decode)) save_app_config(cfg_, config_path_);
    ImGui::Separator();
    ImGui::TextColored(p.text_main, "Тема");
    if (ImGui::RadioButton("Світла Лаванда", !cfg_.dark_theme)) {
        cfg_.dark_theme = false;
        apply_studio_theme(false);
        save_app_config(cfg_, config_path_);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Темна Студія", cfg_.dark_theme)) {
        cfg_.dark_theme = true;
        apply_studio_theme(true);
        save_app_config(cfg_, config_path_);
    }
    ImGui::EndChild();
}

}  // namespace lv::gui
