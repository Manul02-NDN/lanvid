// LanVid Video Multi-Cam: simultaneous reception of 3+ phones on one port, per-camera ISO recording into
// separate files, per-camera OS virtual camera channels on LanVidVideoBus_v1, and persistence of the
// slot / ISO / virtual-camera configuration in lanvid_video_config.json.
#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "common/image.hpp"
#include "gui/app_config.hpp"
#include "lanvid.hpp"
#include "video/iso_recorder.hpp"
#include "video/multicam_manager.hpp"
#include "video/video_bus.hpp"

namespace fs = std::filesystem;

namespace {

std::string tmp_dir(const char* tag) {
    const auto d = fs::temp_directory_path() / (std::string("lanvid_") + tag + "_" + std::to_string(lv::now_us()));
    fs::create_directories(d);
    return d.string();
}

std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Walks the top-level MP4/MOV boxes: ftyp, mdat (64-bit size patched on close) and moov must be present
// and cover the file exactly; returns the video sample count from the first stsz.
bool mp4_valid(const std::vector<uint8_t>& f, uint32_t& samples) {
    size_t pos = 0;
    bool ftyp = false, mdat = false, moov = false;
    samples = 0;
    while (pos + 8 <= f.size()) {
        uint64_t sz = be32(&f[pos]);
        const std::string type(reinterpret_cast<const char*>(&f[pos + 4]), 4);
        size_t hdr = 8;
        if (sz == 1) {
            if (pos + 16 > f.size()) return false;
            sz = (uint64_t(be32(&f[pos + 8])) << 32) | be32(&f[pos + 12]);
            hdr = 16;
        }
        if (sz < hdr || pos + sz > f.size()) return false;
        if (type == "ftyp") ftyp = true;
        if (type == "mdat") mdat = sz > 16;
        if (type == "moov") {
            moov = true;
            for (size_t k = pos + hdr; k + 20 <= pos + sz; ++k) {
                if (std::memcmp(&f[k], "stsz", 4) == 0) {
                    samples = be32(&f[k + 12]);
                    break;
                }
            }
        }
        pos += size_t(sz);
    }
    return ftyp && mdat && moov && pos == f.size();
}

bool mkv_valid(const std::vector<uint8_t>& f) {
    if (f.size() < 64 || be32(&f[0]) != 0x1A45DFA3u) return false;
    // Segment size must have been patched (not the "unknown size" placeholder)
    for (size_t k = 0; k + 12 < f.size() && k < 256; ++k) {
        if (be32(&f[k]) == 0x18538067u) {
            if (f[k + 4] != 0x01) return false;
            uint64_t sz = 0;
            for (int i = 0; i < 7; ++i) sz = (sz << 8) | f[k + 5 + i];
            return sz != 0x00FFFFFFFFFFFFFFull && k + 12 + sz == f.size();
        }
    }
    return false;
}

struct ColorCam {
    lanvid::Sender tx;
    uint8_t b, g, r;
};

}  // namespace

TEST(multicam_three_cameras_iso_recording_and_video_bus) {
    const auto probe = lv::create_video_encoder(lv::VideoCodecId::kH264, lv::VideoEncoderConfig{320, 240, 30.0, 2000, 30, true});
    const bool have_h264 = probe != nullptr;
    const std::string out_dir = tmp_dir("iso");
    const std::string bus_name = "LanVidVideoBusTest_" + std::to_string(lv::now_us() % 1000000);

    lv::MultiCamManager mgr;
    lv::MultiCamConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.base_port = 0;  // ephemeral: all three phones send to the same port
    cfg.max_slots = 4;
    cfg.video_bus_name = bus_name;
    cfg.vcam_count = 4;
    REQUIRE(mgr.start(cfg));
    const uint16_t port = mgr.base_port();
    REQUIRE(port != 0);
    REQUIRE(mgr.video_bus_open());
    mgr.set_slot_label(0, "CAM 1 • Wide");
    mgr.set_slot_label(1, "CAM 2 • Close");
    mgr.set_slot_label(2, "CAM 3 • Stage");

    // Three "phones": each its own socket (own source port), each a distinct solid colour.
    const uint8_t colors[3][3] = {{40, 40, 220}, {40, 200, 40}, {220, 60, 40}};  // BGR: red, green, blue
    std::vector<std::unique_ptr<ColorCam>> cams;
    for (int c = 0; c < 3; ++c) {
        auto cam = std::make_unique<ColorCam>();
        cam->b = colors[c][0];
        cam->g = colors[c][1];
        cam->r = colors[c][2];
        auto sc = lanvid::Sender::default_config("127.0.0.1", port);
        sc.width = 320;
        sc.height = 240;
        sc.fps = 30.0;
        sc.codec = have_h264 ? LV_CODEC_H264 : LV_CODEC_LANVID_DCT;
        sc.bitrate_kbps = 2000;
        REQUIRE(cam->tx.start(sc).is_ok());
        cams.push_back(std::move(cam));
    }

    lv::IsoSessionConfig session;
    session.output_dir = out_dir;
    session.session_name = "UnitTest";
    session.container = lv::IsoContainer::kMp4;

    std::vector<uint8_t> frame(320 * 240 * 4);
    bool rec_started = false;
    const uint64_t t0 = lv::now_us();
    uint64_t next = t0;
    for (int f = 0; lv::now_us() - t0 < 3'500'000; ++f) {
        for (auto& cam : cams) {
            for (size_t p = 0; p < frame.size(); p += 4) {
                frame[p] = cam->b;
                frame[p + 1] = cam->g;
                frame[p + 2] = cam->r;
                frame[p + 3] = 255;
            }
            cam->tx.push_frame(frame.data(), 320, 240, 320 * 4, LV_PIXEL_FORMAT_BGRA8888);
        }
        if (!rec_started && lv::now_us() - t0 > 800'000) {
            REQUIRE(mgr.rec_start_all(session));  // REC ALL once all three are streaming
            rec_started = true;
        }
        next += 33'333;
        lv::sleep_until_us(next, [] {});
    }

    // 1) three independent slots, all streaming
    int streaming = 0;
    for (uint32_t i = 0; i < mgr.slot_count(); ++i) {
        const auto info = mgr.slot_info(i);
        if (info.streaming) ++streaming;
        if (i < 3) {
            CHECK(info.bound);
            CHECK(info.width == 320 && info.height == 240);
            CHECK(info.recording);
        }
    }
    CHECK(streaming == 3);

    // 2) each slot's frames are in its own LanVidVideoBus_v1 channel, never mixed: colour per channel
    lv::VideoBus reader;
    REQUIRE(reader.open(bus_name.c_str()));
    std::vector<uint8_t> px(lv::kVideoBusFrameBytes);
    int matched = 0;
    bool used[3] = {false, false, false};
    for (uint32_t i = 0; i < 3; ++i) {
        lv::VideoBus::FrameInfo fi;
        CHECK(reader.read_latest(i, px.data(), px.size(), fi));
        CHECK(fi.width == 320 && fi.height == 240 && fi.format == lv::VideoBusFormat::kBGRA);
        CHECK(std::string(fi.label).rfind("CAM " + std::to_string(i + 1), 0) == 0);
        // centre pixel -> which camera?
        const uint8_t* c = &px[(size_t(120) * fi.stride) + 160 * 4];
        for (int k = 0; k < 3; ++k) {
            if (std::abs(int(c[0]) - colors[k][0]) < 40 && std::abs(int(c[1]) - colors[k][1]) < 40 &&
                std::abs(int(c[2]) - colors[k][2]) < 40) {
                CHECK(!used[k]);  // a colour may appear in exactly one channel
                used[k] = true;
                ++matched;
                // the slot's preview must show the same camera as its bus channel
                lv::Image prev;
                uint64_t seq = 0;
                CHECK(mgr.latest_frame(i, prev, seq));
                const uint8_t* q = &prev.data[size_t(120) * prev.stride + 160 * 4];
                CHECK(std::abs(int(q[0]) - colors[k][0]) < 40 && std::abs(int(q[2]) - colors[k][2]) < 40);
            }
        }
    }
    CHECK(matched == 3);
    // slot 4 has no camera: the virtual device must fall back to the placeholder
    lv::VideoBus::FrameInfo idle;
    CHECK(!reader.read_latest(3, px.data(), px.size(), idle));
    CHECK(std::string(idle.label) == "CAM 4");

    // 3) per-camera ISO files, independent and correctly closed
    const std::string dir = mgr.session_dir();
    mgr.rec_stop_all();
    for (auto& cam : cams) cam->tx.stop();
    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(dir)) files.push_back(e.path().filename().string());
    std::printf("    [multicam] port=%u slots streaming=%d bus matched=%d | session %s -> %zu files (h264=%d)\n",
                unsigned(port), streaming, matched, fs::path(dir).filename().string().c_str(), files.size(),
                int(have_h264));
    if (have_h264) {
        for (int c = 1; c <= 3; ++c) {
            std::string found;
            for (const auto& fn : files)
                if (fn.rfind("CAM" + std::to_string(c) + "_", 0) == 0) found = fn;
            CHECK(!found.empty());
            if (found.empty()) continue;
            const auto data = read_file(dir + "/" + found);
            uint32_t samples = 0;
            CHECK(mp4_valid(data, samples));
            CHECK(samples >= 30);  // >= ~1 s of video
            std::printf("      %s: %zu bytes, %u video samples\n", found.c_str(), data.size(), samples);
        }
        CHECK(files.size() == 3);
    }
    mgr.stop();
    std::error_code ec;
    fs::remove_all(out_dir, ec);
}

TEST(iso_recorder_mp4_mkv_mov_containers_close_cleanly) {
    auto enc = lv::create_video_encoder(lv::VideoCodecId::kH264, lv::VideoEncoderConfig{320, 240, 30.0, 1500, 15, true});
    if (!enc) {
        std::printf("    [iso] SKIPPED (no H.264 encoder on this build)\n");
        return;
    }
    const std::string dir = tmp_dir("iso_containers");
    const lv::IsoContainer kinds[3] = {lv::IsoContainer::kMp4, lv::IsoContainer::kMkv, lv::IsoContainer::kMov};
    lv::Image img;
    img.allocate(320, 240, lv::PixelOrder::kBGRA);
    for (auto kind : kinds) {
        lv::IsoRecorder rec;
        lv::IsoRecorderConfig rc;
        rc.path = dir + "/CAM1_Test." + lv::iso_container_extension(kind);
        rc.container = kind;
        rc.width = 320;
        rc.height = 240;
        rc.audio_channels = 2;
        rc.timecode_start_frame = 3600 * 30;  // 01:00:00:00
        REQUIRE(rec.open(rc));
        std::vector<int16_t> pcm(1600 * 2, 1000);
        for (int f = 0; f < 45; ++f) {
            for (size_t p = 0; p < img.data.size(); ++p) img.data[p] = uint8_t((p / 4 + f * 7) & 0xFF);
            lv::EncodedVideoPacket pkt;
            if (!enc->encode_image(img, pkt)) continue;
            CHECK(rec.write_video(pkt.data.data(), pkt.data.size(), pkt.is_keyframe, uint64_t(f) * 33333));
            CHECK(rec.write_audio(pcm.data(), 1600, uint64_t(f) * 33333));
        }
        CHECK(rec.close());
        const auto st = rec.stats();
        const auto data = read_file(rc.path);
        CHECK(st.video_frames >= 40);
        CHECK(data.size() == st.bytes_written || kind == lv::IsoContainer::kMkv);
        if (kind == lv::IsoContainer::kMkv) {
            CHECK(mkv_valid(data));
        } else {
            uint32_t samples = 0;
            CHECK(mp4_valid(data, samples));
            CHECK(samples == st.video_frames);
        }
        std::printf("    [iso %s] %llu frames, %zu bytes, timecode %s\n", lv::iso_container_extension(kind),
                    (unsigned long long)st.video_frames, data.size(), lv::iso_timecode_string(rc.timecode_start_frame, 30).c_str());
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(video_bus_slots_isolated_and_placeholder) {
    const std::string name = "LanVidVideoBusIso_" + std::to_string(lv::now_us() % 1000000);
    lv::VideoBus host, reader;
    REQUIRE(host.open(name.c_str()));
    REQUIRE(reader.open(name.c_str()));
    std::vector<uint8_t> f(64 * 48 * 4), out(lv::kVideoBusFrameBytes);
    for (uint32_t s = 0; s < lv::kVideoBusSlots; ++s) {
        std::fill(f.begin(), f.end(), uint8_t(10 + s * 20));
        host.set_slot_info(s, (std::string("CAM ") + std::to_string(s + 1)).c_str(), "phone", true, false);
        CHECK(host.publish(s, lv::VideoBusFormat::kBGRA, 64, 48, f.data(), 0, lv::now_us()));
    }
    for (uint32_t s = 0; s < lv::kVideoBusSlots; ++s) {
        lv::VideoBus::FrameInfo fi;
        CHECK(reader.read_latest(s, out.data(), out.size(), fi));
        CHECK(fi.width == 64 && fi.height == 48 && out[0] == uint8_t(10 + s * 20) && out[64 * 48 * 4 - 1] == uint8_t(10 + s * 20));
    }
    // a disconnected camera reports "no fresh frame" -> placeholder card is rendered by the driver
    host.set_slot_info(1, "CAM 2", "phone", false, false);
    lv::VideoBus::FrameInfo fi;
    CHECK(!reader.read_latest(1, out.data(), out.size(), fi));
    std::vector<uint8_t> card(1280 * 720 * 4);
    lv::video_bus_render_placeholder_bgra(card.data(), 1280, 720, "LanVid Cam 2", "No Signal / Standby", 0);
    // lavender background, darker text pixels in the middle band
    CHECK(card[2] > 200 && card[0] == 255);
    int dark = 0;
    for (size_t y = 340; y < 380; ++y)
        for (size_t x = 300; x < 980; ++x) dark += card[(y * 1280 + x) * 4 + 2] < 150 ? 1 : 0;
    CHECK(dark > 200);
    CHECK(std::string(lv::video_bus_device_name(lv::kVideoBusProgramSlot)) == "LanVid Program Out");
    // 4K NV12 fits, 4K BGRA does not
    std::vector<uint8_t> big(3840 * 2160 * 3 / 2, 128);
    CHECK(host.publish(2, lv::VideoBusFormat::kNV12, 3840, 2160, big.data(), 0, 1));
    CHECK(!host.publish(2, lv::VideoBusFormat::kBGRA, 3840, 2160, big.data(), 0, 1));
}

TEST(video_config_slots_iso_vcam_persist) {
    const std::string dir = tmp_dir("cfg");
    const std::string path = dir + "/lanvid_video_config.json";
    lv::gui::AppConfig a;
    a.video_max_slots = 6;
    a.video_port_mode = 1;
    a.video_multiview_layout = 3;
    a.video_slot_labels = {"CAM 1 • Wide", "CAM 2 \"Close\"", "CAM 3"};
    a.video_slot_ips = {"192.168.1.20", "", "192.168.1.22"};
    a.iso_output_dir = dir + "/Recordings";
    a.iso_session_name = "Concert";
    a.iso_container = 2;
    a.iso_record_program = true;
    a.iso_slot_mask = 0x5;
    a.vcam_count = 3;
    a.vcam_placeholder = false;
    REQUIRE(lv::gui::save_app_config(a, path));
    lv::gui::AppConfig b;
    REQUIRE(lv::gui::load_app_config(b, path));
    CHECK(b.video_max_slots == 6 && b.video_port_mode == 1 && b.video_multiview_layout == 3);
    REQUIRE(b.video_slot_labels.size() == 3);
    CHECK(b.video_slot_labels[0] == "CAM 1 • Wide");
    CHECK(b.video_slot_labels[1] == "CAM 2 \"Close\"");
    REQUIRE(b.video_slot_ips.size() == 3);
    CHECK(b.video_slot_ips[0] == "192.168.1.20" && b.video_slot_ips[1].empty() && b.video_slot_ips[2] == "192.168.1.22");
    CHECK(b.iso_output_dir == a.iso_output_dir && b.iso_session_name == "Concert");
    CHECK(b.iso_container == 2 && b.iso_record_program && b.iso_slot_mask == 0x5);
    CHECK(b.vcam_count == 3 && !b.vcam_placeholder);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Remote phone control from LanVid Video (lens / focus / quality preset) over the Tally channel:
// packet round-trip, Studio -> Camera delivery, and de-duplication of the 3x redundant sends.
TEST(camera_remote_control_lens_focus_quality_roundtrip) {
    lv::CameraControlPayload in;
    in.camera_id = 7;
    in.revision = 42;
    in.lens = lv::kLensTele;
    in.focus_auto = 0;
    in.focus_pos = 733;
    in.quality = lv::kQuality4K;
    in.zoom_x100 = 250;
    uint8_t wire[lv::kCameraControlSize] = {};
    lv::write_camera_control(wire, in);
    lv::CameraControlPayload out;
    REQUIRE(lv::parse_camera_control(wire, sizeof(wire), out));
    CHECK(out.camera_id == 7 && out.revision == 42 && out.lens == lv::kLensTele && out.focus_auto == 0);
    CHECK(out.focus_pos == 733 && out.quality == lv::kQuality4K && out.zoom_x100 == 250);
    CHECK(!lv::parse_camera_control(wire, sizeof(wire) - 1, out));

    lv::TallyIntercomEngine studio, camera;
    REQUIRE(studio.start_studio(0, "127.0.0.1"));
    std::atomic<int> calls{0};
    std::atomic<int> last_lens{-1}, last_quality{-1}, last_focus{-1};
    camera.camera_set_control_callback([&](const lv::CameraControlPayload& c) {
        ++calls;
        last_lens = c.lens;
        last_quality = c.quality;
        last_focus = c.focus_pos;
    });
    REQUIRE(camera.start_camera("127.0.0.1", studio.bound_port(), 3, "CAM 3"));
    for (int i = 0; i < 100 && studio.studio_cameras().empty(); ++i) {
        camera.poll();
        studio.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(!studio.studio_cameras().empty());

    lv::CameraControlPayload ctl;
    ctl.lens = lv::kLensUltraWide;
    ctl.quality = lv::kQuality360p;
    ctl.focus_auto = 0;
    ctl.focus_pos = 120;
    REQUIRE(studio.studio_set_camera_control(3, ctl));
    for (int i = 0; i < 60 && calls.load() == 0; ++i) {
        studio.poll();
        camera.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    for (int i = 0; i < 20; ++i) {  // let the redundant copies arrive too
        studio.poll();
        camera.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(calls.load() == 1);  // 3x redundant packets -> one callback
    CHECK(last_lens.load() == lv::kLensUltraWide && last_quality.load() == lv::kQuality360p && last_focus.load() == 120);
    CHECK(camera.camera_last_control().lens == lv::kLensUltraWide);

    ctl.lens = lv::kLensFront;
    ctl.quality = lv::kQuality1080p;
    REQUIRE(studio.studio_set_camera_control(3, ctl));
    for (int i = 0; i < 60 && calls.load() < 2; ++i) {
        studio.poll();
        camera.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    CHECK(calls.load() == 2 && last_lens.load() == lv::kLensFront && last_quality.load() == lv::kQuality1080p);
    CHECK(!studio.studio_set_camera_control(99, ctl));  // unknown camera
    camera.stop();
    studio.stop();
}
