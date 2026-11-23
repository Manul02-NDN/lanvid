// macOS CoreAudio System Audio slots of the MultiDeviceAudioMatrix + the streaming resampler they use.
#include "mini_test.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "audio/multi_device_matrix.hpp"
#include "audio/stream_resampler.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;  // M_PI is not defined by MSVC without _USE_MATH_DEFINES

// Converts a sine from `src_rate` to `dst_rate` in random-sized blocks and checks frame count,
// amplitude and sample-to-sample continuity (a dropped/duplicated frame would show as a jump).
void check_resample(uint32_t src_rate, uint32_t dst_rate, double& max_err, double& ratio_err) {
    lv::StreamResampler rs;
    rs.configure(2, 8192);
    const double ratio = double(src_rate) / double(dst_rate);
    const double f = 997.0;  // Hz
    std::mt19937 rng(src_rate ^ dst_rate);
    uint64_t in_pos = 0, out_pos = 0;
    std::vector<float> a(4096), b(4096), o0(8192), o1(8192);
    max_err = 0.0;
    const uint64_t total_in = uint64_t(src_rate) * 2;  // 2 s
    while (in_pos < total_in) {
        const uint32_t n = 16 + rng() % 1000;
        for (uint32_t i = 0; i < n; ++i) {
            a[i] = float(0.8 * std::sin(2.0 * kPi * f * double(in_pos + i) / src_rate));
            b[i] = -a[i];
        }
        const float* in[2] = {a.data(), b.data()};
        rs.push(in, 2, n);
        in_pos += n;
        float* out[2] = {o0.data(), o1.data()};
        const uint32_t got = rs.produce(out, 2, 8192, ratio);
        for (uint32_t i = 0; i < got; ++i) {
            // Expected value at the output instant (linear interpolation error bound is tiny at 1 kHz).
            const double t = double(out_pos + i) / dst_rate;
            const double ref = 0.8 * std::sin(2.0 * kPi * f * t);
            max_err = std::max(max_err, std::fabs(double(o0[i]) - ref));
            max_err = std::max(max_err, std::fabs(double(o1[i]) + ref));
        }
        out_pos += got;
    }
    const double expect_out = double(in_pos) / ratio;
    ratio_err = std::fabs(double(out_pos) - expect_out) / expect_out;
}

}  // namespace

TEST(coreaudio_matrix_devices_and_48k_resampling) {
    // ---- 1. Resampler: any device rate <-> 48 kHz matrix, exact continuity across blocks ----
    const uint32_t pairs[][2] = {{44100, 48000}, {88200, 48000}, {96000, 48000}, {48000, 44100}, {48000, 96000}, {48000, 48000}};
    for (const auto& p : pairs) {
        double err = 0, rerr = 0;
        check_resample(p[0], p[1], err, rerr);
        std::printf("    [resampler] %5u -> %5u Hz: max error %.2e, frame-count error %.2e\n", p[0], p[1], err, rerr);
        CHECK(err < 5e-3);     // linear interpolation of a 1 kHz tone: < -46 dBFS, no glitches
        CHECK(rerr < 1e-3);    // no frames lost or duplicated between blocks
    }

#if defined(__APPLE__)
    // ---- 2. CoreAudio enumeration excludes the LanVid virtual device (it is the Virtual Bus slot) ----
    const auto outs = lv::list_system_audio_devices(true);
    const auto ins = lv::list_system_audio_devices(false);
    const auto outs_all = lv::list_system_audio_devices(true, false);
    bool lanvid_listed = false, lanvid_present = false;
    for (const auto& d : outs) lanvid_listed |= (d.uid == "LanVidVirtual64ch_UID");
    for (const auto& d : outs_all) lanvid_present |= (d.uid == "LanVidVirtual64ch_UID");
    std::printf("    [coreaudio] outputs=%zu inputs=%zu (LanVid Virtual 64ch installed: %s, listed: %s)\n", outs.size(),
                ins.size(), lanvid_present ? "yes" : "no", lanvid_listed ? "yes" : "no");
    for (const auto& d : outs) std::printf("      out: %s | %d ch | %u Hz%s\n", d.name.c_str(), d.channels, d.sample_rate, d.is_default ? " | default" : "");
    for (const auto& d : ins) std::printf("      in : %s | %d ch | %u Hz%s\n", d.name.c_str(), d.channels, d.sample_rate, d.is_default ? " | default" : "");
    CHECK(!lanvid_listed);
    for (const auto& d : outs) CHECK(d.channels > 0 && !d.uid.empty());

    // ---- 3. Live System Audio output slot on the default device (plays silence) ----
    bool has_default = false;
    for (const auto& d : outs) has_default |= d.is_default;
    if (!has_default) {
        std::printf("    [coreaudio] no default output device (CI runner?) -> live slot check skipped\n");
        return;
    }
    lv::MultiDeviceAudioMatrix mx;
    lv::MatrixSlotConfig s{};
    s.id = -1;
    s.type = lv::MatrixSlotType::kSystemAudio;
    s.role = lv::MatrixSlotRole::kOutput;
    s.name = "default output";
    s.device_id = "";  // follow the system default
    s.channels = 2;
    s.buffer_size = 256;
    const int sid = mx.add_slot(s);
    REQUIRE(sid >= 0);
    REQUIRE(mx.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto st = mx.slot_status(sid);
    mx.stop();
    std::printf("    [coreaudio] live output slot: connected=%d rate=%u Hz | %s\n", int(st.connected),
                st.device_sample_rate, st.status_text.c_str());
    CHECK(st.connected);
    CHECK(st.device_sample_rate >= 8000);
    CHECK(st.status_text.find("CoreAudio Active") != std::string::npos);
    CHECK(!mx.slot_status(sid).connected);  // stopped cleanly

    // Opening the LanVid virtual device as a System Audio slot is refused (would duplicate the SHM slot).
    if (lanvid_present) {
        lv::MultiDeviceAudioMatrix mx2;
        s.device_id = "LanVidVirtual64ch_UID";
        const int sid2 = mx2.add_slot(s);
        mx2.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto st2 = mx2.slot_status(sid2);
        mx2.stop();
        CHECK(!st2.connected);
        CHECK(st2.status_text.find("Virtual Bus") != std::string::npos);
    }
#endif
}
