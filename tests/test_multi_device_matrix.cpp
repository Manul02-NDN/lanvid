#include "mini_test.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "audio/multi_device_matrix.hpp"

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

    std::printf("    [matrix] 1-to-many + many-to-1 summing (0.30+0.20=0.50) + mute + feedback guard OK\n");
}

TEST(patchbay_visual_sync_with_64x64_matrix_and_feedback_loop_guard) {
    using namespace lv;

    MultiDeviceAudioMatrix mx;
    const int net = mx.add_slot({0, MatrixSlotType::kNetworkAoip, MatrixSlotRole::kDuplex, "Network AoIP", "", -1, 64, 256, true});
    const int daw = mx.add_slot({0, MatrixSlotType::kVirtualBus, MatrixSlotRole::kDuplex, "REAPER Virtual ASIO", "", -1, 64, 256, true});
    const int usb_in = mx.add_slot({0, MatrixSlotType::kSystemAudio, MatrixSlotRole::kInput, "USB Mic In", "usb-focusrite-2i2", -1, 2, 256, true});
    const int usb_out = mx.add_slot({0, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, "USB Headphones", "usb-focusrite-2i2", -1, 2, 256, true});
    const int phones = mx.add_slot({0, MatrixSlotType::kSystemAudio, MatrixSlotRole::kOutput, "PC Headphones", "realtek-hd-out", -1, 2, 256, true});
    CHECK(net >= 0 && daw >= 0 && usb_in >= 0 && usb_out >= 0 && phones >= 0);

    // 1. Visual Patchbay 64ch block -> syncs 64 crosspoints in 64x64 Matrix
    CHECK(mx.connect_slot_block(net, daw, 64, 0.0f, false) == CrosspointResult::kOk);
    CHECK(mx.crosspoints().size() == 64u);
    auto links = mx.slot_patch_links();
    CHECK(links.size() == 1u);
    CHECK(links[0].src_slot == net && links[0].dst_slot == daw && links[0].channel_count == 64);

    // 2. Visual Patchbay Stereo Ch 1-2 block -> syncs 2 crosspoints in 64x64 Matrix
    CHECK(mx.connect_slot_block(net, phones, 2, -3.5f, false) == CrosspointResult::kOk);
    CHECK(mx.crosspoints().size() == 66u);
    links = mx.slot_patch_links();
    CHECK(links.size() == 2u);

    // 3. Detailed 64x64 Matrix edit (remove Ch 63) -> immediately reflected in Visual Patchbay link count!
    CHECK(mx.remove_crosspoint(net, 63, daw, 63));
    links = mx.slot_patch_links();
    CHECK(links[0].channel_count == 63);

    // 4. Visual Patchbay Gain & Mute -> updates all crosspoints in 64x64 Matrix simultaneously
    CHECK(mx.set_slot_block_gain_mute(net, daw, -6.0f, true));
    MatrixCrosspoint cp0{};
    CHECK(mx.has_crosspoint(net, 0, daw, 0, &cp0));
    CHECK(std::fabs(cp0.gain_db - (-6.0f)) < 0.01f && cp0.mute);
    links = mx.slot_patch_links();
    CHECK(links[0].all_muted && std::fabs(links[0].gain_db - (-6.0f)) < 0.01f);

    // 5. Feedback Loop Guard: blocks BOTH self-loop AND same physical device loop!
    mx.set_feedback_guard(true);
    CHECK(mx.connect_slot_block(daw, daw, 64) == CrosspointResult::kBlockedFeedbackLoop);
    CHECK(mx.connect_slot_block(usb_in, usb_out, 2) == CrosspointResult::kBlockedFeedbackLoop);
    CHECK(mx.set_crosspoint(usb_in, 0, usb_out, 0) == CrosspointResult::kBlockedFeedbackLoop);

    // Disabling Feedback Loop Guard explicitly permits the route
    mx.set_feedback_guard(false);
    CHECK(mx.connect_slot_block(usb_in, usb_out, 2) == CrosspointResult::kOk);
    CHECK(mx.disconnect_slot_block(usb_in, usb_out));

    std::printf("    [patchbay<->matrix] 64ch+Stereo sync, gain/mute propagation & same-device Feedback Loop Guard OK\n");
}
