#include "mini_test.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "audio/multi_device_matrix.hpp"
#include "gui/app_config.hpp"

TEST(multi_device_matrix_routing_and_summing) {
    using namespace lv;

    MultiDeviceAudioMatrix mx;
    mx.set_buffer_size(256);
    mx.set_feedback_guard(true);

    // 4 slots: DAW (64ch), Net (64ch), Mic (2ch), Headphones (2ch)
    MatrixSlotConfig daw{0, MatrixSlotType::kVirtualBus, MatrixSlotRole::kDuplex, "DAW", "", -1, 64, 256, true};
    MatrixSlotConfig net{1, MatrixSlotType::kNetworkAoip, MatrixSlotRole::kDuplex, "Net", "", -1, 64, 256, true};
    MatrixSlotConfig mic{2, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput, "Mic", "", -1, 2, 256, true};
    MatrixSlotConfig hp{3, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, "Headphones", "", -1, 2, 256, true};

    CHECK(mx.add_slot(daw) == 0);
    CHECK(mx.add_slot(net) == 1);
    CHECK(mx.add_slot(mic) == 2);
    CHECK(mx.add_slot(hp) == 3);

    // 1. Feedback Loop Guard must block self-routing unless explicitly allowed
    CHECK(mx.set_crosspoint(0, 0, 0, 0) == CrosspointResult::kBlockedFeedbackLoop);
    CHECK(mx.set_crosspoint(0, 0, 0, 0, 0.0f, false, true) == CrosspointResult::kOk);
    CHECK(mx.remove_crosspoint(0, 0, 0, 0));

    // 2. 1-to-many fan-out + many-to-1 summing:
    //    Net Ch 0 (0.30) -> DAW Ch 0 (0 dB) AND Headphones Ch 0 (0 dB)
    //    Net Ch 1 (0.20) -> DAW Ch 1 (0 dB) AND Headphones Ch 1 (0 dB)
    //    Mic Ch 0 (0.40) -> Headphones Ch 0 (-6.0206 dB => x0.5 => +0.20 => total 0.50)
    //    Mic Ch 1 (0.50) -> Headphones Ch 1 (MUTED => should NOT contribute)
    CHECK(mx.set_crosspoint(1, 0, 0, 0, 0.0f, false) == CrosspointResult::kOk);
    CHECK(mx.set_crosspoint(1, 1, 0, 1, 0.0f, false) == CrosspointResult::kOk);
    CHECK(mx.set_crosspoint(1, 0, 3, 0, 0.0f, false) == CrosspointResult::kOk);
    CHECK(mx.set_crosspoint(1, 1, 3, 1, 0.0f, false) == CrosspointResult::kOk);
    CHECK(mx.set_crosspoint(2, 0, 3, 0, -6.0206f, false) == CrosspointResult::kOk);
    CHECK(mx.set_crosspoint(2, 1, 3, 1, 0.0f, true) == CrosspointResult::kOk);

    constexpr uint32_t kBlk = 256;
    std::vector<float> net_in0(kBlk, 0.30f), net_in1(kBlk, 0.20f);
    std::vector<float> mic_in0(kBlk, 0.40f), mic_in1(kBlk, 0.50f);
    const float* net_ptrs[2] = {net_in0.data(), net_in1.data()};
    const float* mic_ptrs[2] = {mic_in0.data(), mic_in1.data()};

    CHECK(mx.inject_slot_input(1, net_ptrs, 2, kBlk) == kBlk);
    CHECK(mx.inject_slot_input(2, mic_ptrs, 2, kBlk) == kBlk);

    mx.process_block_sync(kBlk);

    std::vector<float> daw_out0(kBlk, 0.0f), daw_out1(kBlk, 0.0f);
    std::vector<float> hp_out0(kBlk, 0.0f), hp_out1(kBlk, 0.0f);
    float* daw_ptrs[2] = {daw_out0.data(), daw_out1.data()};
    float* hp_ptrs[2] = {hp_out0.data(), hp_out1.data()};

    CHECK(mx.extract_slot_output(0, daw_ptrs, 2, kBlk) == kBlk);
    CHECK(mx.extract_slot_output(3, hp_ptrs, 2, kBlk) == kBlk);

    for (uint32_t i = 0; i < kBlk; ++i) {
        CHECK(std::fabs(daw_out0[i] - 0.30f) < 1e-4f);
        CHECK(std::fabs(daw_out1[i] - 0.20f) < 1e-4f);
        CHECK(std::fabs(hp_out0[i] - 0.50f) < 1e-3f);  // 0.30 + 0.40 * 0.5
        CHECK(std::fabs(hp_out1[i] - 0.20f) < 1e-4f);  // Mic Ch 1 muted
    }

    // 3. Preset 0 ("Mac/Net -> DAW (64ch) + Headphones (Ch 1/2)") creates 66 crosspoints
    mx.apply_preset(0, 1, 0, 3);
    CHECK(mx.crosspoints().size() == size_t(66));

    // 4. AppConfig persistence roundtrip (preserve user's existing config.json)
    namespace fs = std::filesystem;
    const fs::path cfg_path(lv::gui::config_file_path());
    std::string backup;
    bool had_existing = false;
    if (fs::exists(cfg_path)) {
        std::ifstream in(cfg_path, std::ios::binary);
        backup.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        had_existing = true;
    }

    lv::gui::AppConfig saved{};
    saved.audio_only = true;
    saved.active_tab = 3;
    saved.matrix_buffer_size = 128;
    saved.matrix_feedback_guard = true;
    saved.adaptive_jitter = 0;  // Fixed (Ethernet) must survive a restart
    saved.matrix_slots = mx.slots();
    saved.matrix_crosspoints = mx.crosspoints();
    CHECK(lv::gui::save_app_config(saved));

    lv::gui::AppConfig loaded{};
    CHECK(lv::gui::load_app_config(loaded));
    CHECK(loaded.audio_only == true);
    CHECK(loaded.active_tab == 3);
    CHECK(loaded.matrix_buffer_size == 128);
    CHECK(loaded.adaptive_jitter == 0);
    CHECK(loaded.matrix_slots.size() == size_t(4));
    CHECK(loaded.matrix_crosspoints.size() == size_t(66));
    CHECK(loaded.matrix_slots[3].name == "Headphones");

    // Restore original user config.json
    if (had_existing) {
        std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
        out.write(backup.data(), std::streamsize(backup.size()));
    } else {
        std::error_code ec;
        fs::remove(cfg_path, ec);
    }

    std::printf("    [matrix] 1-to-many + many-to-1 summing (0.30+0.20=0.50) + mute + feedback guard + config persistence OK\n");
}
